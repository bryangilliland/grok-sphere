/*
 * Grok Sphere - Wi-Fi + captive setup portal.
 */
#include "net.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "sysctl.h"
#include "ui.h"
#include "formutil.h"

#define BIT_CONNECTED  BIT0
#define MAX_SCAN       20

static const char *TAG = "net";
static EventGroupHandle_t s_ev;
static esp_netif_t *s_sta_netif, *s_ap_netif;
static httpd_handle_t s_httpd;
static volatile bool s_portal, s_dns_run;
static char s_mac[18];
static char s_ap_ssid[24], s_ap_pass[12];
static wifi_ap_record_t s_scan[MAX_SCAN];
static uint16_t s_scan_n;
static sphere_settings_t s_current;
static volatile bool s_sta_wanted;
static esp_timer_handle_t s_retry_timer;

static void retry_cb(void *arg)
{
    (void)arg;
    if (s_sta_wanted) esp_wifi_connect();
}

const char *net_mac_str(void) { return s_mac; }
bool net_is_connected(void) { return s_ev && (xEventGroupGetBits(s_ev) & BIT_CONNECTED); }
bool net_portal_running(void) { return s_portal; }

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
        ESP_LOGI(TAG, "got IP " IPSTR, IP2STR(&e->ip_info.ip));
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
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_mac, sizeof(s_mac), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    esp_netif_set_hostname(s_sta_netif, "grok-sphere");
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
    esp_wifi_start();
    esp_wifi_connect();
    EventBits_t b = xEventGroupWaitBits(s_ev, BIT_CONNECTED, pdFALSE, pdTRUE, pdMS_TO_TICKS(timeout_ms));
    return (b & BIT_CONNECTED) != 0;
}

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

#define SETUP_PAGE_MAX 12288

static esp_err_t send_form(httpd_req_t *req)
{
    const char *scans[MAX_SCAN];
    int nscan = 0;
    for (int i = 0; i < s_scan_n; i++) {
        scans[nscan++] = (const char *)s_scan[i].ssid;
    }
    char *page = malloc(SETUP_PAGE_MAX);
    if (!page) return ESP_ERR_NO_MEM;
    int n = net_setup_page_build(page, SETUP_PAGE_MAX, s_current.ssid, s_current.bridge_host,
                                 s_current.bridge_port, s_current.mirror_x, scans, nscan);
    if (n < 0) {
        free(page);
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "text/html");
    esp_err_t err = httpd_resp_send(req, page, n);
    free(page);
    return err;
}

static esp_err_t h_root(httpd_req_t *req) { return send_form(req); }

static void reboot_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
}

static void flash_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(600));
    sys_enter_flash_mode();
}

static esp_err_t h_save(httpd_req_t *req)
{
    char body[768];
    int len = req->content_len < (int)sizeof(body) - 1 ? req->content_len : (int)sizeof(body) - 1;
    int got = 0;
    while (got < len) {
        int r = httpd_req_recv(req, body + got, len - got);
        if (r <= 0) return ESP_FAIL;
        got += r;
    }
    body[got] = 0;

    sphere_settings_t s = s_current;
    char tmp[80];
    net_form_get(body, "ssid", s.ssid, sizeof(s.ssid));
    if (net_form_get(body, "pass", tmp, sizeof(s.pass)) && tmp[0]) {
        net_form_get(body, "pass", s.pass, sizeof(s.pass));
    } else if (strcmp(s.ssid, s_current.ssid) != 0) {
        s.pass[0] = 0;
    }
    net_form_get(body, "host", s.bridge_host, sizeof(s.bridge_host));
    if (net_form_get(body, "port", tmp, sizeof(tmp))) {
        int p = atoi(tmp);
        s.bridge_port = (p > 0 && p < 65536) ? (uint16_t)p : BRIDGE_DEFAULT_PORT;
    }
    s.mirror_x = net_form_get(body, "mirror", tmp, sizeof(tmp));

    httpd_resp_set_type(req, "text/html");
    if (!s.ssid[0] || settings_save(&s) != ESP_OK) {
        httpd_resp_sendstr(req, "<html><body style='font-family:sans-serif'><h2>Could not save. Go back and enter the Wi-Fi name.</h2></body></html>");
        return ESP_OK;
    }
    httpd_resp_sendstr(req,
        "<html><body style='font-family:sans-serif;background:#080b0a;color:#eef;padding:24px'>"
        "<h2 style='color:#00C972'>Saved. The sphere is restarting.</h2>"
        "<p>You can switch your phone back to your home Wi-Fi.</p></body></html>");
    xTaskCreate(reboot_task, "reboot", 2048, NULL, 5, NULL);
    return ESP_OK;
}

static esp_err_t h_flash(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req,
        "<html><body style='font-family:sans-serif;background:#080b0a;color:#eef;padding:24px'>"
        "<h2 style='color:#00C972'>Entering USB Flash Mode...</h2>"
        "<p>Plug into your Windows PC and open the browser flasher.</p></body></html>");
    xTaskCreate(flash_task, "flash", 2048, NULL, 5, NULL);
    return ESP_OK;
}

static esp_err_t h_redirect(httpd_req_t *req, httpd_err_code_t err)
{
    (void)err;
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_send(req, "Redirecting", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

void net_start_portal(const sphere_settings_t *current)
{
    if (s_portal) return;
    s_current = *current;
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
    s_sta_wanted = false;
    esp_timer_stop(s_retry_timer);
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(200));
    wifi_scan_config_t sc = {0};
    if (esp_wifi_scan_start(&sc, true) == ESP_OK) {
        s_scan_n = MAX_SCAN;
        esp_wifi_scan_get_ap_records(&s_scan_n, s_scan);
    }
    s_sta_wanted = sta_was_wanted;
    if (s_sta_wanted) esp_wifi_connect();

    httpd_config_t hc = HTTPD_DEFAULT_CONFIG();
    hc.max_uri_handlers = 6;
    hc.lru_purge_enable = true;
    hc.stack_size = 8192;
    if (httpd_start(&s_httpd, &hc) == ESP_OK) {
        httpd_uri_t root  = { .uri = "/",      .method = HTTP_GET,  .handler = h_root };
        httpd_uri_t save  = { .uri = "/save",  .method = HTTP_POST, .handler = h_save };
        httpd_uri_t flash = { .uri = "/flash", .method = HTTP_POST, .handler = h_flash };
        httpd_register_uri_handler(s_httpd, &root);
        httpd_register_uri_handler(s_httpd, &save);
        httpd_register_uri_handler(s_httpd, &flash);
        httpd_register_err_handler(s_httpd, HTTPD_404_NOT_FOUND, h_redirect);
    }
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
    if (s_httpd) { httpd_stop(s_httpd); s_httpd = NULL; }
    esp_wifi_set_mode(WIFI_MODE_STA);
    s_portal = false;
    ui_hide_setup();
    ESP_LOGI(TAG, "setup hotspot stopped");
}
