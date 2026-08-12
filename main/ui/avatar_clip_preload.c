#include "avatar_clip_preload.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>
#include "avatar_clip_cache.h"
#include "lvgl_port.h"
#include "esp_crc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "julia_sd.h"

#define PRELOAD_PENDING_COUNT 16
#define PRELOAD_HINT_COUNT 24
#define PRELOAD_NAME_SIZE 32
#define PRELOAD_PATH_SIZE 96
#define PRELOAD_CHUNK_SIZE 8192
#define PRELOAD_RESUME_HYSTERESIS (256U * 1024U)

typedef struct {
    bool used;
    char name[PRELOAD_NAME_SIZE];
    char path[PRELOAD_PATH_SIZE];
    uint32_t crc32;
    uint32_t epoch;
    uint32_t order;
    avatar_preload_priority_t priority;
} preload_request_t;

typedef struct {
    bool used;
    uint8_t state;
    uint8_t phase;
    char name[PRELOAD_NAME_SIZE];
    char path[PRELOAD_PATH_SIZE];
    uint32_t crc32;
} preload_hint_t;

static const char *TAG = "AVATAR_PRELOAD";
static SemaphoreHandle_t s_mutex;
static TaskHandle_t s_task;
static preload_request_t s_pending[PRELOAD_PENDING_COUNT];
static preload_hint_t s_hints[PRELOAD_HINT_COUNT];
static avatar_clip_preload_metrics_t s_metrics;
static volatile bool s_inject_read_failure;

void avatar_clip_preload_inject_read_failure(void) { s_inject_read_failure = true; }
static uint32_t s_epoch = 1;
static uint32_t s_order;
static uint8_t s_state;
static uint8_t s_phase;
static bool s_active;
static char s_active_name[PRELOAD_NAME_SIZE];
static uint32_t s_active_epoch;
static avatar_preload_priority_t s_active_priority;
static bool s_cancel_active;
static bool s_watermark_paused;
static int64_t s_pause_started_us;
static bool s_churn_test_running;

static bool request_cancelled(const preload_request_t *request)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool cancelled = s_cancel_active ||
        (request->priority != AVATAR_PRELOAD_EXPLICIT && request->epoch != s_epoch);
    xSemaphoreGive(s_mutex);
    return cancelled;
}

static bool cache_contains(const char *name)
{
    const uint8_t *data = NULL;
    size_t size = 0;
    if (avatar_clip_cache_acquire(name, &data, &size, NULL) != ESP_OK) return false;
    avatar_clip_cache_release(name);
    return true;
}

static bool take_next_request(preload_request_t *output)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    preload_request_t *best = NULL;
    for (size_t i = 0; i < PRELOAD_PENDING_COUNT; ++i) {
        preload_request_t *candidate = &s_pending[i];
        if (!candidate->used) continue;
        if (!best || candidate->priority > best->priority ||
            (candidate->priority == best->priority && candidate->order < best->order)) {
            best = candidate;
        }
    }
    if (best) {
        *output = *best;
        memset(best, 0, sizeof(*best));
        s_active = true;
        strcpy(s_active_name, output->name);
        s_active_epoch = output->epoch;
        s_active_priority = output->priority;
        s_cancel_active = false;
    }
    xSemaphoreGive(s_mutex);
    return best != NULL;
}

static void finish_active(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_active = false;
    s_cancel_active = false;
    s_active_name[0] = 0;
    xSemaphoreGive(s_mutex);
}

static bool wait_for_watermark(const preload_request_t *request)
{
    while (true) {
        if (lvgl_port_display_off()) {
            if (request_cancelled(request)) return false;
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        avatar_clip_cache_maintain_watermark();
        size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        bool enough = s_watermark_paused
                          ? free_psram >= AVATAR_PSRAM_SAFE_WATERMARK + PRELOAD_RESUME_HYSTERESIS
                          : free_psram >= AVATAR_PSRAM_SAFE_WATERMARK;
        if (enough && avatar_clip_cache_preload_allowed()) {
            if (s_watermark_paused) {
                s_watermark_paused = false;
                xSemaphoreTake(s_mutex, portMAX_DELAY);
                s_metrics.resume_events++;
                uint32_t paused_ms = (uint32_t)((esp_timer_get_time() - s_pause_started_us) / 1000);
                if (paused_ms > s_metrics.pause_max_ms) s_metrics.pause_max_ms = paused_ms;
                xSemaphoreGive(s_mutex);
                ESP_LOGI(TAG, "preload resumed: psram_free=%u paused_ms=%u",
                         (unsigned)free_psram, paused_ms);
            }
            return true;
        }
        if (!s_watermark_paused) {
            s_watermark_paused = true;
            s_pause_started_us = esp_timer_get_time();
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            s_metrics.pause_events++;
            xSemaphoreGive(s_mutex);
            ESP_LOGW(TAG, "preload paused: psram_free=%u", (unsigned)free_psram);
        }
        if (request_cancelled(request)) return false;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

static esp_err_t read_and_publish(const preload_request_t *request, bool *cancelled)
{
    *cancelled = false;
    if (!julia_sd_is_mounted()) return ESP_ERR_INVALID_STATE;
    if (!wait_for_watermark(request)) {
        *cancelled = true;
        return ESP_ERR_INVALID_STATE;
    }

    struct stat info;
    if (!julia_sd_lock(pdMS_TO_TICKS(500))) return ESP_ERR_TIMEOUT;
    int stat_result = stat(request->path, &info);
    julia_sd_unlock();
    if (stat_result != 0 || info.st_size <= 0 || (size_t)info.st_size > AVATAR_CLIP_CACHE_SOFT_LIMIT)
        return ESP_ERR_INVALID_SIZE;

    uint8_t *staging = NULL;
    esp_err_t err = avatar_clip_cache_alloc_staging((size_t)info.st_size, &staging);
    if (err != ESP_OK) return err;

    if (!julia_sd_lock(pdMS_TO_TICKS(500))) {
        heap_caps_free(staging);
        return ESP_ERR_TIMEOUT;
    }
    FILE *file = fopen(request->path, "rb");
    julia_sd_unlock();
    if (!file) {
        heap_caps_free(staging);
        return ESP_FAIL;
    }

    size_t offset = 0;
    uint32_t crc = 0;
    int64_t started = esp_timer_get_time();
    while (offset < (size_t)info.st_size) {
        while (lvgl_port_display_off()) {
            if (request_cancelled(request)) { *cancelled = true; break; }
            vTaskDelay(pdMS_TO_TICKS(200));
        }
        if (*cancelled) break;
        if (s_inject_read_failure) {
            s_inject_read_failure = false;
            err = ESP_FAIL;
            ESP_LOGW(TAG, "injected SD read failure clip=%s", request->name);
            break;
        }
        if (request_cancelled(request)) {
            *cancelled = true;
            break;
        }
        size_t wanted = (size_t)info.st_size - offset;
        if (wanted > PRELOAD_CHUNK_SIZE) wanted = PRELOAD_CHUNK_SIZE;
        if (!julia_sd_lock(pdMS_TO_TICKS(500))) {
            err = ESP_ERR_TIMEOUT;
            break;
        }
        size_t got = fread(staging + offset, 1, wanted, file);
        julia_sd_unlock();
        if (got != wanted) {
            err = ESP_ERR_INVALID_SIZE;
            break;
        }
        crc = esp_crc32_le(crc, staging + offset, got);
        offset += got;
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_metrics.sd_bytes += got;
        xSemaphoreGive(s_mutex);
        vTaskDelay(1);
    }
    if (julia_sd_lock(pdMS_TO_TICKS(500))) {
        fclose(file);
        julia_sd_unlock();
    } else {
        /* FAT 文件句柄仍需关闭；此任务是唯一持有者。 */
        fclose(file);
    }

    if (*cancelled || err != ESP_OK || offset != (size_t)info.st_size ||
        request->crc32 == 0 || crc != request->crc32) {
        if (!*cancelled && err == ESP_OK) err = ESP_ERR_INVALID_CRC;
        heap_caps_free(staging);
        if (!*cancelled && crc != request->crc32) {
            ESP_LOGE(TAG, "CRC failed clip=%s actual=%08lx expected=%08lx",
                     request->name, (unsigned long)crc, (unsigned long)request->crc32);
        }
        return err;
    }

    err = avatar_clip_cache_publish(request->name, staging, offset);
    if (err != ESP_OK) heap_caps_free(staging);
    if (err == ESP_OK) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_metrics.total_us += (uint64_t)(esp_timer_get_time() - started);
        xSemaphoreGive(s_mutex);
    }
    return err;
}

static void preload_task(void *argument)
{
    (void)argument;
    while (true) {
        preload_request_t request;
        if (!take_next_request(&request)) {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }
        bool cancelled = false;
        esp_err_t err;
        if (request_cancelled(&request)) {
            cancelled = true;
            err = ESP_ERR_INVALID_STATE;
        } else if (cache_contains(request.name)) {
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            s_metrics.cache_hits++;
            xSemaphoreGive(s_mutex);
            finish_active();
            continue;
        } else {
            err = read_and_publish(&request, &cancelled);
        }

        xSemaphoreTake(s_mutex, portMAX_DELAY);
        if (cancelled) s_metrics.cancelled++;
        else if (err == ESP_OK) s_metrics.completed++;
        else s_metrics.failed++;
        xSemaphoreGive(s_mutex);
        if (err == ESP_OK) ESP_LOGI(TAG, "loaded clip=%s", request.name);
        else if (!cancelled) ESP_LOGW(TAG, "load failed clip=%s: %s",
                                      request.name, esp_err_to_name(err));
        finish_active();
    }
}

esp_err_t avatar_clip_preload_init(void)
{
    if (s_task) return ESP_OK;
    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) return ESP_ERR_NO_MEM;
    if (xTaskCreatePinnedToCore(preload_task, "avatar_preload", 5120, NULL, 1, &s_task, 0) != pdPASS)
        return ESP_ERR_NO_MEM;
    ESP_LOGI(TAG, "async preload ready: core=0 priority=1 chunk=%u", PRELOAD_CHUNK_SIZE);
    return ESP_OK;
}

esp_err_t avatar_clip_preload_request(const char *name, const char *path,
                                      uint32_t expected_crc32,
                                      avatar_preload_priority_t priority)
{
    const size_t name_len = name ? strlen(name) : 0;
    const size_t path_len = path ? strlen(path) : 0;
    if (!s_mutex || !name || !path || !expected_crc32 || priority > AVATAR_PRELOAD_EXPLICIT ||
        name_len >= PRELOAD_NAME_SIZE || path_len >= PRELOAD_PATH_SIZE) {
        ESP_LOGE(TAG, "request rejected clip=%s len=%u priority=%u reason=invalid",
                 name ? name : "(null)", (unsigned)name_len, (unsigned)priority);
        return ESP_ERR_INVALID_ARG;
    }
    ESP_LOGI(TAG, "request source=%s clip=%s len=%u",
             priority == AVATAR_PRELOAD_EXPLICIT ? "explicit" :
             priority == AVATAR_PRELOAD_PREDICTED ? "predicted" : "background",
             name, (unsigned)name_len);
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_metrics.requests++;
    if (s_active && strcmp(s_active_name, name) == 0) {
        ESP_LOGI(TAG, "request clip=%s result=active-duplicate", name);
        xSemaphoreGive(s_mutex);
        return ESP_OK;
    }
    for (size_t i = 0; i < PRELOAD_PENDING_COUNT; ++i) {
        if (s_pending[i].used && strcmp(s_pending[i].name, name) == 0) {
            if (priority > s_pending[i].priority) s_pending[i].priority = priority;
            ESP_LOGI(TAG, "request clip=%s result=pending-duplicate", name);
            xSemaphoreGive(s_mutex);
            return ESP_OK;
        }
    }
    preload_request_t *slot = NULL;
    for (size_t i = 0; i < PRELOAD_PENDING_COUNT; ++i) {
        if (!s_pending[i].used) { slot = &s_pending[i]; break; }
    }
    if (!slot) {
        s_metrics.failed++;
        ESP_LOGW(TAG, "request clip=%s result=queue-full", name);
        xSemaphoreGive(s_mutex);
        return ESP_ERR_NO_MEM;
    }
    slot->used = true;
    strcpy(slot->name, name);
    strcpy(slot->path, path);
    slot->crc32 = expected_crc32;
    slot->priority = priority;
    slot->epoch = priority == AVATAR_PRELOAD_EXPLICIT ? UINT32_MAX : s_epoch;
    slot->order = ++s_order;
    xSemaphoreGive(s_mutex);
    ESP_LOGI(TAG, "request clip=%s result=queued", name);
    xTaskNotifyGive(s_task);
    return ESP_OK;
}

esp_err_t avatar_clip_preload_register_hint(uint8_t state, uint8_t phase,
                                            const char *name, const char *path,
                                            uint32_t expected_crc32)
{
    if (!s_mutex || !name || !path || !expected_crc32 || strlen(name) >= PRELOAD_NAME_SIZE ||
        strlen(path) >= PRELOAD_PATH_SIZE) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    preload_hint_t *slot = NULL;
    for (size_t i = 0; i < PRELOAD_HINT_COUNT; ++i) {
        if (s_hints[i].used && s_hints[i].state == state && s_hints[i].phase == phase) {
            slot = &s_hints[i]; break;
        }
        if (!slot && !s_hints[i].used) slot = &s_hints[i];
    }
    if (!slot) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_NO_MEM;
    }
    slot->used = true;
    slot->state = state;
    slot->phase = phase;
    strcpy(slot->name, name);
    strcpy(slot->path, path);
    slot->crc32 = expected_crc32;
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

static void update_context(uint8_t state, uint8_t phase)
{
    preload_hint_t hint = {0};
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_state = state;
    s_phase = phase;
    s_epoch++;
    for (size_t i = 0; i < PRELOAD_PENDING_COUNT; ++i) {
        if (s_pending[i].used && s_pending[i].priority != AVATAR_PRELOAD_EXPLICIT) {
            memset(&s_pending[i], 0, sizeof(s_pending[i]));
            s_metrics.cancelled++;
        }
    }
    for (size_t i = 0; i < PRELOAD_HINT_COUNT; ++i) {
        if (s_hints[i].used && s_hints[i].state == state && s_hints[i].phase == phase) {
            hint = s_hints[i];
            break;
        }
    }
    xSemaphoreGive(s_mutex);
    if (hint.used) {
        avatar_clip_cache_set_next(hint.name);
        avatar_clip_preload_request(hint.name, hint.path, hint.crc32, AVATAR_PRELOAD_PREDICTED);
    } else {
        avatar_clip_cache_set_next(NULL);
    }
}

void avatar_clip_preload_on_state(uint8_t state)
{
    if (s_mutex) update_context(state, s_phase);
}

void avatar_clip_preload_on_dialog_phase(uint8_t phase)
{
    if (s_mutex) update_context(s_state, phase);
}

void avatar_clip_preload_get_metrics(avatar_clip_preload_metrics_t *metrics)
{
    if (!s_mutex || !metrics) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    *metrics = s_metrics;
    xSemaphoreGive(s_mutex);
}

bool avatar_clip_preload_is_pending(const char *name)
{
    if (!s_mutex || !name) return false;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool pending = s_active && strcmp(s_active_name, name) == 0;
    for (size_t i = 0; !pending && i < PRELOAD_PENDING_COUNT; ++i)
        pending = s_pending[i].used && strcmp(s_pending[i].name, name) == 0;
    xSemaphoreGive(s_mutex);
    return pending;
}

void avatar_clip_preload_cancel_except(const char *keep_name)
{
    if (!s_mutex) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_active && (!keep_name || strcmp(s_active_name, keep_name) != 0))
        s_cancel_active = true;
    for (size_t i = 0; i < PRELOAD_PENDING_COUNT; ++i) {
        if (s_pending[i].used && (!keep_name || strcmp(s_pending[i].name, keep_name) != 0)) {
            memset(&s_pending[i], 0, sizeof(s_pending[i]));
            s_metrics.cancelled++;
        }
    }
    xSemaphoreGive(s_mutex);
}

static void churn_test_task(void *argument)
{
    (void)argument;
    enum { TEST_CLIPS = 8, TEST_BYTES = 1024 * 1024 };
    const char *directory = JULIA_SD_MOUNT_POINT "/julia/tclip";
    uint32_t crc[TEST_CLIPS] = {0};
    uint8_t *block = heap_caps_malloc(PRELOAD_CHUNK_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!block) goto done;

    if (julia_sd_lock(pdMS_TO_TICKS(1000))) {
        mkdir(JULIA_SD_MOUNT_POINT "/julia", 0775);
        int result = mkdir(directory, 0775);
        julia_sd_unlock();
        if (result != 0 && errno != EEXIST) goto done;
    } else {
        goto done;
    }

    ESP_LOGI(TAG, "churn test: creating %u clips, total=%u bytes", TEST_CLIPS,
             TEST_CLIPS * TEST_BYTES);
    for (int index = 0; index < TEST_CLIPS; ++index) {
        char path[PRELOAD_PATH_SIZE];
        snprintf(path, sizeof(path), "%s/T%d.BIN", directory, index);
        memset(block, 0x31 + index, PRELOAD_CHUNK_SIZE);
        struct stat existing;
        bool fixture_ready = false;
        if (julia_sd_lock(pdMS_TO_TICKS(1000))) {
            fixture_ready = stat(path, &existing) == 0 && existing.st_size == TEST_BYTES;
            julia_sd_unlock();
        }
        if (fixture_ready) {
            for (size_t offset = 0; offset < TEST_BYTES; offset += PRELOAD_CHUNK_SIZE)
                crc[index] = esp_crc32_le(crc[index], block, PRELOAD_CHUNK_SIZE);
            continue;
        }
        if (!julia_sd_lock(pdMS_TO_TICKS(1000))) goto done;
        FILE *file = fopen(path, "wb");
        julia_sd_unlock();
        if (!file) goto done;
        bool write_ok = true;
        for (size_t offset = 0; offset < TEST_BYTES; offset += PRELOAD_CHUNK_SIZE) {
            if (!julia_sd_lock(pdMS_TO_TICKS(1000))) { write_ok = false; break; }
            size_t written = fwrite(block, 1, PRELOAD_CHUNK_SIZE, file);
            julia_sd_unlock();
            if (written != PRELOAD_CHUNK_SIZE) { write_ok = false; break; }
            crc[index] = esp_crc32_le(crc[index], block, PRELOAD_CHUNK_SIZE);
            vTaskDelay(1);
        }
        if (julia_sd_lock(pdMS_TO_TICKS(1000))) {
            fflush(file);
            fclose(file);
            julia_sd_unlock();
        } else {
            fclose(file);
            write_ok = false;
        }
        if (!write_ok) goto done;
    }

    /* 先验证预测请求取消：上下文跳转后，未完整读取的数据不得发布。 */
    avatar_clip_preload_request("CANCEL", JULIA_SD_MOUNT_POINT "/julia/tclip/T0.BIN",
                                crc[0], AVATAR_PRELOAD_PREDICTED);
    avatar_clip_preload_on_state(0xfe);
    avatar_clip_cache_set_next("T1");

    avatar_clip_preload_metrics_t before = {0};
    avatar_clip_preload_get_metrics(&before);
    for (int index = 0; index < TEST_CLIPS; ++index) {
        char name[PRELOAD_NAME_SIZE];
        char path[PRELOAD_PATH_SIZE];
        snprintf(name, sizeof(name), "T%d", index);
        snprintf(path, sizeof(path), "%s/T%d.BIN", directory, index);
        avatar_clip_preload_request(name, path, crc[index], AVATAR_PRELOAD_EXPLICIT);
    }

    int64_t deadline = esp_timer_get_time() + 90000000LL;
    while (esp_timer_get_time() < deadline) {
        avatar_clip_preload_metrics_t now;
        avatar_clip_preload_get_metrics(&now);
        if (now.completed + now.failed >= before.completed + before.failed + TEST_CLIPS) break;
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    /* T0 应已被 LRU 淘汰并重新从 SD 加载；T7 应仍驻留并直接命中。 */
    avatar_clip_preload_request("T0", JULIA_SD_MOUNT_POINT "/julia/tclip/T0.BIN",
                                crc[0], AVATAR_PRELOAD_EXPLICIT);
    avatar_clip_preload_request("T7", JULIA_SD_MOUNT_POINT "/julia/tclip/T7.BIN",
                                crc[7], AVATAR_PRELOAD_EXPLICIT);
    avatar_clip_preload_request("BAD", JULIA_SD_MOUNT_POINT "/julia/tclip/T7.BIN",
                                crc[7] ^ 1U, AVATAR_PRELOAD_EXPLICIT);
    ESP_LOGI(TAG, "churn requests queued: reload=T0 cache-hit=T7 bad-crc=BAD");

    avatar_clip_preload_metrics_t pressure_before;
    avatar_clip_preload_get_metrics(&pressure_before);
    deadline = esp_timer_get_time() + 30000000LL;
    while (esp_timer_get_time() < deadline) {
        avatar_clip_preload_metrics_t now;
        avatar_clip_preload_get_metrics(&now);
        if (now.completed >= pressure_before.completed + 1 &&
            now.cache_hits >= pressure_before.cache_hits + 1 &&
            now.failed >= pressure_before.failed + 1) break;
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    /* 短时持有所有驻留测试片段引用，使水位维护无法通过 LRU 立即回收，
     * 验证暂停/迟滞恢复。压力解除后马上释放，低水位远小于 2 秒。 */
    const char *protected_names[] = {"T1", "T6", "T7", "T0"};
    const uint8_t *protected_data[4] = {0};
    size_t protected_size[4] = {0};
    for (size_t i = 0; i < 4; ++i)
        avatar_clip_cache_acquire(protected_names[i], &protected_data[i], &protected_size[i], NULL);
    size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t pressure_size = free_psram > AVATAR_PSRAM_SAFE_WATERMARK
                               ? free_psram - (AVATAR_PSRAM_SAFE_WATERMARK - 128U * 1024U)
                               : 0;
    uint8_t *pressure = pressure_size ? heap_caps_malloc(pressure_size,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : NULL;
    avatar_clip_preload_request("PRESS", JULIA_SD_MOUNT_POINT "/julia/tclip/T2.BIN",
                                crc[2], AVATAR_PRELOAD_EXPLICIT);
    avatar_clip_preload_metrics_t pause_before;
    avatar_clip_preload_get_metrics(&pause_before);
    deadline = esp_timer_get_time() + 1500000LL;
    while (esp_timer_get_time() < deadline) {
        avatar_clip_preload_metrics_t now;
        avatar_clip_preload_get_metrics(&now);
        if (now.pause_events > pause_before.pause_events) break;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (pressure) heap_caps_free(pressure);
    for (size_t i = 0; i < 4; ++i) {
        if (protected_data[i]) avatar_clip_cache_release(protected_names[i]);
    }
    ESP_LOGI(TAG, "watermark pressure released bytes=%u", (unsigned)pressure_size);

done:
    if (block) heap_caps_free(block);
    s_churn_test_running = false;
    vTaskDelete(NULL);
}

esp_err_t avatar_clip_preload_run_churn_test(void)
{
    if (!s_task || !julia_sd_is_mounted()) return ESP_ERR_INVALID_STATE;
    if (s_churn_test_running) return ESP_ERR_INVALID_STATE;
    s_churn_test_running = true;
    if (xTaskCreatePinnedToCore(churn_test_task, "preload_churn", 4096, NULL, 1, NULL, 0) != pdPASS) {
        s_churn_test_running = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
