#include "julia_home.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "julia_ai_client.h"
#include "julia_ui.h"
#include "sdkconfig.h"

#define TAG "JULIA_HOME"
#define DEVICE_COUNT 4
#define HOME_LOG_PATH "/sdcard/julia/home.log"

static julia_home_backend_t s_backend;
static julia_home_device_state_t s_states[DEVICE_COUNT];
static SemaphoreHandle_t s_lock;
static julia_home_transport_cb_t s_transport;
static void *s_transport_ctx;

static const char *device_name(uint8_t id)
{
    static const char *names[] = {"living_room", "bedroom", "study", "kitchen"};
    return id < sizeof(names) / sizeof(names[0]) ? names[id] : "unknown";
}

static int device_id_from_json(const cJSON *root)
{
    cJSON *device = cJSON_GetObjectItemCaseSensitive(root, "device_id");
    if (cJSON_IsNumber(device)) return device->valueint;
    device = cJSON_GetObjectItemCaseSensitive(root, "device");
    if (!cJSON_IsString(device)) return -1;
    if (!strcmp(device->valuestring, "living_room")) return 0;
    if (!strcmp(device->valuestring, "bedroom")) return 1;
    if (!strcmp(device->valuestring, "study")) return 2;
    if (!strcmp(device->valuestring, "kitchen")) return 3;
    return -1;
}

static void audit_command(const char *action, int id, esp_err_t result, const char *detail)
{
    struct stat st;
    const char *mode = "ab";
    if (stat(HOME_LOG_PATH, &st) == 0 && st.st_size >= 64 * 1024) {
        mode = "wb";
        ESP_LOGW(TAG, "home audit log reached 64 KB; rotating");
    }
    FILE *file = fopen(HOME_LOG_PATH, mode);
    if (!file) return;
    cJSON *entry = cJSON_CreateObject();
    cJSON_AddNumberToObject(entry, "timestamp", (double)time(NULL));
    cJSON_AddStringToObject(entry, "action", action ? action : "unknown");
    cJSON_AddStringToObject(entry, "device", id >= 0 && id < DEVICE_COUNT ? device_name(id) : "invalid");
    cJSON_AddStringToObject(entry, "result", esp_err_to_name(result));
    if (detail) cJSON_AddStringToObject(entry, "detail", detail);
    char *line = cJSON_PrintUnformatted(entry); cJSON_Delete(entry);
    if (line) { fputs(line, file); fputc('\n', file); cJSON_free(line); }
    fclose(file);
}

static void virtual_feedback(const char *message)
{
    ESP_LOGI(TAG, "%s", message);
    julia_ui_speak(message);
}

static esp_err_t home_assistant_call(const char *domain, const char *service,
                                     const char *json_body)
{
    if (!CONFIG_JULIA_HA_URL[0] || !CONFIG_JULIA_HA_TOKEN[0]) return ESP_ERR_INVALID_STATE;
    char url[256];
    snprintf(url, sizeof(url), "%s/api/services/%s/%s", CONFIG_JULIA_HA_URL, domain, service);
    esp_http_client_config_t cfg = {
        .url = url, .method = HTTP_METHOD_POST, .timeout_ms = 5000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return ESP_ERR_NO_MEM;
    char authorization[256];
    snprintf(authorization, sizeof(authorization), "Bearer %s", CONFIG_JULIA_HA_TOKEN);
    esp_http_client_set_header(client, "Authorization", authorization);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, json_body, strlen(json_body));
    esp_err_t err = ESP_FAIL;
    int status = 0;
    for (int attempt = 1; attempt <= 2; ++attempt) {
        err = esp_http_client_perform(client);
        status = esp_http_client_get_status_code(client);
        if (err == ESP_OK && status >= 200 && status < 300) break;
        ESP_LOGW(TAG, "Home Assistant attempt %d failed: %s, HTTP %d",
                 attempt, esp_err_to_name(err), status);
        if (attempt < 2) vTaskDelay(pdMS_TO_TICKS(500));
    }
    esp_http_client_cleanup(client);
    if (err != ESP_OK) return err;
    if (status < 200 || status >= 300) {
        ESP_LOGE(TAG, "Home Assistant returned HTTP %d", status);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t dispatch(const char *domain, const char *service, cJSON *body,
                          const char *feedback)
{
    if (!body || !domain || !service) {
        if (body) cJSON_Delete(body);
        return ESP_ERR_NO_MEM;
    }
    char *json = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (!json) return ESP_ERR_NO_MEM;
    esp_err_t err;
    if (s_backend == JULIA_HOME_BACKEND_VIRTUAL) {
        virtual_feedback(feedback);
        err = ESP_OK;
    } else if (s_backend == JULIA_HOME_BACKEND_HOME_ASSISTANT) {
        err = home_assistant_call(domain, service, json);
        if (err == ESP_OK) virtual_feedback(feedback);
    } else {
        err = ESP_ERR_NOT_SUPPORTED;
    }
    if (err == ESP_OK && s_transport) {
        char topic[96];
        snprintf(topic, sizeof(topic), "julia/home/%s/%s", domain, service);
        esp_err_t transport_err = s_transport(topic, json, s_transport_ctx);
        if (transport_err != ESP_OK) err = transport_err;
    }
    cJSON_free(json);
    return err;
}

esp_err_t julia_home_init(void)
{
#if CONFIG_JULIA_HOME_BACKEND_HA
    s_backend = JULIA_HOME_BACKEND_HOME_ASSISTANT;
#else
    s_backend = JULIA_HOME_BACKEND_VIRTUAL;
#endif
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;
    for (int i = 0; i < DEVICE_COUNT; ++i) {
        s_states[i].brightness = 70; s_states[i].color_temp = 3000;
        s_states[i].temperature = 26; s_states[i].fan_speed = 2;
        s_states[i].curtain_percent = 50;
    }
    ESP_LOGI(TAG, "backend=%s, devices=%d", s_backend == JULIA_HOME_BACKEND_VIRTUAL ? "virtual" : "home_assistant", DEVICE_COUNT);
    return ESP_OK;
}

esp_err_t julia_home_light_set(uint8_t id, bool on, uint8_t brightness, uint16_t color_temp)
{
    if (id >= DEVICE_COUNT || brightness > 100 || (color_temp && (color_temp < 2000 || color_temp > 6500)))
        return ESP_ERR_INVALID_ARG;
    cJSON *body = cJSON_CreateObject();
    if (!body) return ESP_ERR_NO_MEM;
    char entity[48], feedback[96];
    snprintf(entity, sizeof(entity), "light.%s", device_name(id));
    cJSON_AddStringToObject(body, "entity_id", entity);
    if (on) {
        cJSON_AddNumberToObject(body, "brightness_pct", brightness);
        if (color_temp) cJSON_AddNumberToObject(body, "color_temp_kelvin", color_temp);
    }
    snprintf(feedback, sizeof(feedback), "Light %s: %s, brightness %u%%",
             device_name(id), on ? "on" : "off", brightness);
    esp_err_t err = dispatch("light", on ? "turn_on" : "turn_off", body, feedback);
    if (err == ESP_OK) { xSemaphoreTake(s_lock, portMAX_DELAY); s_states[id].light_on=on; s_states[id].brightness=brightness; s_states[id].color_temp=color_temp; xSemaphoreGive(s_lock); }
    return err;
}

esp_err_t julia_home_ac_set(uint8_t id, bool on, uint8_t mode, uint8_t temp, uint8_t fan_speed)
{
    if (id >= DEVICE_COUNT || mode > 4 || temp < 16 || temp > 30 || fan_speed > 5)
        return ESP_ERR_INVALID_ARG;
    cJSON *body = cJSON_CreateObject(); if (!body) return ESP_ERR_NO_MEM; char entity[48], feedback[96];
    snprintf(entity, sizeof(entity), "climate.%s", device_name(id));
    cJSON_AddStringToObject(body, "entity_id", entity);
    cJSON_AddNumberToObject(body, "temperature", temp);
    cJSON_AddNumberToObject(body, "mode", mode);
    cJSON_AddNumberToObject(body, "fan_speed", fan_speed);
    snprintf(feedback, sizeof(feedback), "AC %s: %s, %u C", device_name(id), on ? "on" : "off", temp);
    esp_err_t err = dispatch("climate", on ? "turn_on" : "turn_off", body, feedback);
    if (err == ESP_OK) { xSemaphoreTake(s_lock, portMAX_DELAY); s_states[id].ac_on=on; s_states[id].ac_mode=mode; s_states[id].temperature=temp; s_states[id].fan_speed=fan_speed; xSemaphoreGive(s_lock); }
    return err;
}

esp_err_t julia_home_curtain_set(uint8_t id, uint8_t percent)
{
    if (id >= DEVICE_COUNT || percent > 100) return ESP_ERR_INVALID_ARG;
    cJSON *body = cJSON_CreateObject(); if (!body) return ESP_ERR_NO_MEM; char entity[48], feedback[96];
    snprintf(entity, sizeof(entity), "cover.%s", device_name(id));
    cJSON_AddStringToObject(body, "entity_id", entity);
    cJSON_AddNumberToObject(body, "position", percent);
    snprintf(feedback, sizeof(feedback), "Curtain %s: %u%%", device_name(id), percent);
    esp_err_t err = dispatch("cover", "set_cover_position", body, feedback);
    if (err == ESP_OK) { xSemaphoreTake(s_lock, portMAX_DELAY); s_states[id].curtain_percent=percent; xSemaphoreGive(s_lock); }
    return err;
}

esp_err_t julia_home_socket_set(uint8_t id, bool on)
{
    if (id >= DEVICE_COUNT) return ESP_ERR_INVALID_ARG;
    cJSON *body = cJSON_CreateObject(); if (!body) return ESP_ERR_NO_MEM; char entity[48], feedback[96];
    snprintf(entity, sizeof(entity), "switch.%s", device_name(id));
    cJSON_AddStringToObject(body, "entity_id", entity);
    snprintf(feedback, sizeof(feedback), "Socket %s: %s", device_name(id), on ? "on" : "off");
    esp_err_t err = dispatch("switch", on ? "turn_on" : "turn_off", body, feedback);
    if (err == ESP_OK) { xSemaphoreTake(s_lock, portMAX_DELAY); s_states[id].socket_on=on; xSemaphoreGive(s_lock); }
    return err;
}

static int json_int(const cJSON *root, const char *name, int fallback)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
    return cJSON_IsNumber(item) ? item->valueint : fallback;
}

esp_err_t julia_home_handle_ai_command(const char *arguments)
{
    cJSON *root = cJSON_Parse(arguments);
    if (!root) return ESP_ERR_INVALID_ARG;
    cJSON *action = cJSON_GetObjectItemCaseSensitive(root, "action");
    if (!cJSON_IsString(action)) { cJSON_Delete(root); return ESP_ERR_INVALID_ARG; }
    int id = device_id_from_json(root); esp_err_t err = ESP_ERR_NOT_SUPPORTED;
    if (id < 0 || id >= DEVICE_COUNT) {
        audit_command(action->valuestring, id, ESP_ERR_INVALID_ARG, "invalid device");
        cJSON_Delete(root); return ESP_ERR_INVALID_ARG;
    }
    cJSON *confirmed = cJSON_GetObjectItemCaseSensitive(root, "confirmed");
    if (!strcmp(action->valuestring, "socket_on") && !cJSON_IsTrue(confirmed)) {
        audit_command(action->valuestring, id, ESP_ERR_INVALID_STATE, "confirmation required");
        virtual_feedback("Socket power-on requires explicit confirmation");
        cJSON_Delete(root); return ESP_ERR_INVALID_STATE;
    }
    if (!strcmp(action->valuestring, "light_on") || !strcmp(action->valuestring, "light_off"))
        err = julia_home_light_set(id, !strcmp(action->valuestring, "light_on"),
                                   json_int(root, "brightness", 70), json_int(root, "color_temp", 3000));
    else if (!strcmp(action->valuestring, "ac_on") || !strcmp(action->valuestring, "ac_off"))
        err = julia_home_ac_set(id, !strcmp(action->valuestring, "ac_on"), json_int(root,"mode",0),
                                json_int(root,"temp",26), json_int(root,"fan_speed",2));
    else if (!strcmp(action->valuestring, "curtain_set"))
        err = julia_home_curtain_set(id, json_int(root, "percent", 50));
    else if (!strcmp(action->valuestring, "socket_on") || !strcmp(action->valuestring, "socket_off"))
        err = julia_home_socket_set(id, !strcmp(action->valuestring, "socket_on"));
    audit_command(action->valuestring, id, err, NULL);
    cJSON_Delete(root);
    return err;
}

static esp_err_t ai_home_callback(const char *arguments, void *ctx)
{
    (void)ctx;
    return julia_home_handle_ai_command(arguments);
}

esp_err_t julia_home_register_ai_functions(void)
{
    return julia_ai_register_function("control_home", ai_home_callback, NULL);
}

esp_err_t julia_home_get_state(uint8_t id, julia_home_device_state_t *state)
{
    if (id >= DEVICE_COUNT || !state || !s_lock) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_lock, portMAX_DELAY); *state = s_states[id]; xSemaphoreGive(s_lock);
    return ESP_OK;
}

void julia_home_set_transport(julia_home_transport_cb_t callback, void *ctx)
{
    s_transport = callback; s_transport_ctx = ctx;
}
