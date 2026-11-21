#pragma once
#include <stdbool.h>
#include "settings.h"

void net_init(void);
/* Start station mode and wait up to timeout_ms for an IP address. Keeps retrying afterwards. */
bool net_connect_sta(const sphere_settings_t *s, int timeout_ms);
bool net_is_connected(void);
/* Start the setup hotspot + captive portal. Saving the form stores settings and reboots. */
void net_start_portal(const sphere_settings_t *current);
bool net_portal_running(void);
void net_stop_portal(void);
int net_portal_clients(void);   /* phones joined to the setup hotspot */
const char *net_mac_str(void);
