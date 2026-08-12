#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef enum {
    JULIA_SOUND_QUIET = 0,
    JULIA_SOUND_SPEECH,
    JULIA_SOUND_MUSIC,
    JULIA_SOUND_TELEVISION,
    JULIA_SOUND_ACTIVITY,
} julia_sound_class_t;

typedef struct {
    bool user_nearby;
    bool bluetooth_connected;
    int8_t bluetooth_rssi;
    uint16_t ambient_light_lux;
    julia_sound_class_t sound_class;
    uint16_t sound_level;
    bool imu_stationary;
    bool charging;
    uint8_t battery_percent;
    bool user_asleep;
} julia_sensor_status_t;

esp_err_t julia_sensors_init(void);
void julia_sensor_get_status(julia_sensor_status_t *status);
esp_err_t julia_sensor_set_simulated(const julia_sensor_status_t *status);
const char *julia_sensor_sound_name(julia_sound_class_t sound);
