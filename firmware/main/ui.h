#pragma once
#include <stdbool.h>
#include "protocol.h"

void ui_init(void);
void ui_set_state(ui_state_t state, const char *label, const char *text);
ui_state_t ui_get_state(void);
void ui_show_screen(ui_screen_t screen);
void ui_toggle_screen(void);
void ui_show_setup(const char *ap_ssid, const char *ap_pass, const char *ip);
void ui_hide_setup(void);
void ui_show_notice(const char *title, const char *body);
void ui_hide_notice(void);
void ui_set_status(const char *text);
void ui_set_weather(bool valid, float temp_f, int wmo_code, float hi_f, float lo_f);
const char *ui_weather_text(int wmo_code);
