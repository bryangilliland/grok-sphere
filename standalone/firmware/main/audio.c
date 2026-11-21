/*
 * Grok Sphere - audio in/out for both hardware revisions of the Waveshare 1.85C.
 *
 * V2 (ES8311 + ES7210): one duplex I2S port, set up the same way as xiaozhi-esp32's
 *   BoxAudioCodec (TX = standard Philips stereo, RX = 4-slot TDM from ES7210), driven
 *   through Espressif's esp_codec_dev. Amp enable (NS4150B) is GPIO15, toggled by esp_codec_dev.
 * V1 (PCM5101 + ICS-43434): two simplex I2S ports, 32-bit slots, same as xiaozhi's
 *   NoAudioCodecSimplex (speaker = left slot, mic = right slot, mic samples >> 12).
 *
 * Everything runs at 16 kHz so mic frames go straight to the bridge and TTS from the
 * bridge plays without resampling.
 */
#include "audio.h"
#include <math.h>
#include <string.h>
#include "driver/i2s_std.h"
#include "driver/i2s_tdm.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"

#define PLAY_BUF_BYTES   (1024 * 1024)   /* ~32 s of 16 kHz mono in PSRAM */
#define PLAY_CHUNK       256             /* samples per write (16 ms) */
#define MIC_GAIN_DB      30.0f
#define DEFAULT_VOLUME   70

static const char *TAG = "audio";
static board_audio_rev_t s_rev;
static i2s_chan_handle_t s_tx, s_rx;
static esp_codec_dev_handle_t s_out_dev, s_in_dev;
static StreamBufferHandle_t s_play;
static StaticStreamBuffer_t s_play_struct;
static volatile bool s_stream_active, s_stream_ended, s_flush;
static volatile uint8_t s_level;
static volatile int s_volume = DEFAULT_VOLUME;
static audio_done_cb_t s_done_cb;

/* ------------------------------------------------------------------ V2 */
static esp_err_t init_v2(void)
{
    i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan.dma_desc_num = 6;
    chan.dma_frame_num = 240;
    chan.auto_clear = true;
    ESP_ERROR_CHECK(i2s_new_channel(&chan, &s_tx, &s_rx));

    i2s_std_config_t std = {
        .clk_cfg = { .sample_rate_hz = AUDIO_SAMPLE_RATE, .clk_src = I2S_CLK_SRC_DEFAULT,
                     .mclk_multiple = I2S_MCLK_MULTIPLE_256 },
        .slot_cfg = { .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT, .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
                      .slot_mode = I2S_SLOT_MODE_STEREO, .slot_mask = I2S_STD_SLOT_BOTH,
                      .ws_width = I2S_DATA_BIT_WIDTH_16BIT, .ws_pol = false, .bit_shift = true,
                      .left_align = true, .big_endian = false, .bit_order_lsb = false },
        .gpio_cfg = { .mclk = V2_I2S_MCLK, .bclk = V2_I2S_BCLK, .ws = V2_I2S_WS,
                      .dout = V2_I2S_DOUT, .din = I2S_GPIO_UNUSED },
    };
    i2s_tdm_config_t tdm = {
        .clk_cfg = { .sample_rate_hz = AUDIO_SAMPLE_RATE, .clk_src = I2S_CLK_SRC_DEFAULT,
                     .mclk_multiple = I2S_MCLK_MULTIPLE_256, .bclk_div = 8 },
        .slot_cfg = { .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT, .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
                      .slot_mode = I2S_SLOT_MODE_STEREO,
                      .slot_mask = I2S_TDM_SLOT0 | I2S_TDM_SLOT1 | I2S_TDM_SLOT2 | I2S_TDM_SLOT3,
                      .ws_width = I2S_TDM_AUTO_WS_WIDTH, .ws_pol = false, .bit_shift = true,
                      .left_align = false, .big_endian = false, .bit_order_lsb = false,
                      .skip_mask = false, .total_slot = I2S_TDM_AUTO_SLOT_NUM },
        .gpio_cfg = { .mclk = V2_I2S_MCLK, .bclk = V2_I2S_BCLK, .ws = V2_I2S_WS,
                      .dout = I2S_GPIO_UNUSED, .din = V2_I2S_DIN },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_tx, &std));
    ESP_ERROR_CHECK(i2s_channel_init_tdm_mode(s_rx, &tdm));
    ESP_ERROR_CHECK(i2s_channel_enable(s_tx));
    ESP_ERROR_CHECK(i2s_channel_enable(s_rx));

    audio_codec_i2s_cfg_t i2s_cfg = { .port = I2S_NUM_0, .rx_handle = s_rx, .tx_handle = s_tx };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);

    audio_codec_i2c_cfg_t i2c_cfg = { .port = I2C_NUM_0, .addr = ES8311_CODEC_DEFAULT_ADDR,
                                      .bus_handle = board_i2c_bus() };
    const audio_codec_ctrl_if_t *out_ctrl = audio_codec_new_i2c_ctrl(&i2c_cfg);
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    es8311_codec_cfg_t es8311 = {
        .ctrl_if = out_ctrl,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin = V2_PA_PIN,
        .use_mclk = true,
        .hw_gain = { .pa_voltage = 5.0, .codec_dac_voltage = 3.3 },
    };
    const audio_codec_if_t *out_codec = es8311_codec_new(&es8311);
    esp_codec_dev_cfg_t out_cfg = { .dev_type = ESP_CODEC_DEV_TYPE_OUT, .codec_if = out_codec, .data_if = data_if };
    s_out_dev = esp_codec_dev_new(&out_cfg);

    i2c_cfg.addr = ES7210_CODEC_DEFAULT_ADDR;
    const audio_codec_ctrl_if_t *in_ctrl = audio_codec_new_i2c_ctrl(&i2c_cfg);
    es7210_codec_cfg_t es7210 = {
        .ctrl_if = in_ctrl,
        .mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2 | ES7210_SEL_MIC3 | ES7210_SEL_MIC4,
    };
    const audio_codec_if_t *in_codec = es7210_codec_new(&es7210);
    esp_codec_dev_cfg_t in_cfg = { .dev_type = ESP_CODEC_DEV_TYPE_IN, .codec_if = in_codec, .data_if = data_if };
    s_in_dev = esp_codec_dev_new(&in_cfg);
    if (!s_out_dev || !s_in_dev) {
        ESP_LOGE(TAG, "esp_codec_dev setup failed");
        return ESP_FAIL;
    }

    esp_codec_dev_sample_info_t in_fs = {
        .bits_per_sample = 16, .channel = 4, .channel_mask = ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0),
        .sample_rate = AUDIO_SAMPLE_RATE, .mclk_multiple = 0,
    };
    if (esp_codec_dev_open(s_in_dev, &in_fs) != ESP_CODEC_DEV_OK) ESP_LOGE(TAG, "ES7210 open failed");
    esp_codec_dev_set_in_channel_gain(s_in_dev, ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0), MIC_GAIN_DB);

    esp_codec_dev_sample_info_t out_fs = {
        .bits_per_sample = 16, .channel = 1, .channel_mask = 0,
        .sample_rate = AUDIO_SAMPLE_RATE, .mclk_multiple = 0,
    };
    if (esp_codec_dev_open(s_out_dev, &out_fs) != ESP_CODEC_DEV_OK) ESP_LOGE(TAG, "ES8311 open failed");
    esp_codec_dev_set_out_vol(s_out_dev, s_volume);
    return ESP_OK;
}

/* ------------------------------------------------------------------ V1 */
static esp_err_t init_v1(void)
{
    i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan.dma_desc_num = 6;
    chan.dma_frame_num = 240;
    chan.auto_clear = true;
    ESP_ERROR_CHECK(i2s_new_channel(&chan, &s_tx, NULL));
    i2s_std_config_t std = {
        .clk_cfg = { .sample_rate_hz = AUDIO_SAMPLE_RATE, .clk_src = I2S_CLK_SRC_DEFAULT,
                     .mclk_multiple = I2S_MCLK_MULTIPLE_256 },
        .slot_cfg = { .data_bit_width = I2S_DATA_BIT_WIDTH_32BIT, .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
                      .slot_mode = I2S_SLOT_MODE_MONO, .slot_mask = I2S_STD_SLOT_LEFT,
                      .ws_width = I2S_DATA_BIT_WIDTH_32BIT, .ws_pol = false, .bit_shift = true,
                      .left_align = true, .big_endian = false, .bit_order_lsb = false },
        .gpio_cfg = { .mclk = I2S_GPIO_UNUSED, .bclk = V1_SPK_BCLK, .ws = V1_SPK_WS,
                      .dout = V1_SPK_DOUT, .din = I2S_GPIO_UNUSED },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_tx, &std));

    chan.id = I2S_NUM_1;
    ESP_ERROR_CHECK(i2s_new_channel(&chan, NULL, &s_rx));
    std.slot_cfg.slot_mask = I2S_STD_SLOT_RIGHT;
    std.gpio_cfg.bclk = V1_MIC_SCK;
    std.gpio_cfg.ws = V1_MIC_WS;
    std.gpio_cfg.dout = I2S_GPIO_UNUSED;
    std.gpio_cfg.din = V1_MIC_DIN;
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_rx, &std));
    ESP_ERROR_CHECK(i2s_channel_enable(s_tx));
    ESP_ERROR_CHECK(i2s_channel_enable(s_rx));
    return ESP_OK;
}

/* ------------------------------------------------------------------ common */
size_t audio_mic_read(int16_t *dst, size_t samples)
{
    if (s_rev == BOARD_AUDIO_V2) {
        if (!s_in_dev || esp_codec_dev_read(s_in_dev, dst, samples * sizeof(int16_t)) != ESP_CODEC_DEV_OK) return 0;
        return samples;
    }
    if (!s_rx) return 0;
    static int32_t raw[512];
    size_t done = 0;
    while (done < samples) {
        size_t n = samples - done;
        if (n > 512) n = 512;
        size_t got = 0;
        if (i2s_channel_read(s_rx, raw, n * sizeof(int32_t), &got, 200) != ESP_OK) break;
        got /= sizeof(int32_t);
        for (size_t i = 0; i < got; i++) {
            int32_t v = raw[i] >> 12;
            dst[done + i] = v > INT16_MAX ? INT16_MAX : (v < -INT16_MAX ? -INT16_MAX : (int16_t)v);
        }
        done += got;
        if (got == 0) break;
    }
    return done;
}

static void write_out(const int16_t *pcm, size_t n)
{
    if (s_rev == BOARD_AUDIO_V2) {
        esp_codec_dev_write(s_out_dev, (void *)pcm, n * sizeof(int16_t));
        return;
    }
    static int32_t buf[PLAY_CHUNK];
    float g = (float)s_volume / 100.0f;
    int32_t factor = (int32_t)(g * g * 65536.0f);
    for (size_t i = 0; i < n; i++) buf[i] = (int32_t)pcm[i] * factor;
    size_t w;
    i2s_channel_write(s_tx, buf, n * sizeof(int32_t), &w, portMAX_DELAY);
}

static void play_task(void *arg)
{
    static int16_t pcm[PLAY_CHUNK];
    for (;;) {
        size_t got = xStreamBufferReceive(s_play, pcm, sizeof(pcm), pdMS_TO_TICKS(40));
        if (s_flush) {
            while (xStreamBufferReceive(s_play, pcm, sizeof(pcm), 0) > 0) {}
            s_flush = false;
            s_level = 0;
            continue;
        }
        if (got == 0) {
            s_level = 0;
            if (s_stream_active && s_stream_ended) {
                s_stream_active = false;
                s_stream_ended = false;
                if (s_done_cb) s_done_cb();
            }
            continue;
        }
        size_t n = got / sizeof(int16_t);
        double acc = 0;
        for (size_t i = 0; i < n; i++) acc += (double)pcm[i] * pcm[i];
        double rms = n ? sqrt(acc / n) : 0;
        int lvl = (int)(rms / 80.0);              /* ~8000 rms -> 100 */
        s_level = lvl > 100 ? 100 : (uint8_t)lvl;
        write_out(pcm, n);
    }
}

esp_err_t audio_init(board_audio_rev_t rev, audio_done_cb_t on_stream_done)
{
    s_rev = rev;
    s_done_cb = on_stream_done;
    uint8_t *storage = heap_caps_malloc(PLAY_BUF_BYTES + 1, MALLOC_CAP_SPIRAM);
    if (!storage) return ESP_ERR_NO_MEM;
    s_play = xStreamBufferCreateStatic(PLAY_BUF_BYTES, 1, storage, &s_play_struct);
    esp_err_t err = (rev == BOARD_AUDIO_V2) ? init_v2() : init_v1();
    if (err != ESP_OK) return err;
    xTaskCreatePinnedToCore(play_task, "play", 4096, NULL, 7, NULL, 1);
    ESP_LOGI(TAG, "audio ready (%s, %d Hz)", board_audio_rev_name(), AUDIO_SAMPLE_RATE);
    return ESP_OK;
}

void audio_stream_begin(void)
{
    s_stream_ended = false;
    s_stream_active = true;
}

void audio_stream_push(const uint8_t *pcm, size_t len)
{
    len &= ~(size_t)1;
    size_t off = 0;
    while (off < len) {
        size_t sent = xStreamBufferSend(s_play, pcm + off, len - off, pdMS_TO_TICKS(500));
        if (sent == 0) {
            ESP_LOGW(TAG, "playback buffer full, dropping %u bytes", (unsigned)(len - off));
            return;
        }
        off += sent;
    }
}

void audio_stream_end(void) { s_stream_ended = true; }

void audio_stop(void)
{
    s_flush = true;
    s_stream_active = false;
    s_stream_ended = false;
}

void audio_play_chime(void)
{
    /* two short rising tones, generated on the fly */
    static int16_t tone[AUDIO_SAMPLE_RATE / 4];
    const float freqs[2] = {880.0f, 1320.0f};
    size_t half = sizeof(tone) / sizeof(tone[0]) / 2;
    for (int t = 0; t < 2; t++) {
        for (size_t i = 0; i < half; i++) {
            float env = 1.0f;
            if (i < 160) env = i / 160.0f;
            if (i > half - 400) env = (half - i) / 400.0f;
            tone[t * half + i] = (int16_t)(9000.0f * env * sinf(2.0f * (float)M_PI * freqs[t] * i / AUDIO_SAMPLE_RATE));
        }
    }
    audio_stream_push((const uint8_t *)tone, sizeof(tone));
}

void audio_set_volume(int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    s_volume = percent;
    if (s_rev == BOARD_AUDIO_V2 && s_out_dev) esp_codec_dev_set_out_vol(s_out_dev, percent);
}

int audio_get_volume(void) { return s_volume; }
uint8_t audio_level(void) { return s_level; }
bool audio_is_playing(void) { return s_level > 0 || xStreamBufferBytesAvailable(s_play) > 0; }
