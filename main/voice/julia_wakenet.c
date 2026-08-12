#include "julia_wakenet.h"
#include <string.h>
#include "julia_voice.h"
#include "wake_word_config.h"

static float s_threshold = 0.95f;

void julia_wakenet_get_status(julia_wakenet_status_t *status)
{
    if (!status) return;
    *status = (julia_wakenet_status_t){
        .model_name = ACTIVE_WAKENET_MODEL_NAME,
        .wake_word = WAKE_WORD_DISPLAY_TEXT,
        .threshold = s_threshold,
        .debounce_ms = 3000,
        .ready = julia_voice_last_audio_activity_ms() != 0,
    };
}
esp_err_t julia_wakenet_set_word(const char *word)
{
    return word && strcmp(word, WAKE_WORD_DISPLAY_TEXT) == 0 ? ESP_OK : ESP_ERR_NOT_SUPPORTED;
}
esp_err_t julia_wakenet_set_threshold(float threshold)
{
    if (threshold < 0.5f || threshold > 0.95f) return ESP_ERR_INVALID_ARG;
    s_threshold = threshold;
    return ESP_ERR_NOT_SUPPORTED;
}
