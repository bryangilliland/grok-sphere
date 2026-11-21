#pragma once
#include <stdbool.h>
#include "protocol.h"
#include "settings.h"

typedef void (*bridge_msg_cb_t)(const proto_msg_t *msg);
typedef void (*bridge_conn_cb_t)(bool connected, const char *where);

void bridge_init(bridge_msg_cb_t on_msg, bridge_conn_cb_t on_conn);
void bridge_start_wifi(const sphere_settings_t *s);
bool bridge_connected(void);
void bridge_send_touch(const char *action);   /* "tap", "ptt_start", "ptt_stop" */
void bridge_send_simple(const char *type);    /* "playback_done", "pong" */
