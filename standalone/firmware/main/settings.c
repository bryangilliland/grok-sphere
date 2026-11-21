/* Grok Sphere standalone - settings kept in the ESP32's own flash (NVS namespace "sphere",
 * same as firmware 1.0 / Thad edition, so a saved home Wi-Fi carries over). Nothing is hardcoded. */
#include "settings.h"
#include <string.h>
#include "assist_util.h"
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
    s->volume = 70;
    s->model = MODEL_HAIKU;
    strcpy(s->voice, AU_DEFAULT_VOICE);
    s->wake_enabled = true;
    strcpy(s->wake_word, AU_DEFAULT_WAKE);
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return ESP_OK;   /* first boot */
    get_str(h, "ssid", s->ssid, sizeof(s->ssid));
    get_str(h, "pass", s->pass, sizeof(s->pass));
    get_str(h, "akey", s->api_key, sizeof(s->api_key));
    get_str(h, "dgkey", s->speech_key, sizeof(s->speech_key));
    char v[40];
    get_str(h, "voice", v, sizeof(v));
    if (au_voice_valid(v)) strcpy(s->voice, v);
    uint8_t u8;
    if (nvs_get_u8(h, "model", &u8) == ESP_OK && u8 <= MODEL_SONNET) s->model = u8;
    if (nvs_get_u8(h, "mirror", &u8) == ESP_OK) s->mirror_x = u8 != 0;
    if (nvs_get_u8(h, "volume", &u8) == ESP_OK && u8 <= 100) s->volume = u8;
    if (nvs_get_u8(h, "wake_on", &u8) == ESP_OK) s->wake_enabled = u8 != 0;
    char w[28];
    get_str(h, "wake_word", w, sizeof(w));
    if (au_wake_valid(w)) strcpy(s->wake_word, w);
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
    err |= nvs_set_str(h, "akey", s->api_key);
    err |= nvs_set_str(h, "dgkey", s->speech_key);
    err |= nvs_set_str(h, "voice", s->voice);
    err |= nvs_set_u8(h, "model", s->model);
    err |= nvs_set_u8(h, "mirror", s->mirror_x ? 1 : 0);
    err |= nvs_set_u8(h, "volume", s->volume);
    err |= nvs_set_u8(h, "wake_on", s->wake_enabled ? 1 : 0);
    err |= nvs_set_str(h, "wake_word", s->wake_word[0] ? s->wake_word : AU_DEFAULT_WAKE);
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
