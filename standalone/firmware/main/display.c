/*
 * Grok Sphere - ST77916 360x360 QSPI panel + LVGL 8.3 port.
 * Panel bring-up mirrors Waveshare's demo (ESP-IDF/.../LCD_Driver/ST77916.c):
 * read register 0x04 at 3 MHz to tell the two panel revisions apart, then run at 80 MHz.
 */
#include "display.h"
#include "board.h"
#include "esp_lcd_st77916.h"
#include "st77916_init_new.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "touch.h"

#define LCD_OPCODE_READ_CMD   (0x0BULL)
#define LVGL_BUF_LINES        40
#define LVGL_TICK_MS          2

static const char *TAG = "display";
static esp_lcd_panel_handle_t s_panel;
static esp_lcd_panel_io_handle_t s_io;
static SemaphoreHandle_t s_lvgl_mux;
static lv_disp_draw_buf_t s_draw_buf;
static lv_disp_drv_t s_disp_drv;
static lv_indev_drv_t s_indev_drv;

bool display_lock(int timeout_ms)
{
    TickType_t t = timeout_ms < 0 ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    return xSemaphoreTakeRecursive(s_lvgl_mux, t) == pdTRUE;
}

void display_unlock(void) { xSemaphoreGiveRecursive(s_lvgl_mux); }

static bool flush_ready_cb(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *e, void *ctx)
{
    lv_disp_flush_ready((lv_disp_drv_t *)ctx);
    return false;
}

static void flush_cb(lv_disp_drv_t *drv, const lv_area_t *a, lv_color_t *px)
{
    if (esp_lcd_panel_draw_bitmap(s_panel, a->x1, a->y1, a->x2 + 1, a->y2 + 1, px) != ESP_OK) {
        lv_disp_flush_ready(drv);
    }
}

static void touch_read_cb(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    uint16_t x, y;
    if (touch_get_point(&x, &y)) {
        data->point.x = x;
        data->point.y = y;
        data->state = LV_INDEV_STATE_PR;
    } else {
        data->state = LV_INDEV_STATE_REL;
    }
}

static void tick_cb(void *arg) { lv_tick_inc(LVGL_TICK_MS); }

static void lvgl_task(void *arg)
{
    for (;;) {
        uint32_t wait = 10;
        if (display_lock(-1)) {
            wait = lv_timer_handler();
            display_unlock();
        }
        if (wait < 5) wait = 5;
        if (wait > 30) wait = 30;
        vTaskDelay(pdMS_TO_TICKS(wait));
    }
}

esp_err_t display_init(bool mirror_x)
{
    spi_bus_config_t bus = ST77916_PANEL_BUS_QSPI_CONFIG(LCD_PIN_SCK, LCD_PIN_D0, LCD_PIN_D1,
                                                         LCD_PIN_D2, LCD_PIN_D3,
                                                         LCD_H_RES * LVGL_BUF_LINES * sizeof(uint16_t));
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_spi_config_t io_cfg = ST77916_PANEL_IO_QSPI_CONFIG(LCD_PIN_CS, NULL, NULL);
    io_cfg.pclk_hz = 3 * 1000 * 1000;               /* slow clock for the ID read */
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_cfg, &s_io));

    uint8_t id[4] = {0};
    int cmd = (int)((LCD_OPCODE_READ_CMD << 24) | (0x04 << 8));
    esp_err_t r = esp_lcd_panel_io_rx_param(s_io, cmd, id, sizeof(id));
    ESP_LOGI(TAG, "panel id read %s: %02x %02x %02x %02x", r == ESP_OK ? "ok" : "failed",
             id[0], id[1], id[2], id[3]);
    esp_lcd_panel_io_del(s_io);

    io_cfg.pclk_hz = 80 * 1000 * 1000;
    io_cfg.on_color_trans_done = flush_ready_cb;
    io_cfg.user_ctx = &s_disp_drv;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_cfg, &s_io));

    st77916_vendor_config_t vendor = { .flags = { .use_qspi_interface = 1 } };
    if (id[0] == 0x00 && id[1] == 0x02 && id[2] == 0x7F && id[3] == 0x7F) {
        vendor.init_cmds = vendor_specific_init_new;
        vendor.init_cmds_size = sizeof(vendor_specific_init_new) / sizeof(vendor_specific_init_new[0]);
        ESP_LOGI(TAG, "using Waveshare 'case 2' init table");
    } else {
        ESP_LOGI(TAG, "using driver default init table");
    }
    esp_lcd_panel_dev_config_t pcfg = {
        .reset_gpio_num = -1,                        /* reset is on the IO expander (EXIO2) */
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st77916(s_io, &pcfg, &s_panel));
    esp_lcd_panel_reset(s_panel);
    esp_lcd_panel_init(s_panel);
    esp_lcd_panel_mirror(s_panel, mirror_x, false);
    esp_lcd_panel_disp_on_off(s_panel, true);

    /* LVGL */
    lv_init();
    size_t buf_px = LCD_H_RES * LVGL_BUF_LINES;
    lv_color_t *b1 = heap_caps_malloc(buf_px * sizeof(lv_color_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    lv_color_t *b2 = heap_caps_malloc(buf_px * sizeof(lv_color_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!b1 || !b2) {
        ESP_LOGE(TAG, "no DMA memory for LVGL buffers");
        return ESP_ERR_NO_MEM;
    }
    lv_disp_draw_buf_init(&s_draw_buf, b1, b2, buf_px);
    lv_disp_drv_init(&s_disp_drv);
    s_disp_drv.hor_res = LCD_H_RES;
    s_disp_drv.ver_res = LCD_V_RES;
    s_disp_drv.flush_cb = flush_cb;
    s_disp_drv.draw_buf = &s_draw_buf;
    lv_disp_drv_register(&s_disp_drv);

    lv_indev_drv_init(&s_indev_drv);
    s_indev_drv.type = LV_INDEV_TYPE_POINTER;
    s_indev_drv.read_cb = touch_read_cb;
    lv_indev_drv_register(&s_indev_drv);

    s_lvgl_mux = xSemaphoreCreateRecursiveMutex();
    const esp_timer_create_args_t targs = { .callback = tick_cb, .name = "lv_tick" };
    esp_timer_handle_t tick;
    ESP_ERROR_CHECK(esp_timer_create(&targs, &tick));
    ESP_ERROR_CHECK(esp_timer_start_periodic(tick, LVGL_TICK_MS * 1000));
    xTaskCreatePinnedToCore(lvgl_task, "lvgl", 12288, NULL, 4, NULL, 1);

    board_backlight_set(80);
    return ESP_OK;
}
