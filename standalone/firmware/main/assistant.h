#pragma once
#include <stdbool.h>
#include <stddef.h>

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
