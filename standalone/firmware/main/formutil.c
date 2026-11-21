/* Form parsing and setup page builder. Pure C so it is unit-tested on a PC (host_test/). */
#include "formutil.h"
#include "assist_util.h"
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
            char tmp[1600];
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

static int append_esc(char *out, size_t cap, size_t *used, const char *s)
{
    char esc[700];
    net_html_escape(s ? s : "", esc, sizeof(esc));
    return append(out, cap, used, esc);
}

#define A(x) do { if (append(out, cap, &used, (x)) < 0) return -1; } while (0)
#define E(x) do { if (append_esc(out, cap, &used, (x)) < 0) return -1; } while (0)

int net_setup_page_build(char *out, size_t cap, const setup_page_info_t *in)
{
    static const char head[] =
        "<!doctype html><html><head><meta charset=utf-8>"
        "<meta name=viewport content='width=device-width,initial-scale=1'>"
        "<title>Grok Sphere</title><style>"
        "body{font-family:-apple-system,'Segoe UI',Helvetica,Arial,sans-serif;background:#000;color:#EAF2FF;margin:0;padding:20px;max-width:520px}"
        "h1{font-size:22px;margin:0 0 4px}h2{font-size:17px;color:#9AA7FF;margin:26px 0 4px}"
        ".eyes{display:flex;gap:16px;margin:6px 0 12px}.eyes i{width:22px;height:36px;border-radius:11px;background:#EAF2FF;display:block}"
        "label{display:block;margin:14px 0 6px;font-weight:600}"
        "input,select,textarea{width:100%;box-sizing:border-box;padding:12px;font-size:17px;border-radius:10px;border:1px solid #2a2f3a;background:#0d1016;color:#fff}"
        "small{color:#8A93A3;display:block;margin-top:4px}"
        "button{margin-top:18px;width:100%;padding:14px;font-size:18px;border:0;border-radius:12px;background:#EAF2FF;color:#000;font-weight:700}"
        ".sec{background:#151922;color:#C8CED8;border:1px solid #2a2f3a;font-size:15px}"
        ".ok{background:#10251a;border:1px solid #1f5c3a;color:#bff5d3;padding:10px;border-radius:10px}"
        ".card{background:#0d1016;border:1px solid #2a2f3a;border-radius:12px;padding:12px;margin-top:10px}"
        ".cb{display:flex;gap:10px;align-items:center}.cb input{width:auto}"
        "a{color:#3DDCFF}"
        "</style></head><body><div class=eyes><i></i><i></i></div><h1>Grok Sphere setup</h1>";

    size_t used = 0;
    char line[320];
    if (!out || cap < 2 || !in) return -1;
    out[0] = 0;
    A(head);
    if (in->notice && in->notice[0]) { A("<p class=ok>"); E(in->notice); A("</p>"); }
    if (in->wifi_connected && in->ip && in->ip[0]) {
        A("<small>On your home Wi-Fi at <b>http://"); E(in->ip);
        A("</b> (also http://grok-sphere.local)</small>");
    }

    /* Ask box: works without a speech key; phone keyboard dictation (mic key) works here too. */
    if (in->api_key_masked && in->api_key_masked[0]) {
        A("<h2>Ask Thad</h2><form method=post action=/ask>"
          "<textarea name=q rows=2 maxlength=500 placeholder='Type, or tap your keyboard mic and speak'></textarea>"
          "<button type=submit>Ask</button></form>");
        if (in->last_q && in->last_q[0]) {
            A("<div class=card><small>You asked</small>"); E(in->last_q);
            A("<small>Thad</small>"); E(in->last_a && in->last_a[0] ? in->last_a : "(thinking...)"); A("</div>");
            if (!in->last_a || !in->last_a[0]) A("<script>setTimeout(function(){location.replace('/')},3000)</script>");
        }
    }

    A("<form method=post action=/save>");
    A("<h2>1. Claude key</h2><label>Anthropic API key</label>"
      "<input name=akey type=password autocomplete=off autocapitalize=off spellcheck=false placeholder='");
    if (in->api_key_masked && in->api_key_masked[0]) { A("saved "); E(in->api_key_masked); A(" - leave empty to keep"); }
    else A("sk-ant-...");
    A("'><small>From console.anthropic.com &rarr; API keys. Stored only on the sphere; never shown again.</small>");
    A("<label>Model</label><select name=model>");
    A(in->model == 1 ? "<option value=0>Haiku 5.5 (fastest, cheapest)</option><option value=1 selected>Sonnet 5.5 (smarter)</option>"
                     : "<option value=0 selected>Haiku 5.5 (fastest, cheapest)</option><option value=1>Sonnet 5.5 (smarter)</option>");
    A("</select>");

    A("<h2>2. Voice (optional)</h2><label>Deepgram API key</label>"
      "<input name=dgkey type=password autocomplete=off autocapitalize=off spellcheck=false placeholder='");
    if (in->speech_key_masked && in->speech_key_masked[0]) { A("saved "); E(in->speech_key_masked); A(" - leave empty to keep"); }
    else A("needed for talking out loud");
    A("'><small>Claude can't hear or speak by itself. A free Deepgram key (console.deepgram.com, $200 free credit, no card) "
      "lets the sphere listen and talk. Without it, type above and read the answer on the sphere.</small>");
    if (in->speech_key_masked && in->speech_key_masked[0])
        A("<label class=cb><input type=checkbox name=dgclear value=1>Remove the saved Deepgram key</label>");
    A("<label>Thad's voice</label><select name=voice>");
    for (int i = 0; i < AU_VOICE_COUNT; i++) {
        snprintf(line, sizeof(line), "<option value=\"%s\"%s>%s</option>", AU_VOICES[i].id,
                 (in->voice && strcmp(in->voice, AU_VOICES[i].id) == 0) ? " selected" : "", AU_VOICES[i].label);
        A(line);
    }
    A("</select>");
    snprintf(line, sizeof(line), "<label>Volume</label><input name=vol type=range min=10 max=100 value=%d>",
             in->volume < 10 ? 10 : (in->volume > 100 ? 100 : in->volume));
    A(line);

    A("<h2>3. Home Wi-Fi</h2><label>Wi-Fi name</label><input name=ssid list=nets autocomplete=off autocapitalize=off value=\"");
    E(in->ssid);
    A("\"><datalist id=nets>");
    for (int i = 0; i < in->scan_n; i++) {
        const char *s = in->scan_ssids ? in->scan_ssids[i] : NULL;
        if (!s || !s[0]) continue;
        A("<option value=\""); E(s); A("\">");
    }
    A("</datalist><small>2.4 GHz only. Already saved? Leave it as is.</small>"
      "<label>Wi-Fi password</label><input name=pass type=password autocomplete=off>"
      "<small>Leave empty to keep the saved password.</small>");
    A(in->mirror_x ? "<label class=cb><input type=checkbox name=mirror value=1 checked>Flip the screen (only if text looks mirrored)</label>"
                   : "<label class=cb><input type=checkbox name=mirror value=1>Flip the screen (only if text looks mirrored)</label>");
    A("<button type=submit>Save</button></form>");

    A("<h2>Using it</h2><small>Tap the screen (or press BOOT) and talk; it stops when you pause. "
      "Tap again to stop early or to cut Thad off. Touch and hold for the clock. "
      "Hold BOOT about 4 seconds to bring this setup hotspot back.</small>");
    A("<form method=post action=/flash><button class=sec type=submit>USB flash mode</button></form>"
      "<form method=post action=/reset onsubmit=\"return confirm('Erase Wi-Fi and keys from the sphere?')\">"
      "<button class=sec type=submit>Forget Wi-Fi and keys</button></form>");
    A("</body></html>");
    return (int)used;
}
