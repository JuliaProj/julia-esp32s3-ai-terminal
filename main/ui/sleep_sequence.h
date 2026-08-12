#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef void (*sleep_sequence_done_cb_t)(esp_err_t result);

esp_err_t sleep_sequence_init(void);
esp_err_t sleep_sequence_start(sleep_sequence_done_cb_t done_cb);
esp_err_t sleep_sequence_start_fallback(sleep_sequence_done_cb_t done_cb);
void sleep_sequence_stop(void);
bool sleep_sequence_is_running(void);
esp_err_t sleep_sequence_set_hold_ms(uint32_t hold_ms);
uint32_t sleep_sequence_get_hold_ms(void);
