#pragma once

#include <stdbool.h>
#include "esp_err.h"

esp_err_t julia_ui_showcase_init(void);
bool julia_ui_showcase_start(void);
bool julia_ui_showcase_is_running(void);
bool julia_ui_showcase_allows_state_change(void);
