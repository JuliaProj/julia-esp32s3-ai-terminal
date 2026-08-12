#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_netif_ip_addr.h"

esp_err_t julia_wifi_init(void);
esp_err_t julia_wifi_connect(const char *ssid, const char *password);
esp_err_t julia_wifi_disconnect(void);
bool julia_wifi_is_connected(void);
int8_t julia_wifi_get_rssi(void);
esp_err_t julia_wifi_get_ip(esp_ip4_addr_t *ip);
esp_err_t julia_wifi_wait_cloud_ready(uint32_t timeout_ms);
esp_err_t julia_wifi_check_dns(const char *hostname);
esp_err_t julia_wifi_start_provisioning(void);
