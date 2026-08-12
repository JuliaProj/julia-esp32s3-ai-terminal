#include "julia_aec.h"

static julia_afe_levels_t s_levels;
esp_err_t julia_afe_set_aec(uint8_t level)
{
    if (level > 3) return ESP_ERR_INVALID_ARG;
    s_levels.aec = level;
    return level ? ESP_ERR_NOT_SUPPORTED : ESP_OK;
}
esp_err_t julia_afe_set_ns(uint8_t level)
{
    if (level > 3) return ESP_ERR_INVALID_ARG;
    s_levels.ns = level;
    return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t julia_afe_set_agc(uint8_t level)
{
    if (level > 3) return ESP_ERR_INVALID_ARG;
    s_levels.agc = level;
    return ESP_ERR_NOT_SUPPORTED;
}
void julia_afe_get_levels(julia_afe_levels_t *levels) { if (levels) *levels = s_levels; }
