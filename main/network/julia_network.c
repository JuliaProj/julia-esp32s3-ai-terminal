#include "julia_network.h"

#include <string.h>
#include "esp_event.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/dns.h"
#include "lwip/inet.h"

#define TAG "JULIA_NET"
#define CONNECTED_BIT BIT0
#define DISCONNECTED_BIT BIT1
#define RECONNECT_BIT BIT2
#define DNS_DONE_BIT BIT3
#define DNS_FAILED_BIT BIT4
#define INITIAL_RETRIES 3
#define INITIAL_RETRY_MS 5000
#define MAX_BACKOFF_SECONDS 60
#define PRIMARY_DNS_SERVER "223.5.5.5"
#define SCAN_RESULT_LIMIT 12

static EventGroupHandle_t s_events;
static SemaphoreHandle_t s_dns_lock;
static esp_netif_t *s_sta_netif;
static esp_netif_t *s_ap_netif;
static volatile bool s_connected;
static volatile bool s_should_connect;
static volatile bool s_initial_connect;
static volatile bool s_scanning;
static uint32_t s_backoff_seconds = 1;
static esp_ip4_addr_t s_ip;
static ip_addr_t s_dns_result;
static uint32_t s_dns_request_id;

static const char *wifi_disconnect_reason(uint8_t reason)
{
    switch (reason) {
    case WIFI_REASON_NO_AP_FOUND: return "no_ap_found";
    case WIFI_REASON_AUTH_FAIL: return "auth_fail";
    case WIFI_REASON_ASSOC_FAIL: return "assoc_fail";
    case WIFI_REASON_HANDSHAKE_TIMEOUT: return "handshake_timeout";
    case WIFI_REASON_BEACON_TIMEOUT: return "beacon_timeout";
    case WIFI_REASON_CONNECTION_FAIL: return "connection_fail";
    default: return "other";
    }
}

static void log_visible_networks(void)
{
    wifi_scan_config_t scan = { .show_hidden = true };
    esp_err_t err = esp_wifi_scan_start(&scan, true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "WiFi scan failed: %s", esp_err_to_name(err));
        return;
    }
    uint16_t count = SCAN_RESULT_LIMIT;
    wifi_ap_record_t records[SCAN_RESULT_LIMIT] = {0};
    err = esp_wifi_scan_get_ap_records(&count, records);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Reading WiFi scan failed: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "Visible WiFi networks (%u shown):", count);
    for (uint16_t i = 0; i < count; ++i) {
        ESP_LOGI(TAG, "  SSID=%s channel=%u RSSI=%d auth=%d",
                 records[i].ssid[0] ? (char *)records[i].ssid : "<hidden>",
                 records[i].primary, records[i].rssi, records[i].authmode);
    }
}

static void dns_callback(const char *hostname, const ip_addr_t *address, void *arg)
{
    (void)hostname;
    if ((uint32_t)(uintptr_t)arg != s_dns_request_id) return;
    if (address) {
        s_dns_result = *address;
        xEventGroupSetBits(s_events, DNS_DONE_BIT);
    } else {
        xEventGroupSetBits(s_events, DNS_FAILED_BIT);
    }
}

static void configure_dns(void)
{
    esp_netif_dns_info_t dhcp_dns = {0};
    if (esp_netif_get_dns_info(s_sta_netif, ESP_NETIF_DNS_MAIN, &dhcp_dns) == ESP_OK &&
        dhcp_dns.ip.type == ESP_IPADDR_TYPE_V4 && dhcp_dns.ip.u_addr.ip4.addr != 0) {
        /* Managed/lab networks commonly block direct queries to public DNS.
         * Keep the resolver supplied by DHCP as primary and use AliDNS only
         * as backup. */
        esp_netif_dns_info_t backup = {0};
        backup.ip.type = ESP_IPADDR_TYPE_V4;
        backup.ip.u_addr.ip4.addr = ipaddr_addr(PRIMARY_DNS_SERVER);
        esp_netif_set_dns_info(s_sta_netif, ESP_NETIF_DNS_BACKUP, &backup);
        ESP_LOGI(TAG, "DHCP DNS primary: " IPSTR ", backup=%s",
                 IP2STR(&dhcp_dns.ip.u_addr.ip4), PRIMARY_DNS_SERVER);
        return;
    }

    esp_netif_dns_info_t primary = {0};
    primary.ip.type = ESP_IPADDR_TYPE_V4;
    primary.ip.u_addr.ip4.addr = ipaddr_addr(PRIMARY_DNS_SERVER);
    esp_err_t err = esp_netif_set_dns_info(s_sta_netif, ESP_NETIF_DNS_MAIN, &primary);
    if (err == ESP_OK) ESP_LOGI(TAG, "DHCP DNS unavailable, fallback primary=%s", PRIMARY_DNS_SERVER);
    else ESP_LOGW(TAG, "DNS configuration failed: %s", esp_err_to_name(err));
}

static void network_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *event = data;
        s_connected = false;
        s_ip.addr = 0;
        xEventGroupClearBits(s_events, CONNECTED_BIT);
        xEventGroupSetBits(s_events, DISCONNECTED_BIT);
        uint8_t reason = event ? event->reason : 0;
        ESP_LOGW(TAG, "disconnected, reason=%u (%s)", reason, wifi_disconnect_reason(reason));
        if (s_should_connect && !s_initial_connect && !s_scanning) {
            xEventGroupSetBits(s_events, RECONNECT_BIT);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = data;
        s_ip = event->ip_info.ip;
        configure_dns();
        s_connected = true;
        s_backoff_seconds = 1;
        xEventGroupClearBits(s_events, DISCONNECTED_BIT | RECONNECT_BIT);
        xEventGroupSetBits(s_events, CONNECTED_BIT);
        ESP_LOGI(TAG, "got IP: " IPSTR, IP2STR(&s_ip));
    }
}

static void reconnect_task(void *arg)
{
    (void)arg;
    while (true) {
        xEventGroupWaitBits(s_events, RECONNECT_BIT, pdTRUE, pdFALSE, portMAX_DELAY);
        while (s_should_connect && !s_connected && !s_initial_connect) {
            uint32_t delay_seconds = s_backoff_seconds;
            ESP_LOGI(TAG, "reconnect in %u seconds", (unsigned)delay_seconds);
            vTaskDelay(pdMS_TO_TICKS(delay_seconds * 1000));
            if (!s_should_connect || s_connected) break;
            ESP_LOGI(TAG, "reconnecting...");
            esp_err_t err = esp_wifi_connect();
            if (err != ESP_OK) ESP_LOGW(TAG, "esp_wifi_connect: %s", esp_err_to_name(err));
            s_backoff_seconds = delay_seconds < 32 ? delay_seconds * 2 : MAX_BACKOFF_SECONDS;
            EventBits_t bits = xEventGroupWaitBits(s_events, CONNECTED_BIT, pdFALSE, pdFALSE,
                                                   pdMS_TO_TICKS(5000));
            if (bits & CONNECTED_BIT) break;
        }
    }
}

esp_err_t julia_wifi_init(void)
{
    s_events = xEventGroupCreate();
    s_dns_lock = xSemaphoreCreateMutex();
    if (!s_events || !s_dns_lock) return ESP_ERR_NO_MEM;
    ESP_ERROR_CHECK(esp_netif_init());
    esp_err_t loop_err = esp_event_loop_create_default();
    if (loop_err != ESP_OK && loop_err != ESP_ERR_INVALID_STATE) return loop_err;
    s_sta_netif = esp_netif_create_default_wifi_sta();
    if (!s_sta_netif) return ESP_ERR_NO_MEM;
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "WiFi init failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                    network_event_handler, NULL), TAG, "WiFi handler");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                    network_event_handler, NULL), TAG, "IP handler");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "STA mode failed");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "WiFi start failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_ps(WIFI_PS_NONE), TAG, "disable WiFi power save failed");
    if (xTaskCreate(reconnect_task, "wifi_reconnect", 4096, NULL, 4, NULL) != pdPASS)
        return ESP_ERR_NO_MEM;
    ESP_LOGI(TAG, "WiFi STA initialized");
    return ESP_OK;
}

esp_err_t julia_wifi_connect(const char *ssid, const char *password)
{
    if (!ssid || !ssid[0] || !password) return ESP_ERR_INVALID_ARG;
    if (!s_events || !s_sta_netif) return ESP_ERR_INVALID_STATE;
    wifi_config_t config = {0};
    strlcpy((char *)config.sta.ssid, ssid, sizeof(config.sta.ssid));
    strlcpy((char *)config.sta.password, password, sizeof(config.sta.password));
    config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    config.sta.threshold.rssi = -90;
    config.sta.threshold.authmode = password[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    config.sta.failure_retry_cnt = 5;
    config.sta.pmf_cfg.capable = true;
    config.sta.pmf_cfg.required = false;
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &config), TAG, "set config failed");
    s_should_connect = true;
    s_initial_connect = true;
    xEventGroupClearBits(s_events, CONNECTED_BIT | DISCONNECTED_BIT | RECONNECT_BIT);

    for (int attempt = 1; attempt <= INITIAL_RETRIES; ++attempt) {
        ESP_LOGI(TAG, "connecting to %s, attempt %d/%d", ssid, attempt, INITIAL_RETRIES);
        esp_err_t err = esp_wifi_connect();
        if (err != ESP_OK) ESP_LOGW(TAG, "connect call failed: %s", esp_err_to_name(err));
        EventBits_t bits = xEventGroupWaitBits(s_events, CONNECTED_BIT | DISCONNECTED_BIT,
                                               pdTRUE, pdFALSE, pdMS_TO_TICKS(INITIAL_RETRY_MS));
        if (bits & CONNECTED_BIT) {
            s_initial_connect = false;
            return ESP_OK;
        }
        if (attempt < INITIAL_RETRIES) vTaskDelay(pdMS_TO_TICKS(INITIAL_RETRY_MS));
    }
    s_scanning = true;
    s_initial_connect = false;
    xEventGroupClearBits(s_events, RECONNECT_BIT);
    log_visible_networks();
    s_scanning = false;
    xEventGroupSetBits(s_events, RECONNECT_BIT);
    ESP_LOGW(TAG, "initial connection failed; automatic reconnect enabled");
    return ESP_ERR_TIMEOUT;
}

esp_err_t julia_wifi_disconnect(void)
{
    if (!s_events) return ESP_ERR_INVALID_STATE;
    s_should_connect = false;
    s_connected = false;
    s_ip.addr = 0;
    xEventGroupClearBits(s_events, RECONNECT_BIT);
    xEventGroupClearBits(s_events, CONNECTED_BIT);
    esp_err_t err = esp_wifi_disconnect();
    if (err == ESP_ERR_WIFI_NOT_CONNECT) err = ESP_OK;
    return err;
}

bool julia_wifi_is_connected(void) { return s_connected; }

int8_t julia_wifi_get_rssi(void)
{
    if (!s_connected) return INT8_MIN;
    wifi_ap_record_t ap;
    return esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? ap.rssi : INT8_MIN;
}

esp_err_t julia_wifi_get_ip(esp_ip4_addr_t *ip)
{
    if (!ip) return ESP_ERR_INVALID_ARG;
    if (!s_sta_netif) return ESP_ERR_INVALID_STATE;
    if (!s_connected) return ESP_ERR_INVALID_STATE;
    *ip = s_ip;
    return ESP_OK;
}

esp_err_t julia_wifi_wait_cloud_ready(uint32_t timeout_ms)
{
    if (!s_events || !s_sta_netif) return ESP_ERR_INVALID_STATE;
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    int stable_checks = 0;
    do {
        int8_t rssi = julia_wifi_get_rssi();
        if (s_connected && s_ip.addr != 0 && rssi != INT8_MIN && rssi >= -82) {
            if (++stable_checks >= 2) return ESP_OK;
        } else {
            stable_checks = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(250));
    } while ((int32_t)(deadline - xTaskGetTickCount()) > 0);
    return ESP_ERR_TIMEOUT;
}

esp_err_t julia_wifi_check_dns(const char *hostname)
{
    if (!hostname || !hostname[0]) return ESP_ERR_INVALID_ARG;
    if (!s_events || !s_dns_lock || !s_connected) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_dns_lock, pdMS_TO_TICKS(1000)) != pdTRUE) return ESP_ERR_TIMEOUT;
    xEventGroupClearBits(s_events, DNS_DONE_BIT | DNS_FAILED_BIT);
    uint32_t request_id = ++s_dns_request_id;
    err_t err = dns_gethostbyname(hostname, &s_dns_result, dns_callback,
                                  (void *)(uintptr_t)request_id);
    if (err == ERR_OK) {
        ESP_LOGI(TAG, "DNS resolved from cache: %s -> %s", hostname, ipaddr_ntoa(&s_dns_result));
        xSemaphoreGive(s_dns_lock);
        return ESP_OK;
    }
    if (err != ERR_INPROGRESS) {
        ESP_LOGW(TAG, "DNS request failed for %s: %d", hostname, err);
        xSemaphoreGive(s_dns_lock);
        return ESP_ERR_NOT_FOUND;
    }
    EventBits_t bits = xEventGroupWaitBits(s_events, DNS_DONE_BIT | DNS_FAILED_BIT,
                                           pdTRUE, pdFALSE, pdMS_TO_TICKS(5000));
    esp_err_t result = ESP_ERR_TIMEOUT;
    if (bits & DNS_DONE_BIT) {
        ESP_LOGI(TAG, "DNS resolved: %s -> %s", hostname, ipaddr_ntoa(&s_dns_result));
        result = ESP_OK;
    } else if (bits & DNS_FAILED_BIT) {
        ESP_LOGW(TAG, "DNS server could not resolve %s", hostname);
        result = ESP_ERR_NOT_FOUND;
    } else {
        ESP_LOGW(TAG, "DNS timed out after 5 seconds: %s", hostname);
    }
    xSemaphoreGive(s_dns_lock);
    return result;
}

esp_err_t julia_wifi_start_provisioning(void)
{
    if (!s_ap_netif) s_ap_netif = esp_netif_create_default_wifi_ap();
    if (!s_ap_netif) return ESP_ERR_NO_MEM;
    wifi_config_t ap = {0};
    strlcpy((char *)ap.ap.ssid, "Julia-Setup", sizeof(ap.ap.ssid));
    strlcpy((char *)ap.ap.password, "julia-setup", sizeof(ap.ap.password));
    ap.ap.ssid_len = strlen((char *)ap.ap.ssid);
    ap.ap.channel = 1;
    ap.ap.max_connection = 2;
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA), TAG, "AP mode failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &ap), TAG, "AP config failed");
    ESP_LOGW(TAG, "Provisioning AP started: SSID=Julia-Setup");
    return ESP_OK;
}
