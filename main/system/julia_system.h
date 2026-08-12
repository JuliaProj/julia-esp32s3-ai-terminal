#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

typedef struct {
    char firmware_version[32];
    char partition_label[17];
    size_t free_heap;
    size_t minimum_free_heap;
    int8_t wifi_rssi;
    int reset_reason;
    unsigned uptime_seconds;
} julia_system_status_t;

esp_err_t julia_system_init(void);
esp_err_t julia_system_get_status(julia_system_status_t *status);
esp_err_t julia_system_ota_update(const char *https_url);
esp_err_t julia_system_config_set(const char *key, const char *value, bool secret);
esp_err_t julia_system_config_get(const char *key, char *value, size_t value_size);
void julia_system_watchdog_feed(void);
