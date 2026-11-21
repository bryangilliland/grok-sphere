/*
 * Grok Sphere (Thad + Claude Edition) - LVGL 8.3 screens.
 *
 *   Eyes screen: the original two rounded "eyes" from firmware 1.0 / v2 (the look
 *     taken from @Texas_Twins_Dad's X post that started this project):
 *       idle = off-white + random blink, listening = wide cyan, thinking = squint + bouncing dots,
 *       speaking = height pulses with the speaker level, error = red, offline = dim grey.
 *   Clock screen: time, date, Fort Worth weather (filled in by weather.c / SNTP).
 *   Setup / Notice screen: QR code hotspot setup or full-screen USB flash notice.
 */
#include "ui.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "audio.h"
#include "display.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "lvgl.h"

#define EYE_W        74
#define EYE_H        120
#define EYE_DX       62
#define EYE_CY       160

static lv_obj_t *s_scr_eyes, *s_scr_clock, *s_scr_setup;
static lv_obj_t *s_eye[2], *s_dot[3], *s_status;
static lv_obj_t *s_time, *s_date, *s_temp, *s_cond, *s_hilo;
static lv_obj_t *s_setup_title, *s_setup_qr, *s_setup_text;

static char s_setup_qr_str[128], s_setup_txt_str[180];
static bool s_notice, s_notice_over_setup;
static ui_state_t s_state = UI_STATE_OFFLINE;
static ui_screen_t s_screen = SCREEN_EYES;
static bool s_setup_active;
static int64_t s_next_blink_ms, s_blink_start_ms = -1, s_state_since_ms;
static float s_speak_h = 70;
static char s_label[32];

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

static lv_color_t state_color(ui_state_t s)
{
    switch (s) {
    case UI_STATE_LISTENING: return lv_color_hex(0x3DDCFF);
    case UI_STATE_THINKING:  return lv_color_hex(0x9AA7FF);
    case UI_STATE_ERROR:     return lv_color_hex(0xFF3B30);
    case UI_STATE_OFFLINE:   return lv_color_hex(0x4A4F57);
    default:                 return lv_color_hex(0xEAF2FF);
    }
}

static void place_eye(int i, int w, int h, int cy)
{
    if (h < 6) h = 6;
    lv_obj_set_size(s_eye[i], w, h);
    lv_obj_set_style_radius(s_eye[i], w / 2, 0);
    int cx = 180 + (i == 0 ? -EYE_DX : EYE_DX);
    lv_obj_set_pos(s_eye[i], cx - w / 2, cy - h / 2);
}

static void anim_tick(lv_timer_t *t)
{
    (void)t;
    if (lv_scr_act() != s_scr_eyes) return;
    int64_t now = now_ms();
    float sec = (now - s_state_since_ms) / 1000.0f;
    int w = EYE_W, h = EYE_H, cy = EYE_CY;
    bool dots = false;

    switch (s_state) {
    case UI_STATE_IDLE: {
        if (s_blink_start_ms < 0 && now >= s_next_blink_ms) s_blink_start_ms = now;
        if (s_blink_start_ms >= 0) {
            float p = (now - s_blink_start_ms) / 180.0f;           /* 180 ms blink */
            if (p >= 1.0f) {
                s_blink_start_ms = -1;
                s_next_blink_ms = now + 2500 + (esp_random() % 3500);
            } else {
                h = (int)(EYE_H * fabsf(1.0f - 2.0f * p));
            }
        }
        break;
    }
    case UI_STATE_LISTENING:
        w = 84;
        h = 140 + (int)(6 * sinf(sec * 6.0f));
        break;
    case UI_STATE_THINKING:
        h = 56;
        cy = EYE_CY - 18;
        dots = true;
        break;
    case UI_STATE_SPEAKING: {
        float target = 64 + audio_level() * 0.9f;
        s_speak_h += (target - s_speak_h) * 0.45f;
        h = (int)s_speak_h;
        break;
    }
    case UI_STATE_ERROR:
        h = 90;
        break;
    case UI_STATE_OFFLINE:
    default:
        h = 36;
        break;
    }
    place_eye(0, w, h, cy);
    place_eye(1, w, h, cy);
    for (int i = 0; i < 3; i++) {
        if (dots) {
            lv_obj_clear_flag(s_dot[i], LV_OBJ_FLAG_HIDDEN);
            float y = 228 - 14 * fmaxf(0.0f, sinf(sec * 7.0f - i * 0.9f));
            lv_obj_set_pos(s_dot[i], 180 - 8 + (i - 1) * 34, (int)y);
        } else {
            lv_obj_add_flag(s_dot[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void clock_tick(lv_timer_t *t)
{
    (void)t;
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char buf[32];
    if (tm.tm_year < (2024 - 1900)) {
        lv_label_set_text(s_time, "--:--");
        lv_label_set_text(s_date, "waiting for time");
        return;
    }
    int hr = tm.tm_hour % 12;
    if (hr == 0) hr = 12;
    snprintf(buf, sizeof(buf), "%d:%02d %s", hr, tm.tm_min, tm.tm_hour < 12 ? "AM" : "PM");
    lv_label_set_text(s_time, buf);
    strftime(buf, sizeof(buf), "%a - %b %d", &tm);
    for (char *p = buf; *p; p++) if (*p >= 'a' && *p <= 'z') *p -= 32;
    lv_label_set_text(s_date, buf);
}

static lv_obj_t *mk_label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color, int y)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(l, LV_ALIGN_CENTER, 0, y);
    lv_label_set_text(l, "");
    return l;
}

static lv_obj_t *mk_screen(void)
{
    lv_obj_t *s = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s, LV_OBJ_FLAG_SCROLLABLE);
    return s;
}

void ui_init(void)
{
    display_lock(-1);
    s_scr_eyes = mk_screen();
    for (int i = 0; i < 2; i++) {
        s_eye[i] = lv_obj_create(s_scr_eyes);
        lv_obj_set_style_border_width(s_eye[i], 0, 0);
        lv_obj_set_style_bg_opa(s_eye[i], LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(s_eye[i], state_color(s_state), 0);
        lv_obj_clear_flag(s_eye[i], LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        place_eye(i, EYE_W, 36, EYE_CY);
    }
    for (int i = 0; i < 3; i++) {
        s_dot[i] = lv_obj_create(s_scr_eyes);
        lv_obj_set_size(s_dot[i], 16, 16);
        lv_obj_set_style_radius(s_dot[i], 8, 0);
        lv_obj_set_style_border_width(s_dot[i], 0, 0);
        lv_obj_set_style_bg_color(s_dot[i], lv_color_hex(0x9AA7FF), 0);
        lv_obj_clear_flag(s_dot[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(s_dot[i], LV_OBJ_FLAG_HIDDEN);
    }
    s_status = mk_label(s_scr_eyes, &lv_font_montserrat_20, lv_color_hex(0x8A93A3), 110);
    lv_obj_set_width(s_status, 250);
    lv_label_set_long_mode(s_status, LV_LABEL_LONG_DOT);
    lv_obj_set_height(s_status, 52);

    s_scr_clock = mk_screen();
    s_time = mk_label(s_scr_clock, &lv_font_montserrat_48, lv_color_white(), -70);
    s_date = mk_label(s_scr_clock, &lv_font_montserrat_20, lv_color_hex(0x9AA7FF), -20);
    s_temp = mk_label(s_scr_clock, &lv_font_montserrat_48, lv_color_white(), 40);
    s_cond = mk_label(s_scr_clock, &lv_font_montserrat_20, lv_color_hex(0xC8CED8), 90);
    s_hilo = mk_label(s_scr_clock, &lv_font_montserrat_14, lv_color_hex(0x8A93A3), 120);
    lv_label_set_text(s_cond, "Fort Worth");

    s_scr_setup = mk_screen();
    s_setup_title = mk_label(s_scr_setup, &lv_font_montserrat_20, lv_color_white(), -125);
    lv_label_set_text(s_setup_title, "Wi-Fi setup");
    s_setup_qr = lv_qrcode_create(s_scr_setup, 140, lv_color_black(), lv_color_white());
    lv_obj_set_style_border_color(s_setup_qr, lv_color_white(), 0);
    lv_obj_set_style_border_width(s_setup_qr, 6, 0);
    lv_obj_align(s_setup_qr, LV_ALIGN_CENTER, 0, -25);
    s_setup_text = mk_label(s_scr_setup, &lv_font_montserrat_14, lv_color_hex(0xC8CED8), 100);

    lv_timer_create(anim_tick, 33, NULL);
    lv_timer_create(clock_tick, 1000, NULL);
    s_state_since_ms = now_ms();
    s_next_blink_ms = s_state_since_ms + 3000;
    lv_scr_load(s_scr_eyes);
    display_unlock();
}

static void refresh_status_text(const char *text)
{
    char buf[140];
    if (s_label[0] && text && text[0]) snprintf(buf, sizeof(buf), "%s\n%s", s_label, text);
    else if (s_label[0]) snprintf(buf, sizeof(buf), "%s", s_label);
    else snprintf(buf, sizeof(buf), "%s", text ? text : "");
    lv_label_set_text(s_status, buf);
}

void ui_set_state(ui_state_t state, const char *label, const char *text)
{
    display_lock(-1);
    if (state != s_state) s_state_since_ms = now_ms();
    s_state = state;
    snprintf(s_label, sizeof(s_label), "%s", label ? label : "");
    for (int i = 0; i < 2; i++) lv_obj_set_style_bg_color(s_eye[i], state_color(state), 0);
    refresh_status_text(text);
    if (state != UI_STATE_IDLE && state != UI_STATE_OFFLINE && !s_setup_active && !s_notice) {
        s_screen = SCREEN_EYES;
        lv_scr_load(s_scr_eyes);
    }
    display_unlock();
}

ui_state_t ui_get_state(void) { return s_state; }

void ui_set_status(const char *text)
{
    display_lock(-1);
    refresh_status_text(text);
    display_unlock();
}

void ui_show_screen(ui_screen_t screen)
{
    display_lock(-1);
    s_screen = screen;
    if (!s_setup_active && !s_notice) lv_scr_load(screen == SCREEN_CLOCK ? s_scr_clock : s_scr_eyes);
    if (screen == SCREEN_CLOCK) clock_tick(NULL);
    display_unlock();
}

void ui_toggle_screen(void)
{
    ui_show_screen(s_screen == SCREEN_EYES ? SCREEN_CLOCK : SCREEN_EYES);
}

void ui_show_setup(const char *ap_ssid, const char *ap_pass, const char *ip)
{
    snprintf(s_setup_qr_str, sizeof(s_setup_qr_str), "WIFI:T:WPA;S:%s;P:%s;;", ap_ssid, ap_pass);
    snprintf(s_setup_txt_str, sizeof(s_setup_txt_str),
             "Join Wi-Fi  %s\nPassword  %s\nthen open  http://%s", ap_ssid, ap_pass, ip);
    display_lock(-1);
    s_setup_active = true;
    if (!s_notice) {
        lv_label_set_text(s_setup_title, "Wi-Fi setup");
        lv_obj_clear_flag(s_setup_qr, LV_OBJ_FLAG_HIDDEN);
        lv_qrcode_update(s_setup_qr, s_setup_qr_str, strlen(s_setup_qr_str));
        lv_label_set_text(s_setup_text, s_setup_txt_str);
        lv_obj_align(s_setup_text, LV_ALIGN_CENTER, 0, 100);
        lv_scr_load(s_scr_setup);
    }
    display_unlock();
}

void ui_hide_setup(void)
{
    display_lock(-1);
    s_setup_active = false;
    if (!s_notice) lv_scr_load(s_screen == SCREEN_CLOCK ? s_scr_clock : s_scr_eyes);
    display_unlock();
}

void ui_show_notice(const char *title, const char *body)
{
    display_lock(-1);
    s_notice_over_setup = s_setup_active;
    s_notice = true;
    lv_label_set_text(s_setup_title, title ? title : "");
    lv_obj_add_flag(s_setup_qr, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(s_setup_text, body ? body : "");
    lv_obj_align(s_setup_text, LV_ALIGN_CENTER, 0, 0);
    lv_scr_load(s_scr_setup);
    display_unlock();
}

void ui_hide_notice(void)
{
    display_lock(-1);
    bool back_to_setup = s_notice && s_notice_over_setup;
    s_notice = false;
    if (back_to_setup) {
        lv_label_set_text(s_setup_title, "Wi-Fi setup");
        lv_obj_clear_flag(s_setup_qr, LV_OBJ_FLAG_HIDDEN);
        lv_qrcode_update(s_setup_qr, s_setup_qr_str, strlen(s_setup_qr_str));
        lv_label_set_text(s_setup_text, s_setup_txt_str);
        lv_obj_align(s_setup_text, LV_ALIGN_CENTER, 0, 100);
        lv_scr_load(s_scr_setup);
    } else if (!s_setup_active) {
        lv_scr_load(s_screen == SCREEN_CLOCK ? s_scr_clock : s_scr_eyes);
    }
    display_unlock();
}

const char *ui_weather_text(int c)
{
    if (c == 0) return "Clear";
    if (c == 1) return "Mostly clear";
    if (c == 2) return "Partly cloudy";
    if (c == 3) return "Cloudy";
    if (c == 45 || c == 48) return "Fog";
    if (c >= 51 && c <= 57) return "Drizzle";
    if (c >= 61 && c <= 67) return "Rain";
    if (c >= 71 && c <= 77) return "Snow";
    if (c >= 80 && c <= 82) return "Showers";
    if (c == 85 || c == 86) return "Snow showers";
    if (c >= 95) return "Thunderstorms";
    return "Fort Worth";
}

void ui_set_weather(bool valid, float temp_f, int code, float hi_f, float lo_f)
{
    char buf[48];
    display_lock(-1);
    if (!valid) {
        lv_label_set_text(s_temp, "--\xC2\xB0" "F");
        lv_label_set_text(s_cond, "Fort Worth");
        lv_label_set_text(s_hilo, "weather unavailable");
    } else {
        snprintf(buf, sizeof(buf), "%d\xC2\xB0" "F", (int)lroundf(temp_f));
        lv_label_set_text(s_temp, buf);
        lv_label_set_text(s_cond, ui_weather_text(code));
        snprintf(buf, sizeof(buf), "H %d\xC2\xB0  L %d\xC2\xB0  |  Fort Worth", (int)lroundf(hi_f), (int)lroundf(lo_f));
        lv_label_set_text(s_hilo, buf);
    }
    display_unlock();
}
