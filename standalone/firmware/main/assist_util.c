/* Grok Sphere standalone - pure-C pipeline helpers (unit-tested in host_test/). */
#include "assist_util.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "cJSON.h"

const au_voice_t AU_VOICES[] = {
    { "aura-2-orion-en",  "Orion (male, calm)" },
    { "aura-2-apollo-en", "Apollo (male, casual)" },
    { "aura-2-arcas-en",  "Arcas (male, smooth)" },
    { "aura-2-zeus-en",   "Zeus (male, deep)" },
    { "aura-2-thalia-en", "Thalia (female, bright)" },
    { "aura-2-helena-en", "Helena (female, warm)" },
};
const int AU_VOICE_COUNT = sizeof(AU_VOICES) / sizeof(AU_VOICES[0]);

const char *au_model_id(int model) { return model == 1 ? AU_MODEL_SONNET : AU_MODEL_HAIKU; }

bool au_voice_valid(const char *id)
{
    if (!id) return false;
    for (int i = 0; i < AU_VOICE_COUNT; i++) if (strcmp(id, AU_VOICES[i].id) == 0) return true;
    return false;
}

void au_mask_key(const char *key, char *out, size_t cap)
{
    if (!out || cap == 0) return;
    out[0] = 0;
    if (!key || !key[0]) return;
    size_t n = strlen(key);
    const char *tail = n > 4 ? key + n - 4 : "";
    snprintf(out, cap, "****%s", tail);
}

void au_trim(char *s)
{
    if (!s) return;
    char *p = s;
    while (*p && isspace((unsigned char)*p)) p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n - 1])) s[--n] = 0;
}

static void put32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static void put16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }

void au_wav_header(uint8_t out[44], uint32_t samples, uint32_t rate)
{
    uint32_t data = samples * 2;
    memcpy(out, "RIFF", 4);  put32(out + 4, 36 + data);
    memcpy(out + 8, "WAVEfmt ", 8);
    put32(out + 16, 16); put16(out + 20, 1); put16(out + 22, 1);
    put32(out + 24, rate); put32(out + 28, rate * 2); put16(out + 32, 2); put16(out + 34, 16);
    memcpy(out + 36, "data", 4); put32(out + 40, data);
}

int au_thinking_mode_for(const char *model)
{
    return (model && strstr(model, "sonnet")) ? 2 : 1;
}

char *au_claude_request(const char *model, int max_tokens, const char *system,
                        const au_turn_t *history, int n_history, const char *user, int thinking_mode)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;
    cJSON_AddStringToObject(root, "model", model);
    cJSON_AddNumberToObject(root, "max_tokens", max_tokens);
    if (system && system[0]) cJSON_AddStringToObject(root, "system", system);
    if (thinking_mode == 1 || thinking_mode == 2) {
        cJSON *th = cJSON_AddObjectToObject(root, "thinking");
        cJSON_AddStringToObject(th, "type", thinking_mode == 1 ? "disabled" : "between_tools");
        cJSON *oc = cJSON_AddObjectToObject(root, "output_config");
        cJSON_AddStringToObject(oc, "effort", "low");
    }
    cJSON *msgs = cJSON_AddArrayToObject(root, "messages");
    for (int i = 0; i < n_history; i++) {
        if (!history[i].text || !history[i].text[0]) continue;
        cJSON *m = cJSON_CreateObject();
        cJSON_AddStringToObject(m, "role", history[i].role);
        cJSON_AddStringToObject(m, "content", history[i].text);
        cJSON_AddItemToArray(msgs, m);
    }
    cJSON *u = cJSON_CreateObject();
    cJSON_AddStringToObject(u, "role", "user");
    cJSON_AddStringToObject(u, "content", user ? user : "");
    cJSON_AddItemToArray(msgs, u);
    char *s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return s;
}

static void copy_str(char *out, size_t cap, const char *s)
{
    if (!out || cap == 0) return;
    snprintf(out, cap, "%s", s ? s : "");
}

bool au_parse_claude(const char *json, char *out, size_t cap, char *err, size_t errcap)
{
    if (out && cap) out[0] = 0;
    if (err && errcap) err[0] = 0;
    cJSON *root = json ? cJSON_Parse(json) : NULL;
    if (!root) { copy_str(err, errcap, "Bad reply from Claude"); return false; }
    bool ok = false;
    cJSON *type = cJSON_GetObjectItem(root, "type");
    cJSON *error = cJSON_GetObjectItem(root, "error");
    if ((cJSON_IsString(type) && strcmp(type->valuestring, "error") == 0) || cJSON_IsObject(error)) {
        cJSON *msg = error ? cJSON_GetObjectItem(error, "message") : NULL;
        copy_str(err, errcap, cJSON_IsString(msg) ? msg->valuestring : "Claude returned an error");
    } else {
        cJSON *content = cJSON_GetObjectItem(root, "content");
        size_t used = 0;
        cJSON *blk;
        cJSON_ArrayForEach(blk, content) {
            cJSON *bt = cJSON_GetObjectItem(blk, "type");
            cJSON *tx = cJSON_GetObjectItem(blk, "text");
            if (!cJSON_IsString(bt) || strcmp(bt->valuestring, "text") != 0 || !cJSON_IsString(tx)) continue;
            size_t l = strlen(tx->valuestring);
            if (used && used + 1 < cap) out[used++] = ' ';
            if (used + l >= cap) l = cap - used - 1;
            memcpy(out + used, tx->valuestring, l);
            used += l;
            out[used] = 0;
        }
        ok = used > 0;
        if (!ok) copy_str(err, errcap, "Claude sent an empty reply");
    }
    cJSON_Delete(root);
    return ok;
}

bool au_parse_deepgram(const char *json, char *out, size_t cap)
{
    if (out && cap) out[0] = 0;
    cJSON *root = json ? cJSON_Parse(json) : NULL;
    if (!root) return false;
    bool ok = false;
    cJSON *res = cJSON_GetObjectItem(root, "results");
    cJSON *chans = res ? cJSON_GetObjectItem(res, "channels") : NULL;
    cJSON *ch0 = cJSON_GetArrayItem(chans, 0);
    cJSON *alts = ch0 ? cJSON_GetObjectItem(ch0, "alternatives") : NULL;
    cJSON *a0 = cJSON_GetArrayItem(alts, 0);
    cJSON *tr = a0 ? cJSON_GetObjectItem(a0, "transcript") : NULL;
    if (cJSON_IsString(tr)) {
        copy_str(out, cap, tr->valuestring);
        au_trim(out);
        ok = true;
    }
    cJSON_Delete(root);
    return ok;
}

void au_parse_error_message(const char *json, char *out, size_t cap)
{
    copy_str(out, cap, "");
    cJSON *root = json ? cJSON_Parse(json) : NULL;
    if (!root) return;
    const char *keys[] = { "err_msg", "message", "details" };
    for (int i = 0; i < 3; i++) {
        cJSON *m = cJSON_GetObjectItem(root, keys[i]);
        if (cJSON_IsString(m) && m->valuestring[0]) { copy_str(out, cap, m->valuestring); break; }
    }
    if (!out[0]) {
        cJSON *e = cJSON_GetObjectItem(root, "error");
        cJSON *m = e ? cJSON_GetObjectItem(e, "message") : NULL;
        if (cJSON_IsString(m)) copy_str(out, cap, m->valuestring);
    }
    cJSON_Delete(root);
}

char *au_tts_body(const char *text)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;
    cJSON_AddStringToObject(root, "text", text ? text : "");
    char *s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return s;
}

void au_speech_clean(const char *in, char *out, size_t cap, size_t max_chars)
{
    if (!out || cap == 0) return;
    size_t o = 0;
    bool space = true;   /* suppress leading spaces */
    bool line_start = true;
    if (max_chars == 0 || max_chars > cap - 1) max_chars = cap - 1;
    for (const char *p = in ? in : ""; *p && o < cap - 1; p++) {
        char c = *p;
        const unsigned char *u = (const unsigned char *)p;
        /* common UTF-8 punctuation -> ASCII (the screen font is ASCII + degree sign) */
        if (u[0] == 0xE2 && u[1] == 0x80 && u[2]) {
            const char *rep = NULL;
            switch (u[2]) {
            case 0x98: case 0x99: rep = "'"; break;            /* curly single quotes */
            case 0x9C: case 0x9D: rep = "\""; break;           /* curly double quotes */
            case 0x93: case 0x94: rep = " - "; break;          /* en / em dash */
            case 0xA6: rep = "..."; break;                     /* ellipsis */
            case 0xA2: rep = ""; break;                        /* bullet */
            }
            if (rep) {
                for (const char *r = rep; *r && o < cap - 1; r++) {
                    if (*r == ' ') { if (!space) { out[o++] = ' '; space = true; } }
                    else { out[o++] = *r; space = false; }
                }
                line_start = false;
                p += 2;
                continue;
            }
        }
        if (c == '\r') continue;
        if (c == '\n' || c == '\t') {
            line_start = (c == '\n');
            if (!space && o) { out[o++] = ' '; space = true; }
            continue;
        }
        if (line_start) {
            /* skip list bullets / numbering / headings at line start */
            if ((c == '-' || c == '+') && p[1] == ' ') { p++; continue; }
            if (isdigit((unsigned char)c) && p[1] == '.' && p[2] == ' ') { p += 2; continue; }
            if (c == ' ') continue;
        }
        line_start = false;
        if (c == '*' || c == '#' || c == '`' || c == '_' || c == '~' || c == '>' || c == '|') continue;
        if (c == ' ') {
            if (space) continue;
            space = true;
        } else {
            space = false;
        }
        out[o++] = c;
    }
    while (o && out[o - 1] == ' ') o--;
    out[o] = 0;
    if (o <= max_chars) return;
    /* cut at the last sentence end within max_chars, else at the last space */
    size_t cut = 0;
    for (size_t i = 0; i < max_chars; i++) {
        if ((out[i] == '.' || out[i] == '!' || out[i] == '?') && (out[i + 1] == ' ' || out[i + 1] == 0)) cut = i + 1;
    }
    if (cut < max_chars / 3) {
        cut = max_chars;
        while (cut > 0 && out[cut] != ' ') cut--;
        if (cut == 0) cut = max_chars;
    }
    out[cut] = 0;
    while (cut && out[cut - 1] == ' ') out[--cut] = 0;
}

int au_system_prompt(char *out, size_t cap, const char *local_time)
{
    return snprintf(out, cap,
        "You are Thad, Bryan Gilliland's personal assistant, living in a small round desk sphere "
        "with two white eyes. Bryan lives in Fort Worth, Texas (US Central time). "
        "Everything you write is read aloud by a text-to-speech voice and shown on a tiny round screen, so: "
        "answer in one to three short sentences (under 60 words) unless he asks for more; "
        "plain conversational English; no markdown, lists, emojis, URLs or code. "
        "Be warm, direct and a little witty. If you are unsure, say so briefly. "
        "You cannot browse the web, send email, or see his calendar from this device.%s%s",
        (local_time && local_time[0]) ? " Current local date and time: " : "",
        (local_time && local_time[0]) ? local_time : "");
}

/* ----------------------------------------------------------------- VAD */
#define VAD_NO_SPEECH_MS 7000
#define VAD_SILENCE_MS   1100
#define VAD_MIN_SPEECH_MS 350
#define VAD_MAX_MS       15000
#define VAD_MIN_RMS      350.0f

void au_vad_init(au_vad_t *v, int frame_ms)
{
    memset(v, 0, sizeof(*v));
    v->frame_ms = frame_ms > 0 ? frame_ms : 32;
    v->noise = -1.0f;
}

uint32_t au_rms(const int16_t *pcm, size_t n)
{
    if (!pcm || !n) return 0;
    double acc = 0;
    for (size_t i = 0; i < n; i++) acc += (double)pcm[i] * pcm[i];
    return (uint32_t)sqrt(acc / n);
}

vad_result_t au_vad_feed(au_vad_t *v, uint32_t rms)
{
    float r = (float)rms;
    v->elapsed_ms += v->frame_ms;
    if (v->noise < 0) v->noise = r < 800.0f ? r : 800.0f;   /* first frame may already be speech */
    float thresh = v->noise * 3.0f;
    if (thresh < VAD_MIN_RMS) thresh = VAD_MIN_RMS;
    bool loud = r > thresh;
    if (!loud) {
        /* track background noise slowly (only while quiet) */
        v->noise = v->noise * 0.95f + r * 0.05f;
    }
    if (loud) {
        v->speech_ms += v->frame_ms;
        v->silence_ms = 0;
        if (v->speech_ms >= VAD_MIN_SPEECH_MS) v->started = true;
    } else {
        v->silence_ms += v->frame_ms;
        if (!v->started) v->speech_ms = 0;
    }
    if (v->elapsed_ms >= VAD_MAX_MS) return VAD_MAX_LEN;
    if (v->started && v->silence_ms >= VAD_SILENCE_MS) return VAD_DONE;
    if (!v->started && v->elapsed_ms >= VAD_NO_SPEECH_MS) return VAD_NO_SPEECH;
    return v->started ? VAD_SPEECH : VAD_WAITING;
}
