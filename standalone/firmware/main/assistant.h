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
    uint32_t noise_floor;
    bool speech_detected;
    uint32_t rec_ms;
    char wake_active[36];
    int last_web_searches;
} assistant_diag_t;

void assistant_init(void);
/* Tap / BOOT: start listening, or stop (listening -> send now, speaking -> hush). */
void assistant_tap(void);
/* Ask from the phone web page when there is no Deepgram key (or just typing). */
void assistant_ask_text(const char *question);
/* Speak the sample line in a chosen Deepgram voice (from the web page). */
void assistant_preview(const char *voice);
/* Re-read wake word settings (after saving in the web page). */
void assistant_wake_reconfig(void);
bool assistant_busy(void);
/* Last question / answer for the web page (copied, never NULL). */
void assistant_last(char *q, size_t qcap, char *a, size_t acap);
/* Runtime diagnostics for the setup page (no keys or secrets). */
void assistant_diag(assistant_diag_t *out);
