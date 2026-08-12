#pragma once

#include <stdbool.h>
#include "esp_err.h"

typedef void (*wake_sequence_done_cb_t)(esp_err_t result);

esp_err_t wake_sequence_init(void);
esp_err_t wake_sequence_start(wake_sequence_done_cb_t done_cb);
esp_err_t wake_sequence_start_fallback(wake_sequence_done_cb_t done_cb);
void wake_sequence_stop(void);
bool wake_sequence_is_running(void);
