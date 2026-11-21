#pragma once
#include <stdbool.h>
#include "settings.h"

void net_init(void);
/* Start station mode and wait up to timeout_ms for an IP address. Keeps retrying afterwards. */
bool net_connect_sta(const sphere_settings_t *s, int timeout_ms);
bool net_is_connected(void);
const char *net_ip_str(void);         /* home-network IP or "" */
/* Web setup page: http://<ip>/ on the home network and http://192.168.4.1/ on the hotspot. */
void net_http_start(void);
/* Setup hotspot (+ captive DNS). Shows the QR screen. */
void net_start_portal(void);
bool net_portal_running(void);
void net_stop_portal(void);
int net_portal_clients(void);
void net_mdns_start(void);            /* grok-sphere.local */
