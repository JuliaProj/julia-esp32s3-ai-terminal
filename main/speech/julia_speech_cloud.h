#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

esp_err_t julia_speech_asr(const int16_t *pcm, size_t sample_count,
                           char *text, size_t text_size);
esp_err_t julia_speech_tts(const char *text, int16_t **pcm, size_t *sample_count);

