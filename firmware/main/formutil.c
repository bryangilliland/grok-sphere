/* Form parsing and setup page builder. Pure C so it is unit-tested on a PC (host_test/). */
#include "formutil.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    c = (char)tolower((unsigned char)c);
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

void net_url_decode(char *s)
{
    char *o = s;
    for (; *s; s++) {
        if (*s == '+') {
            *o++ = ' ';
        } else if (*s == '%' && hexval(s[1]) >= 0 && hexval(s[2]) >= 0) {
            *o++ = (char)(hexval(s[1]) * 16 + hexval(s[2]));
            s += 2;
        } else {
            *o++ = *s;
        }
    }
    *o = 0;
}

bool net_form_get(const char *body, const char *key, char *out, size_t cap)
{
    size_t klen = strlen(key);
    const char *p = body;
    out[0] = 0;
    while (p && *p) {
        if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
            const char *v = p + klen + 1;
            const char *end = strchr(v, '&');
            size_t n = end ? (size_t)(end - v) : strlen(v);
            if (n >= cap * 3) n = cap * 3 - 1;
            char tmp[256];
            if (n >= sizeof(tmp)) n = sizeof(tmp) - 1;
            memcpy(tmp, v, n);
            tmp[n] = 0;
            net_url_decode(tmp);
            strncpy(out, tmp, cap - 1);
            out[cap - 1] = 0;
            return true;
        }
        p = strchr(p, '&');
        if (p) p++;
    }
    return false;
}

void net_html_escape(const char *in, char *out, size_t cap)
{
    size_t o = 0;
    if (!in) in = "";
    if (cap == 0) return;
    for (; *in && o + 7 < cap; in++) {
        const char *rep = NULL;
        switch (*in) {
        case '&':  rep = "&amp;"; break;
        case '<':  rep = "&lt;"; break;
        case '>':  rep = "&gt;"; break;
        case '"':  rep = "&quot;"; break;
        case '\'': rep = "&#39;"; break;
        }
        if (rep) { size_t l = strlen(rep); memcpy(out + o, rep, l); o += l; }
        else out[o++] = *in;
    }
    out[o] = 0;
}

static int append(char *out, size_t cap, size_t *used, const char *s)
{
    if (!s) s = "";
    size_t len = strlen(s);
    if (*used + len + 1 > cap) return -1;
    memcpy(out + *used, s, len);
    *used += len;
    out[*used] = 0;
    return 0;
}

int net_setup_page_build(char *out, size_t cap,
                         const char *ssid,
                         const char *bridge_host,
                         unsigned bridge_port,
                         bool mirror_x,
                         const char *const *scan_ssids,
                         int scan_n)
{
    static const char head[] =
        "<!doctype html><html><head><meta charset=utf-8>"
        "<meta name=viewport content='width=device-width,initial-scale=1'>"
        "<title>Thad Sphere setup</title><style>"
        "body{font-family:-apple-system,'Segoe UI',Helvetica,Arial,sans-serif;background:#080b0a;color:#eef;margin:0;padding:24px}"
        "h1{font-size:22px;color:#00C972}label{display:block;margin:16px 0 6px;font-weight:600}"
        "input,select{width:100%;box-sizing:border-box;padding:12px;font-size:17px;border-radius:10px;border:1px solid #2b4538;background:#101915;color:#fff}"
        "small{color:#9ab}button{margin-top:22px;width:100%;padding:14px;font-size:18px;border:0;border-radius:12px;background:#00C972;color:#000;font-weight:700}"
        ".sec{background:#18241e;color:#cfe;border:1px solid #2b4538;margin-top:14px;font-size:15px}"
        ".cb{display:flex;gap:10px;align-items:center}.cb input{width:auto}"
        "</style></head><body>";

    size_t used = 0;
    char esc[200];
    if (!out || cap < 2) return -1;
    out[0] = 0;
    if (append(out, cap, &used, head) < 0) return -1;
    if (append(out, cap, &used,
        "<h1>Thad Sphere setup</h1><form method=post action=/save>"
        "<label>Home Wi-Fi name</label><input name=ssid list=nets autocomplete=off required value=\"") < 0) return -1;
    net_html_escape(ssid, esc, sizeof(esc));
    if (append(out, cap, &used, esc) < 0) return -1;
    if (append(out, cap, &used, "\"><datalist id=nets>") < 0) return -1;
    for (int i = 0; i < scan_n; i++) {
        net_html_escape(scan_ssids && scan_ssids[i] ? scan_ssids[i] : "", esc, sizeof(esc));
        if (!esc[0]) continue;
        char line[260];
        snprintf(line, sizeof(line), "<option value=\"%s\">", esc);
        if (append(out, cap, &used, line) < 0) return -1;
    }
    if (append(out, cap, &used,
        "</datalist><small>2.4 GHz networks only. Tap the box to pick from nearby networks.</small>"
        "<label>Wi-Fi password</label><input name=pass type=password autocomplete=off>"
        "<small>Leave empty to keep the saved password (or for an open network).</small>"
        "<label>Windows PC address (optional)</label><input name=host placeholder='leave empty = find automatically' value=\"") < 0) return -1;
    net_html_escape(bridge_host, esc, sizeof(esc));
    if (append(out, cap, &used, esc) < 0) return -1;
    char port[220];
    snprintf(port, sizeof(port),
             "\"><small>Example: Bryan-Laptop.local or 192.168.1.20 (used when Wi-Fi fallback is active)</small>"
             "<label>Companion port</label><input name=port type=number value=%u>",
             bridge_port);
    if (append(out, cap, &used, port) < 0) return -1;
    if (append(out, cap, &used, mirror_x
        ? "<label class=cb><input type=checkbox name=mirror value=1 checked>Flip the screen (only if text looks mirrored)</label>"
        : "<label class=cb><input type=checkbox name=mirror value=1>Flip the screen (only if text looks mirrored)</label>") < 0) return -1;
    if (append(out, cap, &used,
        "<button type=submit>Save and restart</button></form>"
        "<form method=post action=/flash><button class=sec type=submit>Enter USB Flash Mode</button></form>"
        "</body></html>") < 0) return -1;
    return (int)used;
}
