#include "julia_audio_player.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "avatar_mouth.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "julia_audio.h"

#define TAG "JULIA_AUDIO_PLAYER"
#define FRAME_SAMPLES (JULIA_AUDIO_SAMPLE_RATE / 25U)

typedef struct { uint8_t *pcm; uint32_t bytes; uint32_t generation; } play_request_t;
static volatile bool s_playing;
static volatile uint32_t s_generation;
static volatile uint32_t s_position_ms;
static volatile uint16_t s_rms;
static void (*s_complete)(esp_err_t);
static volatile bool s_streaming;
static volatile bool s_file_playing;
static portMUX_TYPE s_stage_lock = portMUX_INITIALIZER_UNLOCKED;
static char s_staged_resource[32];
static volatile bool s_audio_pending;

static uint16_t normalized_rms(const int16_t *samples, size_t count)
{
    uint64_t energy = 0;
    for (size_t i = 0; i < count; ++i) {
        int32_t sample = samples[i];
        energy += (uint64_t)(sample * sample);
    }
    uint32_t raw = count ? (uint32_t)sqrt((double)energy / count) : 0;
    uint32_t scaled = raw / 16U;
    return (uint16_t)(scaled > 1023U ? 1023U : scaled);
}

static void play_task(void *argument)
{
    play_request_t *request = argument;
    esp_err_t err = ESP_OK;
    size_t samples = request->bytes / sizeof(int16_t), offset = 0;
    while (offset < samples && request->generation == s_generation) {
        size_t count = samples - offset;
        if (count > FRAME_SAMPLES) count = FRAME_SAMPLES;
        const int16_t *frame = (const int16_t *)request->pcm + offset;
        s_rms = normalized_rms(frame, count);
        avatar_mouth_set_rms(s_rms);
        err = julia_audio_play_start((uint8_t *)frame, count * sizeof(int16_t));
        if (err != ESP_OK) break;
        offset += count;
        s_position_ms = (uint32_t)(offset * 1000U / JULIA_AUDIO_SAMPLE_RATE);
    }
    esp_err_t stop = julia_audio_play_stop();
    if (err == ESP_OK) err = stop;
    if (request->generation != s_generation && err == ESP_OK) err = ESP_ERR_INVALID_STATE;
    bool current = request->generation == s_generation;
    if (current) { s_rms = 0; mouth_force_idle(); s_playing = false; }
    ESP_LOGI(TAG, "complete position_ms=%lu result=%s",
             (unsigned long)s_position_ms, esp_err_to_name(err));
    free(request->pcm); free(request);
    if (current && s_complete) s_complete(err);
    vTaskDeleteWithCaps(NULL);
}

void audio_player_init(void) {}
void audio_player_on_complete(void (*callback)(esp_err_t)) { s_complete = callback; }

esp_err_t audio_player_play(const uint8_t *pcm, uint32_t len)
{
    if (!pcm || !len || (len & 1U)) return ESP_ERR_INVALID_ARG;
    audio_player_stop();
    play_request_t *request = calloc(1, sizeof(*request));
    if (!request) return ESP_ERR_NO_MEM;
    request->pcm = heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!request->pcm) { free(request); return ESP_ERR_NO_MEM; }
    memcpy(request->pcm, pcm, len); request->bytes = len;
    request->generation = ++s_generation; s_position_ms = 0; s_rms = 0; s_playing = true;
    if (xTaskCreateWithCaps(play_task, "audio_player", 4096, request, 6, NULL,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        s_playing = false; free(request->pcm); free(request); return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void audio_player_stop(void)
{
    if (s_playing || s_streaming) {
        ++s_generation; s_streaming = false; s_file_playing = false;
        julia_audio_play_stop(); s_rms = 0; mouth_force_idle(); s_playing = false;
    }
}
bool audio_player_is_playing(void) { return s_playing; }
uint32_t audio_player_get_position_ms(void) { return s_position_ms; }
uint16_t audio_player_get_rms(void) { return s_rms; }

esp_err_t audio_player_stream_begin(void)
{
    audio_player_stop();
    ++s_generation; s_position_ms = 0; s_rms = 0; s_playing = true; s_streaming = true;
    return ESP_OK;
}

esp_err_t audio_player_stream_write(const uint8_t *pcm, uint32_t len)
{
    if (!s_streaming || !pcm || !len || (len & 1U)) return ESP_ERR_INVALID_STATE;
    const int16_t *samples = (const int16_t *)pcm;
    s_rms = normalized_rms(samples, len / 2U);
    avatar_mouth_set_rms(s_rms);
    esp_err_t err = julia_audio_play_start((uint8_t *)pcm, len);
    if (err == ESP_OK) s_position_ms += (len / 2U) * 1000U / JULIA_AUDIO_SAMPLE_RATE;
    return err;
}

void audio_player_stream_end(void)
{
    if (!s_streaming) return;
    julia_audio_play_stop(); s_streaming = false; s_file_playing = false;
    s_playing = false; s_rms = 0; mouth_force_idle();
    if (s_complete) s_complete(ESP_OK);
}

static void file_task(void *argument)
{
    char *path = argument;
    FILE *file = fopen(path, "rb");
    uint8_t *block = heap_caps_malloc(4096, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    esp_err_t err = file && block ? audio_player_stream_begin() : ESP_FAIL;
    s_file_playing = err == ESP_OK;
    if (err == ESP_OK) {
        size_t probe = fread(block, 1, 44, file);
        if (probe == 44 && !memcmp(block, "RIFF", 4) && !memcmp(block + 8, "WAVE", 4)) {
            uint16_t channels = (uint16_t)block[22] | ((uint16_t)block[23] << 8);
            uint32_t rate = (uint32_t)block[24] | ((uint32_t)block[25] << 8) |
                            ((uint32_t)block[26] << 16) | ((uint32_t)block[27] << 24);
            uint16_t bits = (uint16_t)block[34] | ((uint16_t)block[35] << 8);
            if (channels != 1 || rate != JULIA_AUDIO_SAMPLE_RATE || bits != 16)
                err = ESP_ERR_NOT_SUPPORTED;
        } else {
            rewind(file);
        }
    }
    while (err == ESP_OK && s_file_playing) {
        size_t bytes = fread(block, 1, 4096, file);
        if (!bytes) break;
        bytes &= ~1U;
        if (bytes) err = audio_player_stream_write(block, bytes);
    }
    if (file) fclose(file);
    heap_caps_free(block); free(path);
    if (s_streaming) audio_player_stream_end();
    if (err != ESP_OK) ESP_LOGE(TAG, "file playback failed: %s", esp_err_to_name(err));
    vTaskDelete(NULL);
}

esp_err_t audio_player_play_file(const char *path)
{
    if (!path || strncmp(path, "/sdcard/", 8)) return ESP_ERR_INVALID_ARG;
    char *copy = strdup(path); if (!copy) return ESP_ERR_NO_MEM;
    audio_player_stop();
    if (xTaskCreate(file_task, "pcm_file", 4096, copy, 5, NULL) != pdPASS) {
        free(copy); return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
void audio_player_stop_file(void) { audio_player_stop(); }
bool audio_player_is_file_playing(void) { return s_file_playing; }
esp_err_t audio_player_play_resource(const char *name)
{
    if (!name || !name[0] || strlen(name) > 31) return ESP_ERR_INVALID_ARG;
    for (const char *p = name; *p; ++p)
        if (!( (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_'))
            return ESP_ERR_INVALID_ARG;
    char path[80]; snprintf(path, sizeof(path), "/sdcard/julia/audio/%s.pcm", name);
    return audio_player_play_file(path);
}

esp_err_t audio_player_stage_resource(const char *name)
{
    if (!name || !name[0] || strlen(name) >= sizeof(s_staged_resource))
        return ESP_ERR_INVALID_ARG;
    for (const char *p = name; *p; ++p)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_'))
            return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&s_stage_lock);
    strlcpy(s_staged_resource, name, sizeof(s_staged_resource));
    s_audio_pending = true;
    portEXIT_CRITICAL(&s_stage_lock);
    ESP_LOGI(TAG, "audio staged resource=%s", name);
    return ESP_OK;
}

bool audio_player_has_pending(void) { return s_audio_pending; }

esp_err_t audio_player_play_staged(void)
{
    char resource[sizeof(s_staged_resource)] = {0};
    portENTER_CRITICAL(&s_stage_lock);
    if (s_audio_pending) {
        memcpy(resource, s_staged_resource, sizeof(resource));
        s_audio_pending = false;
        s_staged_resource[0] = 0;
    }
    portEXIT_CRITICAL(&s_stage_lock);
    return resource[0] ? audio_player_play_resource(resource) : ESP_ERR_NOT_FOUND;
}

void audio_player_clear_staged(void)
{
    portENTER_CRITICAL(&s_stage_lock);
    s_audio_pending = false;
    s_staged_resource[0] = 0;
    portEXIT_CRITICAL(&s_stage_lock);
}
