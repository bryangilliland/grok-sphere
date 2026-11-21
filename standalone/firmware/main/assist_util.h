/*
 * Grok Sphere standalone - pure-C helpers for the Claude + speech pipeline.
 * No ESP-IDF dependencies (only cJSON) so they are unit-tested on a PC in host_test/.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AU_MODEL_HAIKU     "claude-haiku-5-5"
#define AU_MODEL_SONNET    "claude-sonnet-5-5"
#define AU_DEFAULT_VOICE   "aura-2-orion-en"
#define AU_PREVIEW_TEXT    "Hi, I'm Thad. This is how I sound."
#define AU_ANTHROPIC_VERSION "2026-10-21"

typedef struct { const char *id; const char *label; } au_voice_t;
extern const au_voice_t AU_VOICES[];
extern const int AU_VOICE_COUNT;

const char *au_model_id(int model);            /* 0 = Haiku, 1 = Sonnet; anything else = Haiku */
bool au_voice_valid(const char *id);

/* "****abcd" (last 4 chars only). Empty key -> "". Never returns more of the key. */
void au_mask_key(const char *key, char *out, size_t cap);
/* Trim spaces/newlines a phone may paste around a key, in place. */
void au_trim(char *s);

/* 44-byte RIFF/WAVE header for 16-bit mono PCM. */
void au_wav_header(uint8_t out[44], uint32_t samples, uint32_t sample_rate);

typedef struct { const char *role; const char *text; } au_turn_t;   /* role: "user" / "assistant" */

/* thinking_mode: 0 = omit thinking + output_config (fallback), 1 = {"type":"disabled"} + effort low,
 * 2 = {"type":"between_tools"} + effort low. Returns malloc'd JSON (free()), or NULL. */
char *au_claude_request(const char *model, int max_tokens, const char *system,
                        const au_turn_t *history, int n_history, const char *user, int thinking_mode);
/* Best thinking mode for a model id (Haiku 5.5 accepts "disabled"; Sonnet 5.5 needs "between_tools"). */
int au_thinking_mode_for(const char *model);

/* Parse a Messages API response. On success copies all text blocks into out and returns true.
 * On an API error returns false and copies error.message (or a short reason) into err. */
bool au_parse_claude(const char *json, char *out, size_t cap, char *err, size_t errcap);

/* Parse a Deepgram /v1/listen response: results.channels[0].alternatives[0].transcript. */
bool au_parse_deepgram(const char *json, char *out, size_t cap);
/* Deepgram error body -> short message ("err_msg" / "message"). */
void au_parse_error_message(const char *json, char *out, size_t cap);

/* Deepgram /v1/speak body {"text": ...}. malloc'd, free(). */
char *au_tts_body(const char *text);

/* Make Claude's reply speakable/displayable: drop markdown, collapse whitespace,
 * cut at a sentence end at or before max_chars. */
void au_speech_clean(const char *in, char *out, size_t cap, size_t max_chars);

/* Build the Thad system prompt (date/time string may be empty). */
int au_system_prompt(char *out, size_t cap, const char *local_time);

/* -------- simple energy voice-activity detector for push-to-talk auto-stop -------- */
typedef enum { VAD_WAITING = 0, VAD_SPEECH, VAD_DONE, VAD_NO_SPEECH, VAD_MAX_LEN } vad_result_t;
typedef struct {
    int frame_ms;
    int elapsed_ms, speech_ms, silence_ms;
    float noise;
    bool started;
} au_vad_t;
void au_vad_init(au_vad_t *v, int frame_ms);
uint32_t au_rms(const int16_t *pcm, size_t n);
/* Feed one frame's RMS. Thresholds: no speech within 7 s -> VAD_NO_SPEECH,
 * 1.1 s of silence after >= 0.35 s of speech -> VAD_DONE, 15 s total -> VAD_MAX_LEN. */
vad_result_t au_vad_feed(au_vad_t *v, uint32_t rms);
