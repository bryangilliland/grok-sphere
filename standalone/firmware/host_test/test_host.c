/* PC-side unit tests for the standalone firmware's pure-C parts:
 * setup page + form parsing, key masking, Claude/Deepgram request+response handling,
 * reply cleanup for speech/screen, WAV header and the push-to-talk voice detector. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "assist_util.h"
#include "cJSON.h"
#include "formutil.h"
#include "protocol.h"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static const char *FAKE_KEY = "sk-ant-api03-FAKEFAKEFAKEFAKEFAKEFAKEFAKEFAKEFAKEFAKEFAKEFAKEFAKEFAKEFAKEFAKEFAKEFAKE-xY9z";

static void test_mask_and_trim(void)
{
    char out[16];
    au_mask_key(FAKE_KEY, out, sizeof(out));
    CHECK(strcmp(out, "****xY9z") == 0);
    au_mask_key("", out, sizeof(out));
    CHECK(out[0] == 0);
    au_mask_key("abc", out, sizeof(out));
    CHECK(strcmp(out, "****") == 0);          /* short keys reveal nothing */
    char t[64] = "  \n sk-ant-123 \r\n";
    au_trim(t);
    CHECK(strcmp(t, "sk-ant-123") == 0);
}

static void test_form(void)
{
    char out[300];
    const char *body = "akey=sk-ant-api03-a%2Bb%2Fc&ssid=My+Home&pass=p%26ss%3D1&model=1&vol=55&q=what%27s+up%3F";
    CHECK(net_form_get(body, "akey", out, sizeof(out)) && strcmp(out, "sk-ant-api03-a+b/c") == 0);
    CHECK(net_form_get(body, "ssid", out, sizeof(out)) && strcmp(out, "My Home") == 0);
    CHECK(net_form_get(body, "pass", out, sizeof(out)) && strcmp(out, "p&ss=1") == 0);
    CHECK(net_form_get(body, "q", out, sizeof(out)) && strcmp(out, "what's up?") == 0);
    CHECK(!net_form_get(body, "dgkey", out, sizeof(out)) && out[0] == 0);
    /* a full-length Anthropic key survives the form parser intact */
    char enc[400];
    snprintf(enc, sizeof(enc), "x=1&akey=%s&y=2", FAKE_KEY);
    char key[200];
    CHECK(net_form_get(enc, "akey", key, sizeof(key)) && strcmp(key, FAKE_KEY) == 0);
    char esc[64];
    net_html_escape("<a href=\"x\">&'", esc, sizeof(esc));
    CHECK(strcmp(esc, "&lt;a href=&quot;x&quot;&gt;&amp;&#39;") == 0);
}

static void test_page(void)
{
    char *page = malloc(20480);
    const char *scans[] = { "HomeNet", "<evil>", "" };
    char masked[16];
    au_mask_key(FAKE_KEY, masked, sizeof(masked));
    setup_page_info_t in = {
        .ssid = "HomeNet", .wifi_connected = true, .ip = "192.168.1.50",
        .api_key_masked = masked, .speech_key_masked = "", .model = 0, .voice = "aura-2-orion-en",
        .volume = 70, .wake_enabled = true, .wake_word = "wn9_hiesp",
        .chime_enabled = false, .scan_ssids = scans, .scan_n = 3,
        .last_q = "What's 2+2?", .last_a = "Four.", .notice = "Saved.",
        .diag_err = "None", .diag_mic_peak = 3200, .diag_mic_rms = 850,
        .diag_rssi = -56, .diag_free_heap = 7200 * 1024, .diag_free_int = 138 * 1024,
        .diag_wake = "on (Jarvis)",
    };
    int n = net_setup_page_build(page, 20480, &in);
    CHECK(n > 1000 && (size_t)n == strlen(page));
    CHECK(strstr(page, "****xY9z") != NULL);
    CHECK(strstr(page, "FAKEFAKE") == NULL);                 /* never the key itself */
    CHECK(strstr(page, "&lt;evil&gt;") && !strstr(page, "<evil>"));
    CHECK(strstr(page, "192.168.1.50") && strstr(page, "grok-sphere.local"));
    CHECK(strstr(page, "action=/ask") && strstr(page, "What&#39;s 2+2?") && strstr(page, "Four."));
    CHECK(strstr(page, "<option value=0 selected>Haiku"));
    CHECK(strstr(page, "value=\"aura-2-orion-en\" selected"));
    CHECK(strstr(page, "Orion (calm male, US)"));
    CHECK(strstr(page, "value=\"aura-2-draco-en\"") && strstr(page, "Draco (warm male, British)"));
    CHECK(strstr(page, "value=\"aura-2-amalthea-en\"") && strstr(page, "Amalthea (cheerful female, Filipino)"));
    CHECK(strstr(page, "formaction=/preview") && strstr(page, "Preview this voice"));
    CHECK(strstr(page, "name=wakeon value=1 checked"));
    CHECK(strstr(page, "value=\"wn9_hiesp\" selected>Hi, ESP"));
    CHECK(strstr(page, "value=\"wn9_alexa\"") && strstr(page, "value=\"wn9_jarvis_tts\"") && strstr(page, "value=\"wn9_computer_tts\""));
    CHECK(!strstr(page, "name=chime value=1 checked"));
    CHECK(strstr(page, "action=/moods") && strstr(page, "Preview moods"));
    CHECK(strstr(page, "Diagnostics") && strstr(page, "peak 3200 / rms 850") && strstr(page, "-56 dBm") && strstr(page, "7200 KB"));
    CHECK(strstr(page, "name=dgkey") && strstr(page, "console.deepgram.com"));
    CHECK(strstr(page, "type=password") != NULL);
    /* first boot: no key -> no ask box, Sonnet selected shows */
    setup_page_info_t first = { .ssid = "", .model = 1, .voice = "aura-2-thalia-en", .volume = 70, .wake_enabled = false, .wake_word = "wn9_jarvis_tts" };
    n = net_setup_page_build(page, 20480, &first);
    CHECK(n > 0 && !strstr(page, "action=/ask") && strstr(page, "sk-ant-...") && strstr(page, "<option value=1 selected>Sonnet"));
    CHECK(!strstr(page, "name=wakeon value=1 checked") && strstr(page, "value=\"wn9_jarvis_tts\" selected>Jarvis"));
    /* thinking... auto refresh */
    in.last_a = "";
    n = net_setup_page_build(page, 20480, &in);
    CHECK(n > 0 && strstr(page, "(thinking...)") && strstr(page, "location.replace"));
    CHECK(net_setup_page_build(page, 200, &in) == -1);       /* too small -> error, no overflow */
    free(page);
}

static void test_models_voices(void)
{
    CHECK(strcmp(au_model_id(0), "claude-haiku-5-5") == 0);
    CHECK(strcmp(au_model_id(1), "claude-sonnet-5-5") == 0);
    CHECK(strcmp(au_model_id(7), "claude-haiku-5-5") == 0);
    CHECK(au_thinking_mode_for("claude-haiku-5-5") == 1);
    CHECK(au_thinking_mode_for("claude-sonnet-5-5") == 2);
    CHECK(strcmp(AU_DEFAULT_VOICE, "aura-2-orion-en") == 0);
    CHECK(au_voice_valid(AU_DEFAULT_VOICE) && !au_voice_valid("aura-2-evil\"><script>") && !au_voice_valid(NULL));
    CHECK(AU_VOICE_COUNT >= 15);
    CHECK(strcmp(AU_VOICES[0].id, AU_DEFAULT_VOICE) == 0);
    /* every catalog id is a real Aura-2 English model (docs: developers.deepgram.com/docs/tts-models) */
    static const char *required[] = {
        "aura-2-orion-en", "aura-2-apollo-en", "aura-2-arcas-en", "aura-2-aries-en", "aura-2-zeus-en",
        "aura-2-draco-en", "aura-2-hyperion-en", "aura-2-thalia-en", "aura-2-helena-en",
        "aura-2-andromeda-en", "aura-2-athena-en", "aura-2-luna-en", "aura-2-pandora-en",
        "aura-2-theia-en", "aura-2-amalthea-en",
    };
    for (int r = 0; r < (int)(sizeof(required) / sizeof(required[0])); r++)
        CHECK(au_voice_valid(required[r]));
    /* ids are safe to drop into HTML attributes; labels are unique and mention gender */
    for (int i = 0; i < AU_VOICE_COUNT; i++) {
        CHECK(strncmp(AU_VOICES[i].id, "aura-2-", 7) == 0);
        CHECK(strchr(AU_VOICES[i].id, '"') == NULL && strchr(AU_VOICES[i].id, '<') == NULL);
        CHECK(strstr(AU_VOICES[i].label, "male") != NULL || strstr(AU_VOICES[i].label, "female") != NULL);
        for (int j = i + 1; j < AU_VOICE_COUNT; j++)
            CHECK(strcmp(AU_VOICES[i].id, AU_VOICES[j].id) != 0);
    }
    CHECK(strstr(AU_PREVIEW_TEXT, "Thad") != NULL);
    /* wake word catalog */
    CHECK(strcmp(AU_DEFAULT_WAKE, "wn9_hiesp") == 0);
    CHECK(AU_WAKE_COUNT == 4);
    CHECK(au_wake_valid("wn9_hiesp") && au_wake_valid("wn9_alexa") && au_wake_valid("wn9_jarvis_tts") && au_wake_valid("wn9_computer_tts"));
    CHECK(!au_wake_valid("wn9_nope") && !au_wake_valid(NULL));
    CHECK(strstr(au_wake_label("wn9_hiesp"), "Hi, ESP") != NULL);
    CHECK(strcmp(au_wake_label("wn9_jarvis_tts"), "Jarvis") == 0);
}

static void test_claude_request(void)
{
    au_turn_t hist[] = { { "user", "Hi" }, { "assistant", "Hey Bryan." } };
    char *r = au_claude_request("claude-haiku-5-5", 600, "You are \"Thad\".", hist, 2, "Line1\nLine \"2\"", 1);
    CHECK(r != NULL);
    cJSON *j = cJSON_Parse(r);
    CHECK(j != NULL);
    CHECK(strcmp(cJSON_GetObjectItem(j, "model")->valuestring, "claude-haiku-5-5") == 0);
    CHECK(cJSON_GetObjectItem(j, "max_tokens")->valueint == 600);
    CHECK(strcmp(cJSON_GetObjectItem(j, "system")->valuestring, "You are \"Thad\".") == 0);
    CHECK(strcmp(cJSON_GetObjectItem(cJSON_GetObjectItem(j, "thinking"), "type")->valuestring, "disabled") == 0);
    CHECK(strcmp(cJSON_GetObjectItem(cJSON_GetObjectItem(j, "output_config"), "effort")->valuestring, "low") == 0);
    CHECK(cJSON_GetObjectItem(j, "temperature") == NULL);   /* removed in API 2026-10-21 */
    cJSON *msgs = cJSON_GetObjectItem(j, "messages");
    CHECK(cJSON_GetArraySize(msgs) == 3);
    cJSON *last = cJSON_GetArrayItem(msgs, 2);
    CHECK(strcmp(cJSON_GetObjectItem(last, "role")->valuestring, "user") == 0);
    CHECK(strcmp(cJSON_GetObjectItem(last, "content")->valuestring, "Line1\nLine \"2\"") == 0);
    cJSON_Delete(j);
    free(r);
    r = au_claude_request("claude-sonnet-5-5", 600, "", NULL, 0, "x", 2);
    j = cJSON_Parse(r);
    CHECK(strcmp(cJSON_GetObjectItem(cJSON_GetObjectItem(j, "thinking"), "type")->valuestring, "between_tools") == 0);
    CHECK(cJSON_GetObjectItem(j, "system") == NULL);
    cJSON_Delete(j);
    free(r);
    r = au_claude_request("claude-sonnet-5-5", 600, "s", NULL, 0, "x", 0);   /* fallback: plain */
    j = cJSON_Parse(r);
    CHECK(cJSON_GetObjectItem(j, "thinking") == NULL && cJSON_GetObjectItem(j, "output_config") == NULL);
    cJSON_Delete(j);
    free(r);
}

static void test_claude_parse(void)
{
    char out[256], err[128];
    const char *ok = "{\"id\":\"msg_1\",\"type\":\"message\",\"role\":\"assistant\",\"content\":["
                     "{\"type\":\"thinking\",\"thinking\":\"hmm\",\"signature\":\"x\"},"
                     "{\"type\":\"text\",\"text\":\"It's 72 and sunny.\"},"
                     "{\"type\":\"text\",\"text\":\"Enjoy!\"}],\"stop_reason\":\"end_turn\"}";
    CHECK(au_parse_claude(ok, out, sizeof(out), err, sizeof(err)));
    CHECK(strcmp(out, "It's 72 and sunny. Enjoy!") == 0);
    const char *e = "{\"type\":\"error\",\"error\":{\"type\":\"authentication_error\",\"message\":\"invalid x-api-key\"}}";
    CHECK(!au_parse_claude(e, out, sizeof(out), err, sizeof(err)) && strcmp(err, "invalid x-api-key") == 0);
    CHECK(!au_parse_claude("not json", out, sizeof(out), err, sizeof(err)) && err[0]);
    CHECK(!au_parse_claude("{\"type\":\"message\",\"content\":[]}", out, sizeof(out), err, sizeof(err)));
    char tiny[8];
    CHECK(au_parse_claude(ok, tiny, sizeof(tiny), err, sizeof(err)) && strlen(tiny) == 7);
}

static void test_deepgram(void)
{
    char out[128];
    const char *j = "{\"metadata\":{},\"results\":{\"channels\":[{\"alternatives\":[{\"transcript\":\" What's the weather tomorrow? \",\"confidence\":0.98}]}]}}";
    CHECK(au_parse_deepgram(j, out, sizeof(out)) && strcmp(out, "What's the weather tomorrow?") == 0);
    CHECK(au_parse_deepgram("{\"results\":{\"channels\":[{\"alternatives\":[{\"transcript\":\"\"}]}]}}", out, sizeof(out)) && out[0] == 0);
    CHECK(!au_parse_deepgram("{\"err_code\":\"INVALID_AUTH\"}", out, sizeof(out)));
    au_parse_error_message("{\"err_code\":\"INVALID_AUTH\",\"err_msg\":\"Invalid credentials.\"}", out, sizeof(out));
    CHECK(strcmp(out, "Invalid credentials.") == 0);
    au_parse_error_message("{\"category\":\"x\",\"message\":\"Bad text\"}", out, sizeof(out));
    CHECK(strcmp(out, "Bad text") == 0);
    char *b = au_tts_body("Say \"hi\"\n");
    cJSON *bj = cJSON_Parse(b);
    CHECK(bj && strcmp(cJSON_GetObjectItem(bj, "text")->valuestring, "Say \"hi\"\n") == 0);
    cJSON_Delete(bj);
    free(b);
}

static void test_speech_clean(void)
{
    char out[256];
    au_speech_clean("## Plan\n- **Buy** milk\n- Call `Bob`\n1. Done", out, sizeof(out), 0);
    CHECK(strcmp(out, "Plan Buy milk Call Bob Done") == 0);
    au_speech_clean("It\xE2\x80\x99s \xE2\x80\x9C" "fine\xE2\x80\x9D \xE2\x80\x94 really\xE2\x80\xA6", out, sizeof(out), 0);
    CHECK(strcmp(out, "It's \"fine\" - really...") == 0);
    au_speech_clean("One sentence here. Two sentence here. Three sentence here.", out, sizeof(out), 40);
    CHECK(strcmp(out, "One sentence here. Two sentence here.") == 0);
    au_speech_clean("averyveryverylongwordwithoutanyspaces andmore", out, sizeof(out), 10);
    CHECK(strlen(out) <= 10);
    au_speech_clean("   ", out, sizeof(out), 0);
    CHECK(out[0] == 0);
    au_speech_clean("72\xC2\xB0" "F today", out, sizeof(out), 0);
    CHECK(strcmp(out, "72\xC2\xB0" "F today") == 0);    /* degree sign kept */
}

static void test_wav(void)
{
    uint8_t h[44];
    au_wav_header(h, 16000, 16000);
    CHECK(memcmp(h, "RIFF", 4) == 0 && memcmp(h + 8, "WAVEfmt ", 8) == 0 && memcmp(h + 36, "data", 4) == 0);
    CHECK(h[40] == 0x00 && h[41] == 0x7D && h[42] == 0 && h[43] == 0);   /* 32000 data bytes */
    CHECK(h[24] == 0x80 && h[25] == 0x3E);                               /* 16000 Hz */
    CHECK(h[34] == 16 && h[22] == 1);
}

static void test_prompt(void)
{
    char p[1200];
    int n = au_system_prompt(p, sizeof(p), "Saturday, November 21, 2026, 11:30 AM");
    CHECK(n > 200 && n < (int)sizeof(p));
    CHECK(strstr(p, "Thad") && strstr(p, "Fort Worth") && strstr(p, "November 21, 2026") && strstr(p, "no markdown"));
    au_system_prompt(p, sizeof(p), "");
    CHECK(!strstr(p, "Current local date"));
}

static vad_result_t run_vad(const int *pattern, int n, int *frames_used)
{
    au_vad_t v;
    au_vad_init(&v, 32);
    vad_result_t r = AU_VAD_WAITING;
    for (int i = 0; i < n; i++) {
        r = au_vad_feed(&v, (uint32_t)pattern[i]);
        *frames_used = i + 1;
        if (r == AU_VAD_DONE || r == AU_VAD_NO_SPEECH || r == AU_VAD_MAX_LEN) break;
    }
    return r;
}

static void test_mood(void)
{
    char buf[128];
    strcpy(buf, "[mood:happy] Good morning Bryan.");
    CHECK(au_extract_mood(buf) == AU_MOOD_HAPPY && strcmp(buf, "Good morning Bryan.") == 0);
    strcpy(buf, "  [MOOD:Curious]  Wait, really?");
    CHECK(au_extract_mood(buf) == AU_MOOD_CURIOUS && strcmp(buf, "Wait, really?") == 0);
    strcpy(buf, "[mood:surprised]\nWhoa!");
    CHECK(au_extract_mood(buf) == AU_MOOD_SURPRISED && strcmp(buf, "Whoa!") == 0);
    strcpy(buf, "[mood:sad] Sorry about that.");
    CHECK(au_extract_mood(buf) == AU_MOOD_SAD && strcmp(buf, "Sorry about that.") == 0);
    strcpy(buf, "[mood:bored] Nothing new.");
    CHECK(au_extract_mood(buf) == AU_MOOD_BORED && strcmp(buf, "Nothing new.") == 0);
    strcpy(buf, "[mood:sleepy] Yawn.");
    CHECK(au_extract_mood(buf) == AU_MOOD_SLEEPY && strcmp(buf, "Yawn.") == 0);
    strcpy(buf, "[mood:unknown] Still clean.");
    CHECK(au_extract_mood(buf) == AU_MOOD_NONE && strcmp(buf, "Still clean.") == 0);
    strcpy(buf, "No mood tag here.");
    CHECK(au_extract_mood(buf) == AU_MOOD_NONE && strcmp(buf, "No mood tag here.") == 0);
}

static void test_vad(void)
{
    int pat[600], used;
    int16_t pcm[4] = { 1000, -1000, 1000, -1000 };
    CHECK(au_rms(pcm, 4) == 1000 && au_rms(NULL, 0) == 0);
    /* DC offset of +2500 stripped by au_rms_ac */
    int16_t dc_quiet[4] = { 2550, 2450, 2550, 2450 };
    uint16_t pk = 0;
    CHECK(au_rms_ac(dc_quiet, 4, &pk) == 50 && pk == 50);
    /* 0.5 s quiet, 2 s speech, then quiet -> DONE about 0.9 s after speech ends */
    for (int i = 0; i < 600; i++) pat[i] = (i >= 16 && i < 78) ? 3000 : 120;
    CHECK(run_vad(pat, 600, &used) == AU_VAD_DONE && used >= 78 + 27 && used <= 78 + 30);
    /* only quiet -> NO_SPEECH at 5 s */
    for (int i = 0; i < 600; i++) pat[i] = 150;
    CHECK(run_vad(pat, 600, &used) == AU_VAD_NO_SPEECH && used * 32 >= 5000 && used * 32 < 5100);
    /* a short click does not count as speech */
    for (int i = 0; i < 600; i++) pat[i] = (i == 20 || i == 21) ? 5000 : 150;
    CHECK(run_vad(pat, 600, &used) == AU_VAD_NO_SPEECH);
    /* nonstop talking -> MAX_LEN at 8 s */
    for (int i = 0; i < 600; i++) pat[i] = 4000 + (i % 7) * 100;
    CHECK(run_vad(pat, 600, &used) == AU_VAD_MAX_LEN && used * 32 >= 8000 && used * 32 < 8100);
    /* noisy room: background 900 adapts, speech at 4000 still detected */
    for (int i = 0; i < 600; i++) pat[i] = (i >= 30 && i < 90) ? 4000 : 900 + (i % 3) * 50;
    CHECK(run_vad(pat, 600, &used) == AU_VAD_DONE);
}

static void test_protocol_states(void)
{
    ui_state_t s;
    CHECK(proto_state_from_name("thinking", &s) && s == UI_STATE_THINKING);
    CHECK(strcmp(proto_state_name(UI_STATE_SPEAKING), "speaking") == 0);
}

int main(void)
{
    test_mask_and_trim();
    test_form();
    test_page();
    test_models_voices();
    test_claude_request();
    test_claude_parse();
    test_deepgram();
    test_speech_clean();
    test_wav();
    test_prompt();
    test_mood();
    test_vad();
    test_protocol_states();
    printf("%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
