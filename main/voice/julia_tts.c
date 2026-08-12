#include "julia_tts.h"

#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "julia_speech_cloud.h"
#include "julia_audio_player.h"

#define TAG "JULIA_TTS"

static void (*s_callback)(const uint8_t *, uint32_t);
static volatile bool s_busy;
static volatile uint32_t s_generation;
static volatile int64_t s_asr_input_us;
static TaskHandle_t s_gate_task;
static volatile bool s_waiting_for_transition;
static volatile bool s_transition_ready;

static bool wait_for_speaking_transition(uint32_t generation)
{
    s_gate_task = xTaskGetCurrentTaskHandle();
    s_waiting_for_transition = true;
    uint32_t notified = 1;
    while (generation == s_generation && !s_transition_ready && notified)
        notified = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(3000));
    s_waiting_for_transition = false;
    s_gate_task = NULL;
    if (generation != s_generation) return false;
    s_transition_ready = false;
    if (!notified) ESP_LOGW(TAG, "AUDIO: transition timeout, force start");
    return true;
}

#if !CONFIG_JULIA_LLM_MOCK_MODE
static esp_err_t stream_chunk(const uint8_t *pcm, size_t bytes, void *context)
{
    uint32_t generation = (uint32_t)(uintptr_t)context;
    if (generation != s_generation) return ESP_ERR_INVALID_STATE;
    return audio_player_stream_write(pcm, (uint32_t)bytes);
}
#endif

static void synth_task(void *argument)
{
    char *text = argument;
    uint32_t generation = s_generation;
    if (!wait_for_speaking_transition(generation)) goto done;
#if CONFIG_JULIA_LLM_MOCK_MODE
    esp_err_t err = audio_player_play_staged();
    uint32_t first_ms = s_asr_input_us ?
        (uint32_t)((esp_timer_get_time() - s_asr_input_us) / 1000) : 0;
    ESP_LOGI(TAG, "[TIME] mock_audio_start first_audio_ms=%u resource=%s result=%s",
             (unsigned)first_ms, text, esp_err_to_name(err));
    ESP_LOGI(TAG, "Transition complete, audio started");
#else
    int64_t started = esp_timer_get_time();
    uint32_t first_audio_ms = 0;
    esp_err_t err = audio_player_stream_begin();
    if (err == ESP_OK)
        err = julia_speech_tts_stream(text, stream_chunk, (void *)(uintptr_t)generation,
                                      &first_audio_ms);
    uint32_t elapsed_ms = (uint32_t)((esp_timer_get_time() - started) / 1000);
    ESP_LOGI(TAG, "ready latency_ms=%lu bytes=%u result=%s",
             (unsigned long)elapsed_ms, 0U,
             esp_err_to_name(err));
    if (generation == s_generation) {
        audio_player_stream_end();
        if (err != ESP_OK) {
            int16_t *pcm = NULL; size_t samples = 0;
            ESP_LOGW(TAG, "stream failed, trying whole-response fallback");
            if (julia_speech_tts(text, &pcm, &samples) == ESP_OK && s_callback)
                s_callback((const uint8_t *)pcm, (uint32_t)(samples * sizeof(int16_t)));
            free(pcm);
        }
    }
    ESP_LOGI(TAG, "first_audio_ms=%u", (unsigned)first_audio_ms);
#endif
done:
    free(text);
    if (generation == s_generation) s_busy = false;
    vTaskDeleteWithCaps(NULL);
}

void tts_init(void) {}
void tts_on_audio_ready(void (*callback)(const uint8_t *, uint32_t)) { s_callback = callback; }

esp_err_t tts_synthesize(const char *text)
{
    if (!text || !text[0]) return ESP_ERR_INVALID_ARG;
#if CONFIG_JULIA_LLM_MOCK_MODE
    const char *resource = "error";
    if (strstr(text, "今天过得")) resource = "mock_hello";
    else if (strstr(text, "天气不错")) resource = "mock_weather";
    else if (strstr(text, "早点休息")) resource = "mock_sleep";
    else if (strstr(text, "安静待着")) resource = "sorry";
    else if (strstr(text, "我在听")) resource = "listen";
    if (s_busy) return ESP_ERR_INVALID_STATE;
    esp_err_t staged = audio_player_stage_resource(resource);
    if (staged != ESP_OK) return staged;
    char *copy = strdup(resource);
    if (!copy) { audio_player_clear_staged(); return ESP_ERR_NO_MEM; }
    ++s_generation; s_transition_ready = false; s_busy = true;
    if (xTaskCreateWithCaps(synth_task, "mock_tts", 4096, copy, 5, NULL,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        s_busy = false; audio_player_clear_staged(); free(copy); return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "Audio staged, waiting for transition complete resource=%s", resource);
    return ESP_OK;
#else
    if (s_busy) return ESP_ERR_INVALID_STATE;
    char *copy = strdup(text);
    if (!copy) return ESP_ERR_NO_MEM;
    ++s_generation; s_transition_ready = false; s_busy = true;
    if (xTaskCreateWithCaps(synth_task, "mock_tts", 12288, copy, 5, NULL,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        s_busy = false; free(copy); return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
#endif
}

bool tts_is_busy(void) { return s_busy; }
void tts_cancel(void)
{
    ++s_generation; s_busy = false; s_transition_ready = false; audio_player_clear_staged();
    TaskHandle_t task = s_gate_task;
    if (task) xTaskNotifyGive(task);
}
void tts_note_asr_input(void) { s_asr_input_us = esp_timer_get_time(); }
void tts_on_transition_event(bool speaking_ready)
{
    if (speaking_ready) s_transition_ready = true;
    TaskHandle_t task = s_gate_task;
    if (s_waiting_for_transition && task) xTaskNotifyGive(task);
}
bool tts_audio_is_staged(void) { return audio_player_has_pending() || s_waiting_for_transition; }
