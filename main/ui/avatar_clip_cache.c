#include "avatar_clip_cache.h"

#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define CACHE_ENTRY_COUNT 24
#define CLIP_NAME_CAPACITY 32

typedef struct {
    bool used;
    avatar_clip_source_t source;
    char name[CLIP_NAME_CAPACITY];
    const uint8_t *data;
    size_t size;
    uint32_t last_used;
    uint16_t references;
    bool current;
    bool next;
} clip_entry_t;

static const char *TAG = "AVATAR_CACHE";
static clip_entry_t s_entries[CACHE_ENTRY_COUNT];
static SemaphoreHandle_t s_mutex;
static avatar_clip_cache_metrics_t s_metrics;
static uint32_t s_lru_clock;
static char s_current_name[CLIP_NAME_CAPACITY];
static char s_next_name[CLIP_NAME_CAPACITY];

static void sample_psram_locked(void)
{
    size_t free_bytes = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    if (!s_metrics.psram_min_free || free_bytes < s_metrics.psram_min_free) {
        s_metrics.psram_min_free = free_bytes;
    }
}

static clip_entry_t *find_locked(const char *name)
{
    for (size_t i = 0; i < CACHE_ENTRY_COUNT; ++i) {
        if (s_entries[i].used && strcmp(s_entries[i].name, name) == 0) return &s_entries[i];
    }
    return NULL;
}

static clip_entry_t *empty_locked(void)
{
    for (size_t i = 0; i < CACHE_ENTRY_COUNT; ++i) {
        if (!s_entries[i].used) return &s_entries[i];
    }
    return NULL;
}

static bool evict_one_locked(void)
{
    clip_entry_t *victim = NULL;
    for (size_t i = 0; i < CACHE_ENTRY_COUNT; ++i) {
        clip_entry_t *entry = &s_entries[i];
        if (!entry->used || entry->source == AVATAR_CLIP_SOURCE_FLASH || entry->current ||
            entry->next || entry->references) continue;
        if (!victim || entry->last_used < victim->last_used) victim = entry;
    }
    if (!victim) return false;

    heap_caps_free((void *)victim->data);
    s_metrics.cached_bytes -= victim->size;
    s_metrics.evictions++;
    ESP_LOGI(TAG, "evicted clip=%s bytes=%u", victim->name, (unsigned)victim->size);
    memset(victim, 0, sizeof(*victim));
    return true;
}

static bool make_room_locked(size_t incoming)
{
    size_t free_bytes = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    while ((s_metrics.cached_bytes + incoming > s_metrics.soft_limit_bytes) ||
           (free_bytes < AVATAR_PSRAM_SAFE_WATERMARK + incoming)) {
        if (!evict_one_locked()) {
            s_metrics.preload_paused = true;
            sample_psram_locked();
            return false;
        }
        free_bytes = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    }
    s_metrics.preload_paused = false;
    return true;
}

esp_err_t avatar_clip_cache_init(void)
{
    if (s_mutex) return ESP_OK;
    memset(s_entries, 0, sizeof(s_entries));
    memset(&s_metrics, 0, sizeof(s_metrics));
    s_metrics.soft_limit_bytes = AVATAR_CLIP_CACHE_SOFT_LIMIT;
    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) return ESP_ERR_NO_MEM;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    sample_psram_locked();
    xSemaphoreGive(s_mutex);
    ESP_LOGI(TAG, "clip LRU ready: entries=%u soft_limit=%u watermark=%u",
             CACHE_ENTRY_COUNT, AVATAR_CLIP_CACHE_SOFT_LIMIT, AVATAR_PSRAM_SAFE_WATERMARK);
    return ESP_OK;
}

esp_err_t avatar_clip_cache_register_flash(const char *name, const uint8_t *data, size_t size)
{
    if (!s_mutex || !name || !data || !size || strlen(name) >= CLIP_NAME_CAPACITY)
        return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (find_locked(name)) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    clip_entry_t *entry = empty_locked();
    if (!entry) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_NO_MEM;
    }
    entry->used = true;
    entry->source = AVATAR_CLIP_SOURCE_FLASH;
    strcpy(entry->name, name);
    entry->data = data;
    entry->size = size;
    entry->last_used = ++s_lru_clock;
    entry->current = strcmp(entry->name, s_current_name) == 0;
    entry->next = strcmp(entry->name, s_next_name) == 0;
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

esp_err_t avatar_clip_cache_put(const char *name, const uint8_t *data, size_t size)
{
    if (!s_mutex || !name || !data || !size || size > AVATAR_CLIP_CACHE_SOFT_LIMIT ||
        strlen(name) >= CLIP_NAME_CAPACITY) return ESP_ERR_INVALID_ARG;
    uint8_t *copy = NULL;
    esp_err_t err = avatar_clip_cache_alloc_staging(size, &copy);
    if (err != ESP_OK) return err;
    memcpy(copy, data, size);
    err = avatar_clip_cache_publish(name, copy, size);
    if (err != ESP_OK) heap_caps_free(copy);
    return err;
}

esp_err_t avatar_clip_cache_alloc_staging(size_t size, uint8_t **buffer)
{
    if (!s_mutex || !buffer || !size || size > AVATAR_CLIP_CACHE_SOFT_LIMIT)
        return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (!make_room_locked(size)) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_NO_MEM;
    }
    uint8_t *allocated = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!allocated) {
        s_metrics.preload_paused = true;
        sample_psram_locked();
        xSemaphoreGive(s_mutex);
        return ESP_ERR_NO_MEM;
    }
    sample_psram_locked();
    xSemaphoreGive(s_mutex);
    *buffer = allocated;
    return ESP_OK;
}

esp_err_t avatar_clip_cache_publish(const char *name, uint8_t *buffer, size_t size)
{
    if (!s_mutex || !name || !buffer || !size || size > AVATAR_CLIP_CACHE_SOFT_LIMIT ||
        strlen(name) >= CLIP_NAME_CAPACITY) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    clip_entry_t *existing = find_locked(name);
    if (existing) {
        existing->last_used = ++s_lru_clock;
        xSemaphoreGive(s_mutex);
        heap_caps_free(buffer);
        return ESP_OK;
    }
    while (s_metrics.cached_bytes + size > s_metrics.soft_limit_bytes) {
        if (!evict_one_locked()) {
            s_metrics.preload_paused = true;
            xSemaphoreGive(s_mutex);
            return ESP_ERR_NO_MEM;
        }
    }
    clip_entry_t *entry = empty_locked();
    while (!entry && evict_one_locked()) entry = empty_locked();
    if (!entry) {
        s_metrics.preload_paused = true;
        xSemaphoreGive(s_mutex);
        return ESP_ERR_NO_MEM;
    }
    entry->used = true;
    entry->source = AVATAR_CLIP_SOURCE_PSRAM;
    strcpy(entry->name, name);
    entry->data = buffer;
    entry->size = size;
    entry->last_used = ++s_lru_clock;
    entry->current = strcmp(entry->name, s_current_name) == 0;
    entry->next = strcmp(entry->name, s_next_name) == 0;
    s_metrics.cached_bytes += size;
    sample_psram_locked();
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

esp_err_t avatar_clip_cache_acquire(const char *name, const uint8_t **data, size_t *size,
                                    avatar_clip_source_t *source)
{
    if (!s_mutex || !name || !data || !size) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    clip_entry_t *entry = find_locked(name);
    if (!entry) {
        s_metrics.misses++;
        xSemaphoreGive(s_mutex);
        return ESP_ERR_NOT_FOUND;
    }
    entry->references++;
    entry->last_used = ++s_lru_clock;
    s_metrics.hits++;
    *data = entry->data;
    *size = entry->size;
    if (source) *source = entry->source;
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

void avatar_clip_cache_release(const char *name)
{
    if (!s_mutex || !name) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    clip_entry_t *entry = find_locked(name);
    if (entry && entry->references) entry->references--;
    xSemaphoreGive(s_mutex);
}

bool avatar_clip_cache_contains(const char *name)
{
    const uint8_t *data = NULL;
    size_t size = 0;
    if (avatar_clip_cache_acquire(name, &data, &size, NULL) != ESP_OK) return false;
    avatar_clip_cache_release(name);
    return true;
}

static void set_protection(const char *name, bool current)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    char *protected_name = current ? s_current_name : s_next_name;
    if (name) {
        strncpy(protected_name, name, CLIP_NAME_CAPACITY - 1);
        protected_name[CLIP_NAME_CAPACITY - 1] = 0;
    } else {
        protected_name[0] = 0;
    }
    for (size_t i = 0; i < CACHE_ENTRY_COUNT; ++i) {
        if (current) s_entries[i].current = false;
        else s_entries[i].next = false;
    }
    clip_entry_t *entry = name ? find_locked(name) : NULL;
    if (entry) {
        if (current) entry->current = true;
        else entry->next = true;
        entry->last_used = ++s_lru_clock;
    }
    xSemaphoreGive(s_mutex);
}

void avatar_clip_cache_set_current(const char *name)
{
    if (s_mutex) set_protection(name, true);
}

void avatar_clip_cache_set_next(const char *name)
{
    if (s_mutex) set_protection(name, false);
}

void avatar_clip_cache_maintain_watermark(void)
{
    if (!s_mutex) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    size_t free_bytes = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    while (free_bytes < AVATAR_PSRAM_SAFE_WATERMARK && evict_one_locked()) {
        free_bytes = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    }
    s_metrics.preload_paused = free_bytes < AVATAR_PSRAM_SAFE_WATERMARK;
    sample_psram_locked();
    xSemaphoreGive(s_mutex);
}

bool avatar_clip_cache_preload_allowed(void)
{
    if (!s_mutex) return false;
    avatar_clip_cache_maintain_watermark();
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool allowed = !s_metrics.preload_paused;
    xSemaphoreGive(s_mutex);
    return allowed;
}

void avatar_clip_cache_get_metrics(avatar_clip_cache_metrics_t *metrics)
{
    if (!s_mutex || !metrics) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    sample_psram_locked();
    *metrics = s_metrics;
    xSemaphoreGive(s_mutex);
}
