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
    const char *const *scan_ssids;
    int scan_n;
    const char *last_q;          /* last question / answer (may be NULL) */
    const char *last_a;
    const char *notice;          /* one-line banner, e.g. "Saved." (may be NULL) */
} setup_page_info_t;

/* Build the phone-friendly setup page. Returns bytes written (excl. NUL) or -1 if too small. */
int net_setup_page_build(char *out, size_t cap, const setup_page_info_t *info);
