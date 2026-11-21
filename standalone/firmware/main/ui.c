/*
 * Grok Sphere (Standalone Claude Edition) - LVGL 8.3 screens.
 *
 *   Eyes screen: the original two rounded "eyes" from firmware 1.0 / v2 (the look
 *     taken from @Texas_Twins_Dad's X post that started this project), with subtle
 *     emotional capsule expressions (happy, sad, bored, sleepy, surprised, curious)
 *     and natural blinks / idle glances.
 *       idle = off-white + blink/glance, listening = wide cyan, thinking = squint + dots,
 *       speaking = height pulses with speaker level, error = red + sad brow, offline = dim grey.
 *   Clock screen: time, date, Fort Worth weather (filled in by weather.c / SNTP).
 *   Setup / Notice screen: QR code hotspot setup or full-screen USB flash notice.
 *
 *   All eye animations use plain axis-aligned LVGL objects (zero transform_angle /
 *   LVGL software layers / mask buffers) and dirty-checked style/geometry updates so
 *   boot and idle never allocate extra layer buffers or churn redundant redraws.
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

#define EYE_W            74
#define EYE_H            120
#define EYE_DX           62
#define EYE_CY           160
#define IDLE_BORED_MS    (2 * 60 * 1000)
#define IDLE_SLEEPY_MS   (10 * 60 * 1000)
#define PREVIEW_STEP_MS  1500

static lv_obj_t *s_scr_eyes, *s_scr_clock, *s_scr_setup;
static lv_obj_t *s_eye[2], *s_lid[2], *s_dot[3], *s_status, *s_zzz;
static lv_obj_t *s_reply_box, *s_reply;
static int s_reply_y, s_reply_pause;
static int64_t s_reply_hide_at;
static lv_obj_t *s_time, *s_date, *s_temp, *s_cond, *s_hilo;
static lv_obj_t *s_setup_title, *s_setup_qr, *s_setup_text;

static char s_setup_qr_str[128], s_setup_txt_str[180];
static bool s_notice, s_notice_over_setup;
static bool s_ui_ready, s_mood_ui_ready;
static ui_state_t s_state = UI_STATE_OFFLINE;
static ui_screen_t s_screen = SCREEN_EYES;
static ui_mood_t s_mood = UI_MOOD_NONE;
static bool s_setup_active, s_preview_active;
static int64_t s_next_blink_ms, s_blink_start_ms = -1, s_state_since_ms;
static int64_t s_last_activity_ms, s_mood_until_ms, s_preview_start_ms;
static int64_t s_next_glance_ms, s_glance_end_ms;
static float s_glance_tx, s_glance_ty;
static float s_cur_w[2] = {EYE_W, EYE_W}, s_cur_h[2] = {36, 36};
static float s_cur_cx[2] = {180 - EYE_DX, 180 + EYE_DX}, s_cur_cy[2] = {EYE_CY, EYE_CY};
static float s_speak_h = 70;
static char s_label[32];
static const char *s_last_preview_lbl;

/* Cached LVGL geometry/style state to avoid redundant invalidation/flushes */
static int s_c_ew[2] = {-1, -1}, s_c_eh[2] = {-1, -1}, s_c_ex[2] = {-999, -999}, s_c_ey[2] = {-999, -999};
static uint32_t s_c_ecol[2] = {0xFFFFFFFFu, 0xFFFFFFFFu};
static bool s_c_lid_vis[2];
static int s_c_lid_x[2], s_c_lid_y[2], s_c_lid_w[2], s_c_lid_h[2], s_c_lid_r[2];
static bool s_c_dot_vis[3];
static int s_c_dot_x[3], s_c_dot_y[3];
static bool s_c_zzz_vis;
static int s_c_zzz_y = -999;

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

static lv_color_t state_color(ui_state_t s, ui_mood_t mood)
{
    switch (s) {
    case UI_STATE_LISTENING: return lv_color_hex(0x3DDCFF);
    case UI_STATE_THINKING:  return lv_color_hex(0x9AA7FF);
    case UI_STATE_ERROR:     return lv_color_hex(0xFF3B30);
    case UI_STATE_OFFLINE:   return lv_color_hex(0x4A4F57);
    default:
        if (s_mood_ui_ready) {
            if (mood == UI_MOOD_SAD)    return lv_color_hex(0xA8C8FF);
            if (mood == UI_MOOD_SLEEPY) return lv_color_hex(0x9AA6BA);
            if (mood == UI_MOOD_BORED)  return lv_color_hex(0xCFD8E6);
        }
        return lv_color_hex(0xEAF2FF);
    }
}

static void set_eye_color(int i, lv_color_t col)
{
    if (!s_eye[i]) return;
    uint32_t c32 = (uint32_t)lv_color_to32(col);
    if (s_c_ecol[i] != c32) {
        s_c_ecol[i] = c32;
        lv_obj_set_style_bg_color(s_eye[i], col, 0);
    }
}

static void place_eye_geom(int i, int w, int h, int cx, int cy)
{
    if (!s_eye[i]) return;
    if (w < 8) w = 8;
    if (h < 6) h = 6;
    int x = cx - w / 2;
    int y = cy - h / 2;
    if (s_c_ew[i] != w || s_c_eh[i] != h) {
        lv_obj_set_size(s_eye[i], w, h);
        if (s_c_ew[i] != w) lv_obj_set_style_radius(s_eye[i], w / 2, 0);
        s_c_ew[i] = w;
        s_c_eh[i] = h;
    }
    if (s_c_ex[i] != x || s_c_ey[i] != y) {
        lv_obj_set_pos(s_eye[i], x, y);
        s_c_ex[i] = x;
        s_c_ey[i] = y;
    }
}

static void place_lid(int idx, bool show, int x, int y, int w, int h, int radius)
{
    if (!s_lid[idx]) return;
    if (!show || h <= 0 || w <= 0) {
        if (s_c_lid_vis[idx]) {
            lv_obj_add_flag(s_lid[idx], LV_OBJ_FLAG_HIDDEN);
            s_c_lid_vis[idx] = false;
        }
        return;
    }
    if (!s_c_lid_vis[idx]) {
        lv_obj_clear_flag(s_lid[idx], LV_OBJ_FLAG_HIDDEN);
        s_c_lid_vis[idx] = true;
    }
    if (s_c_lid_w[idx] != w || s_c_lid_h[idx] != h) {
        lv_obj_set_size(s_lid[idx], w, h);
        s_c_lid_w[idx] = w;
        s_c_lid_h[idx] = h;
    }
    if (s_c_lid_r[idx] != radius) {
        lv_obj_set_style_radius(s_lid[idx], radius, 0);
        s_c_lid_r[idx] = radius;
    }
    if (s_c_lid_x[idx] != x || s_c_lid_y[idx] != y) {
        lv_obj_set_pos(s_lid[idx], x, y);
        s_c_lid_x[idx] = x;
        s_c_lid_y[idx] = y;
    }
}

static ui_mood_t effective_mood(int64_t now, const char **preview_label)
{
    if (preview_label) *preview_label = NULL;
    if (!s_mood_ui_ready) return UI_MOOD_NONE;
    if (s_preview_active) {
        static const struct { ui_mood_t m; const char *name; } seq[] = {
            { UI_MOOD_HAPPY,     "Happy" },
            { UI_MOOD_CURIOUS,   "Curious" },
            { UI_MOOD_SURPRISED, "Surprised" },
            { UI_MOOD_SAD,       "Sad" },
            { UI_MOOD_BORED,     "Bored" },
            { UI_MOOD_SLEEPY,    "Sleepy" },
        };
        int idx = (int)((now - s_preview_start_ms) / PREVIEW_STEP_MS);
        if (idx >= 0 && idx < (int)(sizeof(seq) / sizeof(seq[0]))) {
            if (preview_label) *preview_label = seq[idx].name;
            return seq[idx].m;
        }
        s_preview_active = false;
        s_mood = UI_MOOD_NONE;
        s_last_preview_lbl = NULL;
        if (s_status) lv_label_set_text(s_status, "");
    }
    if (s_mood != UI_MOOD_NONE && s_mood_until_ms > 0 && now >= s_mood_until_ms) {
        s_mood = UI_MOOD_NONE;
        s_mood_until_ms = 0;
    }
    if (s_mood != UI_MOOD_NONE) return s_mood;
    if (s_state == UI_STATE_IDLE) {
        int64_t idle_ms = now - s_last_activity_ms;
        if (idle_ms >= IDLE_SLEEPY_MS) return UI_MOOD_SLEEPY;
        if (idle_ms >= IDLE_BORED_MS)  return UI_MOOD_BORED;
    }
    return UI_MOOD_NONE;
}

static void anim_tick(lv_timer_t *t)
{
    (void)t;
    if (!s_ui_ready || lv_scr_act() != s_scr_eyes) return;
    int64_t now = now_ms();
    float sec = (now - s_state_since_ms) / 1000.0f;

    const char *prev_lbl = NULL;
    ui_mood_t mood = effective_mood(now, &prev_lbl);
    if (prev_lbl && prev_lbl != s_last_preview_lbl && s_status) {
        s_last_preview_lbl = prev_lbl;
        lv_label_set_text(s_status, prev_lbl);
    }

    lv_color_t col = state_color(s_state, mood);
    for (int i = 0; i < 2; i++) set_eye_color(i, col);

    float tw[2] = {EYE_W, EYE_W}, th[2] = {EYE_H, EYE_H};
    float tcx[2] = {180.0f - EYE_DX, 180.0f + EYE_DX}, tcy[2] = {EYE_CY, EYE_CY};
    bool dots = false, show_zzz = false;
    int zzz_y = 96;
    typedef enum { LID_NONE = 0, LID_HAPPY, LID_SAD, LID_FLAT } lid_kind_t;
    lid_kind_t lid_kind = LID_NONE;
    int lid_flat_h = 0;

    /* Natural random blink helper (shared across neutral / bored / curious) */
    float blink_scale = 1.0f;
    if ((s_state == UI_STATE_IDLE || s_state == UI_STATE_SPEAKING) &&
        mood != UI_MOOD_HAPPY && mood != UI_MOOD_SLEEPY) {
        if (s_blink_start_ms < 0 && now >= s_next_blink_ms) s_blink_start_ms = now;
        if (s_blink_start_ms >= 0) {
            float p = (now - s_blink_start_ms) / 180.0f;
            if (p >= 1.0f) {
                s_blink_start_ms = -1;
                s_next_blink_ms = now + 2600 + (esp_random() % 3600);
            } else {
                blink_scale = fabsf(1.0f - 2.0f * p);
            }
        }
    }

    /* Small natural idle glances when neutral idle */
    if (s_mood_ui_ready && s_state == UI_STATE_IDLE && mood == UI_MOOD_NONE) {
        if (now >= s_glance_end_ms) {
            s_glance_tx = 0;
            s_glance_ty = 0;
        }
        if (now >= s_next_glance_ms) {
            int r = (int)(esp_random() % 3);
            s_glance_tx = (r == 0) ? -9.0f : (r == 1 ? 9.0f : 0.0f);
            s_glance_ty = ((int)(esp_random() % 3) - 1) * 3.0f;
            s_glance_end_ms = now + 1300;
            s_next_glance_ms = now + 4500 + (esp_random() % 4500);
        }
        tcx[0] += s_glance_tx; tcx[1] += s_glance_tx;
        tcy[0] += s_glance_ty; tcy[1] += s_glance_ty;
    }

    switch (s_state) {
    case UI_STATE_LISTENING:
        if (s_mood_ui_ready && mood == UI_MOOD_SURPRISED) {
            tw[0] = tw[1] = 88;
            th[0] = th[1] = 148 + 4 * sinf(sec * 7.0f);
            tcy[0] = tcy[1] = EYE_CY - 8;
        } else {
            tw[0] = tw[1] = 84;
            th[0] = th[1] = 140 + 6 * sinf(sec * 6.0f);
        }
        break;
    case UI_STATE_THINKING:
        if (s_mood_ui_ready) {
            tw[0] = 78; th[0] = 66; tcy[0] = EYE_CY - 22;
            tw[1] = 68; th[1] = 48; tcy[1] = EYE_CY - 16;
        } else {
            th[0] = th[1] = 56;
            tcy[0] = tcy[1] = EYE_CY - 18;
        }
        dots = true;
        break;
    case UI_STATE_SPEAKING: {
        float target = 64 + audio_level() * 0.9f;
        s_speak_h += (target - s_speak_h) * 0.45f;
        th[0] = th[1] = s_speak_h;
        if (s_mood_ui_ready && mood == UI_MOOD_CURIOUS) {
            tw[0] = 80; th[0] = fmaxf(s_speak_h * 1.12f, 76); tcy[0] = EYE_CY - 6;
            tw[1] = 68; th[1] = fmaxf(s_speak_h * 0.80f, 54); tcy[1] = EYE_CY + 4;
        } else if (s_mood_ui_ready && mood == UI_MOOD_HAPPY) {
            tw[0] = tw[1] = 78;
            th[0] = th[1] = fmaxf(s_speak_h * 0.85f, 78);
            tcy[0] = tcy[1] = EYE_CY - 4;
            lid_kind = LID_HAPPY;
        }
        break;
    }
    case UI_STATE_ERROR:
        if (s_mood_ui_ready) {
            tw[0] = tw[1] = 72;
            th[0] = th[1] = 88;
            tcy[0] = tcy[1] = EYE_CY + 8;
            lid_kind = LID_SAD;
        } else {
            th[0] = th[1] = 90;
        }
        break;
    case UI_STATE_OFFLINE:
        th[0] = th[1] = 36;
        break;
    case UI_STATE_IDLE:
    default:
        if (!s_mood_ui_ready) break;
        switch (mood) {
        case UI_MOOD_HAPPY:
            tw[0] = tw[1] = 78;
            th[0] = th[1] = 86;
            tcy[0] = tcy[1] = EYE_CY - 6 + 3 * sinf(sec * 4.0f);
            lid_kind = LID_HAPPY;
            break;
        case UI_MOOD_SAD:
            tw[0] = tw[1] = 72;
            th[0] = th[1] = 88;
            tcy[0] = tcy[1] = EYE_CY + 8;
            lid_kind = LID_SAD;
            break;
        case UI_MOOD_BORED: {
            float gx = 10.0f * sinf(sec * 0.75f);
            tw[0] = tw[1] = 76;
            th[0] = th[1] = 106;
            tcx[0] += gx; tcx[1] += gx;
            lid_kind = LID_FLAT;
            lid_flat_h = 56;
            break;
        }
        case UI_MOOD_SLEEPY: {
            float cyc = 0.5f + 0.5f * sinf(sec * 1.1f);
            int lh = 56 + (int)(36.0f * cyc);          /* slowly closes down to ~14 px slit */
            tw[0] = tw[1] = 76;
            th[0] = th[1] = 104;
            tcy[0] = tcy[1] = EYE_CY + 6;
            lid_kind = LID_FLAT;
            lid_flat_h = lh;
            if (lh >= 70) {
                show_zzz = true;
                zzz_y = 96 - (int)(8.0f * sinf(sec * 1.6f));
            }
            break;
        }
        case UI_MOOD_SURPRISED:
            tw[0] = tw[1] = 88;
            th[0] = th[1] = 148 + 3 * sinf(sec * 8.0f);
            tcy[0] = tcy[1] = EYE_CY - 10;
            break;
        case UI_MOOD_CURIOUS:
            tw[0] = 82; th[0] = 134; tcy[0] = EYE_CY - 8;
            tw[1] = 68; th[1] = 84;  tcy[1] = EYE_CY + 6;
            break;
        case UI_MOOD_NONE:
        default:
            break;
        }
        break;
    }

    float alpha = (s_blink_start_ms >= 0) ? 0.65f : 0.34f;
    for (int i = 0; i < 2; i++) {
        float h_target = th[i] * blink_scale;
        if (s_blink_start_ms >= 0 && lid_kind != LID_NONE) h_target = th[i];
        s_cur_w[i]  += (tw[i]  - s_cur_w[i])  * alpha;
        s_cur_h[i]  += (h_target - s_cur_h[i]) * alpha;
        s_cur_cx[i] += (tcx[i] - s_cur_cx[i]) * alpha;
        s_cur_cy[i] += (tcy[i] - s_cur_cy[i]) * alpha;
        if (fabsf(tw[i] - s_cur_w[i]) < 0.25f) s_cur_w[i] = tw[i];
        if (fabsf(h_target - s_cur_h[i]) < 0.25f) s_cur_h[i] = h_target;
        if (fabsf(tcx[i] - s_cur_cx[i]) < 0.25f) s_cur_cx[i] = tcx[i];
        if (fabsf(tcy[i] - s_cur_cy[i]) < 0.25f) s_cur_cy[i] = tcy[i];

        int cw = (int)lroundf(s_cur_w[i]);
        int ch = (int)lroundf(s_cur_h[i]);
        int ccx = (int)lroundf(s_cur_cx[i]);
        int ccy = (int)lroundf(s_cur_cy[i]);
        place_eye_geom(i, cw, ch, ccx, ccy);

        if (s_mood_ui_ready) {
            if (lid_kind == LID_HAPPY) {
                int dy = (s_state == UI_STATE_SPEAKING) ? 10 : -2;
                int lw = (s_state == UI_STATE_SPEAKING) ? 94 : 96;
                int lh = (s_state == UI_STATE_SPEAKING) ? 66 : 76;
                place_lid(i, true, ccx - lw / 2, ccy + dy, lw, lh, lw / 2);
            } else if (lid_kind == LID_SAD) {
                /* Single unrotated rounded black mask shifted outward for a smooth sad droop */
                int lx = (i == 0) ? (ccx - 68) : (ccx - 20);
                place_lid(i, true, lx, ccy - 64, 88, 66, 33);
            } else if (lid_kind == LID_FLAT) {
                int yoff = (mood == UI_MOOD_SLEEPY) ? -54 : -58;
                place_lid(i, true, ccx - 48, ccy + yoff, 96, lid_flat_h, 0);
            } else {
                place_lid(i, false, 0, 0, 0, 0, 0);
            }
        }
    }
    for (int i = 0; i < 3; i++) {
        if (!s_dot[i]) continue;
        if (dots) {
            if (!s_c_dot_vis[i]) {
                lv_obj_clear_flag(s_dot[i], LV_OBJ_FLAG_HIDDEN);
                s_c_dot_vis[i] = true;
            }
            int dx = 180 - 8 + (i - 1) * 34;
            int dy = (int)(228 - 14 * fmaxf(0.0f, sinf(sec * 7.0f - i * 0.9f)));
            if (s_c_dot_x[i] != dx || s_c_dot_y[i] != dy) {
                lv_obj_set_pos(s_dot[i], dx, dy);
                s_c_dot_x[i] = dx;
                s_c_dot_y[i] = dy;
            }
        } else if (s_c_dot_vis[i]) {
            lv_obj_add_flag(s_dot[i], LV_OBJ_FLAG_HIDDEN);
            s_c_dot_vis[i] = false;
        }
    }
    if (s_mood_ui_ready && s_zzz) {
        if (show_zzz) {
            if (!s_c_zzz_vis) {
                lv_obj_clear_flag(s_zzz, LV_OBJ_FLAG_HIDDEN);
                s_c_zzz_vis = true;
            }
            if (s_c_zzz_y != zzz_y) {
                lv_obj_set_pos(s_zzz, 250, zzz_y);
                s_c_zzz_y = zzz_y;
            }
        } else if (s_c_zzz_vis) {
            lv_obj_add_flag(s_zzz, LV_OBJ_FLAG_HIDDEN);
            s_c_zzz_vis = false;
        }
    }
}

static void clock_tick(lv_timer_t *t)
{
    (void)t;
    if (!s_ui_ready || !s_time || !s_date) return;
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

#define REPLY_W   232
#define REPLY_H   76
#define REPLY_Y   248

static void reply_tick(lv_timer_t *t)
{
    (void)t;
    if (!s_ui_ready || !s_reply_box || !s_reply) return;
    if (lv_obj_has_flag(s_reply_box, LV_OBJ_FLAG_HIDDEN)) return;
    if (s_reply_hide_at && now_ms() >= s_reply_hide_at) {
        s_reply_hide_at = 0;
        lv_obj_add_flag(s_reply_box, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_status, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_update_layout(s_reply);
    int h = lv_obj_get_height(s_reply);
    if (h <= REPLY_H) { lv_obj_set_y(s_reply, 0); return; }
    if (s_reply_pause > 0) { s_reply_pause--; return; }
    s_reply_y--;
    if (s_reply_y < REPLY_H - h - 8) { s_reply_y = 0; s_reply_pause = 60; }   /* restart after a pause */
    else if (s_reply_y == REPLY_H - h - 8) s_reply_pause = 50;
    lv_obj_set_y(s_reply, s_reply_y);
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
    if (!s_scr_eyes) { display_unlock(); return; }

    /* Step 1: essential plain capsule eyes, dots, status, and reply box */
    for (int i = 0; i < 2; i++) {
        s_eye[i] = lv_obj_create(s_scr_eyes);
        if (!s_eye[i]) { display_unlock(); return; }
        lv_obj_set_style_border_width(s_eye[i], 0, 0);
        lv_obj_set_style_bg_opa(s_eye[i], LV_OPA_COVER, 0);
        lv_obj_clear_flag(s_eye[i], LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        set_eye_color(i, state_color(s_state, UI_MOOD_NONE));
        place_eye_geom(i, EYE_W, 36, 180 + (i == 0 ? -EYE_DX : EYE_DX), EYE_CY);
    }
    for (int i = 0; i < 3; i++) {
        s_dot[i] = lv_obj_create(s_scr_eyes);
        if (s_dot[i]) {
            lv_obj_set_size(s_dot[i], 16, 16);
            lv_obj_set_style_radius(s_dot[i], 8, 0);
            lv_obj_set_style_border_width(s_dot[i], 0, 0);
            lv_obj_set_style_bg_color(s_dot[i], lv_color_hex(0x9AA7FF), 0);
            lv_obj_clear_flag(s_dot[i], LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_flag(s_dot[i], LV_OBJ_FLAG_HIDDEN);
        }
        s_c_dot_vis[i] = false;
    }
    s_status = mk_label(s_scr_eyes, &lv_font_montserrat_20, lv_color_hex(0x8A93A3), 110);
    if (s_status) {
        lv_obj_set_width(s_status, 250);
        lv_label_set_long_mode(s_status, LV_LABEL_LONG_DOT);
        lv_obj_set_height(s_status, 52);
    }

    s_reply_box = lv_obj_create(s_scr_eyes);
    if (s_reply_box) {
        lv_obj_remove_style_all(s_reply_box);
        lv_obj_set_size(s_reply_box, REPLY_W, REPLY_H);
        lv_obj_set_pos(s_reply_box, 180 - REPLY_W / 2, REPLY_Y);
        lv_obj_clear_flag(s_reply_box, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_clip_corner(s_reply_box, false, 0);
        s_reply = lv_label_create(s_reply_box);
        if (s_reply) {
            lv_obj_set_width(s_reply, REPLY_W);
            lv_label_set_long_mode(s_reply, LV_LABEL_LONG_WRAP);
            lv_obj_set_style_text_font(s_reply, &lv_font_montserrat_20, 0);
            lv_obj_set_style_text_color(s_reply, lv_color_hex(0xC8CED8), 0);
            lv_obj_set_style_text_align(s_reply, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_pos(s_reply, 0, 0);
            lv_label_set_text(s_reply, "");
        }
        lv_obj_add_flag(s_reply_box, LV_OBJ_FLAG_HIDDEN);
    }

    /* Core eyes screen is usable now; load it before optional mood extras */
    s_ui_ready = true;
    lv_scr_load(s_scr_eyes);

    /* Step 2: optional mood overlays; if any fail, fall back cleanly to plain eyes */
    bool mood_ok = true;
    for (int i = 0; i < 2; i++) {
        s_lid[i] = lv_obj_create(s_scr_eyes);
        if (!s_lid[i]) { mood_ok = false; continue; }
        lv_obj_set_style_border_width(s_lid[i], 0, 0);
        lv_obj_set_style_bg_opa(s_lid[i], LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(s_lid[i], lv_color_black(), 0);
        lv_obj_clear_flag(s_lid[i], LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(s_lid[i], LV_OBJ_FLAG_HIDDEN);
        s_c_lid_vis[i] = false;
        s_c_lid_x[i] = s_c_lid_y[i] = s_c_lid_w[i] = s_c_lid_h[i] = s_c_lid_r[i] = -1;
    }
    s_zzz = lv_label_create(s_scr_eyes);
    if (!s_zzz) {
        mood_ok = false;
    } else {
        lv_obj_set_style_text_font(s_zzz, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(s_zzz, lv_color_hex(0x8A93A3), 0);
        lv_label_set_text(s_zzz, "z Z");
        lv_obj_add_flag(s_zzz, LV_OBJ_FLAG_HIDDEN);
        s_c_zzz_vis = false;
    }
    /* Keep status text and reply box above lid overlays */
    if (s_status) lv_obj_move_foreground(s_status);
    if (s_reply_box) lv_obj_move_foreground(s_reply_box);
    s_mood_ui_ready = mood_ok;

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

    s_state_since_ms = now_ms();
    s_last_activity_ms = s_state_since_ms;
    s_next_blink_ms = s_state_since_ms + 3000;
    s_next_glance_ms = s_state_since_ms + 5000;
    lv_timer_create(anim_tick, 33, NULL);
    lv_timer_create(reply_tick, 40, NULL);
    lv_timer_create(clock_tick, 1000, NULL);
    display_unlock();
}

static void refresh_status_text(const char *text)
{
    if (!s_status) return;
    char buf[140];
    if (s_label[0] && text && text[0]) snprintf(buf, sizeof(buf), "%s\n%s", s_label, text);
    else if (s_label[0]) snprintf(buf, sizeof(buf), "%s", s_label);
    else snprintf(buf, sizeof(buf), "%s", text ? text : "");
    lv_label_set_text(s_status, buf);
}

void ui_set_state(ui_state_t state, const char *label, const char *text)
{
    if (!s_ui_ready) return;
    display_lock(-1);
    int64_t now = now_ms();
    if (state != s_state) s_state_since_ms = now;
    if (state != UI_STATE_IDLE) {
        s_preview_active = false;
        s_last_preview_lbl = NULL;
        s_last_activity_ms = now;
        if (state == UI_STATE_ERROR) s_mood = UI_MOOD_SAD;
    }
    s_state = state;
    snprintf(s_label, sizeof(s_label), "%s", label ? label : "");
    lv_color_t col = state_color(state, s_mood);
    for (int i = 0; i < 2; i++) set_eye_color(i, col);
    refresh_status_text(text);
    if (state != UI_STATE_IDLE && state != UI_STATE_OFFLINE && !s_setup_active && !s_notice) {
        s_screen = SCREEN_EYES;
        lv_scr_load(s_scr_eyes);
    }
    display_unlock();
}

void ui_set_mood(ui_mood_t mood, int duration_ms)
{
    if (!s_ui_ready) return;
    display_lock(-1);
    int64_t now = now_ms();
    s_preview_active = false;
    s_last_preview_lbl = NULL;
    s_mood = s_mood_ui_ready ? mood : UI_MOOD_NONE;
    s_mood_until_ms = (duration_ms > 0) ? (now + duration_ms) : 0;
    s_last_activity_ms = now;
    lv_color_t col = state_color(s_state, s_mood);
    for (int i = 0; i < 2; i++) set_eye_color(i, col);
    display_unlock();
}

void ui_preview_moods(void)
{
    if (!s_ui_ready || !s_mood_ui_ready) return;
    display_lock(-1);
    int64_t now = now_ms();
    s_state = UI_STATE_IDLE;
    s_mood = UI_MOOD_NONE;
    s_mood_until_ms = 0;
    s_last_activity_ms = now;
    s_preview_start_ms = now;
    s_last_preview_lbl = NULL;
    s_preview_active = true;
    if (s_reply_box) lv_obj_add_flag(s_reply_box, LV_OBJ_FLAG_HIDDEN);
    if (s_status) lv_obj_clear_flag(s_status, LV_OBJ_FLAG_HIDDEN);
    if (!s_setup_active && !s_notice) {
        s_screen = SCREEN_EYES;
        lv_scr_load(s_scr_eyes);
    }
    display_unlock();
}

void ui_wake_activity(void)
{
    if (!s_ui_ready) return;
    display_lock(-1);
    s_last_activity_ms = now_ms();
    s_preview_active = false;
    s_last_preview_lbl = NULL;
    display_unlock();
}

ui_state_t ui_get_state(void) { return s_state; }

void ui_set_reply(const char *text)
{
    if (!s_ui_ready || !s_reply_box || !s_reply) return;
    display_lock(-1);
    s_reply_hide_at = 0;
    if (!text || !text[0]) {
        lv_obj_add_flag(s_reply_box, LV_OBJ_FLAG_HIDDEN);
        if (s_status) lv_obj_clear_flag(s_status, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_label_set_text(s_reply, text);
        s_reply_y = 0;
        s_reply_pause = 40;                        /* ~1.6 s before scrolling */
        lv_obj_set_y(s_reply, 0);
        if (s_status) lv_obj_add_flag(s_status, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_reply_box, LV_OBJ_FLAG_HIDDEN);
    }
    display_unlock();
}

void ui_fade_reply(int after_ms)
{
    if (!s_ui_ready || !s_reply_box) return;
    display_lock(-1);
    if (!lv_obj_has_flag(s_reply_box, LV_OBJ_FLAG_HIDDEN)) s_reply_hide_at = now_ms() + after_ms;
    display_unlock();
}

void ui_show_link(const char *title, const char *url, const char *body)
{
    if (!s_ui_ready) return;
    snprintf(s_setup_qr_str, sizeof(s_setup_qr_str), "%s", url);
    snprintf(s_setup_txt_str, sizeof(s_setup_txt_str), "%s", body);
    display_lock(-1);
    s_setup_active = true;
    if (!s_notice) {
        lv_label_set_text(s_setup_title, title);
        lv_obj_clear_flag(s_setup_qr, LV_OBJ_FLAG_HIDDEN);
        lv_qrcode_update(s_setup_qr, s_setup_qr_str, strlen(s_setup_qr_str));
        lv_label_set_text(s_setup_text, s_setup_txt_str);
        lv_obj_align(s_setup_text, LV_ALIGN_CENTER, 0, 100);
        lv_scr_load(s_scr_setup);
    }
    display_unlock();
}

void ui_set_status(const char *text)
{
    if (!s_ui_ready) return;
    display_lock(-1);
    refresh_status_text(text);
    display_unlock();
}

void ui_show_screen(ui_screen_t screen)
{
    if (!s_ui_ready) return;
    display_lock(-1);
    s_screen = screen;
    if (!s_setup_active && !s_notice) lv_scr_load(screen == SCREEN_CLOCK ? s_scr_clock : s_scr_eyes);
    if (screen == SCREEN_CLOCK) clock_tick(NULL);
    display_unlock();
}

void ui_toggle_screen(void)
{
    ui_wake_activity();
    ui_show_screen(s_screen == SCREEN_EYES ? SCREEN_CLOCK : SCREEN_EYES);
}

void ui_show_setup(const char *ap_ssid, const char *ap_pass, const char *ip)
{
    if (!s_ui_ready) return;
    snprintf(s_setup_qr_str, sizeof(s_setup_qr_str), "WIFI:T:WPA;S:%s;P:%s;;", ap_ssid, ap_pass);
    snprintf(s_setup_txt_str, sizeof(s_setup_txt_str),
             "Scan with your phone camera, or join\n%s  /  %s\nthen open  http://%s", ap_ssid, ap_pass, ip);
    display_lock(-1);
    s_setup_active = true;
    if (!s_notice) {
        lv_label_set_text(s_setup_title, "Sphere setup");
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
    if (!s_ui_ready) return;
    display_lock(-1);
    s_setup_active = false;
    if (!s_notice) lv_scr_load(s_screen == SCREEN_CLOCK ? s_scr_clock : s_scr_eyes);
    display_unlock();
}

void ui_show_notice(const char *title, const char *body)
{
    if (!s_ui_ready) return;
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
    if (!s_ui_ready) return;
    display_lock(-1);
    bool back_to_setup = s_notice && s_notice_over_setup;
    s_notice = false;
    if (back_to_setup) {
        lv_label_set_text(s_setup_title, "Sphere setup");
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
    if (!s_ui_ready || !s_temp || !s_cond || !s_hilo) return;
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
