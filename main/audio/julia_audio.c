#include "julia_audio.h"

#include <stdbool.h>
#include <math.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

#define TAG "JULIA_AUDIO"

/* Waveshare ESP32-S3-LCD-1.85 board connections. */
#define MIC_I2S_PORT I2S_NUM_1
#define MIC_BCLK     GPIO_NUM_15
#define MIC_WS       GPIO_NUM_2
#define MIC_DIN      GPIO_NUM_39
#define AMP_I2S_PORT I2S_NUM_0
#define AMP_BCLK     GPIO_NUM_48
#define AMP_WS       GPIO_NUM_38
#define AMP_DOUT     GPIO_NUM_47
#define TX_DMA_DESC_NUM  8U
#define TX_DMA_FRAME_NUM 256U

static i2s_chan_handle_t s_rx;
static i2s_chan_handle_t s_tx;
static bool s_rx_enabled;
static bool s_tx_enabled;
static volatile bool s_muted;

void julia_audio_set_muted(bool muted) { s_muted = muted; }
bool julia_audio_is_muted(void) { return s_muted; }

static int16_t amplify_sample(int32_t raw)
{
    int32_t value = (raw >> 14) * JULIA_AUDIO_MIC_GAIN;
    if (value > INT16_MAX) return INT16_MAX;
    if (value < INT16_MIN) return INT16_MIN;
    return (int16_t)value;
}

esp_err_t julia_audio_init(void)
{
    i2s_chan_config_t rx_chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(MIC_I2S_PORT, I2S_ROLE_MASTER);
    rx_chan_cfg.dma_desc_num = 8;
    rx_chan_cfg.dma_frame_num = 256;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&rx_chan_cfg, NULL, &s_rx), TAG, "create RX channel");

    i2s_std_config_t rx_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(JULIA_AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = GPIO_NUM_NC, .bclk = MIC_BCLK, .ws = MIC_WS,
            .dout = GPIO_NUM_NC, .din = MIC_DIN,
            .invert_flags = {.mclk_inv=false, .bclk_inv=false, .ws_inv=false},
        },
    };
    rx_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_RIGHT;
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_rx, &rx_cfg), TAG, "configure RX channel");

    i2s_chan_config_t tx_chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(AMP_I2S_PORT, I2S_ROLE_MASTER);
    tx_chan_cfg.auto_clear = true;
    tx_chan_cfg.dma_desc_num = TX_DMA_DESC_NUM;
    tx_chan_cfg.dma_frame_num = TX_DMA_FRAME_NUM;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&tx_chan_cfg, &s_tx, NULL), TAG, "create TX channel");

    i2s_std_config_t tx_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(JULIA_AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = GPIO_NUM_NC, .bclk = AMP_BCLK, .ws = AMP_WS,
            .dout = AMP_DOUT, .din = GPIO_NUM_NC,
            .invert_flags = {.mclk_inv=false, .bclk_inv=false, .ws_inv=false},
        },
    };
    tx_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx, &tx_cfg), TAG, "configure TX channel");
    ESP_LOGI(TAG, "16 kHz mono ready: MIC BCLK=%d WS=%d DIN=%d, AMP BCLK=%d WS=%d DOUT=%d",
             MIC_BCLK, MIC_WS, MIC_DIN, AMP_BCLK, AMP_WS, AMP_DOUT);
    return ESP_OK;
}

esp_err_t julia_audio_record_start(uint8_t *buffer, size_t len)
{
    ESP_RETURN_ON_FALSE(buffer && len >= sizeof(int16_t), ESP_ERR_INVALID_ARG, TAG, "invalid record buffer");
    if (!s_rx_enabled) {
        ESP_RETURN_ON_ERROR(i2s_channel_enable(s_rx), TAG, "enable RX");
        s_rx_enabled = true;
    }

    int32_t raw[256];
    int16_t *out = (int16_t *)buffer;
    size_t samples_left = len / sizeof(int16_t);
    while (samples_left) {
        size_t wanted = samples_left > 256 ? 256 : samples_left;
        size_t bytes_read = 0;
        ESP_RETURN_ON_ERROR(i2s_channel_read(s_rx, raw, wanted * sizeof(int32_t),
                                             &bytes_read, portMAX_DELAY), TAG, "record");
        size_t samples = bytes_read / sizeof(int32_t);
        for (size_t i = 0; i < samples; ++i) {
            *out++ = amplify_sample(raw[i]);
        }
        samples_left -= samples;
    }
    ESP_LOGI(TAG, "recorded %u bytes", (unsigned)len);
    return ESP_OK;
}

esp_err_t julia_audio_mic_start(void)
{
    if (!s_rx_enabled) {
        ESP_RETURN_ON_ERROR(i2s_channel_enable(s_rx), TAG, "enable RX");
        s_rx_enabled = true;
    }
    return ESP_OK;
}

esp_err_t julia_audio_mic_read(int16_t *samples, size_t sample_count, size_t *samples_read)
{
    ESP_RETURN_ON_FALSE(samples && samples_read && sample_count, ESP_ERR_INVALID_ARG, TAG, "invalid mic read");
    int32_t raw[256];
    size_t total = 0;
    while (total < sample_count) {
        size_t wanted = sample_count - total;
        if (wanted > 256) wanted = 256;
        size_t bytes_read = 0;
        ESP_RETURN_ON_ERROR(i2s_channel_read(s_rx, raw, wanted * sizeof(int32_t),
                                             &bytes_read, portMAX_DELAY), TAG, "mic read");
        size_t count = bytes_read / sizeof(int32_t);
        for (size_t i = 0; i < count; ++i) samples[total + i] = amplify_sample(raw[i]);
        total += count;
    }
    *samples_read = total;
    return ESP_OK;
}

esp_err_t julia_audio_record_stop(void)
{
    if (!s_rx_enabled) return ESP_OK;
    ESP_RETURN_ON_ERROR(i2s_channel_disable(s_rx), TAG, "disable RX");
    s_rx_enabled = false;
    ESP_LOGI(TAG, "recording stopped");
    return ESP_OK;
}

esp_err_t julia_audio_play_start(uint8_t *buffer, size_t len)
{
    ESP_RETURN_ON_FALSE(buffer && len, ESP_ERR_INVALID_ARG, TAG, "invalid playback buffer");
    if (s_muted) {
        uint32_t delay_ms = (uint32_t)(((len / sizeof(int16_t)) * 1000U) /
                                       JULIA_AUDIO_SAMPLE_RATE);
        if (delay_ms) vTaskDelay(pdMS_TO_TICKS(delay_ms));
        return ESP_OK;
    }
    if (!s_tx_enabled) {
        ESP_RETURN_ON_ERROR(i2s_channel_enable(s_tx), TAG, "enable TX");
        s_tx_enabled = true;
    }
    size_t offset = 0;
    while (offset < len) {
        size_t written = 0;
        ESP_RETURN_ON_ERROR(i2s_channel_write(s_tx, buffer + offset, len - offset,
                                              &written, portMAX_DELAY), TAG, "playback");
        offset += written;
    }
    ESP_LOGD(TAG, "played %u bytes", (unsigned)len);
    return ESP_OK;
}

esp_err_t julia_audio_play_stop(void)
{
    if (!s_tx_enabled) return ESP_OK;
    /* i2s_channel_write() completes when bytes enter DMA, not when the speaker
     * has emitted them. Queue one full DMA ring of silence: accepting the last
     * silent frame proves every preceding payload frame has left the ring. */
    static const int16_t silence[TX_DMA_FRAME_NUM] = {0};
    for (unsigned i = 0; i < TX_DMA_DESC_NUM; ++i) {
        size_t written = 0;
        ESP_RETURN_ON_ERROR(i2s_channel_write(s_tx, silence, sizeof(silence),
                                              &written, portMAX_DELAY),
                            TAG, "drain TX DMA");
        ESP_RETURN_ON_FALSE(written == sizeof(silence), ESP_ERR_INVALID_SIZE,
                            TAG, "short TX DMA drain");
    }
    ESP_RETURN_ON_ERROR(i2s_channel_disable(s_tx), TAG, "disable TX");
    s_tx_enabled = false;
    ESP_LOGI(TAG, "playback stopped");
    return ESP_OK;
}

esp_err_t julia_audio_play_tone(uint16_t frequency_hz, uint16_t duration_ms, uint8_t volume_percent)
{
    ESP_RETURN_ON_FALSE(frequency_hz && duration_ms, ESP_ERR_INVALID_ARG, TAG, "invalid tone");
    if (volume_percent > 100) volume_percent = 100;
    size_t count = (size_t)JULIA_AUDIO_SAMPLE_RATE * duration_ms / 1000;
    int16_t *pcm = malloc(count * sizeof(int16_t));
    ESP_RETURN_ON_FALSE(pcm, ESP_ERR_NO_MEM, TAG, "tone allocation failed");
    float amplitude = 10000.0f * volume_percent / 100.0f;
    for (size_t i = 0; i < count; ++i) {
        float envelope = i < 80 ? i / 80.0f : (count - i < 80 ? (count - i) / 80.0f : 1.0f);
        pcm[i] = (int16_t)(amplitude * envelope * sinf(2.0f * (float)M_PI * frequency_hz * i /
                                                       JULIA_AUDIO_SAMPLE_RATE));
    }
    esp_err_t err = julia_audio_play_start((uint8_t *)pcm, count * sizeof(int16_t));
    if (err == ESP_OK) err = julia_audio_play_stop();
    free(pcm);
    return err;
}

esp_err_t julia_audio_save_pcm(const char *path, const int16_t *samples, size_t sample_count)
{
    ESP_RETURN_ON_FALSE(path && samples && sample_count, ESP_ERR_INVALID_ARG, TAG, "invalid PCM cache");
    FILE *file = fopen(path, "wb");
    if (!file) {
        ESP_LOGE(TAG, "open PCM cache failed: path=%s errno=%d", path, errno);
        return ESP_FAIL;
    }
    const uint8_t *data = (const uint8_t *)samples;
    size_t total = sample_count * sizeof(int16_t), offset = 0;
    while (offset < total) {
        size_t chunk = total - offset > 4096 ? 4096 : total - offset;
        size_t written = fwrite(data + offset, 1, chunk, file);
        if (written != chunk) {
            ESP_LOGE(TAG, "PCM cache short write: %u/%u errno=%d",
                     (unsigned)offset, (unsigned)total, errno);
            fclose(file); return ESP_FAIL;
        }
        offset += written;
    }
    int close_result = fclose(file);
    ESP_LOGI(TAG, "PCM cache saved: %s, %u bytes", path, (unsigned)total);
    return close_result == 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t julia_audio_play_pcm_file(const char *path)
{
    ESP_RETURN_ON_FALSE(path, ESP_ERR_INVALID_ARG, TAG, "invalid PCM path");
    FILE *file = fopen(path, "rb");
    if (!file) return ESP_ERR_NOT_FOUND;
    fseek(file, 0, SEEK_END); long bytes = ftell(file); rewind(file);
    if (bytes <= 0 || bytes > 512 * 1024) { fclose(file); return ESP_ERR_INVALID_SIZE; }
    uint8_t *pcm = heap_caps_malloc((size_t)bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!pcm) { fclose(file); return ESP_ERR_NO_MEM; }
    size_t loaded_bytes = fread(pcm, 1, (size_t)bytes, file);
    bool loaded = loaded_bytes == (size_t)bytes;
    fclose(file);
    esp_err_t err = loaded ? julia_audio_play_start(pcm, (size_t)bytes) : ESP_FAIL;
    size_t played_bytes = err == ESP_OK ? (size_t)bytes : 0;
    if (err == ESP_OK) err = julia_audio_play_stop();
    if (err != ESP_OK || played_bytes != (size_t)bytes) {
        ESP_LOGW(TAG, "PCM incomplete path=%s file_bytes=%ld loaded_bytes=%u played_bytes=%u result=%s",
                 path, bytes, (unsigned)loaded_bytes, (unsigned)played_bytes,
                 esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "PCM complete path=%s file_bytes=%ld played_bytes=%u dma_drain_frames=%u",
                 path, bytes, (unsigned)played_bytes,
                 (unsigned)(TX_DMA_DESC_NUM * TX_DMA_FRAME_NUM));
    }
    free(pcm);
    return err;
}

esp_err_t julia_audio_play_pcm_memory(const uint8_t *pcm, size_t bytes)
{
    ESP_RETURN_ON_FALSE(pcm && bytes && !(bytes & 1U) && bytes <= 512 * 1024,
                        ESP_ERR_INVALID_ARG, TAG, "invalid embedded PCM");
    esp_err_t err = julia_audio_play_start((uint8_t *)pcm, bytes);
    size_t played_bytes = err == ESP_OK ? bytes : 0;
    if (err == ESP_OK) err = julia_audio_play_stop();
    if (err != ESP_OK || played_bytes != bytes)
        ESP_LOGW(TAG, "embedded PCM incomplete file_bytes=%u played_bytes=%u result=%s",
                 (unsigned)bytes, (unsigned)played_bytes, esp_err_to_name(err));
    else
        ESP_LOGI(TAG, "embedded PCM complete file_bytes=%u played_bytes=%u dma_drain_frames=%u",
                 (unsigned)bytes, (unsigned)played_bytes,
                 (unsigned)(TX_DMA_DESC_NUM * TX_DMA_FRAME_NUM));
    return err;
}
