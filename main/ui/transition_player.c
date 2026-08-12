#include "transition_player.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "avatar_face.h"
#include "avatar_eyes.h"
#include "avatar_mouth.h"
#include "avatar_rle.h"
#include "esp_heap_caps.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "julia_sd.h"
#include "julia_ui.h"
#include "transition_cache.h"

#define FILE_COUNT 48
#define PATH_CAPACITY 160
#define STARTUP_PRELOAD_LIMIT 1
#define FRAME_BYTES (360U * 360U * 2U)

typedef struct __attribute__((packed)) {
    char magic[4];
    uint8_t version;
    uint16_t width;
    uint16_t height;
    uint16_t frame_count;
    uint8_t fps;
    uint8_t format;
    uint32_t bytes_per_frame;
    uint32_t total_size;
    uint8_t reserved[11];
} trn_header_t;

typedef struct {
    julia_main_state_t from;
    julia_main_state_t to;
    char path[PATH_CAPACITY];
} trn_file_t;

typedef struct {
    julia_main_state_t from;
    julia_main_state_t to;
    char path[PATH_CAPACITY];
    transition_player_done_cb_t callback;
    void *context;
    uint32_t generation;
    uint32_t loop_ms;
} play_request_t;

typedef struct {
    FILE *file;
    uint32_t *offsets;
    uint8_t *packed;
    size_t data_start;
    size_t frame_data_bytes;
    size_t packed_capacity;
    bool sd_locked;
} trn_stream_t;

static trn_file_t *s_files;
static size_t s_file_count;
static SemaphoreHandle_t s_mutex;
static TaskHandle_t s_play_task;
static play_request_t s_request;
static volatile uint32_t s_generation;
static volatile bool s_playing;
static volatile bool s_queued;
static volatile int64_t s_started_us;
static transition_player_observer_cb_t s_observer;
static volatile bool s_fallback_enabled = true;
static const char *TAG = "TRN_PLAYER";

void julia_state_machine_on_transition_first_frame(julia_main_state_t from,
                                                    julia_main_state_t to)
    __attribute__((weak));

static void register_flash_standby(void)
{
    if (s_file_count >= FILE_COUNT) return;
    const esp_partition_t *partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "storage");
    uint8_t magic[4] = {0};
    if (!partition || esp_partition_read(partition, 0, magic, sizeof(magic)) != ESP_OK ||
        memcmp(magic, "JTRN", sizeof(magic))) return;
    trn_file_t *file = &s_files[s_file_count++];
    file->from = JULIA_MAIN_STATE_S1_STANDBY;
    file->to = JULIA_MAIN_STATE_S1_STANDBY;
    strcpy(file->path, "flash:storage");
    ESP_LOGI(TAG, "flash standby asset registered partition=storage");
}

static bool parse_route(const char *name, julia_main_state_t *from, julia_main_state_t *to)
{
    unsigned a, b;
    char tail;
    if (sscanf(name, "S%u_S%u%c", &a, &b, &tail) != 2 ||
        a >= JULIA_MAIN_STATE_COUNT || b >= JULIA_MAIN_STATE_COUNT) return false;
    *from = (julia_main_state_t)a;
    *to = (julia_main_state_t)b;
    return true;
}

static bool has_suffix(const char *name, const char *suffix)
{
    size_t n = strlen(name), s = strlen(suffix);
    return n >= s && !strcasecmp(name + n - s, suffix);
}

esp_err_t transition_player_rescan(void)
{
    if (!s_mutex) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_file_count = 0;
    register_flash_standby();
    if (!julia_sd_is_mounted()) {
        size_t count = s_file_count;
        xSemaphoreGive(s_mutex);
        return count ? ESP_OK : ESP_ERR_INVALID_STATE;
    }
    DIR *root = opendir(TRANSITION_ROOT);
    if (!root) {
        size_t count = s_file_count;
        xSemaphoreGive(s_mutex);
        return count ? ESP_OK : ESP_ERR_NOT_FOUND;
    }
    struct dirent *route_entry;
    while ((route_entry = readdir(root)) && s_file_count < FILE_COUNT) {
        if (route_entry->d_name[0] == '.') continue;
        julia_main_state_t from, to;
        if (!parse_route(route_entry->d_name, &from, &to)) continue;
        char directory[PATH_CAPACITY];
        int dn = snprintf(directory, sizeof(directory), "%s/%s", TRANSITION_ROOT,
                          route_entry->d_name);
        if (dn < 0 || dn >= (int)sizeof(directory)) continue;
        DIR *clips = opendir(directory);
        if (!clips) continue;
        struct dirent *clip;
        while ((clip = readdir(clips)) && s_file_count < FILE_COUNT) {
            if (clip->d_name[0] == '.' || !has_suffix(clip->d_name, ".trn")) continue;
            trn_file_t *file = &s_files[s_file_count];
            int pn = snprintf(file->path, sizeof(file->path), "%s/%s", directory, clip->d_name);
            if (pn < 0 || pn >= (int)sizeof(file->path)) continue;
            file->from = from;
            file->to = to;
            ++s_file_count;
        }
        closedir(clips);
    }
    closedir(root);
    size_t count = s_file_count;
    xSemaphoreGive(s_mutex);
    ESP_LOGI(TAG, "scan root=%s files=%u", TRANSITION_ROOT, (unsigned)count);
    return count ? ESP_OK : ESP_ERR_NOT_FOUND;
}

static bool validate(const uint8_t *data, size_t bytes, trn_header_t *header,
                     const uint32_t **offsets, const uint8_t **frames)
{
    if (!data || bytes < sizeof(*header)) return false;
    memcpy(header, data, sizeof(*header));
    if (memcmp(header->magic, "JTRN", 4) || header->version != 1 ||
        header->width != header->height ||
        (header->width != 180 && header->width != 360) || !header->frame_count ||
        header->frame_count > 120 || !header->fps || header->fps > 60 ||
        header->format > 1 ||
        header->bytes_per_frame != (uint32_t)header->width * header->height * 2U ||
        header->total_size != bytes) return false;
    size_t table_bytes = (size_t)header->frame_count * sizeof(uint32_t);
    size_t data_start = sizeof(*header) + table_bytes;
    if (data_start > bytes) return false;
    *offsets = (const uint32_t *)(data + sizeof(*header));
    *frames = data + data_start;
    size_t frame_data_bytes = bytes - data_start;
    uint32_t previous = 0;
    for (uint16_t i = 0; i < header->frame_count; ++i) {
        uint32_t offset, next;
        memcpy(&offset, data + sizeof(*header) + i * sizeof(offset), sizeof(offset));
        next = (uint32_t)frame_data_bytes;
        if (i + 1U < header->frame_count)
            memcpy(&next, data + sizeof(*header) + (i + 1U) * sizeof(next), sizeof(next));
        if ((i && offset < previous) || offset >= next || next > frame_data_bytes ||
            (!header->format && (size_t)offset + header->bytes_per_frame > frame_data_bytes))
            return false;
        previous = offset;
    }
    return true;
}

static bool validate_header(const trn_header_t *header, size_t bytes)
{
    return header && !memcmp(header->magic, "JTRN", 4) && header->version == 1 &&
        (header->width == 180 || header->width == 360) &&
        header->height == header->width &&
        header->frame_count && header->frame_count <= 120 && header->fps && header->fps <= 60 &&
        header->format <= 1 &&
        header->bytes_per_frame == (uint32_t)header->width * header->height * 2U &&
        header->total_size == bytes;
}

static void stream_close(trn_stream_t *stream)
{
    if (!stream) return;
    if (stream->file) fclose(stream->file);
    heap_caps_free(stream->offsets);
    heap_caps_free(stream->packed);
    if (stream->sd_locked) julia_sd_unlock();
    memset(stream, 0, sizeof(*stream));
}

static esp_err_t stream_open(const char *path, trn_header_t *header, trn_stream_t *stream)
{
    if (!path || !header || !stream || strncmp(path, "/sdcard/", 8))
        return ESP_ERR_INVALID_ARG;
    memset(stream, 0, sizeof(*stream));
    ESP_LOGI(TAG, "SD stream memory: internal=%u internal_largest=%u dma=%u dma_largest=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
    if (!julia_sd_is_mounted() || !julia_sd_lock(pdMS_TO_TICKS(3000)))
        return ESP_ERR_TIMEOUT;
    stream->sd_locked = true;
    stream->file = fopen(path, "rb");
    if (!stream->file) { stream_close(stream); return ESP_ERR_NOT_FOUND; }
    if (fseek(stream->file, 0, SEEK_END) || ftell(stream->file) <= 0) {
        stream_close(stream); return ESP_FAIL;
    }
    long length = ftell(stream->file);
    rewind(stream->file);
    if (fread(header, 1, sizeof(*header), stream->file) != sizeof(*header) ||
        !validate_header(header, (size_t)length)) {
        stream_close(stream); return ESP_ERR_INVALID_RESPONSE;
    }
    size_t table_bytes = (size_t)header->frame_count * sizeof(uint32_t);
    stream->offsets = heap_caps_malloc(table_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!stream->offsets) stream->offsets = heap_caps_malloc(table_bytes,
                                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!stream->offsets || fread(stream->offsets, 1, table_bytes, stream->file) != table_bytes) {
        stream_close(stream); return ESP_ERR_NO_MEM;
    }
    stream->data_start = sizeof(*header) + table_bytes;
    stream->frame_data_bytes = (size_t)length - stream->data_start;
    uint32_t previous = 0;
    for (uint16_t i = 0; i < header->frame_count; ++i) {
        uint32_t offset = stream->offsets[i];
        uint32_t next = i + 1U < header->frame_count ? stream->offsets[i + 1U] :
                        (uint32_t)stream->frame_data_bytes;
        if ((i && offset < previous) || offset >= next || next > stream->frame_data_bytes ||
            (!header->format && (size_t)offset + header->bytes_per_frame >
             stream->frame_data_bytes)) {
            stream_close(stream); return ESP_ERR_INVALID_RESPONSE;
        }
        size_t packed_bytes = next - offset;
        if (packed_bytes > stream->packed_capacity) stream->packed_capacity = packed_bytes;
        previous = offset;
    }
    stream->packed = heap_caps_malloc(stream->packed_capacity,
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!stream->packed) { stream_close(stream); return ESP_ERR_NO_MEM; }
    ESP_LOGI(TAG, "stream open path=%s bytes=%ld packed_max=%u", path, length,
             (unsigned)stream->packed_capacity);
    return ESP_OK;
}

static esp_err_t stream_frame(trn_stream_t *stream, uint16_t index,
                              const trn_header_t *header, const uint8_t **source,
                              size_t *source_bytes)
{
    uint32_t offset = stream->offsets[index];
    uint32_t next = index + 1U < header->frame_count ? stream->offsets[index + 1U] :
                    (uint32_t)stream->frame_data_bytes;
    size_t bytes = next - offset;
    if (bytes > stream->packed_capacity ||
        fseek(stream->file, (long)(stream->data_start + offset), SEEK_SET) ||
        fread(stream->packed, 1, bytes, stream->file) != bytes) return ESP_FAIL;
    *source = stream->packed;
    *source_bytes = bytes;
    return ESP_OK;
}

static void upscale_2x_rgb565(const uint16_t *source, uint16_t *target)
{
    for (unsigned y = 0; y < 180; ++y) {
        const uint16_t *src = source + y * 180U;
        uint16_t *row0 = target + (y * 2U) * 360U;
        uint16_t *row1 = row0 + 360U;
        for (unsigned x = 0; x < 180; ++x) {
            uint16_t pixel = src[x];
            row0[x * 2U] = pixel;
            row0[x * 2U + 1U] = pixel;
            row1[x * 2U] = pixel;
            row1[x * 2U + 1U] = pixel;
        }
    }
}

static void finish_request(const play_request_t *request, esp_err_t result)
{
    if (request->generation != s_generation) return;
    s_playing = false; s_queued = false;
    if (request->callback) request->callback(request->from, request->to, result, request->context);
    if (s_observer) s_observer(request->from, request->to, result);
}

static void player_task(void *argument)
{
    (void)argument;
    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        play_request_t request;
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        request = s_request;
        xSemaphoreGive(s_mutex);
        if (request.generation != s_generation) continue;

        bool sd_source = !strncmp(request.path, "/sdcard/", 8);
        esp_err_t err = ESP_OK;
        const uint8_t *data = NULL;
        size_t bytes = 0;
        trn_header_t header;
        const uint32_t *offsets = NULL;
        const uint8_t *frames = NULL;
        trn_stream_t stream = {0};
        if (sd_source) {
            err = transition_cache_acquire(request.path, &data, &bytes);
            if (err == ESP_OK && !validate(data, bytes, &header, &offsets, &frames))
                err = ESP_ERR_INVALID_RESPONSE;
            if (err == ESP_ERR_NOT_FOUND) {
                err = stream_open(request.path, &header, &stream);
                if (err == ESP_OK) offsets = stream.offsets;
            }
        } else {
            err = transition_cache_load(request.path);
            if (err == ESP_OK) err = transition_cache_acquire(request.path, &data, &bytes);
            if (err == ESP_OK && !validate(data, bytes, &header, &offsets, &frames))
                err = ESP_ERR_INVALID_RESPONSE;
        }
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "TRANSITION: fallback to static, reason=load/format path=%s err=%s",
                     request.path, esp_err_to_name(err));
            if (data) transition_cache_release(request.path);
            finish_request(&request, err);
            continue;
        }

        for (unsigned attempt = 0; attempt < 10; ++attempt) {
            err = julia_ui_transition_direct_begin();
            if (err != ESP_ERR_INVALID_STATE || request.generation != s_generation) break;
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        if (err == ESP_OK) {
            uint16_t *decoded = NULL;
            uint16_t *upscaled = NULL;
            if (header.format)
                decoded = heap_caps_malloc(header.bytes_per_frame,
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (header.format && !decoded) err = ESP_ERR_NO_MEM;
            if (err == ESP_OK && header.width == 180)
                upscaled = heap_caps_malloc(FRAME_BYTES,
                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (header.width == 180 && !upscaled) err = ESP_ERR_NO_MEM;
            s_playing = true;
            bool first_frame_notified = false;
            uint32_t frame_us = 1000000U / header.fps;
            int64_t loop_started_us = esp_timer_get_time();
            do {
              int64_t started = esp_timer_get_time();
              uint16_t shown = 0;
              while (err == ESP_OK && shown < header.frame_count &&
                     request.generation == s_generation) {
                uint32_t due = (uint32_t)((esp_timer_get_time() - started) / frame_us);
                uint16_t index = due < header.frame_count ? (uint16_t)due : header.frame_count - 1;
                if (index < shown) { vTaskDelay(pdMS_TO_TICKS(1)); continue; }
                uint32_t offset = offsets[index];
                uint32_t next = stream.file ? (index + 1U < header.frame_count ?
                    offsets[index + 1U] : (uint32_t)stream.frame_data_bytes) :
                    (uint32_t)(bytes - (size_t)(frames - data));
                if (!stream.file && index + 1U < header.frame_count) next = offsets[index + 1U];
                const uint8_t *source = stream.file ? NULL : frames + offset;
                size_t source_bytes = next - offset;
                if (stream.file)
                    err = stream_frame(&stream, index, &header, &source, &source_bytes);
                const uint16_t *draw_pixels = (const uint16_t *)source;
                if (err == ESP_OK && header.format == 1) {
                    err = avatar_rle_decode_rgb565(source, source_bytes, decoded,
                                                   header.bytes_per_frame / 2U);
                    draw_pixels = decoded;
                }
                if (err == ESP_OK && header.width == 180) {
                    upscale_2x_rgb565(draw_pixels, upscaled);
                    draw_pixels = upscaled;
                }
                if (err == ESP_OK)
                    avatar_eyes_correct_pupils_rgb565((uint16_t *)draw_pixels, 360, 360);
                if (err == ESP_OK &&
                    avatar_eyes_has_green_blob_rgb565(draw_pixels, 360, 360)) {
                    ESP_LOGE(TAG, "green_blob path=%s frame=%u fallback=black",
                             request.path, index);
                    memset((void *)draw_pixels, 0, FRAME_BYTES);
                }
                int64_t draw_started = esp_timer_get_time();
                if (err == ESP_OK)
                    err = julia_ui_transition_direct_draw(draw_pixels, FRAME_BYTES,
                                                          request.path);
                if (err == ESP_OK && !first_frame_notified) {
                    first_frame_notified = true;
                    if (julia_state_machine_on_transition_first_frame)
                        julia_state_machine_on_transition_first_frame(request.from, request.to);
                }
                uint32_t draw_us = (uint32_t)(esp_timer_get_time() - draw_started);
                ESP_LOGI(TAG, "frame path=%s index=%u/%u draw_us=%lu", request.path,
                         index + 1U, header.frame_count, (unsigned long)draw_us);
                if (err != ESP_OK) break;
                shown = index + 1U;
                int64_t deadline = started + (int64_t)shown * frame_us;
                int64_t remaining = deadline - esp_timer_get_time();
                if (remaining > 1000) vTaskDelay(pdMS_TO_TICKS((uint32_t)(remaining / 1000)));
              }
            } while (err == ESP_OK && request.generation == s_generation && request.loop_ms &&
                     (request.loop_ms == UINT32_MAX ||
                      esp_timer_get_time() - loop_started_us < (int64_t)request.loop_ms * 1000LL));
            if (request.generation != s_generation) err = ESP_ERR_INVALID_STATE;
            esp_err_t end_err = julia_ui_transition_direct_end();
            if (err == ESP_OK) err = end_err;
            heap_caps_free(upscaled);
            heap_caps_free(decoded);
        }
        if (stream.file) stream_close(&stream);
        else if (data) transition_cache_release(request.path);
        if (err == ESP_ERR_INVALID_STATE && request.generation != s_generation)
            ESP_LOGI(TAG, "cancelled from=S%u to=S%u", request.from, request.to);
        else
            ESP_LOGI(TAG, "complete from=S%u to=S%u result=%s", request.from, request.to,
                     esp_err_to_name(err));
        finish_request(&request, err);
    }
}

static bool is_hot(const trn_file_t *file)
{
    return (file->from == JULIA_MAIN_STATE_S1_STANDBY &&
            file->to == JULIA_MAIN_STATE_S1_STANDBY) ||
           (file->from == JULIA_MAIN_STATE_S1_STANDBY &&
            (file->to == JULIA_MAIN_STATE_S2_COMPANION ||
             file->to == JULIA_MAIN_STATE_S3_INITIATIVE || file->to == JULIA_MAIN_STATE_S0_SLEEP)) ||
           (file->from == JULIA_MAIN_STATE_S0_SLEEP && file->to == JULIA_MAIN_STATE_S1_STANDBY) ||
           (file->from == JULIA_MAIN_STATE_S3_INITIATIVE && file->to == JULIA_MAIN_STATE_S4_DIALOG) ||
           (file->from == JULIA_MAIN_STATE_S4_DIALOG && file->to == JULIA_MAIN_STATE_S1_STANDBY);
}

static void preload_task(void *argument)
{
    (void)argument;
    vTaskDelay(pdMS_TO_TICKS(1500));

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    size_t count = s_file_count;
    trn_file_t *files = heap_caps_malloc(count * sizeof(*files),
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (files) memcpy(files, s_files, count * sizeof(*files));
    xSemaphoreGive(s_mutex);

    if (!files) {
        ESP_LOGW(TAG, "preload snapshot allocation failed count=%u", (unsigned)count);
        vTaskDelete(NULL);
        return;
    }

    unsigned loaded = 0;
    for (size_t i = 0; i < count; ++i) {
        if (!strncmp(files[i].path, "flash:", 6) || !is_hot(&files[i])) continue;
        esp_err_t err = transition_cache_load(files[i].path);
        ESP_LOGI(TAG, "preload path=%s result=%s", files[i].path, esp_err_to_name(err));
        if (err == ESP_ERR_NO_MEM) break;
        if (err == ESP_OK && ++loaded >= STARTUP_PRELOAD_LIMIT) break;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    heap_caps_free(files);
    vTaskDelete(NULL);
}

esp_err_t transition_player_init(void)
{
    if (s_mutex) return ESP_OK;
    ESP_RETURN_ON_ERROR(transition_cache_init(), TAG, "cache init");
    s_files = heap_caps_calloc(FILE_COUNT, sizeof(*s_files),
                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_files) return ESP_ERR_NO_MEM;
    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) { heap_caps_free(s_files); s_files = NULL; return ESP_ERR_NO_MEM; }
    /* Scan and pin the flash-backed standby clip while app_main is still on an
     * internal stack. Playback tasks can then remain in PSRAM. */
    transition_player_rescan();
    if (transition_player_has(JULIA_MAIN_STATE_S1_STANDBY, JULIA_MAIN_STATE_S1_STANDBY))
        ESP_RETURN_ON_ERROR(transition_cache_load("flash:storage"), TAG,
                            "preload flash standby");
    if (xTaskCreateWithCaps(player_task, "trn_player", 6144, NULL, 5, &s_play_task,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS)
        return ESP_ERR_NO_MEM;
    ESP_LOGI(TAG, "startup SD preloading disabled; transitions load on demand");
    return ESP_OK;
}

bool transition_player_has(julia_main_state_t from, julia_main_state_t to)
{
    if (!s_mutex) return false;
    bool found = false;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (size_t i = 0; i < s_file_count; ++i)
        if (s_files[i].from == from && s_files[i].to == to) { found = true; break; }
    xSemaphoreGive(s_mutex);
    return found;
}

esp_err_t transition_player_play(julia_main_state_t from, julia_main_state_t to,
                                 transition_player_done_cb_t callback, void *context)
{
    if (!s_mutex || !s_play_task) return ESP_ERR_INVALID_STATE;
    trn_file_t choices[5];
    size_t count = 0;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (size_t i = 0; i < s_file_count && count < 5; ++i)
        if (s_files[i].from == from && s_files[i].to == to) choices[count++] = s_files[i];
    if (!count) { xSemaphoreGive(s_mutex); return ESP_ERR_NOT_FOUND; }
    uint32_t generation = ++s_generation;
    const trn_file_t *selected = &choices[esp_random() % count];
    s_request = (play_request_t){.from=from, .to=to, .callback=callback,
                                 .context=context, .generation=generation};
    strcpy(s_request.path, selected->path);
    s_queued = true; s_started_us = esp_timer_get_time();
    xSemaphoreGive(s_mutex);
    xTaskNotifyGive(s_play_task);
    ESP_LOGI(TAG, "request from=S%u to=S%u choices=%u path=%s", from, to,
             (unsigned)count, selected->path);
    return ESP_OK;
}

esp_err_t transition_player_play_path(const char *path,
                                      transition_player_done_cb_t callback, void *context)
{
    return transition_player_play_path_for(path, 0, callback, context);
}

esp_err_t transition_player_play_path_for(const char *path, uint32_t loop_ms,
                                          transition_player_done_cb_t callback, void *context)
{
    if (!path || !s_mutex || !s_play_task) return ESP_ERR_INVALID_STATE;
    bool found = !strncmp(path, "/sdcard/", 8);
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (size_t i = 0; i < s_file_count; ++i) {
        if (!strcmp(s_files[i].path, path)) { found = true; break; }
    }
    if (!found) { xSemaphoreGive(s_mutex); return ESP_ERR_NOT_FOUND; }
    uint32_t generation = ++s_generation;
    s_request = (play_request_t){
        .from=JULIA_MAIN_STATE_S1_STANDBY, .to=JULIA_MAIN_STATE_S1_STANDBY,
        .callback=callback, .context=context, .generation=generation,
        .loop_ms=loop_ms,
    };
    strlcpy(s_request.path, path, sizeof(s_request.path));
    s_queued = true; s_started_us = esp_timer_get_time();
    xSemaphoreGive(s_mutex);
    xTaskNotifyGive(s_play_task);
    return ESP_OK;
}

void transition_player_stop(void)
{
    ++s_generation;
    s_playing = false; s_queued = false;
}

bool transition_player_is_playing(void) { return s_playing || s_queued; }
uint32_t transition_player_elapsed_ms(void)
{
    return transition_player_is_playing() && s_started_us ?
        (uint32_t)((esp_timer_get_time() - s_started_us) / 1000) : 0;
}
void transition_player_set_observer(transition_player_observer_cb_t callback)
{
    s_observer = callback;
}

void transition_player_list(transition_player_list_cb_t callback, void *context)
{
    if (!s_mutex || !callback) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (size_t i = 0; i < s_file_count; ++i)
        callback(s_files[i].from, s_files[i].to, s_files[i].path, context);
    xSemaphoreGive(s_mutex);
}

static void print_cache_entry(const char *path, size_t bytes, unsigned references, void *context)
{
    (void)context;
    printf("TRANSITION_CACHE path=%s bytes=%u refs=%u\n", path, (unsigned)bytes, references);
}

void transition_player_cache_print(void)
{
    transition_cache_print(print_cache_entry, NULL);
    printf("TRANSITION_CACHE total=%u free_psram=%u\n", (unsigned)transition_cache_bytes(),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

void transition_player_set_fallback_enabled(bool enabled) { s_fallback_enabled = enabled; }
bool transition_player_fallback_enabled(void) { return s_fallback_enabled; }
