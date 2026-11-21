#pragma once
#include <stdbool.h>
#include <stddef.h>

/* Decode application/x-www-form-urlencoded text in place ("+" -> space, %XX -> byte). */
void net_url_decode(char *s);
/* Find key=value in a form body, URL-decode it into out (always NUL-terminated). */
bool net_form_get(const char *body, const char *key, char *out, size_t cap);
/* HTML-escape in into out (always NUL-terminated). */
void net_html_escape(const char *in, char *out, size_t cap);

typedef struct {
    const char *ssid;            /* saved home Wi-Fi (may be empty) */
    bool wifi_connected;
    const char *ip;              /* home-network IP, or "" */
    const char *api_key_masked;  /* "****abcd" or "" - NEVER the real key */
    const char *speech_key_masked;
    int model;                   /* 0 haiku, 1 sonnet */
    const char *voice;
    int volume;
    bool mirror_x;
    bool wake_enabled;
    const char *wake_word;
    bool chime_enabled;
    const char *const *scan_ssids;
    int scan_n;
    const char *last_q;          /* last question / answer (may be NULL) */
    const char *last_a;
    const char *notice;          /* one-line banner, e.g. "Saved." (may be NULL) */
    /* Runtime diagnostics (never includes keys or passwords). */
    const char *diag_err;
    unsigned int diag_mic_peak;
    unsigned int diag_mic_rms;
    unsigned int diag_noise_floor;
    bool diag_speech_detected;
    unsigned int diag_rec_ms;
    int diag_rssi;               /* 0 if disconnected */
    unsigned int diag_free_heap;
    unsigned int diag_free_int;
    unsigned int diag_min_free_int;
    const char *diag_wake;
} setup_page_info_t;

/* Build the phone-friendly setup page. Returns bytes written (excl. NUL) or -1 if too small. */
int net_setup_page_build(char *out, size_t cap, const setup_page_info_t *info);
