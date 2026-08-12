#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* Selects and plays one offline reply. SD is preferred; four common replies
 * embedded in the application image are the no-SD fallback. */
esp_err_t wake_reply_play(void);
bool wake_reply_is_known_asset(const char *filename, size_t bytes);
esp_err_t wake_reply_install_stream(FILE *input, const char *filename, size_t bytes,
                                    uint32_t expected_crc);
