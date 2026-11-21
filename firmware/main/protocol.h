/*
 * Grok Sphere board <-> Mac bridge protocol (see code/PROTOCOL.md).
 * Pure C + cJSON so it can be unit-tested on a PC (firmware/host_test).
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>

#define PROTO_VERSION 1

typedef enum {
    MSG_UNKNOWN = 0,
    MSG_STATE,        /* {"type":"state","state":"idle|listening|thinking|speaking|error","label":"Thad","text":"..."} */
    MSG_AUDIO_START,  /* {"type":"audio_start","sample_rate":16000,"format":"pcm_s16le","channels":1} */
    MSG_AUDIO_END,    /* {"type":"audio_end"} */
    MSG_STOP,         /* {"type":"stop"}  drop queued audio now */
    MSG_VOLUME,       /* {"type":"volume","value":0..100} */
    MSG_SCREEN,       /* {"type":"screen","screen":"eyes|clock"} */
    MSG_CHIME,        /* {"type":"chime"} */
    MSG_PING,         /* {"type":"ping"} -> board answers {"type":"pong"} */
} proto_msg_type_t;

typedef enum {
    UI_STATE_IDLE = 0,
    UI_STATE_LISTENING,
    UI_STATE_THINKING,
    UI_STATE_SPEAKING,
    UI_STATE_ERROR,
    UI_STATE_OFFLINE,   /* board-local: no bridge connection (never sent by the bridge) */
    UI_STATE_COUNT
} ui_state_t;

typedef enum { SCREEN_EYES = 0, SCREEN_CLOCK } ui_screen_t;

typedef struct {
    proto_msg_type_t type;
    ui_state_t state;
    ui_screen_t screen;
    int value;
    int sample_rate;
    char label[32];
    char text[96];
} proto_msg_t;

/* Parse one JSON text frame. Returns false (and type MSG_UNKNOWN) on bad input. */
bool proto_parse(const char *json, size_t len, proto_msg_t *out);

const char *proto_state_name(ui_state_t s);
bool proto_state_from_name(const char *name, ui_state_t *out);

/* Builders write a JSON object into buf; return length or -1 when it does not fit. */
int proto_build_hello(char *buf, size_t cap, const char *fw, const char *hw, const char *mac);
int proto_build_touch(char *buf, size_t cap, const char *action);
int proto_build_simple(char *buf, size_t cap, const char *type);
