/*
 * Grok Sphere standalone - Wi-Fi, setup hotspot (captive portal), mDNS and the web setup page.
 * The same setup page is served on the hotspot (http://192.168.4.1) and on the home
 * network (http://<ip> or http://grok-sphere.local). Keys are never sent back to the
 * browser: the page only shows "****" + the last 4 characters.
 */
#include "net.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app.h"
#include "assist_util.h"
#include "assistant.h"
#include "clock_weather.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "formutil.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "mdns.h"
#include "sysctl.h"
#include "ui.h"

#define BIT_CONNECTED  BIT0
#define MAX_SCAN       20
#define PAGE_MAX       24576
#define BODY_MAX       2048

static const char *TAG = "net";
static EventGroupHandle_t s_ev;
static esp_netif_t *s_sta_netif, *s_ap_netif;
static httpd_handle_t s_httpd;
static volatile bool s_portal, s_dns_run, s_mdns;
static char s_ap_ssid[24], s_ap_pass[12], s_ip[16];
static EXT_RAM_BSS_ATTR wifi_ap_record_t s_scan[MAX_SCAN];
static uint16_t s_scan_n;
static volatile bool s_sta_wanted;
static esp_timer_handle_t s_retry_timer;

static void retry_cb(void *arg) { (void)arg; if (s_sta_wanted) esp_wifi_connect(); }

bool net_is_connected(void) { return s_ev && (xEventGroupGetBits(s_ev) & BIT_CONNECTED); }
bool net_portal_running(void) { return s_portal; }
const char *net_ip_str(void) { return net_is_connected() ? s_ip : ""; }
void net_set_wifi_ps(bool no_power_save) {
    esp_wifi_set_ps(no_power_save ? WIFI_PS_NONE : WIFI_PS_MIN_MODEM);
}

int net_wifi_rssi(void) {
    if (!net_is_connected()) return 0;
    wifi_ap_record_t ap;
    return (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) ? ap.rssi : 0;
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (s_sta_wanted) esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_ev, BIT_CONNECTED);
        if (s_sta_wanted && !esp_timer_is_active(s_retry_timer)) {
            esp_timer_start_once(s_retry_timer, (s_portal ? 8000 : 2000) * 1000ULL);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&e->ip_info.ip));
        ESP_LOGI(TAG, "got IP %s", s_ip);
        xEventGroupSetBits(s_ev, BIT_CONNECTED);
    }
}

void net_init(void)
{
    s_ev = xEventGroupCreate();
    const esp_timer_create_args_t ta = { .callback = retry_cb, .name = "wifi_retry" };
    esp_timer_create(&ta, &s_retry_timer);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif = esp_netif_create_default_wifi_ap();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL);
    esp_netif_set_hostname(s_sta_netif, "grok-sphere");
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_start();
}

bool net_connect_sta(const sphere_settings_t *s, int timeout_ms)
{
    wifi_config_t wc = {0};
    strncpy((char *)wc.sta.ssid, s->ssid, sizeof(wc.sta.ssid));
    strncpy((char *)wc.sta.password, s->pass, sizeof(wc.sta.password));
    wc.sta.threshold.authmode = s->pass[0] ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
    wc.sta.pmf_cfg.capable = true;
    s_sta_wanted = true;
    esp_wifi_set_mode(s_portal ? WIFI_MODE_APSTA : WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wc);
    esp_wifi_set_ps(WIFI_PS_NONE);
    esp_wifi_connect();
    EventBits_t b = xEventGroupWaitBits(s_ev, BIT_CONNECTED, pdFALSE, pdTRUE, pdMS_TO_TICKS(timeout_ms));
    return (b & BIT_CONNECTED) != 0;
}

void net_mdns_start(void)
{
    if (s_mdns) return;
    if (mdns_init() != ESP_OK) return;
    mdns_hostname_set("grok-sphere");
    mdns_instance_name_set("Grok Sphere");
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    s_mdns = true;
}

/* ------------------------------------------------------------ captive DNS */
static void dns_task(void *arg)
{
    (void)arg;
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons(53), .sin_addr.s_addr = htonl(INADDR_ANY) };
    if (sock < 0 || bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "DNS bind failed");
        if (sock >= 0) close(sock);
        vTaskDelete(NULL);
        return;
    }
    struct timeval tv = { .tv_sec = 1 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    uint8_t buf[512];
    while (s_dns_run) {
        struct sockaddr_in from;
        socklen_t fl = sizeof(from);
        int n = recvfrom(sock, buf, sizeof(buf) - 16, 0, (struct sockaddr *)&from, &fl);
        if (n < 12) continue;
        /* only answer clients on the hotspot subnet (192.168.4.x) */
        if ((ntohl(from.sin_addr.s_addr) & 0xFFFFFF00u) != 0xC0A80400u) continue;
        int q = 12;
        while (q < n && buf[q] != 0) q += buf[q] + 1;
        q += 5;
        if (q > n) continue;
        buf[2] = 0x81; buf[3] = 0x80;
        buf[6] = 0; buf[7] = 1;
        buf[8] = buf[9] = buf[10] = buf[11] = 0;
        uint8_t ans[16] = { 0xC0, 0x0C, 0, 1, 0, 1, 0, 0, 0, 30, 0, 4, 192, 168, 4, 1 };
        memcpy(buf + q, ans, sizeof(ans));
        sendto(sock, buf, q + sizeof(ans), 0, (struct sockaddr *)&from, fl);
    }
    close(sock);
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------ web pages */
static esp_err_t send_page(httpd_req_t *req, const char *notice)
{
    sphere_settings_t s;
    app_settings_get(&s);
    char akm[16], dkm[16];
    au_mask_key(s.api_key, akm, sizeof(akm));
    au_mask_key(s.speech_key, dkm, sizeof(dkm));
    memset(&s.api_key, 0, sizeof(s.api_key));
    memset(&s.speech_key, 0, sizeof(s.speech_key));
    memset(&s.pass, 0, sizeof(s.pass));

    const char *scans[MAX_SCAN];
    int nscan = 0;
    for (int i = 0; i < s_scan_n && i < MAX_SCAN; i++) scans[nscan++] = (const char *)s_scan[i].ssid;

    assistant_diag_t *diag = heap_caps_calloc(1, sizeof(assistant_diag_t), MALLOC_CAP_SPIRAM);
    char *page = heap_caps_malloc(PAGE_MAX, MALLOC_CAP_SPIRAM);
    if (!diag || !page) { free(diag); free(page); return ESP_ERR_NO_MEM; }
    assistant_diag(diag);
    setup_page_info_t info = {
        .ssid = s.ssid, .wifi_connected = net_is_connected(), .ip = net_ip_str(),
        .api_key_masked = akm, .speech_key_masked = dkm, .model = s.model, .voice = s.voice,
        .volume = s.volume, .mirror_x = s.mirror_x, .wake_enabled = s.wake_enabled, .wake_word = s.wake_word,
        .chime_enabled = s.chime_enabled, .location = s.location, .scan_ssids = scans, .scan_n = nscan,
        .last_q = diag->last_q, .last_a = diag->last_a, .notice = notice,
        .diag_err = diag->last_err, .diag_mic_peak = diag->mic_peak, .diag_mic_rms = diag->mic_rms,
        .diag_noise_floor = diag->noise_floor, .diag_speech_detected = diag->speech_detected,
        .diag_rec_ms = diag->rec_ms,
        .diag_rssi = net_wifi_rssi(), .diag_free_heap = (unsigned)esp_get_free_heap_size(),
        .diag_free_int = (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        .diag_min_free_int = (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
        .diag_wake = diag->wake_active,
    };
    int n = net_setup_page_build(page, PAGE_MAX, &info);
    free(diag);
    if (n < 0) { free(page); return ESP_FAIL; }
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send(req, page, n);
    free(page);
    return err;
}

static esp_err_t h_root(httpd_req_t *req) { return send_page(req, NULL); }

static int read_body(httpd_req_t *req, char *body, size_t cap)
{
    int len = req->content_len < (int)cap - 1 ? req->content_len : (int)cap - 1;
    int got = 0;
    while (got < len) {
        int r = httpd_req_recv(req, body + got, len - got);
        if (r <= 0) return -1;
        got += r;
    }
    body[got] = 0;
    return got;
}

static void reboot_task(void *arg) { (void)arg; vTaskDelay(pdMS_TO_TICKS(1500)); esp_restart(); }
static void flash_task(void *arg) { (void)arg; vTaskDelay(pdMS_TO_TICKS(600)); sys_enter_flash_mode(); }

static esp_err_t h_save(httpd_req_t *req)
{
    char *body = heap_caps_calloc(1, BODY_MAX, MALLOC_CAP_SPIRAM);
    if (!body) return ESP_ERR_NO_MEM;
    if (read_body(req, body, BODY_MAX) < 0) { free(body); return ESP_FAIL; }

    sphere_settings_t cur, s;
    app_settings_get(&cur);
    s = cur;
    char tmp[SETTINGS_KEY_MAX];
    bool wifi_changed = false;

    if (net_form_get(body, "akey", tmp, sizeof(tmp))) {
        au_trim(tmp);
        if (tmp[0]) strcpy(s.api_key, tmp);
    }
    if (net_form_get(body, "dgkey", tmp, sizeof(s.speech_key))) {
        au_trim(tmp);
        if (tmp[0]) strcpy(s.speech_key, tmp);
    }
    if (net_form_get(body, "dgclear", tmp, sizeof(tmp))) s.speech_key[0] = 0;
    if (net_form_get(body, "model", tmp, sizeof(tmp))) s.model = (tmp[0] == '1') ? MODEL_SONNET : MODEL_HAIKU;
    if (net_form_get(body, "voice", tmp, sizeof(tmp)) && au_voice_valid(tmp)) strcpy(s.voice, tmp);
    if (net_form_get(body, "vol", tmp, sizeof(tmp))) {
        int v = atoi(tmp);
        s.volume = (uint8_t)(v < 10 ? 10 : (v > 100 ? 100 : v));
    }
    s.wake_enabled = net_form_get(body, "wakeon", tmp, sizeof(tmp));
    if (net_form_get(body, "wake", tmp, sizeof(tmp)) && au_wake_valid(tmp)) strcpy(s.wake_word, tmp);
    s.chime_enabled = net_form_get(body, "chime", tmp, sizeof(tmp));
    if (net_form_get(body, "loc", tmp, sizeof(s.location))) {
        au_trim(tmp);
        if (tmp[0]) snprintf(s.location, sizeof(s.location), "%.63s", tmp);
        else strcpy(s.location, "Fort Worth, TX");
    }
    s.mirror_x = net_form_get(body, "mirror", tmp, sizeof(tmp));
    if (net_form_get(body, "ssid", tmp, sizeof(s.ssid))) {
        au_trim(tmp);
        if (tmp[0] && strcmp(tmp, cur.ssid) != 0) {
            strcpy(s.ssid, tmp);
            s.pass[0] = 0;
            wifi_changed = true;
        }
    }
    if (net_form_get(body, "pass", tmp, sizeof(s.pass)) && tmp[0]) {
        strcpy(s.pass, tmp);
        wifi_changed = true;
    }
    if (s.mirror_x != cur.mirror_x) wifi_changed = true;   /* screen flip needs a restart too */
    memset(body, 0, BODY_MAX);
    free(body);
    memset(tmp, 0, sizeof(tmp));

    if (!s.ssid[0]) {
        app_settings_apply(&s, false);
        return send_page(req, "Saved. Now enter your home Wi-Fi name and password.");
    }
    app_settings_apply(&s, wifi_changed);
    clock_weather_refresh();
    memset(&s, 0, sizeof(s));
    if (wifi_changed) {
        httpd_resp_set_type(req, "text/html");
        httpd_resp_sendstr(req,
            "<html><head><meta name=viewport content='width=device-width,initial-scale=1'></head>"
            "<body style='font-family:sans-serif;background:#000;color:#EAF2FF;padding:24px'>"
            "<h2>Saved. The sphere is restarting.</h2>"
            "<p>Switch your phone back to your home Wi-Fi. When the white eyes appear, tap the screen and talk.</p>"
            "<p>To change settings later, open the address shown on the sphere (or http://grok-sphere.local).</p>"
            "</body></html>");
        xTaskCreate(reboot_task, "reboot", 2048, NULL, 5, NULL);
        return ESP_OK;
    }
    return send_page(req, "Saved.");
}

static esp_err_t h_preview(httpd_req_t *req)
{
    char *body = heap_caps_calloc(1, BODY_MAX, MALLOC_CAP_SPIRAM);
    char voice[40] = "";
    if (!body) return ESP_ERR_NO_MEM;
    if (read_body(req, body, BODY_MAX) >= 0)
        net_form_get(body, "voice", voice, sizeof(voice));
    memset(body, 0, BODY_MAX);
    free(body);

    sphere_settings_t s;
    app_settings_get(&s);
    bool have_speech = s.speech_key[0] != 0;
    memset(&s.api_key, 0, sizeof(s.api_key));
    memset(&s.speech_key, 0, sizeof(s.speech_key));
    memset(&s.pass, 0, sizeof(s.pass));

    const char *notice;
    if (!net_is_connected()) {
        notice = "Preview needs Wi-Fi.";
    } else if (!have_speech) {
        notice = "Save a Deepgram key first, then tap Preview.";
    } else if (assistant_busy()) {
        notice = "Wait until Thad finishes, then tap Preview.";
    } else {
        assistant_preview(au_voice_valid(voice) ? voice : s.voice);
        notice = "Playing a short sample on the sphere.";
    }
    memset(&s, 0, sizeof(s));
    return send_page(req, notice);
}

static esp_err_t h_moods(httpd_req_t *req)
{
    const char *notice;
    if (assistant_busy()) {
        notice = "Wait until Thad finishes, then preview moods.";
    } else {
        ui_preview_moods();
        notice = "Showing all eye moods on the sphere.";
    }
    return send_page(req, notice);
}

static esp_err_t h_ask(httpd_req_t *req)
{
    char *body = heap_caps_calloc(1, BODY_MAX, MALLOC_CAP_SPIRAM);
    char *q = calloc(1, 520);
    if (!body || !q) { free(body); free(q); return ESP_ERR_NO_MEM; }
    if (read_body(req, body, BODY_MAX) >= 0 && net_form_get(body, "q", q, 520)) {
        au_trim(q);
        if (q[0]) assistant_ask_text(q);
    }
    free(body);
    free(q);
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t h_flash(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req,
        "<html><body style='font-family:sans-serif;background:#000;color:#EAF2FF;padding:24px'>"
        "<h2>USB flash mode</h2><p>Plug the sphere into the laptop and use esptool.spacehuhn.com. "
        "Unplug and replug to go back to normal.</p></body></html>");
    xTaskCreate(flash_task, "flash", 2048, NULL, 5, NULL);
    return ESP_OK;
}

static esp_err_t h_reset(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req,
        "<html><body style='font-family:sans-serif;background:#000;color:#EAF2FF;padding:24px'>"
        "<h2>Erased. The sphere is restarting into setup.</h2></body></html>");
    app_factory_reset();
    xTaskCreate(reboot_task, "reboot", 2048, NULL, 5, NULL);
    return ESP_OK;
}

static esp_err_t h_redirect(httpd_req_t *req, httpd_err_code_t err)
{
    (void)err;
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", s_portal ? "http://192.168.4.1/" : "/");
    httpd_resp_send(req, "Redirecting", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

void net_http_start(void)
{
    if (s_httpd) return;
    httpd_config_t hc = HTTPD_DEFAULT_CONFIG();
    hc.max_uri_handlers = 8;
    hc.lru_purge_enable = true;
    hc.stack_size = 12288;
    if (httpd_start(&s_httpd, &hc) != ESP_OK) { ESP_LOGE(TAG, "web server failed"); return; }
    httpd_uri_t u[] = {
        { .uri = "/",      .method = HTTP_GET,  .handler = h_root },
        { .uri = "/save",  .method = HTTP_POST, .handler = h_save },
        { .uri = "/ask",     .method = HTTP_POST, .handler = h_ask },
        { .uri = "/preview", .method = HTTP_POST, .handler = h_preview },
        { .uri = "/moods",   .method = HTTP_POST, .handler = h_moods },
        { .uri = "/flash",   .method = HTTP_POST, .handler = h_flash },
        { .uri = "/reset", .method = HTTP_POST, .handler = h_reset },
    };
    for (size_t i = 0; i < sizeof(u) / sizeof(u[0]); i++) httpd_register_uri_handler(s_httpd, &u[i]);
    httpd_register_err_handler(s_httpd, HTTPD_404_NOT_FOUND, h_redirect);
}

/* ------------------------------------------------------------ setup hotspot */
void net_start_portal(void)
{
    if (s_portal) return;
    s_portal = true;
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "GrokSphere-%02X%02X", mac[4], mac[5]);
    snprintf(s_ap_pass, sizeof(s_ap_pass), "%08lu", (unsigned long)(esp_random() % 100000000UL));

    wifi_config_t ap = {0};
    strncpy((char *)ap.ap.ssid, s_ap_ssid, sizeof(ap.ap.ssid));
    ap.ap.ssid_len = strlen(s_ap_ssid);
    strncpy((char *)ap.ap.password, s_ap_pass, sizeof(ap.ap.password));
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ap.ap.max_connection = 2;
    ap.ap.channel = 6;
    esp_wifi_set_mode(WIFI_MODE_APSTA);
    esp_wifi_set_config(WIFI_IF_AP, &ap);
    esp_wifi_start();

    bool sta_was_wanted = s_sta_wanted;
    bool was_connected = net_is_connected();
    if (!was_connected) {
        s_sta_wanted = false;
        esp_timer_stop(s_retry_timer);
        esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    wifi_scan_config_t sc = {0};
    if (esp_wifi_scan_start(&sc, true) == ESP_OK) {
        s_scan_n = MAX_SCAN;
        esp_wifi_scan_get_ap_records(&s_scan_n, s_scan);
    }
    s_sta_wanted = sta_was_wanted;
    if (s_sta_wanted && !was_connected) esp_wifi_connect();

    net_http_start();
    s_dns_run = true;
    xTaskCreate(dns_task, "dns", 4096, NULL, 4, NULL);
    ui_show_setup(s_ap_ssid, s_ap_pass, "192.168.4.1");
    ESP_LOGI(TAG, "setup hotspot %s started", s_ap_ssid);
}

int net_portal_clients(void)
{
    wifi_sta_list_t l;
    if (esp_wifi_ap_get_sta_list(&l) != ESP_OK) return 0;
    return l.num;
}

void net_stop_portal(void)
{
    if (!s_portal) return;
    s_dns_run = false;
    esp_wifi_set_mode(WIFI_MODE_STA);
    s_portal = false;
    ui_hide_setup();
    ESP_LOGI(TAG, "setup hotspot stopped");
}
