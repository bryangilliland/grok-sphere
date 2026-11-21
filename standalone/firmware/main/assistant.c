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
#include "freertos/task.h"
#include "net.h"
#include "ui.h"

#define TAG "thad"
#define FRAME_SAMPLES   512                          /* 32 ms */
#define REC_MAX_SAMPLES (AUDIO_SAMPLE_RATE * 16)     /* 16 s */
#define RESP_MAX        (48 * 1024)
#define REPLY_MAX       1400
#define SPOKEN_MAX      600
#define HIST_TURNS      6
#define HIST_RESET_MS   (10 * 60 * 1000)
#define ANTHROPIC_URL   "https://api.anthropic.com/v1/messages"
#define DG_LISTEN_URL   "https://api.deepgram.com/v1/listen?model=nova-3&smart_format=true&language=en"

typedef enum { EV_TAP, EV_TEXT } ev_type_t;
typedef struct { ev_type_t type; char *text; } ev_t;

static QueueHandle_t s_q;
static SemaphoreHandle_t s_lock;
static volatile bool s_busy, s_listening, s_stop_rec, s_speaking, s_hush;
static char s_last_q[520], s_last_a[REPLY_MAX];
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

void assistant_tap(void)
{
    if (s_listening) { s_stop_rec = true; return; }
    if (s_speaking) { s_hush = true; audio_stop(); return; }
    if (s_busy) return;
    ev_t e = { .type = EV_TAP };
    xQueueSend(s_q, &e, 0);
}

void assistant_ask_text(const char *q)
{
    if (s_busy) return;
    ev_t e = { .type = EV_TEXT, .text = strdup(q) };
    if (!e.text || xQueueSend(s_q, &e, 0) != pdTRUE) free(e.text);
    else set_last(q, "");
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

static void read_all(esp_http_client_handle_t c, http_result_t *r)
{
    r->body = heap_caps_calloc(1, RESP_MAX, MALLOC_CAP_SPIRAM);
    r->len = 0;
    if (!r->body) return;
    int n;
    while (r->len < RESP_MAX - 1 && (n = esp_http_client_read(c, r->body + r->len, RESP_MAX - 1 - r->len)) > 0) {
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
            read_all(c, r);
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
    ui_set_reply(NULL);
    ui_set_state(UI_STATE_ERROR, "", msg);
    vTaskDelay(pdMS_TO_TICKS(3500));
    ui_set_state(UI_STATE_IDLE, "", "");
}

/* Record until pause / tap / 15 s. Returns samples (0 = nothing heard). */
static size_t record(int16_t *buf)
{
    s_stop_rec = false;
    s_listening = true;
    audio_stop();
    ui_set_reply(NULL);
    ui_set_state(UI_STATE_LISTENING, "", "Listening...");
    audio_play_chime();
    vTaskDelay(pdMS_TO_TICKS(320));              /* let the chime finish so it is not recorded */
    au_vad_t vad;
    au_vad_init(&vad, FRAME_SAMPLES * 1000 / AUDIO_SAMPLE_RATE);
    size_t n = 0;
    bool heard = false;
    int16_t frame[FRAME_SAMPLES];
    while (n + FRAME_SAMPLES <= REC_MAX_SAMPLES) {
        size_t got = audio_mic_read(frame, FRAME_SAMPLES);
        if (got == 0) break;
        memcpy(buf + n, frame, got * sizeof(int16_t));
        n += got;
        vad_result_t v = au_vad_feed(&vad, au_rms(frame, got));
        if (v == VAD_SPEECH) heard = true;
        if (s_stop_rec) { heard = heard || vad.speech_ms > 200; break; }
        if (v == VAD_DONE || v == VAD_MAX_LEN) { heard = true; break; }
        if (v == VAD_NO_SPEECH) break;
    }
    s_listening = false;
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
                   hdr, sizeof(hdr), pcm, (int)(samples * 2), 20000, &r);
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
    char *system = malloc(1200);
    if (!system) return false;
    au_system_prompt(system, 1200, tbuf);

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
                         "anthropic-version", AU_ANTHROPIC_VERSION, req, (int)strlen(req), NULL, 0, 30000, &r);
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
    esp_http_client_handle_t c = open_post(url, "application/json", 20000);
    if (!c) { memset(auth, 0, sizeof(auth)); free(body); return; }
    esp_http_client_set_header(c, "Authorization", auth);
    memset(auth, 0, sizeof(auth));
    int blen = (int)strlen(body);
    bool started = false;
    if (esp_http_client_open(c, blen) == ESP_OK && esp_http_client_write(c, body, blen) == blen &&
        esp_http_client_fetch_headers(c) >= 0) {
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
            while (buf && !s_hush && (n = esp_http_client_read(c, (char *)buf + 1, 4096)) > 0) {
                uint8_t *p = buf + 1;
                if (have_odd) { p = buf; p[0] = odd; n++; have_odd = false; }
                if (n & 1) { odd = p[n - 1]; have_odd = true; n--; }
                if (n) audio_stream_push(p, n);
            }
            free(buf);
            audio_stream_end();
        } else {
            http_result_t r = {0};
            read_all(c, &r);
            char err[96];
            au_parse_error_message(r.body, err, sizeof(err));
            ESP_LOGW(TAG, "TTS HTTP %d: %s", status, err);
            free(r.body);
        }
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    free(body);
    if (!started) { s_speaking = false; return; }
    /* wait for the speaker to finish (or a tap to hush) */
    int64_t until = now_ms() + 60000;
    while (!s_hush && audio_is_playing() && now_ms() < until) vTaskDelay(pdMS_TO_TICKS(50));
    if (s_hush) audio_stop();
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

    if (!net_is_connected()) { fail("No Wi-Fi"); goto done; }
    if (!s.api_key[0]) {
        char msg[96];
        snprintf(msg, sizeof(msg), "Add your Claude key at http://%s", net_ip_str());
        fail(msg);
        goto done;
    }
    bool voice = s.speech_key[0] != 0;

    if (e->type == EV_TAP) {
        if (!voice) {
            char msg[120];
            snprintf(msg, sizeof(msg), "Voice needs a Deepgram key. Or type at http://%s", net_ip_str());
            fail(msg);
            goto done;
        }
        size_t n = record(rec);
        if (n == 0) { ui_set_state(UI_STATE_IDLE, "", ""); goto done; }
        ui_set_state(UI_STATE_THINKING, "", "");
        if (!speech_to_text(s.speech_key, rec, n, question, 520)) goto done;
        if (!question[0]) { fail("Didn't catch that"); goto done; }
        set_last(question, "");
    } else {
        snprintf(question, 520, "%s", e->text);
        set_last(question, "");
    }

    ui_set_state(UI_STATE_THINKING, "", "");
    ui_set_reply(question);                       /* show what was heard while thinking */
    if (!ask_claude(&s, question, reply, REPLY_MAX)) { set_last(NULL, "(error - see the sphere)"); goto done; }
    au_speech_clean(reply, spoken, REPLY_MAX, voice ? SPOKEN_MAX : REPLY_MAX - 1);
    set_last(NULL, spoken);

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
        if (!rec) { fail("Out of memory"); free(e.text); continue; }
        s_busy = true;
        s_hush = false;
        handle(&e, rec);
        free(e.text);
        s_busy = false;
    }
}

void assistant_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_q = xQueueCreate(2, sizeof(ev_t));
    xTaskCreatePinnedToCore(task, "thad", 16384, NULL, 5, NULL, 1);
}
