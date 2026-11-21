#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "board.h"

typedef void (*audio_done_cb_t)(void);

esp_err_t audio_init(board_audio_rev_t rev, audio_done_cb_t on_stream_done);
/* Blocking read of 16 kHz mono signed 16-bit samples. Returns samples read. */
size_t audio_mic_read(int16_t *dst, size_t samples);

void audio_stream_begin(void);                       /* bridge sent audio_start */
void audio_stream_push(const uint8_t *pcm, size_t len); /* 16 kHz mono s16le bytes */
void audio_stream_end(void);                         /* bridge sent audio_end */
void audio_stop(void);                               /* drop everything queued */
void audio_play_chime(void);
void audio_set_volume(int percent);                  /* 0..100 */
int  audio_get_volume(void);
uint8_t audio_level(void);                           /* 0..100, recent playback loudness */
bool audio_is_playing(void);
