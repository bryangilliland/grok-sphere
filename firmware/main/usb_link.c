#include "usb_link.h"
#include <stdlib.h>
#include <string.h>
#include "driver/usb_serial_jtag.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "usb_frame.h"

#define USB_RX_TIMEOUT_US   (12LL * 1000 * 1000)
#define USB_DRV_BUF_SIZE    8192

static const char *TAG = "usb_link";
static usb_json_cb_t s_on_json;
static usb_pcm_cb_t s_on_pcm;
static usb_conn_cb_t s_on_conn;
static SemaphoreHandle_t s_tx_mux;
static volatile bool s_active;
static volatile int64_t s_last_rx_us;
static usb_frame_rx_t *s_rx_state;
static uint8_t s_tx_buf[USB_FRAME_HDR_SIZE + USB_FRAME_MAX_PAYLOAD + USB_FRAME_CRC_SIZE];

bool usb_link_connected(void) { return s_active; }

static bool send_frame(uint8_t type, const void *data, size_t len)
{
    if (!usb_serial_jtag_is_driver_installed() || !usb_serial_jtag_is_connected()) return false;
    if (!s_tx_mux || len > USB_FRAME_MAX_PAYLOAD) return false;
    if (xSemaphoreTake(s_tx_mux, pdMS_TO_TICKS(100)) != pdTRUE) return false;

    int total = usb_frame_encode(s_tx_buf, sizeof(s_tx_buf), type, data, len);
    bool ok = false;
    if (total > 0) {
        size_t off = 0;
        while (off < (size_t)total) {
            int w = usb_serial_jtag_write_bytes(s_tx_buf + off, (size_t)total - off, pdMS_TO_TICKS(40));
            if (w <= 0) break;
            off += (size_t)w;
        }
        ok = (off == (size_t)total);
    }
    xSemaphoreGive(s_tx_mux);
    return ok;
}

bool usb_link_send_json(const char *json, size_t len)
{
    return send_frame(USB_FRAME_JSON, json, len);
}

bool usb_link_send_pcm(const uint8_t *pcm, size_t len)
{
    if (!s_active) return false;
    return send_frame(USB_FRAME_PCM, pcm, len);
}

static void on_frame(uint8_t type, const uint8_t *payload, uint16_t len, void *ctx)
{
    (void)ctx;
    s_last_rx_us = esp_timer_get_time();
    if (!s_active) {
        s_active = true;
        ESP_LOGI(TAG, "USB companion active");
        if (s_on_conn) s_on_conn(true);
    }
    if (type == USB_FRAME_JSON) {
        if (s_on_json) s_on_json((const char *)payload, len);
    } else if (type == USB_FRAME_PCM) {
        if (s_on_pcm && len > 0) s_on_pcm(payload, len);
    }
}

static void usb_rx_task(void *arg)
{
    (void)arg;
    static uint8_t chunk[512];
    for (;;) {
        int n = usb_serial_jtag_read_bytes(chunk, sizeof(chunk), pdMS_TO_TICKS(50));
        if (n > 0) {
            usb_frame_rx_feed(s_rx_state, chunk, (size_t)n, on_frame, NULL);
        }
        if (s_active) {
            int64_t now = esp_timer_get_time();
            if (!usb_serial_jtag_is_connected() || (now - s_last_rx_us > USB_RX_TIMEOUT_US)) {
                s_active = false;
                ESP_LOGW(TAG, "USB companion timed out / disconnected");
                if (s_on_conn) s_on_conn(false);
            }
        }
    }
}

void usb_link_start(usb_json_cb_t on_json, usb_pcm_cb_t on_pcm, usb_conn_cb_t on_conn)
{
    s_on_json = on_json;
    s_on_pcm = on_pcm;
    s_on_conn = on_conn;
    s_tx_mux = xSemaphoreCreateMutex();
    s_rx_state = calloc(1, sizeof(usb_frame_rx_t));
    usb_frame_rx_init(s_rx_state);

    usb_serial_jtag_driver_config_t cfg = {
        .tx_buffer_size = USB_DRV_BUF_SIZE,
        .rx_buffer_size = USB_DRV_BUF_SIZE,
    };
    esp_err_t err = usb_serial_jtag_driver_install(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "usb_serial_jtag_driver_install failed: %d", err);
        return;
    }
    xTaskCreatePinnedToCore(usb_rx_task, "usb_rx", 4096, NULL, 7, NULL, 0);
}
