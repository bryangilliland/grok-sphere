/* Shared app state used by the web setup page and the assistant (thread-safe). */
#pragma once
#include <stdbool.h>
#include "settings.h"

void app_settings_get(sphere_settings_t *out);
/* Save new settings. wifi_changed -> the sphere restarts to join the new network. */
void app_settings_apply(const sphere_settings_t *s, bool wifi_changed);
void app_factory_reset(void);
