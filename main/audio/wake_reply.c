#include "wake_reply.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "esp_log.h"
#include "esp_crc.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "julia_audio.h"
#include "julia_sd.h"
#include "reply_pcm_table.h"
#include "wake_word_config.h"

#define TAG "WAKE_REPLY"
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

typedef enum {
    REPLY_GROUP_COMMON,
    REPLY_GROUP_MORNING,
    REPLY_GROUP_LATE_NIGHT,
    REPLY_GROUP_REPEAT,
} reply_group_t;

typedef struct {
    const char *id;
    reply_group_t group;
    const char *text;
    const char *filename;
    uint32_t bytes;
    uint16_t duration_ms;
    bool embedded;
} reply_t;

#define REPLY_ROW(id, group, text, filename, embedded) \
    {#id, REPLY_GROUP_##group, text, filename, WAKE_REPLY_##id##_BYTES, \
     WAKE_REPLY_##id##_DURATION_MS, (embedded) != 0},
static const reply_t s_replies[] = {WAKE_REPLY_CATALOG(REPLY_ROW)};
#undef REPLY_ROW

extern const uint8_t reply_common_01_start[] asm("_binary_reply_common_01_pcm_start");
extern const uint8_t reply_common_01_end[] asm("_binary_reply_common_01_pcm_end");
extern const uint8_t reply_common_02_start[] asm("_binary_reply_common_02_pcm_start");
extern const uint8_t reply_common_02_end[] asm("_binary_reply_common_02_pcm_end");
extern const uint8_t reply_common_03_start[] asm("_binary_reply_common_03_pcm_start");
extern const uint8_t reply_common_03_end[] asm("_binary_reply_common_03_pcm_end");
extern const uint8_t reply_common_04_start[] asm("_binary_reply_common_04_pcm_start");
extern const uint8_t reply_common_04_end[] asm("_binary_reply_common_04_pcm_end");

typedef struct { const uint8_t *start; const uint8_t *end; } embedded_pcm_t;
static const embedded_pcm_t s_embedded[] = {
    {reply_common_01_start, reply_common_01_end},
    {reply_common_02_start, reply_common_02_end},
    {reply_common_03_start, reply_common_03_end},
    {reply_common_04_start, reply_common_04_end},
};

static int16_t s_common_history[WAKE_REPLY_RECENT_HISTORY] = {-1, -1, -1, -1, -1};
static uint8_t s_history_count;
static time_t s_repeat_window_start;
static uint8_t s_repeat_count;

static bool recently_used(size_t index)
{
    for (uint8_t i = 0; i < s_history_count; ++i)
        if (s_common_history[i] == (int16_t)index) return true;
    return false;
}

static void remember_common(size_t index)
{
    if (s_history_count < WAKE_REPLY_RECENT_HISTORY) ++s_history_count;
    memmove(&s_common_history[1], &s_common_history[0],
            (s_history_count - 1U) * sizeof(s_common_history[0]));
    s_common_history[0] = (int16_t)index;
}

static reply_group_t select_group(time_t now)
{
    struct tm local = {0};
    localtime_r(&now, &local);
    if (local.tm_year + 1900 >= 2024) {
        if (local.tm_hour >= 6 && local.tm_hour < 10) return REPLY_GROUP_MORNING;
        if (local.tm_hour >= 23 || local.tm_hour < 6) return REPLY_GROUP_LATE_NIGHT;
    }

    if (!s_repeat_window_start || now < s_repeat_window_start ||
        (uint32_t)(now - s_repeat_window_start) > WAKE_REPLY_REPEAT_WINDOW_S) {
        s_repeat_window_start = now;
        s_repeat_count = 1;
    } else if (s_repeat_count < UINT8_MAX) {
        ++s_repeat_count;
    }
    if (s_repeat_count >= 3) return REPLY_GROUP_REPEAT;
    return REPLY_GROUP_COMMON;
}

static size_t select_reply(reply_group_t group)
{
    size_t candidates[ARRAY_SIZE(s_replies)];
    size_t count = 0;
    for (size_t i = 0; i < ARRAY_SIZE(s_replies); ++i) {
        if (s_replies[i].group != group) continue;
        if (group == REPLY_GROUP_COMMON && recently_used(i)) continue;
        candidates[count++] = i;
    }
    if (!count && group == REPLY_GROUP_COMMON) {
        s_history_count = 0;
        return select_reply(group);
    }
    return candidates[esp_random() % count];
}

static esp_err_t play_embedded_fallback(void)
{
    size_t first = esp_random() % ARRAY_SIZE(s_embedded);
    for (size_t attempt = 0; attempt < ARRAY_SIZE(s_embedded); ++attempt) {
        size_t embedded_index = (first + attempt) % ARRAY_SIZE(s_embedded);
        const embedded_pcm_t *pcm = &s_embedded[embedded_index];
        size_t bytes = (size_t)(pcm->end - pcm->start);
        if (!bytes || (bytes & 1U)) continue;
        esp_err_t err = julia_audio_play_pcm_memory(pcm->start, bytes);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "source=flash text=%s bytes=%u",
                     s_replies[embedded_index].text, (unsigned)bytes);
            remember_common(embedded_index);
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

bool wake_reply_is_known_asset(const char *filename, size_t bytes)
{
    if (!filename || strchr(filename, '/') || strchr(filename, '\\')) return false;
    for (size_t i = 0; i < ARRAY_SIZE(s_replies); ++i)
        if (strcmp(filename, s_replies[i].filename) == 0 && bytes == s_replies[i].bytes)
            return true;
    return false;
}

esp_err_t wake_reply_play(void)
{
    time_t now = time(NULL);
    reply_group_t group = select_group(now);
    size_t index = select_reply(group);
    char path[96];
    int length = snprintf(path, sizeof(path), "%s/%s", WAKE_REPLY_SD_DIRECTORY,
                          s_replies[index].filename);
    esp_err_t err = length > 0 && (size_t)length < sizeof(path)
                        ? julia_audio_play_pcm_file(path) : ESP_ERR_INVALID_SIZE;
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "source=sd group=%d id=%s text=%s duration_ms=%u bytes=%u",
                 group, s_replies[index].id, s_replies[index].text,
                 s_replies[index].duration_ms, (unsigned)s_replies[index].bytes);
        if (group == REPLY_GROUP_COMMON) remember_common(index);
        return ESP_OK;
    }
    ESP_LOGW(TAG, "SD reply unavailable path=%s error=%s; trying flash",
             path, esp_err_to_name(err));
    return play_embedded_fallback();
}

esp_err_t wake_reply_install_stream(FILE *input, const char *filename, size_t bytes,
                                    uint32_t expected_crc)
{
    if (!input || !filename || !bytes || bytes > 128U * 1024U || (bytes & 1U))
        return ESP_ERR_INVALID_ARG;
    if (!wake_reply_is_known_asset(filename, bytes)) return ESP_ERR_INVALID_ARG;
    if (!julia_sd_is_mounted() || !julia_sd_lock(pdMS_TO_TICKS(3000)))
        return ESP_ERR_INVALID_STATE;

    mkdir("/sdcard/julia", 0775);
    mkdir(WAKE_REPLY_SD_DIRECTORY, 0775);
    char path[96], temporary[100];
    snprintf(path, sizeof(path), "%s/%s", WAKE_REPLY_SD_DIRECTORY, filename);
    snprintf(temporary, sizeof(temporary), "%s.tmp", path);
    FILE *file = fopen(temporary, "wb");
    uint8_t block[4096];
    size_t total = 0;
    uint32_t crc = 0;
    esp_err_t result = file ? ESP_OK : ESP_FAIL;
    int64_t deadline = esp_timer_get_time() + 10000000;
    while (result == ESP_OK && total < bytes) {
        size_t want = bytes - total > sizeof(block) ? sizeof(block) : bytes - total;
        size_t received = 0;
        while (received < want && esp_timer_get_time() < deadline) {
            size_t part = fread(block + received, 1, want - received, input);
            if (part) {
                received += part;
                deadline = esp_timer_get_time() + 10000000;
            } else {
                clearerr(input);
                vTaskDelay(pdMS_TO_TICKS(10));
            }
        }
        if (received != want || fwrite(block, 1, want, file) != want) {
            result = ESP_ERR_TIMEOUT;
            break;
        }
        crc = esp_crc32_le(crc, block, want);
        total += want;
    }
    if (file && fclose(file) != 0 && result == ESP_OK) result = ESP_FAIL;
    if (result == ESP_OK && crc != expected_crc) result = ESP_ERR_INVALID_CRC;
    if (result == ESP_OK) {
        remove(path);
        if (rename(temporary, path) != 0) result = ESP_FAIL;
    }
    if (result != ESP_OK) remove(temporary);
    julia_sd_unlock();
    ESP_LOGI(TAG, "install file=%s bytes=%u crc=%08lx result=%s", filename,
             (unsigned)total, (unsigned long)crc, esp_err_to_name(result));
    return result;
}
