#pragma once
#include <stdbool.h>
#include <stddef.h>

void assistant_init(void);
/* Tap / BOOT: start listening, or stop (listening -> send now, speaking -> hush). */
void assistant_tap(void);
/* A typed question from the phone page. */
void assistant_ask_text(const char *q);
bool assistant_busy(void);
/* Last question / answer for the web page (copied, never NULL). */
void assistant_last(char *q, size_t qcap, char *a, size_t acap);
