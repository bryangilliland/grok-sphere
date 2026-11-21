#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef enum {
    TOUCH_EVT_TAP,          /* short touch (< hold threshold) */
    TOUCH_EVT_HOLD_START,   /* finger held past threshold: push-to-talk begins */
    TOUCH_EVT_HOLD_END,     /* finger lifted after a hold: push-to-talk ends */
} touch_event_t;

typedef void (*touch_cb_t)(touch_event_t evt);

esp_err_t touch_init(touch_cb_t cb);
bool touch_get_point(uint16_t *x, uint16_t *y);
