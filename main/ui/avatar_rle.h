#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

esp_err_t avatar_rle_decode_rgb565(const uint8_t *input, size_t input_size,
                                   uint16_t *output, size_t output_pixels);
esp_err_t avatar_rle_run_benchmark(void);
esp_err_t avatar_rle_decode_embedded_frame(uint16_t *output, size_t output_pixels);
