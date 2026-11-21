#pragma once
#include <stdbool.h>
#include <stddef.h>

/* Start SNTP (US Central time) and the Open-Meteo weather refresher (10-min cache). */
void clock_weather_start(void);
/* Trigger an immediate weather refresh (e.g. after changing location on the setup page). */
void clock_weather_refresh(void);
/* Copy a concise live weather + today's forecast summary if fresh (< 30 min old). */
bool clock_weather_summary(char *out, size_t cap);
