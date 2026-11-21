#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define BRIDGE_DEFAULT_PORT 8765

typedef struct {
    char ssid[33];
    char pass[65];
    char bridge_host[64];   /* empty = find the Mac bridge automatically (mDNS) */
    uint16_t bridge_port;
    bool mirror_x;          /* flip the screen if text shows mirrored */
    uint8_t volume;         /* 0..100 */
} sphere_settings_t;

esp_err_t settings_load(sphere_settings_t *s);   /* fills defaults when nothing is saved */
esp_err_t settings_save(const sphere_settings_t *s);
esp_err_t settings_erase(void);
bool settings_have_wifi(const sphere_settings_t *s);
