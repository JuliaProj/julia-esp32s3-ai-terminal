#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    const char *model_name;
    const char *wake_word;
    float threshold;
    uint32_t debounce_ms;
    bool ready;
} julia_wakenet_status_t;

void julia_wakenet_get_status(julia_wakenet_status_t *status);
esp_err_t julia_wakenet_set_word(const char *word);
esp_err_t julia_wakenet_set_threshold(float threshold);
