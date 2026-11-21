/*
 * Grok Sphere (Thad + Claude Edition) - board pin map for Waveshare ESP32-S3-Touch-LCD-1.85C(-BOX).
 *
 * USB rule: GPIO19 (D-) and GPIO20 (D+) belong exclusively to the ESP32-S3 built-in
 * USB-Serial-JTAG controller. Never repurpose them.
 */
#pragma once
#include "driver/gpio.h"

#define BOARD_I2C_SDA            GPIO_NUM_11
#define BOARD_I2C_SCL            GPIO_NUM_10
#define BOARD_I2C_HZ             400000

#define TCA9554_ADDR             0x20
#define TCA9554_REG_OUTPUT       0x01
#define TCA9554_REG_CONFIG       0x03
#define EXIO_TP_RST_BIT          (1 << 0)   /* EXIO1 -> touch reset  */
#define EXIO_LCD_RST_BIT         (1 << 1)   /* EXIO2 -> LCD reset    */
#define EXIO_SD_CS_BIT           (1 << 2)   /* EXIO3 -> SD card CS (unused) */

/* ST77916 360x360 round LCD over QSPI (SPI2) */
#define LCD_HOST                 SPI2_HOST
#define LCD_PIN_SCK              GPIO_NUM_40
#define LCD_PIN_D0               GPIO_NUM_46
#define LCD_PIN_D1               GPIO_NUM_45
#define LCD_PIN_D2               GPIO_NUM_42
#define LCD_PIN_D3               GPIO_NUM_41
#define LCD_PIN_CS               GPIO_NUM_21
#define LCD_PIN_TE               GPIO_NUM_18
#define LCD_PIN_BL               GPIO_NUM_5
#define LCD_H_RES                360
#define LCD_V_RES                360

/* CST816 capacitive touch */
#define TOUCH_ADDR               0x15
#define TOUCH_PIN_INT            GPIO_NUM_4

/* BOOT button (side button) */
#define BOARD_BOOT_BUTTON        GPIO_NUM_0

/* ---- Audio, V2 (ES8311 + ES7210, one duplex I2S bus) ---- */
#define V2_I2S_MCLK              GPIO_NUM_2
#define V2_I2S_BCLK              GPIO_NUM_48
#define V2_I2S_WS                GPIO_NUM_38
#define V2_I2S_DOUT              GPIO_NUM_47
#define V2_I2S_DIN               GPIO_NUM_39
#define V2_PA_PIN                GPIO_NUM_15
#define ES8311_ADDR_7BIT         0x18
#define ES7210_ADDR_7BIT         0x40

/* ---- Audio, V1 (PCM5101 speaker + ICS-43434 mic, two simplex I2S buses) ---- */
#define V1_SPK_BCLK              GPIO_NUM_48
#define V1_SPK_WS                GPIO_NUM_38
#define V1_SPK_DOUT              GPIO_NUM_47
#define V1_MIC_SCK               GPIO_NUM_15
#define V1_MIC_WS                GPIO_NUM_2
#define V1_MIC_DIN               GPIO_NUM_39

#define AUDIO_SAMPLE_RATE        16000

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "driver/i2c_master.h"

typedef enum { BOARD_AUDIO_UNKNOWN = 0, BOARD_AUDIO_V1, BOARD_AUDIO_V2 } board_audio_rev_t;

esp_err_t board_init(void);
i2c_master_bus_handle_t board_i2c_bus(void);
bool board_i2c_probe(uint8_t addr);
board_audio_rev_t board_audio_rev(void);
const char *board_audio_rev_name(void);
void board_backlight_set(uint8_t percent);
bool board_boot_button_pressed(void);
