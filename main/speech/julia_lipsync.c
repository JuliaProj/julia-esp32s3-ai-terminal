#include "julia_lipsync.h"

#include <math.h>
#include <stdio.h>

#include "julia_audio.h"
#include "julia_ui.h"
#include "esp_log.h"

#define LIPSYNC_FRAME_SAMPLES (JULIA_AUDIO_SAMPLE_RATE / 25)

static const char *TAG = "JULIA_LIPSYNC";
static size_t s_frame_count;
static uint32_t s_smoothed_rms;
static uint8_t s_mouth_level;
static uint16_t s_visual_openness_q8;

static uint8_t mouth_level_for_frame(const int16_t *samples, size_t count)
{
    uint64_t energy = 0;
    for (size_t i = 0; i < count; ++i) {
        int32_t sample = samples[i];
        energy += (uint64_t)(sample * sample);
    }
    uint32_t rms = count ? (uint32_t)sqrt((double)energy / count) : 0;
    /* 40 ms PCM frames can fluctuate sharply between adjacent syllables.
     * EMA plus asymmetric thresholds keeps articulation responsive without
     * making the mouth chatter at a threshold. */
    s_smoothed_rms = (s_smoothed_rms * 5U + rms * 3U) / 8U;
    static const uint16_t rise[] = {300, 950, 2300};
    static const uint16_t fall[] = {180, 650, 1650};
    uint8_t target = s_mouth_level;
    if (target < 3 && s_smoothed_rms >= rise[target]) target++;
    else if (target > 0 && s_smoothed_rms < fall[target - 1]) target--;
    s_mouth_level = target;
    return target;
}

void julia_lipsync_begin(void)
{
    s_frame_count = 0;
    s_smoothed_rms = 0;
    s_mouth_level = 0;
    s_visual_openness_q8 = 0;
    julia_ui_talking_start();
    ESP_LOGI(TAG, "started: frame=%dms", 1000 / 25);
}

esp_err_t julia_lipsync_play(const int16_t *samples, size_t sample_count)
{
    if (!samples || !sample_count) return ESP_ERR_INVALID_ARG;
    size_t offset = 0;
    while (offset < sample_count) {
        size_t count = sample_count - offset;
        if (count > LIPSYNC_FRAME_SAMPLES) count = LIPSYNC_FRAME_SAMPLES;
        uint8_t level = mouth_level_for_frame(samples + offset, count);
        uint16_t target_q8 = (uint16_t)level * 256U;
        int32_t delta = (int32_t)target_q8 - s_visual_openness_q8;
        /* Fast attack follows syllables; slower release avoids snapping shut.
         * The UI blends adjacent source mouths at the fractional position. */
        s_visual_openness_q8 = (uint16_t)((int32_t)s_visual_openness_q8 +
            (delta > 0 ? (delta + 1) / 2 : delta / 3));
        julia_ui_set_mouth_openness(s_visual_openness_q8);
        esp_err_t err = julia_audio_play_start((uint8_t *)(samples + offset),
                                               count * sizeof(int16_t));
        if (err != ESP_OK) return err;
        ++s_frame_count;
        offset += count;
    }
    return ESP_OK;
}

esp_err_t julia_lipsync_play_file(const char *path)
{
    if (!path) return ESP_ERR_INVALID_ARG;
    FILE *file = fopen(path, "rb");
    if (!file) return ESP_ERR_NOT_FOUND;
    int16_t frame[LIPSYNC_FRAME_SAMPLES];
    esp_err_t err = ESP_OK;
    while (true) {
        size_t count = fread(frame, sizeof(int16_t), LIPSYNC_FRAME_SAMPLES, file);
        if (count && (err = julia_lipsync_play(frame, count)) != ESP_OK) break;
        if (count < LIPSYNC_FRAME_SAMPLES) {
            if (ferror(file)) err = ESP_FAIL;
            break;
        }
    }
    fclose(file);
    return err;
}

esp_err_t julia_lipsync_end(void)
{
    julia_ui_talking_stop();
    esp_err_t err = julia_audio_play_stop();
    ESP_LOGI(TAG, "stopped: frames=%u result=%s", (unsigned)s_frame_count,
             esp_err_to_name(err));
    return err;
}
