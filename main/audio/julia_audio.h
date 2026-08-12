#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define JULIA_AUDIO_SAMPLE_RATE       16000
#define JULIA_AUDIO_BITS_PER_SAMPLE   16
#define JULIA_AUDIO_MIC_GAIN          2
#define JULIA_AUDIO_DMA_BUFFER_BYTES  5120
#define JULIA_AUDIO_320MS_BYTES       10240

esp_err_t julia_audio_init(void);
esp_err_t julia_audio_record_start(uint8_t *buffer, size_t len);
esp_err_t julia_audio_record_stop(void);
esp_err_t julia_audio_play_start(uint8_t *buffer, size_t len);
esp_err_t julia_audio_play_stop(void);
esp_err_t julia_audio_mic_start(void);
esp_err_t julia_audio_mic_read(int16_t *samples, size_t sample_count, size_t *samples_read);
esp_err_t julia_audio_play_tone(uint16_t frequency_hz, uint16_t duration_ms, uint8_t volume_percent);
esp_err_t julia_audio_save_pcm(const char *path, const int16_t *samples, size_t sample_count);
esp_err_t julia_audio_play_pcm_file(const char *path);
esp_err_t julia_audio_play_pcm_memory(const uint8_t *pcm, size_t bytes);
void julia_audio_set_muted(bool muted);
bool julia_audio_is_muted(void);
