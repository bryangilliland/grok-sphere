/* Grok Sphere standalone - pure-C pipeline helpers (unit-tested in host_test/). */
#include "assist_util.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "cJSON.h"

/* IDs from https://developers.deepgram.com/docs/tts-models (Aura-2 English, /v1/speak). */
const au_voice_t AU_VOICES[] = {
    { "aura-2-orion-en",     "Orion (calm male, US)" },
    { "aura-2-apollo-en",    "Apollo (casual male, US)" },
    { "aura-2-arcas-en",     "Arcas (smooth male, US)" },
    { "aura-2-aries-en",     "Aries (warm male, US)" },
    { "aura-2-zeus-en",      "Zeus (deep male, US)" },
    { "aura-2-draco-en",     "Draco (warm male, British)" },
    { "aura-2-hyperion-en",  "Hyperion (warm male, Australian)" },
    { "aura-2-thalia-en",    "Thalia (bright female, US)" },
    { "aura-2-helena-en",    "Helena (warm female, US)" },
    { "aura-2-andromeda-en", "Andromeda (casual female, US)" },
    { "aura-2-athena-en",    "Athena (calm female, US)" },
    { "aura-2-luna-en",      "Luna (friendly female, US)" },
    { "aura-2-pandora-en",   "Pandora (calm female, British)" },
    { "aura-2-theia-en",     "Theia (polite female, Australian)" },
    { "aura-2-amalthea-en",  "Amalthea (cheerful female, Filipino)" },
};
const int AU_VOICE_COUNT = sizeof(AU_VOICES) / sizeof(AU_VOICES[0]);

const char *au_model_id(int model) { return model == 1 ? AU_MODEL_SONNET : AU_MODEL_HAIKU; }

bool au_voice_valid(const char *id)
{
    if (!id) return false;
    for (int i = 0; i < AU_VOICE_COUNT; i++) if (strcmp(id, AU_VOICES[i].id) == 0) return true;
    return false;
}

/* Prebuilt English WakeNet9 models bundled from espressif/esp-sr 2.6.2 (packed into srmodels.bin). */
const au_wake_t AU_WAKES[] = {
    { "wn9_hiesp",        "Hi, ESP (default)" },
    { "wn9_alexa",        "Alexa" },
    { "wn9_jarvis_tts",   "Jarvis" },
    { "wn9_computer_tts", "Computer" },
};
const int AU_WAKE_COUNT = sizeof(AU_WAKES) / sizeof(AU_WAKES[0]);

bool au_wake_valid(const char *id)
{
    if (!id) return false;
    for (int i = 0; i < AU_WAKE_COUNT; i++) if (strcmp(id, AU_WAKES[i].id) == 0) return true;
    return false;
}

const char *au_wake_label(const char *id)
{
    if (id) {
        for (int i = 0; i < AU_WAKE_COUNT; i++) if (strcmp(id, AU_WAKES[i].id) == 0) return AU_WAKES[i].label;
    }
    return AU_WAKES[0].label;
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
                        const au_turn_t *history, int n_history, const char *user, int thinking_mode,
                        const au_web_search_t *web)
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
    if (web && web->max_uses > 0) {
        cJSON *tools = cJSON_AddArrayToObject(root, "tools");
        cJSON *tool = cJSON_CreateObject();
        cJSON_AddStringToObject(tool, "type", AU_WEB_SEARCH_TYPE);
        cJSON_AddStringToObject(tool, "name", "web_search");
        cJSON_AddNumberToObject(tool, "max_uses", web->max_uses);
        cJSON *loc = cJSON_AddObjectToObject(tool, "user_location");
        cJSON_AddStringToObject(loc, "type", "approximate");
        if (web->city && web->city[0]) cJSON_AddStringToObject(loc, "city", web->city);
        if (web->region && web->region[0]) cJSON_AddStringToObject(loc, "region", web->region);
        cJSON_AddStringToObject(loc, "country", (web->country && web->country[0]) ? web->country : "US");
        cJSON_AddStringToObject(loc, "timezone",
            (web->timezone && web->timezone[0]) ? web->timezone : "America/Chicago");
        cJSON_AddItemToArray(tools, tool);
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

static int is_tool_block(const char *t)
{
    return t && (strcmp(t, "server_tool_use") == 0 || strcmp(t, "web_search_tool_result") == 0 ||
                 strcmp(t, "tool_use") == 0 || strcmp(t, "tool_result") == 0 ||
                 strcmp(t, "code_execution_tool_result") == 0);
}

void au_strip_citations(char *s)
{
    if (!s) return;
    char *d = s;
    const char *p = s;
    while (*p) {
        /* markdown [title](url) -> title */
        if (*p == '[' ) {
            const char *rb = strchr(p, ']');
            if (rb && rb[1] == '(' ) {
                const char *rp = strchr(rb + 2, ')');
                if (rp) {
                    size_t n = (size_t)(rb - (p + 1));
                    /* skip numeric citations like [1](url) */
                    int all_digit = n > 0;
                    for (size_t i = 0; i < n; i++) if (!isdigit((unsigned char)p[1 + i])) { all_digit = 0; break; }
                    if (!all_digit) { memcpy(d, p + 1, n); d += n; }
                    p = rp + 1;
                    continue;
                }
            }
            /* bare [1] / [12] citation markers */
            if (rb) {
                int all_digit = rb > p + 1;
                for (const char *q = p + 1; q < rb; q++) if (!isdigit((unsigned char)*q)) { all_digit = 0; break; }
                if (all_digit) { p = rb + 1; continue; }
            }
        }
        if ((strncmp(p, "http://", 7) == 0) || (strncmp(p, "https://", 8) == 0) ||
            (strncmp(p, "www.", 4) == 0 && p[4] && !isspace((unsigned char)p[4]))) {
            while (*p && !isspace((unsigned char)*p) && *p != ')' && *p != ']') p++;
            continue;
        }
        *d++ = *p++;
    }
    *d = 0;
    /* drop leftover empty parens and collapse whitespace */
    d = s; p = s;
    int space = 1;
    while (*p) {
        if (*p == '(' && p[1] == ')') { p += 2; continue; }
        if (isspace((unsigned char)*p)) {
            if (!space) { *d++ = ' '; space = 1; }
            p++;
            continue;
        }
        space = 0;
        *d++ = *p++;
    }
    while (d > s && isspace((unsigned char)d[-1])) d--;
    *d = 0;
    /* drop spaces left in front of punctuation after a URL/citation was removed */
    d = s; p = s;
    while (*p) {
        if (*p == ' ' && (p[1] == '.' || p[1] == ',' || p[1] == '!' || p[1] == '?' || p[1] == ';' || p[1] == ':')) {
            p++;
            continue;
        }
        *d++ = *p++;
    }
    *d = 0;
}

int au_claude_web_search_count(const char *json)
{
    cJSON *root = json ? cJSON_Parse(json) : NULL;
    if (!root) return 0;
    int n = 0;
    cJSON *usage = cJSON_GetObjectItem(root, "usage");
    cJSON *stu = usage ? cJSON_GetObjectItem(usage, "server_tool_use") : NULL;
    cJSON *req = stu ? cJSON_GetObjectItem(stu, "web_search_requests") : NULL;
    if (cJSON_IsNumber(req)) {
        n = req->valueint;
    } else {
        cJSON *content = cJSON_GetObjectItem(root, "content");
        cJSON *blk;
        cJSON_ArrayForEach(blk, content) {
            cJSON *bt = cJSON_GetObjectItem(blk, "type");
            cJSON *nm = cJSON_GetObjectItem(blk, "name");
            if (cJSON_IsString(bt) && strcmp(bt->valuestring, "server_tool_use") == 0 &&
                cJSON_IsString(nm) && strcmp(nm->valuestring, "web_search") == 0) n++;
        }
    }
    cJSON_Delete(root);
    return n;
}

bool au_web_search_rejected(const char *err)
{
    if (!err || !err[0]) return false;
    char buf[240];
    size_t n = strlen(err);
    if (n >= sizeof(buf)) n = sizeof(buf) - 1;
    for (size_t i = 0; i < n; i++) buf[i] = (char)tolower((unsigned char)err[i]);
    buf[n] = 0;
    if (!strstr(buf, "web_search") && !strstr(buf, "web search")) return false;
    return strstr(buf, "not enabled") || strstr(buf, "disabled") || strstr(buf, "permission") ||
           strstr(buf, "not available") || strstr(buf, "not supported") || strstr(buf, "unknown tool") ||
           strstr(buf, "invalid") || strstr(buf, "unauthorized");
}

void au_web_location_from_setting(const char *loc, char *city, size_t cc, char *region, size_t rc)
{
    if (city && cc) snprintf(city, cc, "Fort Worth");
    if (region && rc) snprintf(region, rc, "Texas");
    if (!loc || !loc[0]) return;
    double lat, lon;
    if (au_parse_latlon(loc, &lat, &lon)) return;
    char tmp[64];
    snprintf(tmp, sizeof(tmp), "%s", loc);
    au_trim(tmp);
    char *comma = strchr(tmp, ',');
    if (comma) {
        *comma = 0;
        au_trim(tmp);
        au_trim(comma + 1);
        if (tmp[0] && city && cc) snprintf(city, cc, "%s", tmp);
        if (comma[1] && region && rc) snprintf(region, rc, "%s", comma + 1);
    } else if (tmp[0] && city && cc) {
        snprintf(city, cc, "%s", tmp);
        if (region && rc) region[0] = 0;
    }
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
        int idx = 0, last_tool = -1;
        cJSON *blk;
        cJSON_ArrayForEach(blk, content) {
            cJSON *bt = cJSON_GetObjectItem(blk, "type");
            if (cJSON_IsString(bt) && is_tool_block(bt->valuestring)) last_tool = idx;
            idx++;
        }
        size_t used = 0;
        idx = 0;
        cJSON_ArrayForEach(blk, content) {
            cJSON *bt = cJSON_GetObjectItem(blk, "type");
            cJSON *tx = cJSON_GetObjectItem(blk, "text");
            int keep = (idx > last_tool);
            idx++;
            if (!keep || !cJSON_IsString(bt) || strcmp(bt->valuestring, "text") != 0 || !cJSON_IsString(tx)) continue;
            size_t l = strlen(tx->valuestring);
            if (used && used + 1 < cap) out[used++] = ' ';
            if (used + l >= cap) l = cap - used - 1;
            memcpy(out + used, tx->valuestring, l);
            used += l;
            out[used] = 0;
        }
        if (used) au_strip_citations(out);
        ok = out && out[0];
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

static int ci_eq(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return 0;
    }
    return b[n] == 0;
}

au_mood_t au_extract_mood(char *text)
{
    if (!text) return AU_MOOD_NONE;
    char *p = text;
    while (*p && isspace((unsigned char)*p)) p++;
    if (!(p[0] == '[' && tolower((unsigned char)p[1]) == 'm' && tolower((unsigned char)p[2]) == 'o' &&
          tolower((unsigned char)p[3]) == 'o' && tolower((unsigned char)p[4]) == 'd' && p[5] == ':')) {
        return AU_MOOD_NONE;
    }
    char *end = strchr(p + 6, ']');
    if (!end || (size_t)(end - (p + 6)) > 24) return AU_MOOD_NONE;

    const char *tag = p + 6;
    size_t len = (size_t)(end - tag);
    while (len && isspace((unsigned char)*tag)) { tag++; len--; }
    while (len && isspace((unsigned char)tag[len - 1])) len--;

    au_mood_t mood = AU_MOOD_NONE;
    if      (len == 5 && ci_eq(tag, "happy", 5))     mood = AU_MOOD_HAPPY;
    else if (len == 3 && ci_eq(tag, "sad", 3))       mood = AU_MOOD_SAD;
    else if (len == 5 && ci_eq(tag, "bored", 5))     mood = AU_MOOD_BORED;
    else if (len == 6 && ci_eq(tag, "sleepy", 6))    mood = AU_MOOD_SLEEPY;
    else if (len == 9 && ci_eq(tag, "surprised", 9)) mood = AU_MOOD_SURPRISED;
    else if (len == 7 && ci_eq(tag, "curious", 7))   mood = AU_MOOD_CURIOUS;

    char *rest = end + 1;
    while (*rest && isspace((unsigned char)*rest)) rest++;
    memmove(text, rest, strlen(rest) + 1);
    return mood;
}

int au_system_prompt(char *out, size_t cap, const char *local_time, const char *weather_summary, bool web_search)
{
    return snprintf(out, cap,
        "You are Thad, Bryan Gilliland's personal assistant, running on his desk sphere (Grok Sphere) "
        "with two white eyes. "
        "Everything you write is read aloud by a text-to-speech voice and shown on a tiny round screen, so: "
        "answer in 1 to 3 short spoken sentences; "
        "plain conversational English; no markdown, lists, emojis, URLs or code. "
        "Never read URLs, citation markers, or source lists aloud. "
        "Be warm, direct and a little witty. If you are unsure, say so briefly. "
        "Optionally start your reply with a hidden tag [mood:happy], [mood:curious], [mood:surprised], "
        "or [mood:sad] when it fits the tone (never mention the tag). "
        "You cannot send email or see his calendar from this device."
        "%s%s%s%s%s%s",
        (local_time && local_time[0]) ? " Current local date and time: " : "",
        (local_time && local_time[0]) ? local_time : "",
        (weather_summary && weather_summary[0]) ? " Live local weather and forecast: " : "",
        (weather_summary && weather_summary[0]) ? weather_summary : "",
        ((local_time && local_time[0]) || (weather_summary && weather_summary[0]))
            ? " This live date, time, and weather data on the sphere is accurate and current; use it first when asked about the weather or the time, and do not search the web for that."
            : "",
        web_search
            ? " You may use web search only for things that need current information (news, scores, prices, business hours, events, and similar). Do not search for stable knowledge, math, or casual conversation."
            : " You cannot browse the web from this device.");
}

int au_split_sentences(const char *text, const char **starts, size_t *lens, int max_segs)
{
    if (!text || !text[0] || !starts || !lens || max_segs <= 0) return 0;
    const char *p = text;
    while (*p == ' ') p++;
    if (!*p) return 0;

    int count = 0;
    const char *seg_start = p;
    for (; *p; p++) {
        bool end_punct = (*p == '.' || *p == '!' || *p == '?');
        bool next_ok = (p[1] == ' ' || p[1] == '\0');
        if (end_punct && next_ok) {
            size_t len = (size_t)((p + 1) - seg_start);
            if (len >= 28 && count + 1 < max_segs) {
                /* Only split if there is substantial text remaining (>= 20 chars) */
                const char *rest = p + 1;
                while (*rest == ' ') rest++;
                if (strlen(rest) >= 20) {
                    starts[count] = seg_start;
                    lens[count] = len;
                    count++;
                    seg_start = rest;
                    p = rest - 1;
                }
            }
        }
    }
    if (*seg_start) {
        size_t len = strlen(seg_start);
        while (len && seg_start[len - 1] == ' ') len--;
        if (len) {
            starts[count] = seg_start;
            lens[count] = len;
            count++;
        }
    }
    return count;
}

bool au_parse_latlon(const char *loc, double *lat_out, double *lon_out)
{
    if (!loc || !loc[0] || !lat_out || !lon_out) return false;
    const char *p = loc;
    while (*p && isspace((unsigned char)*p)) p++;
    if (!*p) return false;
    char *end1 = NULL;
    double lat = strtod(p, &end1);
    if (end1 == p) return false;
    while (*end1 && (isspace((unsigned char)*end1) || *end1 == ',')) end1++;
    if (!*end1) return false;
    char *end2 = NULL;
    double lon = strtod(end1, &end2);
    if (end2 == end1) return false;
    while (*end2 && isspace((unsigned char)*end2)) end2++;
    if (*end2 != '\0') return false;
    if (lat < -90.0 || lat > 90.0 || lon < -180.0 || lon > 180.0) return false;
    *lat_out = lat;
    *lon_out = lon;
    return true;
}

/* ----------------------------------------------------------------- VAD */
#define AU_VAD_NO_SPEECH_MS  5000
#define VAD_SILENCE_MS       900
#define VAD_MIN_SPEECH_MS    150
#define VAD_MIN_RECORD_MS    600
#define VAD_LOCKED_MIN_MS    1500
#define VAD_MAX_MS           8000
#define VAD_MIN_RMS          250.0f
#define VAD_MAX_THRESH       2500.0f

void au_vad_init(au_vad_t *v, int frame_ms)
{
    memset(v, 0, sizeof(*v));
    v->frame_ms = frame_ms > 0 ? frame_ms : 32;
    v->noise = -1.0f;
}

void au_vad_init_with_baseline(au_vad_t *v, int frame_ms, float baseline_rms)
{
    memset(v, 0, sizeof(*v));
    v->frame_ms = frame_ms > 0 ? frame_ms : 30;
    v->noise = baseline_rms > 0.0f ? baseline_rms : 150.0f;
    v->short_rms = v->noise;
    v->baseline_locked = true;
}

uint32_t au_rms(const int16_t *pcm, size_t n)
{
    if (!pcm || !n) return 0;
    double acc = 0;
    for (size_t i = 0; i < n; i++) acc += (double)pcm[i] * pcm[i];
    return (uint32_t)sqrt(acc / n);
}

uint32_t au_rms_ac(const int16_t *pcm, size_t n, uint16_t *peak_out)
{
    if (peak_out) *peak_out = 0;
    if (!pcm || !n) return 0;
    double sum = 0;
    for (size_t i = 0; i < n; i++) sum += pcm[i];
    double mean = sum / (double)n;
    double acc = 0, pk = 0;
    for (size_t i = 0; i < n; i++) {
        double d = (double)pcm[i] - mean;
        double ad = d < 0 ? -d : d;
        if (ad > pk) pk = ad;
        acc += d * d;
    }
    if (peak_out) *peak_out = pk > 65535.0 ? 65535 : (uint16_t)pk;
    return (uint32_t)sqrt(acc / (double)n);
}

uint32_t au_noise_percentile(const uint32_t *rms_hist, size_t count)
{
    if (!rms_hist || count == 0) return 0;
    if (count > 64) count = 64;
    uint32_t tmp[64];
    for (size_t i = 0; i < count; i++) tmp[i] = rms_hist[i];
    for (size_t i = 1; i < count; i++) {
        uint32_t k = tmp[i];
        size_t j = i;
        while (j > 0 && tmp[j - 1] > k) {
            tmp[j] = tmp[j - 1];
            j--;
        }
        tmp[j] = k;
    }
    size_t idx = (count * 35) / 100;
    if (idx >= count) idx = count - 1;
    return tmp[idx];
}

vad_result_t au_vad_feed_vad(au_vad_t *v, uint32_t rms, bool engine_speech)
{
    float r = (float)rms;
    v->elapsed_ms += v->frame_ms;

    if (v->baseline_locked) {
        /* Pre-trigger noise floor is fixed; track short-term speech energy vs baseline */
        v->short_rms = v->short_rms * 0.65f + r * 0.35f;
        float speech_thresh = v->noise * 1.45f;
        if (speech_thresh < 320.0f) speech_thresh = 320.0f;
        float eng_thresh = v->noise * 1.15f;
        if (eng_thresh < 220.0f) eng_thresh = 220.0f;
        float quiet_thresh = v->noise * 1.30f;
        if (quiet_thresh < 260.0f) quiet_thresh = 260.0f;

        bool is_speech = (v->short_rms > speech_thresh) || (engine_speech && v->short_rms > eng_thresh);
        bool is_quiet  = (v->short_rms < quiet_thresh) && !engine_speech;

        if (is_speech) {
            v->speech_ms += v->frame_ms;
            v->silence_ms = 0;
            if (v->speech_ms >= VAD_MIN_SPEECH_MS) v->started = true;
        } else if (is_quiet) {
            v->silence_ms += v->frame_ms;
            if (!v->started) v->speech_ms = 0;
        }
        if (v->elapsed_ms >= VAD_MAX_MS) return AU_VAD_MAX_LEN;
        if (v->started && v->silence_ms >= VAD_SILENCE_MS && v->elapsed_ms >= VAD_LOCKED_MIN_MS) return AU_VAD_DONE;
        /* Never discard audio locally when baseline_locked: keep capturing up to cap / tap / silence drop */
        return v->started ? AU_VAD_SPEECH : AU_VAD_WAITING;
    }

    if (v->noise < 0) v->noise = r < 800.0f ? r : 800.0f;
    float thresh = v->noise * 2.4f + 120.0f;
    if (thresh < VAD_MIN_RMS) thresh = VAD_MIN_RMS;
    if (thresh > VAD_MAX_THRESH) thresh = VAD_MAX_THRESH;
    bool loud = (r > thresh) || (engine_speech && r > VAD_MIN_RMS);
    if (!loud || (!v->started && r < v->noise)) {
        v->noise = v->noise * 0.90f + r * 0.10f;
        if (v->noise > 1200.0f) v->noise = 1200.0f;
    }
    if (loud) {
        v->speech_ms += v->frame_ms;
        v->silence_ms = 0;
        if (v->speech_ms >= 220) v->started = true;
    } else {
        v->silence_ms += v->frame_ms;
        if (!v->started) v->speech_ms = 0;
    }
    if (v->elapsed_ms >= VAD_MAX_MS) return AU_VAD_MAX_LEN;
    if (v->started && v->silence_ms >= VAD_SILENCE_MS && v->elapsed_ms >= VAD_MIN_RECORD_MS) return AU_VAD_DONE;
    if (!v->started && v->elapsed_ms >= AU_VAD_NO_SPEECH_MS) return AU_VAD_NO_SPEECH;
    return v->started ? AU_VAD_SPEECH : AU_VAD_WAITING;
}

vad_result_t au_vad_feed(au_vad_t *v, uint32_t rms)
{
    return au_vad_feed_vad(v, rms, false);
}
