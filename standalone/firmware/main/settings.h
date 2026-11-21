#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define SETTINGS_KEY_MAX     200
#define SETTINGS_SPEECH_MAX  120

typedef enum { MODEL_HAIKU = 0, MODEL_SONNET = 1 } sphere_model_t;

typedef struct {
    char ssid[33];
    char pass[65];
    char api_key[SETTINGS_KEY_MAX];        /* Anthropic key. Never logged or shown (only last 4). */
    char speech_key[SETTINGS_SPEECH_MAX];  /* Optional Deepgram key for voice in/out. */
    char voice[40];                        /* Deepgram Aura-2 voice id */
    uint8_t model;                         /* sphere_model_t */
    bool mirror_x;
    uint8_t volume;                        /* 0..100 */
    bool wake_enabled;                     /* on-device ESP-SR wake word (default true) */
    char wake_word[28];                    /* WakeNet9 model id (default AU_DEFAULT_WAKE) */
} sphere_settings_t;

esp_err_t settings_load(sphere_settings_t *s);   /* fills defaults when nothing is saved */
esp_err_t settings_save(const sphere_settings_t *s);
esp_err_t settings_erase(void);
bool settings_have_wifi(const sphere_settings_t *s);
