/*
 * Grok Sphere standalone - the whole voice assistant runs on the ESP32-S3:
 *
 *   tap -> record mic (16 kHz, PSRAM, auto-stop on pause)
 *       -> Deepgram /v1/listen (speech-to-text, needs the optional Deepgram key)
 *       -> Anthropic /v1/messages (Claude as "Thad")
 *       -> reply scrolls on screen + Deepgram /v1/speak (text-to-speech, raw 16 kHz PCM)
 *
 * Claude's API is text-in/text-out only (no audio), so speech needs the Deepgram key.
 * Without it, questions typed on the phone page still work and answers show on screen.
 * Keys are read from settings at the start of each turn and never logged.
 */
#include "assistant.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "app.h"
#include "assist_util.h"
#include "audio.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "model_path.h"
#include "net.h"
#include "ui.h"

extern const uint8_t srmodels_bin_start[] asm("_binary_srmodels_bin_start");

#define TAG "thad"
#define FRAME_SAMPLES   512                          /* 32 ms */
#define REC_MAX_SAMPLES (AUDIO_SAMPLE_RATE * 9)      /* 9 s */
#define RESP_MAX        (48 * 1024)
#define REPLY_MAX       1400
#define SPOKEN_MAX      600
#define HIST_TURNS      6
#define HIST_RESET_MS   (10 * 60 * 1000)
#define ANTHROPIC_URL   "https://api.anthropic.com/v1/messages"
#define DG_LISTEN_URL   "https://api.deepgram.com/v1/listen?model=nova-3&smart_format=true&language=en"

typedef enum { EV_TAP, EV_WAKE, EV_TEXT, EV_PREVIEW } ev_type_t;
typedef struct { ev_type_t type; char *text; } ev_t;

#define BIT_FETCH_GO     BIT0
#define BIT_FETCH_PARKED BIT1

static QueueHandle_t s_q;
static SemaphoreHandle_t s_lock, s_mic_lock;
static EventGroupHandle_t s_wake_ev;
static volatile bool s_busy, s_listening, s_stop_rec, s_speaking, s_hush;
static volatile bool s_wake_paused, s_wake_reconfig, s_afe_active;
static srmodel_list_t *s_models;
static const esp_afe_sr_iface_t *s_afe;
static esp_afe_sr_data_t *s_afe_data;
static char s_active_word[28];
static char s_last_q[520], s_last_a[REPLY_MAX], s_last_err[128];
static uint32_t s_last_mic_peak, s_last_mic_rms;
static char *s_hist_text[HIST_TURNS];
static const char *s_hist_role[HIST_TURNS];
static int s_hist_n;
static int64_t s_last_turn_ms;

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

bool assistant_busy(void) { return s_busy; }

void assistant_last(char *q, size_t qcap, char *a, size_t acap)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    snprintf(q, qcap, "%s", s_last_q);
    snprintf(a, acap, "%s", s_last_a);
    xSemaphoreGive(s_lock);
}

static void set_last(const char *q, const char *a)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (q) snprintf(s_last_q, sizeof(s_last_q), "%s", q);
    if (a) snprintf(s_last_a, sizeof(s_last_a), "%s", a);
    xSemaphoreGive(s_lock);
}

static void set_err(const char *err)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    snprintf(s_last_err, sizeof(s_last_err), "%s", err ? err : "");
    xSemaphoreGive(s_lock);
}

static void set_mic_diag(uint32_t peak, uint32_t rms)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_last_mic_peak = peak;
    s_last_mic_rms = rms;
    xSemaphoreGive(s_lock);
}

void assistant_diag(assistant_diag_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    xSemaphoreTake(s_lock, portMAX_DELAY);
    snprintf(out->last_err, sizeof(out->last_err), "%s", s_last_err[0] ? s_last_err : "None");
    snprintf(out->last_q, sizeof(out->last_q), "%s", s_last_q);
    snprintf(out->last_a, sizeof(out->last_a), "%.255s", s_last_a);
    out->mic_peak = s_last_mic_peak;
    out->mic_rms = s_last_mic_rms;
    if (s_afe_active && s_active_word[0]) {
        snprintf(out->wake_active, sizeof(out->wake_active), "on (%s)", au_wake_label(s_active_word));
    } else {
        snprintf(out->wake_active, sizeof(out->wake_active), "off");
    }
    xSemaphoreGive(s_lock);
}

static void hist_clear(void)
{
    for (int i = 0; i < s_hist_n; i++) free(s_hist_text[i]);
    s_hist_n = 0;
}

static void hist_add(const char *role, const char *text)
{
    if (s_hist_n == HIST_TURNS) {
        free(s_hist_text[0]);
        memmove(s_hist_text, s_hist_text + 1, sizeof(s_hist_text[0]) * (HIST_TURNS - 1));
        memmove(s_hist_role, s_hist_role + 1, sizeof(s_hist_role[0]) * (HIST_TURNS - 1));
        s_hist_n--;
    }
    s_hist_text[s_hist_n] = strdup(text);
    s_hist_role[s_hist_n] = role;
    if (s_hist_text[s_hist_n]) s_hist_n++;
}

void assistant_wake_reconfig(void) { s_wake_reconfig = true; }

void assistant_tap(void)
{
    if (s_listening) { s_stop_rec = true; return; }
    if (s_speaking) { s_hush = true; audio_stop(); return; }
    if (s_busy) return;
    ui_wake_activity();
    s_wake_paused = true;
    ev_t e = { .type = EV_TAP };
    if (xQueueSend(s_q, &e, 0) != pdTRUE) s_wake_paused = false;
}

void assistant_ask_text(const char *q)
{
    if (s_busy) return;
    s_wake_paused = true;
    ev_t e = { .type = EV_TEXT, .text = strdup(q) };
    if (!e.text || xQueueSend(s_q, &e, 0) != pdTRUE) { free(e.text); s_wake_paused = false; }
    else set_last(q, "");
}

void assistant_preview(const char *voice)
{
    if (s_busy) return;
    s_wake_paused = true;
    ev_t e = { .type = EV_PREVIEW, .text = strdup(voice ? voice : "") };
    if (!e.text || xQueueSend(s_q, &e, 0) != pdTRUE) { free(e.text); s_wake_paused = false; }
}

/* ---------------------------------------------------------------- HTTP helper */
typedef struct {
    int status;
    char *body;      /* PSRAM, NUL-terminated */
    int len;
} http_result_t;

static esp_http_client_handle_t open_post(const char *url, const char *ctype, int timeout_ms)
{
    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = timeout_ms,
        .buffer_size = 4096,
        .buffer_size_tx = 2048,
        .keep_alive_enable = false,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (c) esp_http_client_set_header(c, "Content-Type", ctype);
    return c;
}

static void read_all(esp_http_client_handle_t c, http_result_t *r, int max_ms)
{
    r->body = heap_caps_calloc(1, RESP_MAX, MALLOC_CAP_SPIRAM);
    r->len = 0;
    if (!r->body) return;
    int64_t deadline = now_ms() + max_ms;
    int n;
    while (r->len < RESP_MAX - 1 && now_ms() < deadline &&
           (n = esp_http_client_read(c, r->body + r->len, RESP_MAX - 1 - r->len)) > 0) {
        r->len += n;
    }
    r->body[r->len] = 0;
}

/* POST a body that is in memory (one or two parts) and read the whole response. */
static bool post(const char *url, const char *ctype, const char *auth_name, const char *auth_value,
                 const char *extra_name, const char *extra_value,
                 const void *p1, int l1, const void *p2, int l2, int timeout_ms, http_result_t *r)
{
    memset(r, 0, sizeof(*r));
    esp_http_client_handle_t c = open_post(url, ctype, timeout_ms);
    if (!c) return false;
    esp_http_client_set_header(c, auth_name, auth_value);
    if (extra_name) esp_http_client_set_header(c, extra_name, extra_value);
    bool ok = false;
    if (esp_http_client_open(c, l1 + l2) == ESP_OK) {
        bool wrote = true;
        const char *parts[2] = { p1, p2 };
        int lens[2] = { l1, l2 };
        for (int i = 0; i < 2 && wrote; i++) {
            int off = 0;
            while (off < lens[i]) {
                int w = esp_http_client_write(c, parts[i] + off, lens[i] - off);
                if (w <= 0) { wrote = false; break; }
                off += w;
            }
        }
        if (wrote && esp_http_client_fetch_headers(c) >= 0) {
            r->status = esp_http_client_get_status_code(c);
            read_all(c, r, timeout_ms + 4000);
            ok = true;
        }
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return ok;
}

/* ---------------------------------------------------------------- stages */
static void fail(const char *msg)
{
    ESP_LOGW(TAG, "%s", msg);
    set_err(msg);
    ui_set_reply(NULL);
    ui_set_mood(UI_MOOD_SAD, 3300);
    ui_set_state(UI_STATE_ERROR, "", msg);
    vTaskDelay(pdMS_TO_TICKS(3200));
    ui_set_mood(UI_MOOD_NONE, 0);
    ui_set_state(UI_STATE_IDLE, "", "");
}

/* Record until pause / tap / 8 s hard cap. Returns samples (0 = nothing heard or mic error). */
static size_t record(int16_t *buf, bool *mic_error, bool play_chime)
{
    *mic_error = false;
    s_stop_rec = false;
    s_listening = true;
    if (audio_is_playing()) { audio_stop(); vTaskDelay(pdMS_TO_TICKS(60)); }
    ui_set_reply(NULL);
    ui_set_state(UI_STATE_LISTENING, "", "Listening...");
    int16_t frame[FRAME_SAMPLES];
    if (play_chime) {
        audio_play_chime();
        vTaskDelay(pdMS_TO_TICKS(420));          /* let the 250 ms chime + DMA tail finish */
        for (int i = 0; i < 2; i++) audio_mic_read(frame, FRAME_SAMPLES);   /* drain chime echo */
    } else {
        audio_mic_read(frame, FRAME_SAMPLES);    /* drain 1 stale frame (32 ms) */
    }

    au_vad_t vad;
    au_vad_init(&vad, FRAME_SAMPLES * 1000 / AUDIO_SAMPLE_RATE);
    const size_t min_samples = (AUDIO_SAMPLE_RATE * 6) / 10;            /* 0.6 s minimum */
    size_t n = 0;
    uint32_t max_peak = 0, max_rms = 0;
    int read_failures = 0, zero_frames = 0;
    bool heard = false;
    int64_t rec_start = now_ms();

    while (n + FRAME_SAMPLES <= REC_MAX_SAMPLES) {
        if (now_ms() - rec_start >= 8400) { heard = n >= min_samples; break; }
        size_t got = audio_mic_read(frame, FRAME_SAMPLES);
        if (got == 0) {
            if (++read_failures >= 3) { *mic_error = true; break; }
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        read_failures = 0;
        memcpy(buf + n, frame, got * sizeof(int16_t));
        n += got;

        uint16_t pk = 0;
        uint32_t rms = au_rms_ac(frame, got, &pk);
        if (pk > max_peak) max_peak = pk;
        if (rms > max_rms) max_rms = rms;
        if (pk == 0) {
            if (++zero_frames >= 40 && max_peak == 0) { *mic_error = true; break; }
        } else {
            zero_frames = 0;
        }

        vad_result_t v = au_vad_feed(&vad, rms);
        if (v == AU_VAD_SPEECH) heard = true;
        if (s_stop_rec && n >= min_samples) {
            /* Tap while listening: stop recording and send now (always proceed to STT) */
            heard = true;
            break;
        }
        if (v == AU_VAD_DONE || v == AU_VAD_MAX_LEN) {
            heard = true;
            break;
        }
        if (v == AU_VAD_NO_SPEECH) break;
    }
    s_listening = false;
    set_mic_diag(max_peak, max_rms);
    ESP_LOGI(TAG, "rec %u ms: peak=%u rms=%u heard=%d stop=%d err=%d",
             (unsigned)(n * 1000 / AUDIO_SAMPLE_RATE), (unsigned)max_peak,
             (unsigned)max_rms, heard, (int)s_stop_rec, *mic_error);
    if (max_peak == 0) *mic_error = true;
    if (*mic_error) return 0;
    return heard ? n : 0;
}

static bool speech_to_text(const char *key, const int16_t *pcm, size_t samples, char *out, size_t cap)
{
    uint8_t hdr[44];
    au_wav_header(hdr, samples, AUDIO_SAMPLE_RATE);
    char auth[SETTINGS_SPEECH_MAX + 8];
    snprintf(auth, sizeof(auth), "Token %s", key);
    http_result_t r;
    bool ok = post(DG_LISTEN_URL, "audio/wav", "Authorization", auth, NULL, NULL,
                   hdr, sizeof(hdr), pcm, (int)(samples * 2), 15000, &r);
    memset(auth, 0, sizeof(auth));
    if (!ok) { free(r.body); fail("Can't reach Deepgram"); return false; }
    if (r.status != 200) {
        char msg[120], err[96];
        au_parse_error_message(r.body, err, sizeof(err));
        snprintf(msg, sizeof(msg), r.status == 401 || r.status == 403 ? "Deepgram key rejected" : "Speech error %d %s",
                 r.status, err);
        free(r.body);
        fail(msg);
        return false;
    }
    bool parsed = au_parse_deepgram(r.body, out, cap);
    free(r.body);
    if (!parsed) { fail("Speech reply unreadable"); return false; }
    return true;
}

static bool ask_claude(const sphere_settings_t *s, const char *question, char *reply, size_t cap)
{
    if (now_ms() - s_last_turn_ms > HIST_RESET_MS) hist_clear();
    char tbuf[64] = "";
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    if (tm.tm_year >= (2024 - 1900)) strftime(tbuf, sizeof(tbuf), "%A, %B %d, %Y, %I:%M %p", &tm);
    char *system = malloc(1400);
    if (!system) return false;
    au_system_prompt(system, 1400, tbuf);

    au_turn_t hist[HIST_TURNS];
    for (int i = 0; i < s_hist_n; i++) { hist[i].role = s_hist_role[i]; hist[i].text = s_hist_text[i]; }
    const char *model = au_model_id(s->model);
    int mode = au_thinking_mode_for(model);
    bool ok = false;
    for (int attempt = 0; attempt < 2 && !ok; attempt++) {
        char *req = au_claude_request(model, 600, system, hist, s_hist_n, question, mode);
        if (!req) break;
        http_result_t r;
        bool sent = post(ANTHROPIC_URL, "application/json", "x-api-key", s->api_key,
                         "anthropic-version", AU_ANTHROPIC_VERSION, req, (int)strlen(req), NULL, 0, 20000, &r);
        free(req);
        if (!sent) { free(r.body); fail("Can't reach Claude"); break; }
        char err[160];
        if (r.status == 200 && au_parse_claude(r.body, reply, cap, err, sizeof(err))) {
            ok = true;
        } else if (r.status == 400 && mode != 0 && attempt == 0) {
            mode = 0;   /* model refused the thinking/effort settings: retry plain */
            ESP_LOGW(TAG, "retrying without thinking settings");
        } else {
            au_parse_claude(r.body, reply, cap, err, sizeof(err));
            char msg[200];
            if (r.status == 401) snprintf(msg, sizeof(msg), "Claude key rejected");
            else if (r.status == 429) snprintf(msg, sizeof(msg), "Claude is busy or out of credit");
            else snprintf(msg, sizeof(msg), "Claude error %d: %.120s", r.status, err);
            reply[0] = 0;
            free(r.body);
            fail(msg);
            break;
        }
        free(r.body);
    }
    free(system);
    if (ok) {
        hist_add("user", question);
        hist_add("assistant", reply);
        s_last_turn_ms = now_ms();
    }
    return ok;
}

/* Stream Deepgram TTS (raw 16 kHz s16le) straight into the speaker buffer. */
static void speak(const sphere_settings_t *s, const char *text)
{
    char url[200];
    snprintf(url, sizeof(url),
             "https://api.deepgram.com/v1/speak?model=%s&encoding=linear16&sample_rate=16000&container=none", s->voice);
    char *body = au_tts_body(text);
    if (!body) return;
    char auth[SETTINGS_SPEECH_MAX + 8];
    snprintf(auth, sizeof(auth), "Token %s", s->speech_key);
    esp_http_client_handle_t c = open_post(url, "application/json", 15000);
    if (!c) { memset(auth, 0, sizeof(auth)); free(body); set_err("TTS init failed"); return; }
    esp_http_client_set_header(c, "Authorization", auth);
    memset(auth, 0, sizeof(auth));
    int blen = (int)strlen(body);
    bool started = false;
    bool conn_ok = false;
    if (esp_http_client_open(c, blen) == ESP_OK && esp_http_client_write(c, body, blen) == blen &&
        esp_http_client_fetch_headers(c) >= 0) {
        conn_ok = true;
        int status = esp_http_client_get_status_code(c);
        if (status == 200) {
            s_speaking = true;
            s_hush = false;
            audio_stream_begin();
            started = true;
            uint8_t *buf = malloc(4097);
            bool have_odd = false;
            uint8_t odd = 0;
            int n;
            int64_t dl = now_ms() + 18000;
            while (buf && !s_hush && now_ms() < dl && (n = esp_http_client_read(c, (char *)buf + 1, 4096)) > 0) {
                uint8_t *p = buf + 1;
                if (have_odd) { p = buf; p[0] = odd; n++; have_odd = false; }
                if (n & 1) { odd = p[n - 1]; have_odd = true; n--; }
                if (n) audio_stream_push(p, n);
            }
            free(buf);
            audio_stream_end();
        } else {
            http_result_t r = {0};
            read_all(c, &r, 8000);
            char err[96], msg[128];
            au_parse_error_message(r.body, err, sizeof(err));
            snprintf(msg, sizeof(msg), (status == 401 || status == 403) ? "Deepgram key rejected" : "TTS error %d", status);
            ESP_LOGW(TAG, "TTS HTTP %d: %s", status, err);
            set_err(msg);
            free(r.body);
        }
    }
    if (!conn_ok) set_err("Can't reach Deepgram TTS");
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    free(body);
    if (!started) { s_speaking = false; return; }
    /* wait for the speaker to finish (or a tap to hush), bounded to 25 s */
    int64_t until = now_ms() + 25000;
    while (!s_hush && audio_is_playing() && now_ms() < until) vTaskDelay(pdMS_TO_TICKS(50));
    if (audio_is_playing()) audio_stop();
    s_speaking = false;
}

static void handle(const ev_t *e, int16_t *rec)
{
    sphere_settings_t s;
    app_settings_get(&s);
    char *question = calloc(1, 520);
    char *reply = calloc(1, REPLY_MAX);
    char *spoken = calloc(1, REPLY_MAX);
    if (!question || !reply || !spoken) goto done;

    if (!net_is_connected()) { fail("No internet"); goto done; }

    if (e->type == EV_PREVIEW) {
        if (!s.speech_key[0]) { fail("Save a Deepgram key first, then preview."); goto done; }
        if (e->text && au_voice_valid(e->text)) strcpy(s.voice, e->text);
        ui_set_state(UI_STATE_SPEAKING, "", "");
        ui_set_reply(AU_PREVIEW_TEXT);
        speak(&s, AU_PREVIEW_TEXT);
        ui_set_state(UI_STATE_IDLE, "", "");
        ui_fade_reply(6000);
        goto done;
    }

    if (!s.api_key[0]) {
        char msg[96];
        snprintf(msg, sizeof(msg), "Add your Claude key at http://%s", net_ip_str());
        fail(msg);
        goto done;
    }
    bool voice = s.speech_key[0] != 0;

    ui_wake_activity();
    if (e->type == EV_TAP || e->type == EV_WAKE) {
        if (!voice) {
            char msg[120];
            snprintf(msg, sizeof(msg), "Voice needs a Deepgram key. Or type at http://%s", net_ip_str());
            fail(msg);
            goto done;
        }
        ui_set_mood(e->type == EV_WAKE ? UI_MOOD_SURPRISED : UI_MOOD_NONE, e->type == EV_WAKE ? 700 : 0);
        if (xSemaphoreTake(s_mic_lock, pdMS_TO_TICKS(1000)) != pdTRUE) { fail("Mic busy"); goto done; }
        bool mic_err = false;
        size_t n = record(rec, &mic_err, s.chime_enabled);
        xSemaphoreGive(s_mic_lock);
        if (mic_err) { fail("Mic: no audio"); goto done; }
        if (n == 0) { fail("Didn't catch that"); goto done; }
        ui_set_mood(UI_MOOD_CURIOUS, 0);
        ui_set_state(UI_STATE_THINKING, "", "");
        if (!speech_to_text(s.speech_key, rec, n, question, 520)) goto done;
        if (!question[0]) { fail("Didn't catch that"); goto done; }
        set_last(question, "");
    } else {
        snprintf(question, 520, "%s", e->text);
        set_last(question, "");
    }

    ui_set_mood(UI_MOOD_CURIOUS, 0);
    ui_set_state(UI_STATE_THINKING, "", "");
    ui_set_reply(question);                       /* show what was heard while thinking */
    if (!ask_claude(&s, question, reply, REPLY_MAX)) { set_last(NULL, "(error - see the sphere)"); goto done; }
    au_mood_t am = au_extract_mood(reply);
    ui_mood_t reply_mood = UI_MOOD_HAPPY;
    switch (am) {
    case AU_MOOD_SAD:       reply_mood = UI_MOOD_SAD; break;
    case AU_MOOD_BORED:     reply_mood = UI_MOOD_BORED; break;
    case AU_MOOD_SLEEPY:    reply_mood = UI_MOOD_SLEEPY; break;
    case AU_MOOD_SURPRISED: reply_mood = UI_MOOD_SURPRISED; break;
    case AU_MOOD_CURIOUS:   reply_mood = UI_MOOD_CURIOUS; break;
    case AU_MOOD_HAPPY:
    case AU_MOOD_NONE:
    default:                reply_mood = UI_MOOD_HAPPY; break;
    }
    au_speech_clean(reply, spoken, REPLY_MAX, voice ? SPOKEN_MAX : REPLY_MAX - 1);
    set_last(NULL, spoken);

    ui_set_mood(am == AU_MOOD_NONE ? UI_MOOD_NONE : reply_mood, 0);
    ui_set_state(UI_STATE_SPEAKING, "", "");
    ui_set_reply(spoken);
    if (voice) {
        speak(&s, spoken);
    } else {
        /* no voice: leave the text up long enough to read (about 15 chars/s) */
        int ms = 3000 + (int)strlen(spoken) * 65;
        s_speaking = true;
        for (int t = 0; t < ms && !s_hush; t += 100) vTaskDelay(pdMS_TO_TICKS(100));
        s_speaking = false;
    }
    set_err("");
    ui_set_mood(reply_mood, 3500);
    ui_set_state(UI_STATE_IDLE, "", "");
    /* keep the reply text under the eyes for a while; ui clears it on next turn */
    ui_fade_reply(20000);

done:
    memset(&s, 0, sizeof(s));
    free(question);
    free(reply);
    free(spoken);
}

static void task(void *arg)
{
    (void)arg;
    int16_t *rec = heap_caps_malloc(REC_MAX_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    for (;;) {
        ev_t e;
        if (xQueueReceive(s_q, &e, portMAX_DELAY) != pdTRUE) continue;
        if (!rec) { fail("Out of memory"); free(e.text); s_wake_paused = false; continue; }
        s_busy = true;
        s_wake_paused = true;
        s_hush = false;
        handle(&e, rec);
        free(e.text);
        /* short pause so speaker tail fades before WakeNet resumes */
        vTaskDelay(pdMS_TO_TICKS(200));
        s_busy = false;
        s_wake_paused = false;
    }
}

static bool wake_should_run(char *word_out, size_t cap)
{
    if (!s_models || net_portal_running() || !net_is_connected()) return false;
    sphere_settings_t s;
    app_settings_get(&s);
    bool on = s.wake_enabled && s.api_key[0] != 0;
    if (on) snprintf(word_out, cap, "%s", au_wake_valid(s.wake_word) ? s.wake_word : AU_DEFAULT_WAKE);
    memset(&s, 0, sizeof(s));
    return on;
}

static void wake_fetch_task(void *arg)
{
    (void)arg;
    for (;;) {
        xEventGroupWaitBits(s_wake_ev, BIT_FETCH_GO, pdFALSE, pdTRUE, portMAX_DELAY);
        while (s_afe_active && s_afe_data) {
            afe_fetch_result_t *res = s_afe->fetch_with_delay(s_afe_data, pdMS_TO_TICKS(100));
            if (!s_afe_active) break;
            if (res && res->ret_value != ESP_FAIL && res->wakeup_state == WAKENET_DETECTED) {
                if (!s_wake_paused && !s_busy && !net_portal_running()) {
                    ESP_LOGI(TAG, "wake word '%s' detected", s_active_word);
                    s_wake_paused = true;
                    ev_t e = { .type = EV_WAKE };
                    if (xQueueSend(s_q, &e, 0) != pdTRUE) s_wake_paused = false;
                }
            }
        }
        xEventGroupClearBits(s_wake_ev, BIT_FETCH_GO);
        xEventGroupSetBits(s_wake_ev, BIT_FETCH_PARKED);
    }
}

static void wake_feed_task(void *arg)
{
    (void)arg;
    int16_t *buf = NULL;
    int buf_samples = 0;
    bool was_paused = false;
    int64_t next_check_ms = 0;

    for (;;) {
        int64_t now = now_ms();
        if (s_wake_reconfig || now >= next_check_ms) {
            s_wake_reconfig = false;
            next_check_ms = now + 1000;
            char want_word[28] = "";
            bool want = wake_should_run(want_word, sizeof(want_word));
            if (s_afe_data && (!want || strcmp(want_word, s_active_word) != 0)) {
                s_afe_active = false;
                xEventGroupWaitBits(s_wake_ev, BIT_FETCH_PARKED, pdFALSE, pdTRUE, pdMS_TO_TICKS(500));
                s_afe->destroy(s_afe_data);
                s_afe_data = NULL;
                s_afe = NULL;
                s_active_word[0] = 0;
                ESP_LOGI(TAG, "wake word stopped");
            }
            if (want && !s_afe_data && !s_wake_paused) {
                afe_config_t *cfg = afe_config_init("M", s_models, AFE_TYPE_SR, AFE_MODE_LOW_COST);
                if (cfg) {
                    cfg->aec_init = false;
                    cfg->se_init = false;
                    cfg->ns_init = true;
                    cfg->afe_ns_mode = AFE_NS_MODE_WEBRTC;
                    cfg->vad_init = false;
                    cfg->agc_init = true;
                    cfg->agc_mode = AFE_AGC_MODE_WAKENET;
                    cfg->wakenet_init = true;
                    cfg->wakenet_model_name = want_word;
                    cfg->wakenet_model_name_2 = NULL;
                    cfg->wakenet_mode = DET_MODE_90;
                    cfg->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;
                    cfg->afe_perferred_core = 1;
                    cfg->afe_perferred_priority = 4;
                    s_afe = esp_afe_handle_from_config(cfg);
                    s_afe_data = s_afe ? s_afe->create_from_config(cfg) : NULL;
                    afe_config_free(cfg);
                }
                if (s_afe_data) {
                    int chunk = s_afe->get_feed_chunksize(s_afe_data);
                    if (chunk > buf_samples) {
                        free(buf);
                        buf = malloc((size_t)chunk * sizeof(int16_t));
                        buf_samples = buf ? chunk : 0;
                    }
                    if (buf) {
                        strcpy(s_active_word, want_word);
                        xEventGroupClearBits(s_wake_ev, BIT_FETCH_PARKED);
                        s_afe_active = true;
                        xEventGroupSetBits(s_wake_ev, BIT_FETCH_GO);
                        was_paused = true;   /* drain residual DMA on first frame */
                        ESP_LOGI(TAG, "wake word '%s' active (chunk=%d)", s_active_word, chunk);
                    } else {
                        s_afe->destroy(s_afe_data);
                        s_afe_data = NULL;
                    }
                } else {
                    ESP_LOGE(TAG, "failed to start WakeNet '%s'", want_word);
                }
            }
        }

        if (!s_afe_active || !s_afe_data || s_wake_paused) {
            if (s_wake_paused) was_paused = true;
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        if (was_paused) {
            xSemaphoreTake(s_mic_lock, portMAX_DELAY);
            for (int i = 0; i < 4; i++) audio_mic_read(buf, (size_t)buf_samples);
            xSemaphoreGive(s_mic_lock);
            s_afe->reset_buffer(s_afe_data);
            was_paused = false;
        }

        xSemaphoreTake(s_mic_lock, portMAX_DELAY);
        size_t got = audio_mic_read(buf, (size_t)buf_samples);
        xSemaphoreGive(s_mic_lock);
        if (got == (size_t)buf_samples) {
            s_afe->feed(s_afe_data, buf);
        } else {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
}

void assistant_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_mic_lock = xSemaphoreCreateMutex();
    s_wake_ev = xEventGroupCreate();
    xEventGroupSetBits(s_wake_ev, BIT_FETCH_PARKED);
    s_q = xQueueCreate(2, sizeof(ev_t));
    s_models = srmodel_load(srmodels_bin_start);
    if (!s_models || s_models->num <= 0) ESP_LOGE(TAG, "WakeNet models failed to load");
    else ESP_LOGI(TAG, "loaded %d WakeNet models", s_models->num);
    xTaskCreatePinnedToCore(task, "thad", 16384, NULL, 5, NULL, 1);
    xTaskCreatePinnedToCore(wake_fetch_task, "wake_fetch", 4096, NULL, 4, NULL, 1);
    xTaskCreatePinnedToCore(wake_feed_task, "wake_feed", 6144, NULL, 3, NULL, 1);
}
