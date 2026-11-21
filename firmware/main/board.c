/* Grok Sphere - board bring-up: I2C bus, TCA9554 resets, backlight, audio revision probe. */
#include "board.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "board";
static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_tca;
static board_audio_rev_t s_rev = BOARD_AUDIO_UNKNOWN;

i2c_master_bus_handle_t board_i2c_bus(void) { return s_bus; }

bool board_i2c_probe(uint8_t addr)
{
    return s_bus && i2c_master_probe(s_bus, addr, 50) == ESP_OK;
}

static esp_err_t tca_write(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = {reg, val};
    return i2c_master_transmit(s_tca, buf, sizeof(buf), 100);
}

static void backlight_init(void)
{
    ledc_timer_config_t t = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&t);
    ledc_channel_config_t ch = {
        .gpio_num = LCD_PIN_BL,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
        .hpoint = 0,
    };
    ledc_channel_config(&ch);
}

void board_backlight_set(uint8_t percent)
{
    if (percent > 100) percent = 100;
    uint32_t duty = (1023u * percent) / 100u;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

bool board_boot_button_pressed(void)
{
    return gpio_get_level(BOARD_BOOT_BUTTON) == 0;
}

board_audio_rev_t board_audio_rev(void) { return s_rev; }

const char *board_audio_rev_name(void)
{
    switch (s_rev) {
    case BOARD_AUDIO_V1: return "v1-pcm5101";
    case BOARD_AUDIO_V2: return "v2-es8311";
    default: return "unknown";
    }
}

esp_err_t board_init(void)
{
    gpio_config_t btn = {
        .pin_bit_mask = 1ULL << BOARD_BOOT_BUTTON,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&btn);

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = BOARD_I2C_SDA,
        .scl_io_num = BOARD_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &s_bus));

    i2c_device_config_t dev = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = TCA9554_ADDR,
        .scl_speed_hz = BOARD_I2C_HZ,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(s_bus, &dev, &s_tca));

    /* All expander pins as outputs; pulse LCD + touch reset (EXIO2 / EXIO1). */
    uint8_t high = 0xFF;
    esp_err_t err = tca_write(TCA9554_REG_OUTPUT, high);
    err |= tca_write(TCA9554_REG_CONFIG, 0x00);
    vTaskDelay(pdMS_TO_TICKS(10));
    err |= tca_write(TCA9554_REG_OUTPUT, high & ~(EXIO_TP_RST_BIT | EXIO_LCD_RST_BIT));
    vTaskDelay(pdMS_TO_TICKS(20));
    err |= tca_write(TCA9554_REG_OUTPUT, high);
    vTaskDelay(pdMS_TO_TICKS(120));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "TCA9554 IO expander did not answer (0x%02x)", TCA9554_ADDR);
    }

    backlight_init();

    bool es8311 = board_i2c_probe(ES8311_ADDR_7BIT);
    bool es7210 = board_i2c_probe(ES7210_ADDR_7BIT);
    s_rev = (es8311 || es7210) ? BOARD_AUDIO_V2 : BOARD_AUDIO_V1;
    ESP_LOGI(TAG, "audio revision: %s (ES8311 %s, ES7210 %s)", board_audio_rev_name(),
             es8311 ? "found" : "absent", es7210 ? "found" : "absent");
    return ESP_OK;
}
