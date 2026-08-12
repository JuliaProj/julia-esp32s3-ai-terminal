#include "transition_cache.h"

#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "julia_sd.h"

#define ENTRY_COUNT 8
#define PATH_CAPACITY 160

typedef struct {
    bool used;
    char path[PATH_CAPACITY];
    uint8_t *data;
    size_t bytes;
    uint32_t used_at;
    unsigned references;
    esp_partition_mmap_handle_t mmap_handle;
} cache_entry_t;

static cache_entry_t *s_entries;
static SemaphoreHandle_t s_mutex;
static uint32_t s_clock;
static size_t s_bytes;
static const char *TAG = "TRANSITION_CACHE";

static esp_err_t flash_source_info(const char *path, const esp_partition_t **partition,
                                   size_t *length)
{
    if (strcmp(path, "flash:storage") != 0) return ESP_ERR_NOT_FOUND;
    const esp_partition_t *found = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "storage");
    if (!found) return ESP_ERR_NOT_FOUND;
    uint8_t header[32];
    ESP_RETURN_ON_ERROR(esp_partition_read(found, 0, header, sizeof(header)), TAG,
                        "read flash transition header");
    uint32_t total = 0;
    memcpy(&total, header + 17, sizeof(total));
    if (memcmp(header, "JTRN", 4) || total < sizeof(header) || total > found->size)
        return ESP_ERR_INVALID_RESPONSE;
    *partition = found;
    *length = total;
    return ESP_OK;
}

static cache_entry_t *find_locked(const char *path)
{
    for (size_t i = 0; i < ENTRY_COUNT; ++i)
        if (s_entries[i].used && !strcmp(s_entries[i].path, path)) return &s_entries[i];
    return NULL;
}

static cache_entry_t *empty_locked(void)
{
    for (size_t i = 0; i < ENTRY_COUNT; ++i) if (!s_entries[i].used) return &s_entries[i];
    return NULL;
}

static bool evict_locked(void)
{
    cache_entry_t *victim = NULL;
    for (size_t i = 0; i < ENTRY_COUNT; ++i) {
        cache_entry_t *entry = &s_entries[i];
        if (!entry->used || entry->references || !strncmp(entry->path, "flash:", 6)) continue;
        if (!victim || entry->used_at < victim->used_at) victim = entry;
    }
    if (!victim) return false;
    ESP_LOGI(TAG, "evict path=%s bytes=%u", victim->path, (unsigned)victim->bytes);
    s_bytes -= victim->bytes;
    heap_caps_free(victim->data);
    memset(victim, 0, sizeof(*victim));
    return true;
}

esp_err_t transition_cache_init(void)
{
    if (s_mutex) return ESP_OK;
    s_entries = heap_caps_calloc(ENTRY_COUNT, sizeof(*s_entries),
                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_entries) return ESP_ERR_NO_MEM;
    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) { heap_caps_free(s_entries); s_entries = NULL; return ESP_ERR_NO_MEM; }
    return ESP_OK;
}

esp_err_t transition_cache_load(const char *path)
{
    if (!s_mutex || !path || strlen(path) >= PATH_CAPACITY)
        return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    cache_entry_t *existing = find_locked(path);
    if (existing) {
        existing->used_at = ++s_clock;
        xSemaphoreGive(s_mutex);
        return ESP_OK;
    }
    xSemaphoreGive(s_mutex);

    const esp_partition_t *flash_partition = NULL;
    size_t flash_length = 0;
    bool from_flash = !strncmp(path, "flash:", 6);
    FILE *file = NULL;
    long length = 0;
    if (from_flash) {
        esp_err_t source_err = flash_source_info(path, &flash_partition, &flash_length);
        if (source_err != ESP_OK) return source_err;
        length = (long)flash_length;
    } else {
        if (!julia_sd_is_mounted() || !julia_sd_lock(pdMS_TO_TICKS(3000)))
            return ESP_ERR_TIMEOUT;
        file = fopen(path, "rb");
        if (!file) { julia_sd_unlock(); return ESP_ERR_NOT_FOUND; }
        fseek(file, 0, SEEK_END);
        length = ftell(file);
        rewind(file);
    }
    if (length <= 0 || (!from_flash && (size_t)length > TRANSITION_CACHE_LIMIT_BYTES)) {
        if (file) fclose(file);
        if (!from_flash) julia_sd_unlock();
        return ESP_ERR_INVALID_SIZE;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    while (!from_flash && (s_bytes + (size_t)length > TRANSITION_CACHE_LIMIT_BYTES ||
            heap_caps_get_free_size(MALLOC_CAP_SPIRAM) <
                (size_t)length + TRANSITION_CACHE_WATERMARK_BYTES) && evict_locked()) {}
    bool room = from_flash || (s_bytes + (size_t)length <= TRANSITION_CACHE_LIMIT_BYTES &&
                heap_caps_get_free_size(MALLOC_CAP_SPIRAM) >=
                    (size_t)length + TRANSITION_CACHE_WATERMARK_BYTES);
    xSemaphoreGive(s_mutex);
    if (!room) {
        if (file) fclose(file);
        if (!from_flash) julia_sd_unlock();
        return ESP_ERR_NO_MEM;
    }

    uint8_t *data = NULL;
    esp_partition_mmap_handle_t mmap_handle = 0;
    if (from_flash) {
        const void *mapped = NULL;
        esp_err_t map_err = esp_partition_mmap(flash_partition, 0, (size_t)length,
                                               ESP_PARTITION_MMAP_DATA, &mapped,
                                               &mmap_handle);
        if (map_err != ESP_OK) return map_err;
        data = (uint8_t *)mapped;
    } else {
        data = heap_caps_malloc((size_t)length, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!data) { fclose(file); julia_sd_unlock(); return ESP_ERR_NO_MEM; }
    }
    size_t read_bytes = 0;
    if (from_flash) {
        read_bytes = (size_t)length;
    } else {
        read_bytes = fread(data, 1, (size_t)length, file);
        fclose(file);
        julia_sd_unlock();
    }
    if (read_bytes != (size_t)length) {
        if (from_flash) esp_partition_munmap(mmap_handle); else heap_caps_free(data);
        return ESP_FAIL;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    cache_entry_t *duplicate = find_locked(path);
    if (duplicate) {
        duplicate->used_at = ++s_clock;
        xSemaphoreGive(s_mutex);
        if (from_flash) esp_partition_munmap(mmap_handle); else heap_caps_free(data);
        return ESP_OK;
    }
    cache_entry_t *entry = empty_locked();
    while (!entry && evict_locked()) entry = empty_locked();
    if (!entry) {
        xSemaphoreGive(s_mutex);
        if (from_flash) esp_partition_munmap(mmap_handle); else heap_caps_free(data);
        return ESP_ERR_NO_MEM;
    }
    entry->used = true;
    strcpy(entry->path, path);
    entry->data = data;
    entry->bytes = (size_t)length;
    entry->used_at = ++s_clock;
    entry->mmap_handle = mmap_handle;
    if (!from_flash) s_bytes += entry->bytes;
    ESP_LOGI(TAG, "loaded path=%s bytes=%u storage=%s psram_total=%u", path,
             (unsigned)entry->bytes, from_flash ? "mmap" : "psram", (unsigned)s_bytes);
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

bool transition_cache_contains(const char *path)
{
    if (!s_mutex || !path) return false;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool found = find_locked(path) != NULL;
    xSemaphoreGive(s_mutex);
    return found;
}

esp_err_t transition_cache_acquire(const char *path, const uint8_t **data, size_t *bytes)
{
    if (!s_mutex || !path || !data || !bytes) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    cache_entry_t *entry = find_locked(path);
    if (!entry) { xSemaphoreGive(s_mutex); return ESP_ERR_NOT_FOUND; }
    entry->references++;
    entry->used_at = ++s_clock;
    *data = entry->data;
    *bytes = entry->bytes;
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

void transition_cache_release(const char *path)
{
    if (!s_mutex || !path) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    cache_entry_t *entry = find_locked(path);
    if (entry && entry->references) entry->references--;
    xSemaphoreGive(s_mutex);
}

size_t transition_cache_clear_unreferenced(void)
{
    if (!s_mutex) return 0;
    size_t freed = 0;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (size_t i = 0; i < ENTRY_COUNT; ++i) {
        cache_entry_t *entry = &s_entries[i];
        if (!entry->used || entry->references || !strncmp(entry->path, "flash:", 6)) continue;
        freed += entry->bytes;
        s_bytes -= entry->bytes;
        heap_caps_free(entry->data);
        memset(entry, 0, sizeof(*entry));
    }
    xSemaphoreGive(s_mutex);
    return freed;
}

void transition_cache_print(transition_cache_print_cb_t callback, void *context)
{
    if (!s_mutex || !callback) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (size_t i = 0; i < ENTRY_COUNT; ++i)
        if (s_entries[i].used)
            callback(s_entries[i].path, s_entries[i].bytes, s_entries[i].references, context);
    xSemaphoreGive(s_mutex);
}

size_t transition_cache_bytes(void) { return s_bytes; }
