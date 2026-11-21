/*
 * Grok Sphere - time and weather.
 *   Time: SNTP (pool.ntp.org), shown in US Central time with daylight saving.
 *   Weather: Open-Meteo forecast API for Fort Worth, TX (32.75, -97.33), in deg F,
 *            refreshed every 15 minutes. No API key is needed.
 * These are the only internet requests the board makes.
 */
#include "clock_weather.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ui.h"

#define TZ_CENTRAL   "CST6CDT,M3.2.0,M11.1.0"
#define WEATHER_URL  "https://api.open-meteo.com/v1/forecast?latitude=32.75&longitude=-97.33" \
                     "&current=temperature_2m,weather_code" \
                     "&daily=temperature_2m_max,temperature_2m_min" \
                     "&temperature_unit=fahrenheit&timezone=America%2FChicago&forecast_days=1"
#define WEATHER_EVERY_MS (15 * 60 * 1000)
#define BODY_MAX     4096

static const char *TAG = "weather";

static bool fetch_weather(void)
{
    char *body = calloc(1, BODY_MAX);
    if (!body) return false;
    esp_http_client_config_t cfg = {
        .url = WEATHER_URL,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 10000,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    bool ok = false;
    if (esp_http_client_open(c, 0) == ESP_OK) {
        esp_http_client_fetch_headers(c);
        int total = 0, n;
        while (total < BODY_MAX - 1 && (n = esp_http_client_read(c, body + total, BODY_MAX - 1 - total)) > 0) {
            total += n;
        }
        int status = esp_http_client_get_status_code(c);
        if (status == 200 && total > 0) {
            cJSON *root = cJSON_Parse(body);
            const cJSON *cur = cJSON_GetObjectItem(root, "current");
            const cJSON *daily = cJSON_GetObjectItem(root, "daily");
            const cJSON *t = cJSON_GetObjectItem(cur, "temperature_2m");
            const cJSON *code = cJSON_GetObjectItem(cur, "weather_code");
            const cJSON *hi = cJSON_GetArrayItem(cJSON_GetObjectItem(daily, "temperature_2m_max"), 0);
            const cJSON *lo = cJSON_GetArrayItem(cJSON_GetObjectItem(daily, "temperature_2m_min"), 0);
            if (cJSON_IsNumber(t) && cJSON_IsNumber(code)) {
                ui_set_weather(true, (float)t->valuedouble, code->valueint,
                               cJSON_IsNumber(hi) ? (float)hi->valuedouble : (float)t->valuedouble,
                               cJSON_IsNumber(lo) ? (float)lo->valuedouble : (float)t->valuedouble);
                ESP_LOGI(TAG, "%.1f F, code %d", t->valuedouble, code->valueint);
                ok = true;
            }
            cJSON_Delete(root);
        } else {
            ESP_LOGW(TAG, "Open-Meteo HTTP %d", status);
        }
    }
    esp_http_client_cleanup(c);
    free(body);
    return ok;
}

static void weather_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(3000));
    for (;;) {
        bool ok = fetch_weather();
        if (!ok) ui_set_weather(false, 0, 0, 0, 0);
        vTaskDelay(pdMS_TO_TICKS(ok ? WEATHER_EVERY_MS : 60 * 1000));
    }
}

void clock_weather_start(void)
{
    setenv("TZ", TZ_CENTRAL, 1);
    tzset();
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    esp_netif_sntp_init(&cfg);
    xTaskCreate(weather_task, "weather", 8192, NULL, 3, NULL);
}
