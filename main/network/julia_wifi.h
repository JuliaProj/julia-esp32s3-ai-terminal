#pragma once
#include "esp_err.h"
void julia_wifi_print_status(void);
esp_err_t julia_wifi_scan_print(void);
esp_err_t julia_wifi_connect_runtime(const char *ssid, const char *password);
