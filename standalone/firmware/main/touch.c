/*
 * Grok Sphere - CST816 touch, polled over I2C (address 0x15, reset on TCA9554 EXIO1).
 * Register layout from Waveshare demo Touch_Driver/CST816.c: 0x02 = finger count,
 * 0x03..0x06 = X high nibble, X low, Y high nibble, Y low. 0xFE = disable auto-sleep.
 */
#include "touch.h"
#include "board.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define HOLD_MS     450
#define POLL_MS     25

static const char *TAG = "touch";
static i2c_master_dev_handle_t s_dev;
static touch_cb_t s_cb;
static volatile bool s_down;
static volatile uint16_t s_x, s_y;

bool touch_get_point(uint16_t *x, uint16_t *y)
{
    if (!s_down) return false;
    *x = s_x;
    *y = s_y;
    return true;
}

static bool read_point(uint16_t *x, uint16_t *y)
{
    uint8_t reg = 0x02, d[5];
    if (i2c_master_transmit_receive(s_dev, &reg, 1, d, sizeof(d), 50) != ESP_OK) return false;
    if ((d[0] & 0x0F) == 0) return false;
    *x = ((d[1] & 0x0F) << 8) | d[2];
    *y = ((d[3] & 0x0F) << 8) | d[4];
    return true;
}

static void touch_task(void *arg)
{
    bool was_down = false, holding = false;
    int64_t down_at = 0;
    for (;;) {
        uint16_t x, y;
        bool down = read_point(&x, &y);
        if (down) { s_x = x; s_y = y; }
        s_down = down;
        int64_t now = esp_timer_get_time() / 1000;
        if (down && !was_down) {
            down_at = now;
            holding = false;
        } else if (down && was_down && !holding && now - down_at >= HOLD_MS) {
            holding = true;
            if (s_cb) s_cb(TOUCH_EVT_HOLD_START);
        } else if (!down && was_down) {
            if (holding) {
                if (s_cb) s_cb(TOUCH_EVT_HOLD_END);
            } else if (now - down_at >= 15) {
                if (s_cb) s_cb(TOUCH_EVT_TAP);
            }
            holding = false;
        }
        was_down = down;
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

esp_err_t touch_init(touch_cb_t cb)
{
    s_cb = cb;
    i2c_device_config_t dev = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = TOUCH_ADDR,
        .scl_speed_hz = BOARD_I2C_HZ,
    };
    esp_err_t err = i2c_master_bus_add_device(board_i2c_bus(), &dev, &s_dev);
    if (err != ESP_OK) return err;
    uint8_t reg = 0xA7, id = 0;
    if (i2c_master_transmit_receive(s_dev, &reg, 1, &id, 1, 50) == ESP_OK) {
        ESP_LOGI(TAG, "CST816 chip id 0x%02x", id);
    } else {
        ESP_LOGW(TAG, "CST816 did not answer; touch disabled until it does");
    }
    uint8_t no_sleep[2] = {0xFE, 0x01};
    i2c_master_transmit(s_dev, no_sleep, sizeof(no_sleep), 50);
    xTaskCreatePinnedToCore(touch_task, "touch", 3072, NULL, 5, NULL, 0);
    return ESP_OK;
}
