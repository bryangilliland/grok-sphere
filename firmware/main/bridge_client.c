/*
 * Grok Sphere (Thad + Claude Edition) - Dual-transport link to the Windows companion.
 *
 *   1. USB Serial ("GST1" framed over ESP32-S3 USB-Serial-JTAG) — active whenever plugged
 *      into the PC and the companion app is running.
 *   2. Wi-Fi WebSocket fallback (ws://<laptop>:8765/sphere via mDNS _groksphere._tcp or
 *      configured host) — takes over automatically whenever USB is unplugged or Windows
 *      cannot see the USB COM port.
 */
#include "bridge_client.h"
#include <stdio.h>
#include <string.h>
#include "audio.h"
#include "board.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "mdns.h"
#include "net.h"
#include "usb_link.h"

#define FW_VERSION      "1.2.0-thad"
#define MIC_FRAME       512
#define TXT_MAX         2048
#define CTRL_MAX        160

typedef struct { char json[CTRL_MAX]; } ctrl_msg_t;

static const char *TAG = "bridge";
static sphere_settings_t s_cfg;
static bridge_msg_cb_t s_on_msg;
static bridge_conn_cb_t s_on_conn;
static esp_websocket_client_handle_t s_ws;
static volatile bool s_ws_connected;
static volatile bool s_wifi_started;
static QueueHandle_t s_ctrl_q;
static char s_txt[TXT_MAX];
static size_t s_txt_len;
static bool s_last_binary;
static char s_where[100];

bool bridge_connected(void) { return usb_link_connected() || s_ws_connected; }

static void enqueue(const char *json, int len)
{
    if (!s_ctrl_q || len <= 0 || len >= CTRL_MAX) return;
    ctrl_msg_t m;
    memcpy(m.json, json, len + 1);
    xQueueSend(s_ctrl_q, &m, 0);
}

void bridge_send_touch(const char *action)
{
    char buf[CTRL_MAX];
    enqueue(buf, proto_build_touch(buf, sizeof(buf), action));
}

void bridge_send_simple(const char *type)
{
    char buf[CTRL_MAX];
    enqueue(buf, proto_build_simple(buf, sizeof(buf), type));
}

static void enqueue_hello(void)
{
    char buf[CTRL_MAX];
    enqueue(buf, proto_build_hello(buf, sizeof(buf), FW_VERSION, board_audio_rev_name(), net_mac_str()));
}

static void handle_text(const char *txt, size_t len, bool from_usb)
{
    proto_msg_t m;
    if (!proto_parse(txt, len, &m)) {
        ESP_LOGW(TAG, "ignored message: %.*s", (int)(len > 60 ? 60 : len), txt);
        return;
    }
    if (m.type == MSG_PING) {
        if (from_usb) {
            char buf[CTRL_MAX];
            int n = proto_build_simple(buf, sizeof(buf), "pong");
            if (n > 0) usb_link_send_json(buf, (size_t)n);
        } else {
            bridge_send_simple("pong");
        }
        return;
    }
    if (s_on_msg) s_on_msg(&m);
}

/* ---- USB transport callbacks ---- */
static void on_usb_json(const char *json, size_t len)
{
    handle_text(json, len, true);
}

static void on_usb_pcm(const uint8_t *pcm, size_t len)
{
    audio_stream_push(pcm, len);
}

static void on_usb_conn(bool connected)
{
    if (connected) {
        enqueue_hello();
        if (s_on_conn) s_on_conn(true, "USB");
    } else {
        audio_stop();
        if (s_ws_connected) {
            enqueue_hello();
            if (s_on_conn) s_on_conn(true, s_where);
        } else if (s_on_conn) {
            s_on_conn(false, "USB");
        }
    }
}

/* ---- Wi-Fi WebSocket callbacks ---- */
static void ws_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    esp_websocket_event_data_t *d = data;
    switch (id) {
    case WEBSOCKET_EVENT_CONNECTED:
        s_ws_connected = true;
        if (!usb_link_connected()) {
            enqueue_hello();
            if (s_on_conn) s_on_conn(true, s_where);
        }
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_CLOSED:
        if (s_ws_connected && !usb_link_connected()) {
            audio_stop();
            if (s_on_conn) s_on_conn(false, s_where);
        }
        s_ws_connected = false;
        break;
    case WEBSOCKET_EVENT_DATA: {
        if (usb_link_connected()) break; /* USB takes priority while active */
        int op = d->op_code;
        if (op == 0x08 || op == 0x09 || op == 0x0A) break;
        bool binary = (op == 0x02) || (op == 0x00 && s_last_binary);
        if (op == 0x01 || op == 0x02) s_last_binary = (op == 0x02);
        if (binary) {
            if (d->data_len > 0) audio_stream_push((const uint8_t *)d->data_ptr, d->data_len);
            break;
        }
        if (d->payload_offset == 0) s_txt_len = 0;
        if (s_txt_len + d->data_len < TXT_MAX) {
            memcpy(s_txt + s_txt_len, d->data_ptr, d->data_len);
            s_txt_len += d->data_len;
        }
        if (d->payload_offset + d->data_len >= d->payload_len) {
            s_txt[s_txt_len] = 0;
            handle_text(s_txt, s_txt_len, false);
            s_txt_len = 0;
        }
        break;
    }
    default:
        break;
    }
}

static bool resolve(char *uri, size_t cap)
{
    const char *host = s_cfg.bridge_host;
    uint16_t port = s_cfg.bridge_port ? s_cfg.bridge_port : BRIDGE_DEFAULT_PORT;
    if (host[0]) {
        size_t n = strlen(host);
        if (n > 6 && strcasecmp(host + n - 6, ".local") == 0) {
            char name[64];
            snprintf(name, sizeof(name), "%.*s", (int)(n - 6), host);
            esp_ip4_addr_t a;
            if (mdns_query_a(name, 3000, &a) == ESP_OK) {
                snprintf(uri, cap, "ws://" IPSTR ":%u/sphere", IP2STR(&a), port);
                return true;
            }
            ESP_LOGW(TAG, "could not find %s on the network", host);
            return false;
        }
        snprintf(uri, cap, "ws://%s:%u/sphere", host, port);
        return true;
    }
    mdns_result_t *res = NULL;
    if (mdns_query_ptr("_groksphere", "_tcp", 3000, 4, &res) != ESP_OK || !res) return false;
    bool ok = false;
    for (mdns_result_t *r = res; r && !ok; r = r->next) {
        for (mdns_ip_addr_t *a = r->addr; a; a = a->next) {
            if (a->addr.type == ESP_IPADDR_TYPE_V4) {
                snprintf(uri, cap, "ws://" IPSTR ":%u/sphere", IP2STR(&a->addr.u_addr.ip4), r->port);
                ok = true;
                break;
            }
        }
    }
    mdns_query_results_free(res);
    return ok;
}

static void mic_task(void *arg)
{
    (void)arg;
    static int16_t frame[MIC_FRAME];
    ctrl_msg_t m;
    for (;;) {
        while (xQueueReceive(s_ctrl_q, &m, 0) == pdTRUE) {
            if (usb_link_connected()) {
                usb_link_send_json(m.json, strlen(m.json));
            } else if (s_ws_connected && s_ws) {
                esp_websocket_client_send_text(s_ws, m.json, strlen(m.json), pdMS_TO_TICKS(500));
            }
        }
        size_t n = audio_mic_read(frame, MIC_FRAME);
        if (n == 0) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        if (usb_link_connected()) {
            usb_link_send_pcm((const uint8_t *)frame, n * sizeof(int16_t));
        } else if (s_ws_connected && s_ws) {
            esp_websocket_client_send_bin(s_ws, (const char *)frame, n * sizeof(int16_t), pdMS_TO_TICKS(200));
        }
    }
}

static void supervisor_task(void *arg)
{
    (void)arg;
    char uri[96] = {0};
    int64_t down_since = 0;
    for (;;) {
        if (!net_is_connected()) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        if (!s_ws) {
            if (!resolve(uri, sizeof(uri))) {
                if (!usb_link_connected() && s_on_conn) s_on_conn(false, "searching for PC app");
                vTaskDelay(pdMS_TO_TICKS(4000));
                continue;
            }
            snprintf(s_where, sizeof(s_where), "%s", uri + 5);
            ESP_LOGI(TAG, "connecting to %s", uri);
            esp_websocket_client_config_t wc = {
                .uri = uri,
                .buffer_size = 4096,
                .task_stack = 6144,
                .task_prio = 6,
                .reconnect_timeout_ms = 3000,
                .network_timeout_ms = 8000,
                .ping_interval_sec = 10,
                .disable_auto_reconnect = false,
            };
            s_ws = esp_websocket_client_init(&wc);
            esp_websocket_register_events(s_ws, WEBSOCKET_EVENT_ANY, ws_event, NULL);
            esp_websocket_client_start(s_ws);
            down_since = esp_timer_get_time();
        }
        if (s_ws_connected || usb_link_connected()) {
            down_since = esp_timer_get_time();
        } else if (esp_timer_get_time() - down_since > 30LL * 1000 * 1000) {
            ESP_LOGW(TAG, "Wi-Fi companion unreachable for 30 s, searching again");
            esp_websocket_client_stop(s_ws);
            esp_websocket_client_destroy(s_ws);
            s_ws = NULL;
            continue;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void bridge_init(bridge_msg_cb_t on_msg, bridge_conn_cb_t on_conn)
{
    s_on_msg = on_msg;
    s_on_conn = on_conn;
    s_ctrl_q = xQueueCreate(16, sizeof(ctrl_msg_t));
    usb_link_start(on_usb_json, on_usb_pcm, on_usb_conn);
    xTaskCreatePinnedToCore(mic_task, "mic", 6144, NULL, 6, NULL, 0);
}

void bridge_start_wifi(const sphere_settings_t *s)
{
    if (s_wifi_started) return;
    s_wifi_started = true;
    s_cfg = *s;
    if (mdns_init() == ESP_OK) mdns_hostname_set("grok-sphere");
    xTaskCreatePinnedToCore(supervisor_task, "bridge", 6144, NULL, 4, NULL, 0);
}
