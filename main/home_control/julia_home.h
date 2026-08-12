#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef enum {
    JULIA_HOME_BACKEND_VIRTUAL = 0,
    JULIA_HOME_BACKEND_HOME_ASSISTANT,
    JULIA_HOME_BACKEND_MATTER,
    JULIA_HOME_BACKEND_BLE_MESH,
} julia_home_backend_t;

typedef struct {
    bool light_on;
    uint8_t brightness;
    uint16_t color_temp;
    bool ac_on;
    uint8_t ac_mode;
    uint8_t temperature;
    uint8_t fan_speed;
    uint8_t curtain_percent;
    bool socket_on;
} julia_home_device_state_t;

typedef esp_err_t (*julia_home_transport_cb_t)(const char *topic, const char *json,
                                                void *ctx);

esp_err_t julia_home_init(void);
esp_err_t julia_home_light_set(uint8_t device_id, bool on, uint8_t brightness,
                               uint16_t color_temp);
esp_err_t julia_home_ac_set(uint8_t device_id, bool on, uint8_t mode,
                            uint8_t temp, uint8_t fan_speed);
esp_err_t julia_home_curtain_set(uint8_t device_id, uint8_t percent);
esp_err_t julia_home_socket_set(uint8_t device_id, bool on);
esp_err_t julia_home_handle_ai_command(const char *json_arguments);
esp_err_t julia_home_register_ai_functions(void);
esp_err_t julia_home_get_state(uint8_t device_id, julia_home_device_state_t *state);
void julia_home_set_transport(julia_home_transport_cb_t callback, void *ctx);
