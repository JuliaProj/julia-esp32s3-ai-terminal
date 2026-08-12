#include "julia_local_tts.h"

#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_crc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_tts.h"
#include "esp_tts_voice_xiaole.h"
#include "driver/usb_serial_jtag.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "julia_lipsync.h"
#include "julia_sd.h"

#define TAG "JULIA_LOCAL_TTS"
#define VOICE_BACKUP_PATH "/sdcard/julia/tts/XIAOLE.BAK"
#define VOICE_TEMP_PATH   "/sdcard/julia/tts/XIAOLE.TMP"
#define IO_BLOCK_BYTES 8192U

static esp_tts_handle_t s_tts;
static esp_tts_voice_t *s_voice;
static uint8_t *s_voice_data;
static SemaphoreHandle_t s_mutex;
static uint8_t s_io_block[IO_BLOCK_BYTES];
static julia_local_tts_status_t s_status = {
    .state = JULIA_LOCAL_TTS_UNINITIALIZED,
    .last_error = ESP_ERR_INVALID_STATE,
};

static bool valid_utf8(const uint8_t *text, size_t length)
{
    size_t i = 0;
    while (i < length) {
        uint8_t c = text[i++];
        if (c < 0x80) continue;
        unsigned continuation = 0;
        uint32_t value = 0;
        if ((c & 0xe0) == 0xc0) { continuation = 1; value = c & 0x1f; }
        else if ((c & 0xf0) == 0xe0) { continuation = 2; value = c & 0x0f; }
        else if ((c & 0xf8) == 0xf0) { continuation = 3; value = c & 0x07; }
        else return false;
        if (i + continuation > length) return false;
        for (unsigned n = 0; n < continuation; ++n) {
            uint8_t next = text[i++];
            if ((next & 0xc0) != 0x80) return false;
            value = (value << 6) | (next & 0x3f);
        }
        if ((continuation == 1 && value < 0x80) ||
            (continuation == 2 && value < 0x800) ||
            (continuation == 3 && value < 0x10000) ||
            value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) return false;
    }
    return true;
}

static void set_failure(const char *stage, esp_err_t error)
{
    s_status.state = JULIA_LOCAL_TTS_UNAVAILABLE;
    s_status.last_error = error;
    s_status.free_heap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    s_status.free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    ESP_LOGE(TAG, "init failed stage=%s error=%s heap=%u psram=%u",
             stage, esp_err_to_name(error), (unsigned)s_status.free_heap,
             (unsigned)s_status.free_psram);
}

esp_err_t julia_local_tts_init(void)
{
    if (s_status.state == JULIA_LOCAL_TTS_READY && s_tts && s_voice && s_voice_data)
        return ESP_OK;
    if (s_status.state == JULIA_LOCAL_TTS_UNAVAILABLE) return s_status.last_error;
    if (!s_mutex) {
        s_mutex = xSemaphoreCreateMutex();
        if (!s_mutex) { set_failure("mutex", ESP_ERR_NO_MEM); return ESP_ERR_NO_MEM; }
    }
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(15000)) != pdTRUE) return ESP_ERR_TIMEOUT;
    if (s_status.state == JULIA_LOCAL_TTS_READY) { xSemaphoreGive(s_mutex); return ESP_OK; }
    s_status.state = JULIA_LOCAL_TTS_LOADING;
    s_status.init_attempts++;

    esp_err_t result = ESP_FAIL;
    struct stat info;
    if (!julia_sd_is_mounted()) { result = ESP_ERR_INVALID_STATE; set_failure("sd-not-mounted", result); goto done; }
    if (stat(JULIA_LOCAL_TTS_VOICE_PATH, &info) != 0) {
        result = ESP_ERR_NOT_FOUND; set_failure("voice-stat", result); goto done;
    }
    if ((size_t)info.st_size != JULIA_LOCAL_TTS_VOICE_BYTES) {
        result = ESP_ERR_INVALID_SIZE; set_failure("voice-size", result); goto done;
    }
    s_voice_data = heap_caps_malloc(JULIA_LOCAL_TTS_VOICE_BYTES,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_voice_data) { result = ESP_ERR_NO_MEM; set_failure("voice-alloc", result); goto done; }
    if (!julia_sd_lock(pdMS_TO_TICKS(5000))) {
        result = ESP_ERR_TIMEOUT; set_failure("sd-lock", result); goto done;
    }
    FILE *file = fopen(JULIA_LOCAL_TTS_VOICE_PATH, "rb");
    if (!file) {
        julia_sd_unlock(); result = ESP_ERR_NOT_FOUND; set_failure("voice-open", result); goto done;
    }
    size_t offset = 0;
    uint32_t crc = 0;
    while (offset < JULIA_LOCAL_TTS_VOICE_BYTES) {
        size_t want = JULIA_LOCAL_TTS_VOICE_BYTES - offset;
        if (want > IO_BLOCK_BYTES) want = IO_BLOCK_BYTES;
        size_t got = 0;
        int64_t deadline = esp_timer_get_time() + 5000000;
        while (got < want && esp_timer_get_time() < deadline) {
            size_t part = fread(s_voice_data + offset + got, 1, want - got, file);
            if (part) {
                got += part;
                deadline = esp_timer_get_time() + 5000000;
            } else {
                clearerr(file);
                vTaskDelay(pdMS_TO_TICKS(50));
            }
        }
        if (got != want) { result = ESP_ERR_TIMEOUT; break; }
        crc = esp_crc32_le(crc, s_voice_data + offset, got);
        offset += got;
    }
    fclose(file);
    julia_sd_unlock();
    if (result == ESP_ERR_INVALID_SIZE) { set_failure("voice-read", result); goto done; }
    if (crc != JULIA_LOCAL_TTS_VOICE_CRC32) {
        ESP_LOGE(TAG, "voice CRC mismatch actual=%08lx expected=%08lx",
                 (unsigned long)crc, (unsigned long)JULIA_LOCAL_TTS_VOICE_CRC32);
        result = ESP_ERR_INVALID_CRC; set_failure("voice-crc", result); goto done;
    }
    s_voice = esp_tts_voice_set_init(&esp_tts_voice_xiaole, s_voice_data);
    if (!s_voice) { result = ESP_ERR_NO_MEM; set_failure("voice-set", result); goto done; }
    s_tts = esp_tts_create(s_voice);
    if (!s_tts) { result = ESP_ERR_NO_MEM; set_failure("tts-create", result); goto done; }
    s_status.state = JULIA_LOCAL_TTS_READY;
    s_status.last_error = ESP_OK;
    s_status.voice_bytes = JULIA_LOCAL_TTS_VOICE_BYTES;
    s_status.free_heap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    s_status.free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    result = ESP_OK;
    ESP_LOGI(TAG, "ready voice=%u CRC=%08lx heap=%u psram=%u",
             JULIA_LOCAL_TTS_VOICE_BYTES, (unsigned long)JULIA_LOCAL_TTS_VOICE_CRC32,
             (unsigned)s_status.free_heap, (unsigned)s_status.free_psram);

done:
    if (result != ESP_OK && s_voice) {
        esp_tts_voice_set_free(s_voice);
        s_voice = NULL;
    }
    if (result != ESP_OK && s_voice_data) {
        free(s_voice_data);
        s_voice_data = NULL;
    }
    xSemaphoreGive(s_mutex);
    return result;
}

bool julia_local_tts_is_ready(void)
{
    return s_status.state == JULIA_LOCAL_TTS_READY && s_tts && s_voice && s_voice_data;
}

void julia_local_tts_get_status(julia_local_tts_status_t *status)
{
    if (status) *status = s_status;
}

esp_err_t julia_local_tts_speak(const char *text)
{
    if (!text || !text[0]) return ESP_ERR_INVALID_ARG;
    size_t length = strnlen(text, JULIA_LOCAL_TTS_MAX_TEXT_BYTES + 1);
    if (!length || length > JULIA_LOCAL_TTS_MAX_TEXT_BYTES ||
        !valid_utf8((const uint8_t *)text, length)) return ESP_ERR_INVALID_ARG;
    esp_err_t err = julia_local_tts_init();
    if (err != ESP_OK || !julia_local_tts_is_ready()) {
        s_status.synthesis_failed++;
        return err == ESP_OK ? ESP_ERR_INVALID_STATE : err;
    }
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(30000)) != pdTRUE) return ESP_ERR_TIMEOUT;
    if (!s_tts || !s_voice || !s_voice_data) { xSemaphoreGive(s_mutex); return ESP_ERR_INVALID_STATE; }
    if (!esp_tts_parse_chinese(s_tts, text)) {
        s_status.synthesis_failed++;
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_RESPONSE;
    }
    julia_lipsync_begin();
    while (true) {
        int samples = 0;
        short *pcm = esp_tts_stream_play(s_tts, &samples, 3);
        if (!pcm || samples <= 0) break;
        if (samples > 64 * 1024) { err = ESP_ERR_INVALID_SIZE; break; }
        err = julia_lipsync_play((const int16_t *)pcm, (size_t)samples);
        if (err != ESP_OK) break;
    }
    esp_tts_stream_reset(s_tts);
    esp_err_t stop_err = julia_lipsync_end();
    if (err == ESP_OK) err = stop_err;
    /* esp-tts v1.7 的解析器在多句连续复用时存在内部状态残留。
     * voice data 保持常驻，仅重建较小的合成句柄，避免下一句访问旧 utterance。 */
    esp_tts_destroy(s_tts);
    s_tts = esp_tts_create(s_voice);
    if (!s_tts && err == ESP_OK) err = ESP_ERR_NO_MEM;
    if (err == ESP_OK) s_status.synthesis_ok++; else s_status.synthesis_failed++;
    xSemaphoreGive(s_mutex);
    return err;
}

esp_err_t julia_local_tts_install_stream(FILE *input, size_t size, uint32_t expected_crc)
{
    if (!input || size != JULIA_LOCAL_TTS_VOICE_BYTES ||
        expected_crc != JULIA_LOCAL_TTS_VOICE_CRC32) return ESP_ERR_INVALID_ARG;
    if (!julia_sd_is_mounted() || julia_local_tts_is_ready()) return ESP_ERR_INVALID_STATE;
    if (!julia_sd_lock(pdMS_TO_TICKS(5000))) return ESP_ERR_TIMEOUT;
    mkdir("/sdcard/julia", 0775);
    mkdir("/sdcard/julia/tts", 0775);
    FILE *output = fopen(VOICE_TEMP_PATH, "wb");
    if (!output) { julia_sd_unlock(); return ESP_FAIL; }
    uint32_t crc = 0;
    size_t total = 0;
    esp_err_t result = ESP_OK;
    while (total < size) {
        size_t want = size - total;
        if (want > IO_BLOCK_BYTES) want = IO_BLOCK_BYTES;
        size_t got = 0;
        int64_t deadline = esp_timer_get_time() + 15000000;
        while (got < want && esp_timer_get_time() < deadline) {
            int part = usb_serial_jtag_read_bytes(s_io_block + got, want - got,
                                                  pdMS_TO_TICKS(100));
            if (part > 0) {
                got += (size_t)part;
                deadline = esp_timer_get_time() + 15000000;
            }
        }
        if (got != want || fwrite(s_io_block, 1, got, output) != got) {
            result = ESP_FAIL; break;
        }
        crc = esp_crc32_le(crc, s_io_block, got);
        total += got;
    }
    fflush(output);
    fclose(output);
    if (result == ESP_OK && crc != expected_crc) result = ESP_ERR_INVALID_CRC;
    if (result == ESP_OK) {
        remove(JULIA_LOCAL_TTS_VOICE_PATH);
        if (rename(VOICE_TEMP_PATH, JULIA_LOCAL_TTS_VOICE_PATH) != 0) result = ESP_FAIL;
    } else remove(VOICE_TEMP_PATH);
    julia_sd_unlock();
    ESP_LOGI(TAG, "install bytes=%u CRC=%08lx result=%s", (unsigned)total,
             (unsigned long)crc, esp_err_to_name(result));
    return result;
}

esp_err_t julia_local_tts_set_resource_enabled(bool enabled)
{
    if (!julia_sd_is_mounted() || julia_local_tts_is_ready()) return ESP_ERR_INVALID_STATE;
    if (!julia_sd_lock(pdMS_TO_TICKS(3000))) return ESP_ERR_TIMEOUT;
    int rc;
    if (enabled) rc = rename(VOICE_BACKUP_PATH, JULIA_LOCAL_TTS_VOICE_PATH);
    else {
        remove(VOICE_BACKUP_PATH);
        rc = rename(JULIA_LOCAL_TTS_VOICE_PATH, VOICE_BACKUP_PATH);
    }
    julia_sd_unlock();
    return rc == 0 ? ESP_OK : ESP_FAIL;
}
