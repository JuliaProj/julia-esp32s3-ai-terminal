#include "julia_wifi.h"
#include <stdio.h>
#include "esp_netif_ip_addr.h"
#include "esp_wifi.h"
#include "julia_network.h"
#include "julia_system.h"
void julia_wifi_print_status(void)
{
    esp_ip4_addr_t ip = {0}; bool connected = julia_wifi_is_connected();
    esp_err_t ip_err = julia_wifi_get_ip(&ip);
    printf("WIFI state=%s ip=" IPSTR " rssi=%d\n", connected ? "connected" : "offline",
           IP2STR(&ip), connected && ip_err == ESP_OK ? julia_wifi_get_rssi() : -128);
}
esp_err_t julia_wifi_scan_print(void)
{
    wifi_scan_config_t config = {.show_hidden = true};
    esp_err_t err = esp_wifi_scan_start(&config, true); if (err != ESP_OK) return err;
    uint16_t count = 20; wifi_ap_record_t records[20] = {0};
    err = esp_wifi_scan_get_ap_records(&count, records); if (err != ESP_OK) return err;
    for (uint16_t i = 0; i < count; ++i)
        printf("WIFI_AP ssid=%s rssi=%d channel=%u auth=%u\n", records[i].ssid,
               records[i].rssi, records[i].primary, records[i].authmode);
    return ESP_OK;
}
esp_err_t julia_wifi_connect_runtime(const char *ssid, const char *password)
{
    esp_err_t err = julia_wifi_connect(ssid, password);
    if (err == ESP_OK) {
        julia_system_config_set("wifi_ssid", ssid, false);
        esp_err_t save = julia_system_config_set("wifi_password", password, true);
        if (save == ESP_ERR_NOT_SUPPORTED)
            printf("WIFI warning=password_not_persisted_flash_encryption_disabled\n");
    }
    return err;
}
