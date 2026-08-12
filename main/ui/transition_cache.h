#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define TRANSITION_CACHE_LIMIT_BYTES (5U * 1024U * 1024U / 2U)
#define TRANSITION_CACHE_WATERMARK_BYTES (1200U * 1024U)

typedef void (*transition_cache_print_cb_t)(const char *path, size_t bytes,
                                            unsigned references, void *context);

esp_err_t transition_cache_init(void);
esp_err_t transition_cache_load(const char *path);
bool transition_cache_contains(const char *path);
esp_err_t transition_cache_acquire(const char *path, const uint8_t **data, size_t *bytes);
void transition_cache_release(const char *path);
size_t transition_cache_clear_unreferenced(void);
void transition_cache_print(transition_cache_print_cb_t callback, void *context);
size_t transition_cache_bytes(void);
