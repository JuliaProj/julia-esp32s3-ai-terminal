#include "julia_sensors.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static SemaphoreHandle_t s_lock;
static julia_sensor_status_t s_status = {
    .user_nearby = true,
    .bluetooth_connected = true,
    .bluetooth_rssi = -55,
    .ambient_light_lux = 120,
    .sound_class = JULIA_SOUND_QUIET,
    .battery_percent = 80,
};

esp_err_t julia_sensors_init(void)
{
    if (s_lock) return ESP_OK;
    s_lock = xSemaphoreCreateMutex();
    return s_lock ? ESP_OK : ESP_ERR_NO_MEM;
}

void julia_sensor_get_status(julia_sensor_status_t *status)
{
    if (!status) return;
    if (!s_lock || xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) != pdTRUE) {
        memset(status, 0, sizeof(*status));
        return;
    }
    *status = s_status;
    xSemaphoreGive(s_lock);
}

esp_err_t julia_sensor_set_simulated(const julia_sensor_status_t *status)
{
    if (!status || !s_lock) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_status = *status;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

const char *julia_sensor_sound_name(julia_sound_class_t sound)
{
    static const char *names[] = {"quiet", "speech", "music", "television", "activity"};
    return sound <= JULIA_SOUND_ACTIVITY ? names[sound] : "unknown";
}
