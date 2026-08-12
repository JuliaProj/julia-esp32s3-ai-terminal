#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct { uint8_t aec; uint8_t ns; uint8_t agc; bool reference_available; } julia_afe_levels_t;
esp_err_t julia_afe_set_aec(uint8_t level);
esp_err_t julia_afe_set_ns(uint8_t level);
esp_err_t julia_afe_set_agc(uint8_t level);
void julia_afe_get_levels(julia_afe_levels_t *levels);
