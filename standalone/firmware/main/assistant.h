#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    char last_err[128];
    char last_q[520];
    char last_a[256];
    uint32_t mic_peak;
    uint32_t mic_rms;
    char wake_active[36];
} assistant_diag_t;

void assistant_init(void);
/* Tap / BOOT: start listening, or stop (listening -> send now, speaking -> hush). */
void assistant_tap(void);
/* A typed question from the phone page. */
void assistant_ask_text(const char *q);
/* Speak a short sample in this Aura-2 voice (must already have a Deepgram key). */
void assistant_preview(const char *voice);
/* Call after settings change (wake_enabled / wake_word / portal state). */
void assistant_wake_reconfig(void);
bool assistant_busy(void);
/* Last question / answer for the web page (copied, never NULL). */
void assistant_last(char *q, size_t qcap, char *a, size_t acap);
/* Runtime diagnostics for the setup page (no keys or secrets). */
void assistant_diag(assistant_diag_t *out);
