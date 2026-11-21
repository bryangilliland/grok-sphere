/*
 * Grok Sphere - Standalone Claude Edition (no laptop, no app).
 * Waveshare ESP32-S3-Touch-LCD-1.85C(-BOX).
 *
 *   - Original two white capsule eyes (firmware 1.0 / the X post) + clock/Fort Worth weather.
 *   - First boot (or BOOT held ~4 s): "GrokSphere-XXXX" hotspot + phone setup page for home Wi-Fi,
 *     Anthropic key, optional Deepgram voice key, model and voice. The same page stays reachable
 *     on the home network at http://<ip> or http://grok-sphere.local.
 *   - Tap the screen (or short-press BOOT) and talk. Touch-and-hold toggles the clock.
 *   - BOOT held 10 s = USB flash mode.
 */
#include <string.h>
#include "app.h"
#include "assistant.h"
#include "audio.h"
#include "board.h"
#include "clock_weather.h"
#include "display.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "net.h"
#include "nvs_flash.h"
#include "settings.h"
#include "sysctl.h"
#include "touch.h"
#include "ui.h"

#define FW_VERSION "2.3.4-standalone"

static const char *TAG = "main";
static sphere_settings_t s_settings;
static SemaphoreHandle_t s_lock;
static volatile bool s_key_link_shown;

void app_settings_get(sphere_settings_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_settings;
    xSemaphoreGive(s_lock);
}

void app_settings_apply(const sphere_settings_t *s, bool wifi_changed)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_settings = *s;
    esp_err_t err = settings_save(&s_settings);
    xSemaphoreGive(s_lock);
    if (err != ESP_OK) ESP_LOGE(TAG, "settings save failed");
    else ESP_LOGI(TAG, "settings saved (wifi_changed=%d)", wifi_changed);   /* never log keys */
    audio_set_volume(s->volume);
    assistant_wake_reconfig();
    if (s->api_key[0] && s_key_link_shown && !net_portal_running()) {
        s_key_link_shown = false;
        ui_hide_setup();
    }
}

void app_factory_reset(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    settings_erase();
    memset(&s_settings, 0, sizeof(s_settings));
    xSemaphoreGive(s_lock);
}

static bool have_key(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool k = s_settings.api_key[0] != 0;
    xSemaphoreGive(s_lock);
    return k;
}

static void show_key_link(void)
{
    char url[40], body[180];
    snprintf(url, sizeof(url), "http://%s/", net_ip_str());
    snprintf(body, sizeof(body), "Phone on the same Wi-Fi:\nscan, or open http://%s\n(or http://grok-sphere.local)",
             net_ip_str());
    ui_show_link("Add your Claude key", url, body);
    s_key_link_shown = true;
}

static void on_touch(touch_event_t e)
{
    if (net_portal_running() || s_key_link_shown) return;     /* setup screen is up */
    switch (e) {
    case TOUCH_EVT_TAP:
        assistant_tap();
        break;
    case TOUCH_EVT_HOLD_START:
        if (!assistant_busy()) ui_toggle_screen();
        break;
    default:
        break;
    }
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    s_lock = xSemaphoreCreateMutex();
    board_init();
    settings_load(&s_settings);
    ESP_LOGI(TAG, "Grok Sphere %s", FW_VERSION);
    ESP_ERROR_CHECK(display_init(s_settings.mirror_x));
    touch_init(on_touch);
    ui_init();
    ui_set_state(UI_STATE_OFFLINE, "", "Starting...");
    if (audio_init(board_audio_rev(), NULL) != ESP_OK) ui_set_state(UI_STATE_ERROR, "", "Audio chip error");
    audio_set_volume(s_settings.volume);

    net_init();
    assistant_init();

    int64_t portal_since = 0, portal_empty_since = 0;
    if (!settings_have_wifi(&s_settings)) {
        net_start_portal();
        portal_since = esp_timer_get_time() / 1000;
    } else {
        ui_set_status("Joining Wi-Fi...");
        if (!net_connect_sta(&s_settings, 30000)) {
            ESP_LOGW(TAG, "could not join saved Wi-Fi, opening setup hotspot");
            net_start_portal();
            portal_since = esp_timer_get_time() / 1000;
        }
    }

    bool services = false;
    int boot_ms = 0;
    for (;;) {
        int64_t now = esp_timer_get_time() / 1000;
        if (!services && net_is_connected()) {
            services = true;
            net_http_start();
            net_mdns_start();
            clock_weather_start();
            assistant_wake_reconfig();
            ui_set_state(UI_STATE_IDLE, "", "");
            if (!have_key() && !net_portal_running()) show_key_link();
        }
        if (services && !net_is_connected() && ui_get_state() == UI_STATE_IDLE) {
            ui_set_state(UI_STATE_OFFLINE, "", "Wi-Fi lost, retrying...");
        } else if (services && net_is_connected() && ui_get_state() == UI_STATE_OFFLINE) {
            ui_set_state(UI_STATE_IDLE, "", "");
        }

        /* close the hotspot once the sphere is fully set up and nobody is using it */
        if (net_portal_running() && net_is_connected() && have_key()) {
            if (net_portal_clients() > 0) portal_empty_since = 0;
            else if (!portal_empty_since) portal_empty_since = now;
            if (portal_empty_since && now - portal_empty_since > 60000 && now - portal_since > 120000) {
                net_stop_portal();
                assistant_wake_reconfig();
                portal_empty_since = 0;
            }
        }

        if (board_boot_button_pressed()) {
            boot_ms += 100;
            if (boot_ms == 3000) {
                ui_show_notice("Release for setup", "Let go now to open the\nsetup hotspot.\n\nKeep holding 10 s for\nUSB flash mode.");
            } else if (boot_ms == 10000) {
                sys_enter_flash_mode();
            }
        } else {
            if (boot_ms >= 3000 && boot_ms < 10000) {
                ui_hide_notice();
                s_key_link_shown = false;
                net_start_portal();
                assistant_wake_reconfig();
                portal_since = now;
                portal_empty_since = 0;
            } else if (boot_ms > 0 && boot_ms < 1500 && !net_portal_running()) {
                assistant_tap();
            }
            boot_ms = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
