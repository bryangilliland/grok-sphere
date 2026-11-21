/* Grok Sphere - settings kept in the ESP32's own flash (NVS). Nothing is hardcoded. */
#include "settings.h"
#include <string.h>
#include "nvs.h"
#include "nvs_flash.h"

#define NS "sphere"

static void get_str(nvs_handle_t h, const char *key, char *dst, size_t cap)
{
    size_t len = cap;
    if (nvs_get_str(h, key, dst, &len) != ESP_OK) dst[0] = 0;
}

esp_err_t settings_load(sphere_settings_t *s)
{
    memset(s, 0, sizeof(*s));
    s->bridge_port = BRIDGE_DEFAULT_PORT;
    s->volume = 70;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return ESP_OK;   /* first boot */
    get_str(h, "ssid", s->ssid, sizeof(s->ssid));
    get_str(h, "pass", s->pass, sizeof(s->pass));
    get_str(h, "host", s->bridge_host, sizeof(s->bridge_host));
    uint16_t port;
    if (nvs_get_u16(h, "port", &port) == ESP_OK && port) s->bridge_port = port;
    uint8_t u8;
    if (nvs_get_u8(h, "mirror", &u8) == ESP_OK) s->mirror_x = u8 != 0;
    if (nvs_get_u8(h, "volume", &u8) == ESP_OK && u8 <= 100) s->volume = u8;
    nvs_close(h);
    return ESP_OK;
}

esp_err_t settings_save(const sphere_settings_t *s)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err |= nvs_set_str(h, "ssid", s->ssid);
    err |= nvs_set_str(h, "pass", s->pass);
    err |= nvs_set_str(h, "host", s->bridge_host);
    err |= nvs_set_u16(h, "port", s->bridge_port);
    err |= nvs_set_u8(h, "mirror", s->mirror_x ? 1 : 0);
    err |= nvs_set_u8(h, "volume", s->volume);
    err |= nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t settings_erase(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    nvs_erase_all(h);
    err = nvs_commit(h);
    nvs_close(h);
    return err;
}

bool settings_have_wifi(const sphere_settings_t *s) { return s->ssid[0] != 0; }
