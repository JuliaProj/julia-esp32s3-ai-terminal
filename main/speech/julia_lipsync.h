#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

void julia_lipsync_begin(void);
esp_err_t julia_lipsync_play(const int16_t *samples, size_t sample_count);
esp_err_t julia_lipsync_play_file(const char *path);
esp_err_t julia_lipsync_end(void);
