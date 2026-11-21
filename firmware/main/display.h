#pragma once
#include <stdbool.h>
#include "esp_err.h"

/* Bring up QSPI + ST77916 + LVGL. mirror_x flips the image if the panel shows text mirrored. */
esp_err_t display_init(bool mirror_x);
/* LVGL is not thread-safe: wrap every lv_* call from another task in lock/unlock. */
bool display_lock(int timeout_ms);
void display_unlock(void);
