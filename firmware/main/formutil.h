#pragma once
#include <stdbool.h>
#include <stddef.h>

/* Decode application/x-www-form-urlencoded text in place ("+" -> space, %XX -> byte). */
void net_url_decode(char *s);
/* Find key=value in a form body, URL-decode it into out (always NUL-terminated). */
bool net_form_get(const char *body, const char *key, char *out, size_t cap);

/* HTML-escape in into out (always NUL-terminated). */
void net_html_escape(const char *in, char *out, size_t cap);

/* Build the captive-portal setup page into out.
 * ssid / bridge_host may be empty (first boot). scan_ssids may be NULL;
 * empty scan names are skipped. Returns bytes written (not counting the
 * trailing NUL), or -1 if out is too small. */
int net_setup_page_build(char *out, size_t cap,
                         const char *ssid,
                         const char *bridge_host,
                         unsigned bridge_port,
                         bool mirror_x,
                         const char *const *scan_ssids,
                         int scan_n);
