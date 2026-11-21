/*
 * Grok Sphere - time and weather.
 *   Time: SNTP (pool.ntp.org), shown in US Central time with daylight saving.
 *   Weather: Open-Meteo forecast API for configured location (default Fort Worth, TX: 32.7555, -97.3308),
 *            in deg F / mph, refreshed every 10 minutes and shared with Claude's system prompt.
 */
#include "clock_weather.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "app.h"
#include "assist_util.h"
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "net.h"
#include "settings.h"
#include "ui.h"

#define TZ_CENTRAL       "CST6CDT,M3.2.0,M11.1.0"
#define DEFAULT_LOC      "Fort Worth, TX"
#define DEFAULT_LAT      32.7555
#define DEFAULT_LON      -97.3308
#define WEATHER_EVERY_MS (10 * 60 * 1000)
#define WEATHER_STALE_MS (30 * 60 * 1000)
#define BODY_MAX         4096

static const char *TAG = "weather";
static SemaphoreHandle_t s_wlock;
static TaskHandle_t s_wtask;
static char s_summary[240];
static int64_t s_summary_ms;
static char s_cached_loc[64];
static char s_resolved_name[64] = DEFAULT_LOC;
static double s_lat = DEFAULT_LAT, s_lon = DEFAULT_LON;
static bool s_coords_ok = true;

static int http_get_json(const char *url, char *body, size_t cap)
{
    esp_http_client_config_t cfg = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 10000,
        .buffer_size = 2048,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return -1;
    int status = -1;
    if (esp_http_client_open(c, 0) == ESP_OK) {
        esp_http_client_fetch_headers(c);
        int total = 0, n;
        while (total < (int)cap - 1 && (n = esp_http_client_read(c, body + total, (int)cap - 1 - total)) > 0) {
            total += n;
        }
        body[total] = 0;
        status = esp_http_client_get_status_code(c);
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return status;
}

static void url_encode(const char *in, char *out, size_t cap)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (; *in && o + 4 < cap; in++) {
        unsigned char ch = (unsigned char)*in;
        if (isalnum(ch) || ch == '-' || ch == '_' || ch == '.') {
            out[o++] = (char)ch;
        } else if (ch == ' ') {
            out[o++] = '%'; out[o++] = '2'; out[o++] = '0';
        } else {
            out[o++] = '%'; out[o++] = hex[ch >> 4]; out[o++] = hex[ch & 0x0F];
        }
    }
    out[o] = 0;
}

static bool resolve_location(const char *loc_in, char *body)
{
    char loc[64];
    snprintf(loc, sizeof(loc), "%s", (loc_in && loc_in[0]) ? loc_in : DEFAULT_LOC);
    au_trim(loc);
    if (!loc[0]) strcpy(loc, DEFAULT_LOC);

    if (s_coords_ok && strcmp(loc, s_cached_loc) == 0) return true;

    double lat = 0, lon = 0;
    if (au_parse_latlon(loc, &lat, &lon)) {
        s_lat = lat;
        s_lon = lon;
        snprintf(s_resolved_name, sizeof(s_resolved_name), "%.2f, %.2f", lat, lon);
        snprintf(s_cached_loc, sizeof(s_cached_loc), "%s", loc);
        s_coords_ok = true;
        return true;
    }

    if (strcasecmp(loc, DEFAULT_LOC) == 0 || strcasecmp(loc, "Fort Worth") == 0) {
        s_lat = DEFAULT_LAT;
        s_lon = DEFAULT_LON;
        strcpy(s_resolved_name, DEFAULT_LOC);
        snprintf(s_cached_loc, sizeof(s_cached_loc), "%s", loc);
        s_coords_ok = true;
        return true;
    }

    /* Strip trailing ", ST" if Open-Meteo geocoding needs just the city name */
    char query[64];
    snprintf(query, sizeof(query), "%s", loc);
    char *comma = strchr(query, ',');
    if (comma) *comma = 0;
    au_trim(query);

    char enc[192], url[300];
    url_encode(query, enc, sizeof(enc));
    snprintf(url, sizeof(url),
             "https://geocoding-api.open-meteo.com/v1/search?name=%s&count=1&language=en&format=json", enc);
    if (http_get_json(url, body, BODY_MAX) == 200) {
        cJSON *root = cJSON_Parse(body);
        const cJSON *res = cJSON_GetObjectItem(root, "results");
        const cJSON *r0 = cJSON_IsArray(res) ? cJSON_GetArrayItem(res, 0) : NULL;
        const cJSON *jlat = cJSON_GetObjectItem(r0, "latitude");
        const cJSON *jlon = cJSON_GetObjectItem(r0, "longitude");
        const cJSON *jname = cJSON_GetObjectItem(r0, "name");
        const cJSON *jadm = cJSON_GetObjectItem(r0, "admin1");
        if (cJSON_IsNumber(jlat) && cJSON_IsNumber(jlon)) {
            s_lat = jlat->valuedouble;
            s_lon = jlon->valuedouble;
            if (cJSON_IsString(jname) && cJSON_IsString(jadm)) {
                snprintf(s_resolved_name, sizeof(s_resolved_name), "%s, %s", jname->valuestring, jadm->valuestring);
            } else if (cJSON_IsString(jname)) {
                snprintf(s_resolved_name, sizeof(s_resolved_name), "%s", jname->valuestring);
            } else {
                snprintf(s_resolved_name, sizeof(s_resolved_name), "%s", loc);
            }
            snprintf(s_cached_loc, sizeof(s_cached_loc), "%s", loc);
            s_coords_ok = true;
            cJSON_Delete(root);
            ESP_LOGI(TAG, "resolved '%s' -> %s (%.4f, %.4f)", loc, s_resolved_name, s_lat, s_lon);
            return true;
        }
        cJSON_Delete(root);
    }

    ESP_LOGW(TAG, "geocode failed for '%s', falling back to Fort Worth", loc);
    s_lat = DEFAULT_LAT;
    s_lon = DEFAULT_LON;
    strcpy(s_resolved_name, DEFAULT_LOC);
    return false;
}

static bool fetch_weather(void)
{
    if (!net_is_connected()) return false;
    char *body = heap_caps_calloc(1, BODY_MAX, MALLOC_CAP_SPIRAM);
    if (!body) return false;

    sphere_settings_t s;
    app_settings_get(&s);
    char loc[64];
    snprintf(loc, sizeof(loc), "%s", s.location);
    memset(&s, 0, sizeof(s));

    resolve_location(loc, body);

    char url[384];
    snprintf(url, sizeof(url),
             "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
             "&current=temperature_2m,apparent_temperature,weather_code,wind_speed_10m"
             "&daily=temperature_2m_max,temperature_2m_min,precipitation_probability_max"
             "&temperature_unit=fahrenheit&wind_speed_unit=mph&timezone=auto&forecast_days=1",
             s_lat, s_lon);

    bool ok = false;
    int status = http_get_json(url, body, BODY_MAX);
    if (status == 200 && body[0]) {
        cJSON *root = cJSON_Parse(body);
        const cJSON *cur = cJSON_GetObjectItem(root, "current");
        const cJSON *daily = cJSON_GetObjectItem(root, "daily");
        const cJSON *t = cJSON_GetObjectItem(cur, "temperature_2m");
        const cJSON *feels = cJSON_GetObjectItem(cur, "apparent_temperature");
        const cJSON *code = cJSON_GetObjectItem(cur, "weather_code");
        const cJSON *wind = cJSON_GetObjectItem(cur, "wind_speed_10m");
        const cJSON *hi = cJSON_GetArrayItem(cJSON_GetObjectItem(daily, "temperature_2m_max"), 0);
        const cJSON *lo = cJSON_GetArrayItem(cJSON_GetObjectItem(daily, "temperature_2m_min"), 0);
        const cJSON *pop = cJSON_GetArrayItem(cJSON_GetObjectItem(daily, "precipitation_probability_max"), 0);
        if (cJSON_IsNumber(t) && cJSON_IsNumber(code)) {
            float tf = (float)t->valuedouble;
            float ff = cJSON_IsNumber(feels) ? (float)feels->valuedouble : tf;
            float hif = cJSON_IsNumber(hi) ? (float)hi->valuedouble : tf;
            float lof = cJSON_IsNumber(lo) ? (float)lo->valuedouble : tf;
            int wmph = cJSON_IsNumber(wind) ? (int)lround(wind->valuedouble) : 0;
            int rain = cJSON_IsNumber(pop) ? (int)lround(pop->valuedouble) : 0;
            const char *cond = ui_weather_text(code->valueint);

            ui_set_weather(true, tf, code->valueint, hif, lof);
            if (s_wlock) xSemaphoreTake(s_wlock, portMAX_DELAY);
            snprintf(s_summary, sizeof(s_summary),
                     "%s: %d F (feels like %d F), %s, wind %d mph; today's high %d F, low %d F, rain chance %d%%.",
                     s_resolved_name, (int)lroundf(tf), (int)lroundf(ff), cond, wmph,
                     (int)lroundf(hif), (int)lroundf(lof), rain);
            s_summary_ms = esp_timer_get_time() / 1000;
            if (s_wlock) xSemaphoreGive(s_wlock);
            ESP_LOGI(TAG, "%s", s_summary);
            ok = true;
        }
        cJSON_Delete(root);
    } else {
        ESP_LOGW(TAG, "Open-Meteo HTTP %d", status);
    }
    free(body);
    return ok;
}

bool clock_weather_summary(char *out, size_t cap)
{
    if (!out || !cap) return false;
    out[0] = 0;
    if (!s_wlock) return false;
    bool fresh = false;
    xSemaphoreTake(s_wlock, portMAX_DELAY);
    if (s_summary[0] && (esp_timer_get_time() / 1000 - s_summary_ms) < WEATHER_STALE_MS) {
        snprintf(out, cap, "%s", s_summary);
        fresh = true;
    }
    xSemaphoreGive(s_wlock);
    return fresh;
}

void clock_weather_refresh(void)
{
    if (s_wtask) xTaskNotifyGive(s_wtask);
}

static void weather_task(void *arg)
{
    (void)arg;
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1500));
    for (;;) {
        bool ok = fetch_weather();
        if (!ok) ui_set_weather(false, 0, 0, 0, 0);
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(ok ? WEATHER_EVERY_MS : 45 * 1000));
    }
}

void clock_weather_start(void)
{
    if (!s_wlock) s_wlock = xSemaphoreCreateMutex();
    setenv("TZ", TZ_CENTRAL, 1);
    tzset();
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    esp_netif_sntp_init(&cfg);
    if (xTaskCreatePinnedToCoreWithCaps(weather_task, "weather", 8192, NULL, 3, &s_wtask, tskNO_AFFINITY,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        xTaskCreate(weather_task, "weather", 8192, NULL, 3, &s_wtask);
    }
}
