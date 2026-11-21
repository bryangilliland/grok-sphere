/*
 * Grok Sphere (Thad + Claude Edition) - Waveshare ESP32-S3-Touch-LCD-1.85C(-BOX).
 *
 *   - Looks like Thad (tilted emerald rounded square with animated black pill eyes)
 *     plus a matching Clock + Fort Worth Weather screen (tap when idle to toggle).
 *   - Talks to the Windows companion over USB Serial when plugged into the PC, or
 *     automatically over Wi-Fi WebSocket on the local network if USB isn't available.
 *   - Touch & hold (or short-press BOOT) = push-to-talk.
 *   - Hold BOOT 5 s (and release before 10 s) = forget Wi-Fi; hold BOOT 10 s = USB flash mode.
 */
#include <string.h>
#include "audio.h"
#include "board.h"
#include "bridge_client.h"
#include "clock_weather.h"
#include "display.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "net.h"
#include "nvs_flash.h"
#include "settings.h"
#include "sysctl.h"
#include "touch.h"
#include "ui.h"
#include "usb_link.h"

static const char *TAG = "main";
static sphere_settings_t s_settings;
static volatile bool s_ptt_active;

static void on_playback_done(void) { bridge_send_simple("playback_done"); }

static void on_bridge_msg(const proto_msg_t *m)
{
    switch (m->type) {
    case MSG_STATE:
        ui_set_state(m->state, m->label, m->text);
        break;
    case MSG_AUDIO_START:
        audio_stream_begin();
        break;
    case MSG_AUDIO_END:
        audio_stream_end();
        break;
    case MSG_STOP:
        audio_stop();
        break;
    case MSG_VOLUME:
        audio_set_volume(m->value);
        s_settings.volume = (uint8_t)m->value;
        settings_save(&s_settings);
        break;
    case MSG_SCREEN:
        ui_show_screen(m->screen);
        break;
    case MSG_CHIME:
        audio_play_chime();
        break;
    default:
        break;
    }
}

static void on_bridge_conn(bool connected, const char *where)
{
    if (connected) {
        ESP_LOGI(TAG, "companion connected (%s)", where);
        if (net_portal_running()) ui_hide_setup();
        ui_set_state(UI_STATE_IDLE, "", "");
    } else {
        ui_set_state(UI_STATE_OFFLINE, "", "Waiting for PC...");
    }
}

static void ptt(bool start)
{
    if (start == s_ptt_active) return;
    s_ptt_active = start;
    if (start && bridge_connected()) ui_set_state(UI_STATE_LISTENING, "Thad", "Listening...");
    bridge_send_touch(start ? "ptt_start" : "ptt_stop");
}

static void on_touch(touch_event_t e)
{
    switch (e) {
    case TOUCH_EVT_TAP:
        if (ui_get_state() == UI_STATE_IDLE || ui_get_state() == UI_STATE_OFFLINE) ui_toggle_screen();
        bridge_send_touch("tap");
        break;
    case TOUCH_EVT_HOLD_START: ptt(true); break;
    case TOUCH_EVT_HOLD_END:   ptt(false); break;
    }
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    board_init();
    settings_load(&s_settings);
    ESP_ERROR_CHECK(display_init(s_settings.mirror_x));
    touch_init(on_touch);
    ui_init();
    ui_set_state(UI_STATE_OFFLINE, "", "Starting...");
    if (audio_init(board_audio_rev(), on_playback_done) != ESP_OK) {
        ui_set_state(UI_STATE_ERROR, "Thad", "Audio chip error");
    }
    audio_set_volume(s_settings.volume);

    /* Start USB link right away so plugged-in PC use works even before Wi-Fi setup. */
    net_init();
    bridge_init(on_bridge_msg, on_bridge_conn);

    if (!settings_have_wifi(&s_settings)) {
        if (!usb_link_connected()) {
            net_start_portal(&s_settings);
        }
    } else {
        if (!usb_link_connected()) ui_set_status("Joining Wi-Fi...");
        if (!net_connect_sta(&s_settings, 30000)) {
            ESP_LOGW(TAG, "could not join '%s', opening setup hotspot", s_settings.ssid);
            if (!usb_link_connected()) net_start_portal(&s_settings);
        }
    }

    bool services = false;
    int boot_ms = 0;
    for (;;) {
        if (!services && net_is_connected()) {
            services = true;
            if (!usb_link_connected()) ui_set_state(UI_STATE_OFFLINE, "", "Waiting for PC...");
            clock_weather_start();
            bridge_start_wifi(&s_settings);
        }
        if (net_portal_running() && net_is_connected() && settings_have_wifi(&s_settings) &&
            net_portal_clients() == 0) {
            net_stop_portal();
        }
        if (board_boot_button_pressed()) {
            boot_ms += 100;
            if (boot_ms == 5000) {
                ui_show_notice("Release to reset Wi-Fi", "Keep holding 5 more sec\nfor USB Flash Mode.");
            } else if (boot_ms == 10000) {
                sys_enter_flash_mode();
            }
        } else {
            if (boot_ms >= 5000 && boot_ms < 10000) {
                ui_set_state(UI_STATE_ERROR, "Thad", "Forgetting Wi-Fi...");
                settings_erase();
                vTaskDelay(pdMS_TO_TICKS(1000));
                esp_restart();
            } else if (boot_ms > 0 && boot_ms < 1500) {
                ptt(!s_ptt_active);
            }
            boot_ms = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
