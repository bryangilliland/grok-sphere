/* PC-side unit tests for the pure-C parts of the firmware (protocol + setup form + USB framing). */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "protocol.h"
#include "formutil.h"
#include "usb_frame.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static void test_state(void)
{
    proto_msg_t m;
    const char *j = "{\"type\":\"state\",\"state\":\"thinking\",\"label\":\"Thad\",\"text\":\"On it\"}";
    CHECK(proto_parse(j, strlen(j), &m));
    CHECK(m.type == MSG_STATE && m.state == UI_STATE_THINKING);
    CHECK(strcmp(m.label, "Thad") == 0 && strcmp(m.text, "On it") == 0);
    for (int s = 0; s < UI_STATE_OFFLINE; s++) {
        char buf[96];
        snprintf(buf, sizeof(buf), "{\"type\":\"state\",\"state\":\"%s\"}", proto_state_name((ui_state_t)s));
        CHECK(proto_parse(buf, strlen(buf), &m) && m.state == (ui_state_t)s);
    }
    j = "{\"type\":\"state\",\"state\":\"offline\"}";
    CHECK(!proto_parse(j, strlen(j), &m) && m.type == MSG_UNKNOWN);
    j = "{\"type\":\"state\",\"state\":\"dancing\"}";
    CHECK(!proto_parse(j, strlen(j), &m));
    char longlabel[300] = "{\"type\":\"state\",\"state\":\"idle\",\"label\":\"";
    for (int i = 0; i < 200; i++) strcat(longlabel, "x");
    strcat(longlabel, "\"}");
    CHECK(proto_parse(longlabel, strlen(longlabel), &m) && strlen(m.label) == sizeof(m.label) - 1);
}

static void test_audio_and_misc(void)
{
    proto_msg_t m;
    const char *j = "{\"type\":\"audio_start\",\"sample_rate\":16000,\"format\":\"pcm_s16le\",\"channels\":1}";
    CHECK(proto_parse(j, strlen(j), &m) && m.type == MSG_AUDIO_START && m.sample_rate == 16000);
    j = "{\"type\":\"audio_start\",\"sample_rate\":22050}";
    CHECK(!proto_parse(j, strlen(j), &m));
    j = "{\"type\":\"audio_start\",\"channels\":2}";
    CHECK(!proto_parse(j, strlen(j), &m));
    j = "{\"type\":\"audio_end\"}";
    CHECK(proto_parse(j, strlen(j), &m) && m.type == MSG_AUDIO_END);
    j = "{\"type\":\"volume\",\"value\":150}";
    CHECK(proto_parse(j, strlen(j), &m) && m.type == MSG_VOLUME && m.value == 100);
    j = "{\"type\":\"volume\",\"value\":-3}";
    CHECK(proto_parse(j, strlen(j), &m) && m.value == 0);
    j = "{\"type\":\"screen\",\"screen\":\"clock\"}";
    CHECK(proto_parse(j, strlen(j), &m) && m.type == MSG_SCREEN && m.screen == SCREEN_CLOCK);
    j = "{\"type\":\"screen\",\"screen\":\"tv\"}";
    CHECK(!proto_parse(j, strlen(j), &m));
    j = "{\"type\":\"chime\"}";
    CHECK(proto_parse(j, strlen(j), &m) && m.type == MSG_CHIME);
    j = "{\"type\":\"ping\"}";
    CHECK(proto_parse(j, strlen(j), &m) && m.type == MSG_PING);
    j = "{\"type\":\"stop\"}";
    CHECK(proto_parse(j, strlen(j), &m) && m.type == MSG_STOP);
    j = "not json";
    CHECK(!proto_parse(j, strlen(j), &m));
    j = "[1,2]";
    CHECK(!proto_parse(j, strlen(j), &m));
    CHECK(!proto_parse(NULL, 0, &m));
}

static void test_builders(void)
{
    char buf[200];
    int n = proto_build_hello(buf, sizeof(buf), "1.2.0-thad", "v2-es8311", "AA:BB");
    CHECK(n > 0 && strstr(buf, "\"type\":\"hello\"") && strstr(buf, "\"sample_rate\":16000"));
    n = proto_build_touch(buf, sizeof(buf), "ptt_start");
    CHECK(n > 0 && strcmp(buf, "{\"type\":\"touch\",\"action\":\"ptt_start\"}") == 0);
    n = proto_build_simple(buf, sizeof(buf), "playback_done");
    CHECK(n > 0 && strcmp(buf, "{\"type\":\"playback_done\"}") == 0);
    char tiny[8];
    CHECK(proto_build_hello(tiny, sizeof(tiny), "1", "2", "3") == -1);
}

static void test_form(void)
{
    char out[33];
    const char *body = "ssid=My+Home%20WiFi&pass=p%26ss%3Dw0rd&host=&port=8765&mirror=1";
    CHECK(net_form_get(body, "ssid", out, sizeof(out)) && strcmp(out, "My Home WiFi") == 0);
    char pass[65];
    CHECK(net_form_get(body, "pass", pass, sizeof(pass)) && strcmp(pass, "p&ss=w0rd") == 0);
    CHECK(net_form_get(body, "host", out, sizeof(out)) && out[0] == 0);
    CHECK(net_form_get(body, "mirror", out, sizeof(out)) && strcmp(out, "1") == 0);
    CHECK(!net_form_get(body, "missing", out, sizeof(out)) && out[0] == 0);
    CHECK(!net_form_get("xssid=1", "ssid", out, sizeof(out)));
    char small[5];
    CHECK(net_form_get("ssid=abcdefghij", "ssid", small, sizeof(small)) && strcmp(small, "abcd") == 0);
    char s[] = "100%25+bad%zz";
    net_url_decode(s);
    CHECK(strcmp(s, "100% bad%zz") == 0);
}

static void test_setup_page(void)
{
    char page[8192];
    int n = net_setup_page_build(page, sizeof(page), "", "", 8765, false, NULL, 0);
    CHECK(n > 0 && (size_t)n == strlen(page));
    CHECK(strstr(page, "<h1>Thad Sphere setup</h1>"));
    CHECK(strstr(page, "<label>Home Wi-Fi name</label><input name=ssid list=nets autocomplete=off required value=\"\">"));
    CHECK(strstr(page, "<label>Windows PC address (optional)</label>"));
    CHECK(strstr(page, "action=/flash"));
    CHECK(strstr(page, "</form></body></html>"));

    const char *scans[] = { "Cafe <A&B>", "", "Home" };
    n = net_setup_page_build(page, sizeof(page), "A&B\"ssid", "host>x", 8765, true, scans, 3);
    CHECK(n > 0);
    CHECK(strstr(page, "value=\"A&amp;B&quot;ssid\""));
    CHECK(strstr(page, "value=\"host&gt;x\""));
    CHECK(strstr(page, "checked>Flip the screen"));
}

typedef struct {
    int count;
    uint8_t last_type;
    uint16_t last_len;
    uint8_t last_payload[256];
} usb_cap_t;

static void on_usb_cap(uint8_t type, const uint8_t *payload, uint16_t len, void *ctx)
{
    usb_cap_t *c = ctx;
    c->count++;
    c->last_type = type;
    c->last_len = len;
    if (len <= sizeof(c->last_payload)) memcpy(c->last_payload, payload, len);
}

static void test_usb_frame(void)
{
    uint8_t buf[256];
    const char *msg = "{\"type\":\"ping\"}";
    int n = usb_frame_encode(buf, sizeof(buf), USB_FRAME_JSON, msg, strlen(msg));
    CHECK(n == (int)(strlen(msg) + USB_FRAME_OVERHEAD));

    /* Feed 1 byte at a time with noisy prefix bytes (e.g. boot log) */
    usb_frame_rx_t rx;
    usb_frame_rx_init(&rx);
    usb_cap_t cap = {0};
    const uint8_t noise[] = "ESP-ROM:esp32s3-20210327\r\nGSx";
    usb_frame_rx_feed(&rx, noise, sizeof(noise) - 1, on_usb_cap, &cap);
    CHECK(cap.count == 0);
    for (int i = 0; i < n; i++) {
        usb_frame_rx_feed(&rx, &buf[i], 1, on_usb_cap, &cap);
    }
    CHECK(cap.count == 1 && cap.last_type == USB_FRAME_JSON && cap.last_len == strlen(msg));
    CHECK(memcmp(cap.last_payload, msg, strlen(msg)) == 0);

    /* Corrupt CRC must be rejected, and subsequent valid frame must still decode */
    uint8_t bad[256];
    memcpy(bad, buf, (size_t)n);
    bad[n - 1] ^= 0xFF;
    usb_frame_rx_feed(&rx, bad, (size_t)n, on_usb_cap, &cap);
    CHECK(cap.count == 1);

    uint8_t pcm_payload[8] = {1, 2, 3, 4, 255, 128, 0, 127};
    int np = usb_frame_encode(buf, sizeof(buf), USB_FRAME_PCM, pcm_payload, sizeof(pcm_payload));
    CHECK(np > 0);
    usb_frame_rx_feed(&rx, buf, (size_t)np, on_usb_cap, &cap);
    CHECK(cap.count == 2 && cap.last_type == USB_FRAME_PCM && cap.last_len == sizeof(pcm_payload));
    CHECK(memcmp(cap.last_payload, pcm_payload, sizeof(pcm_payload)) == 0);

    /* Bounds checks */
    CHECK(usb_frame_encode(buf, 5, USB_FRAME_JSON, msg, strlen(msg)) == -1);
    CHECK(usb_frame_encode(buf, sizeof(buf), 99, msg, strlen(msg)) == -1);
    CHECK(usb_frame_encode(buf, sizeof(buf), USB_FRAME_JSON, msg, USB_FRAME_MAX_PAYLOAD + 1) == -1);
}

int main(void)
{
    test_state();
    test_audio_and_misc();
    test_builders();
    test_form();
    test_setup_page();
    test_usb_frame();
    if (fails) { printf("%d host test(s) FAILED\n", fails); return 1; }
    printf("all firmware host tests passed\n");
    return 0;
}
