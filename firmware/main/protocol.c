#include "protocol.h"
#include <stdio.h>
#include <string.h>
#include "cJSON.h"

static const char *const STATE_NAMES[UI_STATE_COUNT] = {
    "idle", "listening", "thinking", "speaking", "error", "offline",
};

const char *proto_state_name(ui_state_t s)
{
    return (s >= 0 && s < UI_STATE_COUNT) ? STATE_NAMES[s] : "idle";
}

bool proto_state_from_name(const char *name, ui_state_t *out)
{
    if (!name) return false;
    for (int i = 0; i < UI_STATE_OFFLINE; i++) {   /* "offline" is board-local only */
        if (strcmp(name, STATE_NAMES[i]) == 0) {
            *out = (ui_state_t)i;
            return true;
        }
    }
    return false;
}

static void copy_str(char *dst, size_t cap, const cJSON *item)
{
    dst[0] = 0;
    if (cJSON_IsString(item) && item->valuestring) {
        strncpy(dst, item->valuestring, cap - 1);
        dst[cap - 1] = 0;
    }
}

bool proto_parse(const char *json, size_t len, proto_msg_t *out)
{
    memset(out, 0, sizeof(*out));
    out->type = MSG_UNKNOWN;
    if (!json || len == 0) return false;
    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!root) return false;
    bool ok = false;
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
    if (!cJSON_IsString(type)) goto done;
    const char *t = type->valuestring;

    if (strcmp(t, "state") == 0) {
        const cJSON *st = cJSON_GetObjectItemCaseSensitive(root, "state");
        if (!cJSON_IsString(st) || !proto_state_from_name(st->valuestring, &out->state)) goto done;
        copy_str(out->label, sizeof(out->label), cJSON_GetObjectItemCaseSensitive(root, "label"));
        copy_str(out->text, sizeof(out->text), cJSON_GetObjectItemCaseSensitive(root, "text"));
        out->type = MSG_STATE;
    } else if (strcmp(t, "audio_start") == 0) {
        const cJSON *sr = cJSON_GetObjectItemCaseSensitive(root, "sample_rate");
        const cJSON *fmt = cJSON_GetObjectItemCaseSensitive(root, "format");
        const cJSON *ch = cJSON_GetObjectItemCaseSensitive(root, "channels");
        out->sample_rate = cJSON_IsNumber(sr) ? sr->valueint : 16000;
        if (out->sample_rate != 16000) goto done;
        if (cJSON_IsString(fmt) && strcmp(fmt->valuestring, "pcm_s16le") != 0) goto done;
        if (cJSON_IsNumber(ch) && ch->valueint != 1) goto done;
        out->type = MSG_AUDIO_START;
    } else if (strcmp(t, "audio_end") == 0) {
        out->type = MSG_AUDIO_END;
    } else if (strcmp(t, "stop") == 0) {
        out->type = MSG_STOP;
    } else if (strcmp(t, "volume") == 0) {
        const cJSON *v = cJSON_GetObjectItemCaseSensitive(root, "value");
        if (!cJSON_IsNumber(v)) goto done;
        int val = v->valueint;
        out->value = val < 0 ? 0 : (val > 100 ? 100 : val);
        out->type = MSG_VOLUME;
    } else if (strcmp(t, "screen") == 0) {
        const cJSON *s = cJSON_GetObjectItemCaseSensitive(root, "screen");
        if (!cJSON_IsString(s)) goto done;
        if (strcmp(s->valuestring, "eyes") == 0) out->screen = SCREEN_EYES;
        else if (strcmp(s->valuestring, "clock") == 0) out->screen = SCREEN_CLOCK;
        else goto done;
        out->type = MSG_SCREEN;
    } else if (strcmp(t, "chime") == 0) {
        out->type = MSG_CHIME;
    } else if (strcmp(t, "ping") == 0) {
        out->type = MSG_PING;
    } else {
        goto done;
    }
    ok = true;
done:
    cJSON_Delete(root);
    if (!ok) out->type = MSG_UNKNOWN;
    return ok;
}

static int finish(cJSON *o, char *buf, size_t cap)
{
    int n = -1;
    if (cJSON_PrintPreallocated(o, buf, (int)cap, false)) n = (int)strlen(buf);
    cJSON_Delete(o);
    return n;
}

int proto_build_hello(char *buf, size_t cap, const char *fw, const char *hw, const char *mac)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "hello");
    cJSON_AddStringToObject(o, "device", "grok-sphere");
    cJSON_AddNumberToObject(o, "proto", PROTO_VERSION);
    cJSON_AddStringToObject(o, "fw", fw ? fw : "");
    cJSON_AddStringToObject(o, "hw", hw ? hw : "");
    cJSON_AddStringToObject(o, "mac", mac ? mac : "");
    cJSON_AddNumberToObject(o, "sample_rate", 16000);
    cJSON_AddStringToObject(o, "format", "pcm_s16le");
    cJSON_AddNumberToObject(o, "channels", 1);
    return finish(o, buf, cap);
}

int proto_build_touch(char *buf, size_t cap, const char *action)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "touch");
    cJSON_AddStringToObject(o, "action", action ? action : "tap");
    return finish(o, buf, cap);
}

int proto_build_simple(char *buf, size_t cap, const char *type)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", type ? type : "");
    return finish(o, buf, cap);
}
