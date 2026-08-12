#include "avatar_anim_engine.h"

#include <string.h>
#include "avatar_clip_cache.h"
#include "avatar_clip_map.h"
#include "avatar_clip_preload.h"
#include "avatar_rle.h"
#include "avatar_loop_player.h"
#include "esp_crc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "julia_ui.h"
#include "lvgl.h"
#include "lvgl_port.h"
#include "transition_director.h"

#define FRAME_PIXELS (360U * 360U)
#define FRAME_BYTES (FRAME_PIXELS * sizeof(uint16_t))
#define TIMER_PERIOD_MS 10
#define SWITCH_TIMEOUT_US 8000000LL
#define BENCH_CLIP_NAME "BENCH"
static const char *TAG = "AVATAR_ANIM";

#ifndef JULIA_ANIM_LOG
#define JULIA_ANIM_LOG 1
#endif

extern const uint8_t transition_s0_s1_start[] asm("_binary_TR_S0_S1_clip_start");
extern const uint8_t transition_s0_s1_end[] asm("_binary_TR_S0_S1_clip_end");
extern const uint8_t transition_s1_s0_start[] asm("_binary_TR_S1_S0_clip_start");
extern const uint8_t transition_s1_s0_end[] asm("_binary_TR_S1_S0_clip_end");
extern const uint8_t transition_s1_s3_start[] asm("_binary_TR_S1_S3_clip_start");
extern const uint8_t transition_s1_s3_end[] asm("_binary_TR_S1_S3_clip_end");

static esp_err_t register_transition_fallback(const char *name, const uint8_t *start,
                                              const uint8_t *end)
{
    esp_err_t err = avatar_clip_cache_register_flash(name, start, (size_t)(end - start));
    if (err == ESP_OK || err == ESP_ERR_INVALID_STATE) {
        ESP_LOGI(TAG, "flash fallback ready clip=%s bytes=%u", name, (unsigned)(end - start));
        return ESP_OK;
    }
    return err;
}

typedef enum {
    SLOT_EMPTY = 0,
    SLOT_DECODING,
    SLOT_READY,
    SLOT_DISPLAYING,
    SLOT_RETIRED,
} slot_state_t;

typedef struct {
    uint8_t slot;
    uint8_t frame_index;
    uint32_t generation;
    bool target;
    const avatar_clip_descriptor_t *clip;
} decode_request_t;

typedef struct {
    const avatar_clip_descriptor_t *clip;
    uint8_t state;
    uint8_t phase;
    int64_t requested_us;
} clip_command_t;

typedef struct {
    uint16_t *frames[2];
    volatile slot_state_t states[2];
    const avatar_clip_descriptor_t *slot_clip[2];
    uint8_t slot_frame[2];
    uint32_t slot_generation[2];
    int64_t slot_retired_us[2];
    bool slot_target[2];
    QueueHandle_t decode_queue;
    QueueHandle_t command_queue;
    lv_timer_t *timer;
    uint8_t displayed_slot;
    const avatar_clip_descriptor_t *current_clip;
    uint8_t current_frame;
    avatar_loop_player_t loop;
    uint32_t current_generation;
    clip_command_t pending;
    bool pending_valid;
    bool pending_cached;
    bool pending_latency_recorded;
    uint32_t pending_generation;
    int64_t last_cache_poll_us;
    bool transitioning;
    uint8_t transition_old_slot;
    uint8_t transition_new_slot;
    int64_t transition_started_us;
    int64_t current_started_us;
    int64_t auto_return_due_us;
    bool thinking_nudge_done;
    uint32_t last_crc_log_frame;
    uint32_t last_metrics_log_frame;
    avatar_anim_metrics_t metrics;
    portMUX_TYPE lock;
} engine_t;

static const avatar_clip_descriptor_t s_bench_clip = {
    .name = BENCH_CLIP_NAME,
    .compressed_crc32 = 0x96e9727e,
    .frame_count = 1,
    .fps = 20,
    .transition_ms = 250,
    .mode = AVATAR_CLIP_LOOP,
    .transition = AVATAR_TRANSITION_CROSS_FADE,
    .frame_offsets = {0},
    .frame_sizes = {88450},
    .decoded_crc32 = {0xe57c30cb},
};

static engine_t s_engine = {
    .displayed_slot = UINT8_MAX,
    .transition_old_slot = UINT8_MAX,
    .transition_new_slot = UINT8_MAX,
    .lock = portMUX_INITIALIZER_UNLOCKED,
};
static volatile bool s_playback_paused;
extern const uint8_t rle_bench_start[] asm("_binary_rle_bench_bin_start");
extern const uint8_t rle_bench_end[] asm("_binary_rle_bench_bin_end");

static bool cache_contains(const char *name)
{
    const uint8_t *data = NULL;
    size_t size = 0;
    if (avatar_clip_cache_acquire(name, &data, &size, NULL) != ESP_OK) return false;
    avatar_clip_cache_release(name);
    return true;
}

static void schedule_decode(uint8_t slot, const avatar_clip_descriptor_t *clip,
                            uint8_t frame_index, uint32_t generation, bool target)
{
    if (slot > 1 || !clip || frame_index >= clip->frame_count ||
        s_engine.states[slot] != SLOT_EMPTY) return;
    decode_request_t request = {
        .slot = slot,
        .frame_index = frame_index,
        .generation = generation,
        .target = target,
        .clip = clip,
    };
    s_engine.states[slot] = SLOT_DECODING;
    if (xQueueSend(s_engine.decode_queue, &request, 0) != pdTRUE)
        s_engine.states[slot] = SLOT_EMPTY;
}

static void schedule_for_empty_slot(uint8_t slot)
{
    if (s_engine.states[slot] != SLOT_EMPTY || s_engine.transitioning) return;
    if (s_engine.pending_valid && s_engine.pending_cached) {
        schedule_decode(slot, s_engine.pending.clip, 0, s_engine.pending_generation, true);
        return;
    }
    s_engine.loop.frame = s_engine.current_frame;
    uint8_t next = (uint8_t)avatar_loop_player_next(&s_engine.loop, s_engine.current_clip);
    schedule_decode(slot, s_engine.current_clip, next, s_engine.current_generation, false);
}

static void decode_task(void *argument)
{
    (void)argument;
    decode_request_t request;
    while (true) {
        if (xQueueReceive(s_engine.decode_queue, &request, portMAX_DELAY) != pdTRUE) continue;
        while (lvgl_port_display_off()) vTaskDelay(pdMS_TO_TICKS(200));
        const uint8_t *input = NULL;
        size_t input_size = 0;
        esp_err_t err = avatar_clip_cache_acquire(request.clip->name, &input, &input_size, NULL);
        uint32_t elapsed = 0;
        if (err == ESP_OK) {
            uint32_t offset = request.clip->frame_offsets[request.frame_index];
            uint32_t size = request.clip->frame_sizes[request.frame_index];
            if ((size_t)offset + size > input_size) {
                err = ESP_ERR_INVALID_SIZE;
            } else {
                int64_t started = esp_timer_get_time();
                err = avatar_rle_decode_rgb565(input + offset, size,
                                               s_engine.frames[request.slot], FRAME_PIXELS);
                elapsed = (uint32_t)(esp_timer_get_time() - started);
                if (err == ESP_OK) {
                    uint32_t crc = esp_crc32_le(0, (const uint8_t *)s_engine.frames[request.slot],
                                                FRAME_BYTES);
                    if (crc != request.clip->decoded_crc32[request.frame_index]) {
                        ESP_LOGE(TAG, "decoded CRC failed clip=%s frame=%u actual=%08lx expected=%08lx",
                                 request.clip->name, request.frame_index, (unsigned long)crc,
                                 (unsigned long)request.clip->decoded_crc32[request.frame_index]);
                        err = ESP_ERR_INVALID_CRC;
                    }
                }
            }
            avatar_clip_cache_release(request.clip->name);
        }

        taskENTER_CRITICAL(&s_engine.lock);
        bool still_wanted = request.target
            ? (s_engine.pending_valid && request.generation == s_engine.pending_generation &&
               request.clip == s_engine.pending.clip)
            : (!s_engine.pending_valid && request.generation == s_engine.current_generation &&
               request.clip == s_engine.current_clip);
        if (err == ESP_OK && still_wanted) {
            s_engine.slot_clip[request.slot] = request.clip;
            s_engine.slot_frame[request.slot] = request.frame_index;
            s_engine.slot_generation[request.slot] = request.generation;
            s_engine.slot_target[request.slot] = request.target;
            s_engine.states[request.slot] = SLOT_READY;
            s_engine.metrics.decoded_frames++;
            s_engine.metrics.decode_total_us += elapsed;
            if (elapsed > s_engine.metrics.decode_max_us) s_engine.metrics.decode_max_us = elapsed;
        } else {
            s_engine.states[request.slot] = SLOT_EMPTY;
        }
        size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        if (!s_engine.metrics.psram_min_free || free_psram < s_engine.metrics.psram_min_free)
            s_engine.metrics.psram_min_free = free_psram;
        taskEXIT_CRITICAL(&s_engine.lock);
        if (!still_wanted) schedule_for_empty_slot(request.slot);
    }
}

static void begin_pending(const clip_command_t *command)
{
    if (!command || !command->clip) return;
    if (!s_engine.transitioning && s_engine.current_clip == command->clip) {
        /* 目标已经在播时必须完成 pending；否则加载失败后的 Flash 回退会
         * 每个 timer tick 重试，形成日志风暴并长期占用状态机。 */
        s_engine.pending_valid = false;
        s_engine.pending_cached = false;
        avatar_clip_cache_set_next(NULL);
        avatar_clip_map_schedule_hints(command->clip, command->state, command->phase);
        return;
    }
    s_engine.pending = *command;
    s_engine.pending_valid = true;
    s_engine.pending_cached = cache_contains(command->clip->name);
    s_engine.pending_latency_recorded = false;
    s_engine.pending_generation++;
    s_engine.last_cache_poll_us = 0;
    avatar_clip_cache_set_next(command->clip->name);
    for (uint8_t slot = 0; slot < 2; ++slot) {
        if (s_engine.states[slot] == SLOT_READY) s_engine.states[slot] = SLOT_EMPTY;
    }
    ESP_LOGI(TAG, "switch target=%s ready=%s", command->clip->name,
             s_engine.pending_cached ? "yes" : "no");
}

static void consume_latest_command(void)
{
    clip_command_t command;
    bool received = false;
    while (xQueueReceive(s_engine.command_queue, &command, 0) == pdTRUE) received = true;
    if (received && s_engine.transitioning) {
        uint8_t abandoned = s_engine.transition_new_slot;
        s_engine.transitioning = false;
        if (abandoned < 2 && abandoned != s_engine.displayed_slot)
            s_engine.states[abandoned] = SLOT_EMPTY;
        s_engine.transition_old_slot = UINT8_MAX;
        s_engine.transition_new_slot = UINT8_MAX;
        ESP_LOGI(TAG, "transition interrupted by clip=%s", command.clip->name);
    }
    if (received) begin_pending(&command);
}

static void poll_pending_cache(int64_t now)
{
    if (!s_engine.pending_valid || s_engine.pending_cached) return;
    if (now - s_engine.pending.requested_us >= SWITCH_TIMEOUT_US) {
        s_engine.metrics.switch_fallbacks++;
        clip_command_t fallback = {
            .clip = &s_bench_clip,
            .state = s_engine.pending.state,
            .phase = s_engine.pending.phase,
            .requested_us = now,
        };
        ESP_LOGW(TAG, "switch timeout clip=%s; fallback to Flash", s_engine.pending.clip->name);
        begin_pending(&fallback);
        return;
    }
    if (now - s_engine.last_cache_poll_us < 100000) return;
    s_engine.last_cache_poll_us = now;
    if (cache_contains(s_engine.pending.clip->name)) {
        s_engine.pending_cached = true;
        avatar_clip_cache_set_next(s_engine.pending.clip->name);
        for (uint8_t slot = 0; slot < 2; ++slot) schedule_for_empty_slot(slot);
    }
}

static uint8_t find_ready_target(void)
{
    for (uint8_t slot = 0; slot < 2; ++slot) {
        if (s_engine.states[slot] == SLOT_READY && s_engine.slot_target[slot] &&
            s_engine.slot_generation[slot] == s_engine.pending_generation &&
            s_engine.slot_clip[slot] == s_engine.pending.clip) return slot;
    }
    return UINT8_MAX;
}

static void complete_switch(uint8_t new_slot, int64_t now)
{
    uint8_t old_slot = s_engine.displayed_slot;
    s_engine.current_clip = s_engine.pending.clip;
    julia_ui_set_program_blink_enabled(!s_engine.current_clip->contains_blink);
    s_engine.current_frame = s_engine.slot_frame[new_slot];
    avatar_loop_player_start(&s_engine.loop, s_engine.current_clip, now);
    s_engine.current_generation = s_engine.pending_generation;
    s_engine.current_started_us = now;
    s_engine.displayed_slot = new_slot;
    s_engine.states[new_slot] = SLOT_DISPLAYING;
    julia_ui_bind_rgb565_frame(s_engine.frames[new_slot], FRAME_PIXELS);
    if (old_slot < 2 && old_slot != new_slot) {
        s_engine.states[old_slot] = SLOT_RETIRED;
        s_engine.slot_retired_us[old_slot] = now;
    }
    avatar_clip_cache_set_current(s_engine.current_clip->name);
    avatar_clip_cache_set_next(NULL);
    s_engine.metrics.clip_switches++;
    uint32_t latency = (uint32_t)(now - s_engine.pending.requested_us);
    if (!s_engine.pending_latency_recorded) {
        s_engine.pending_latency_recorded = true;
        s_engine.metrics.switch_total_latency_us += latency;
        if (latency > s_engine.metrics.switch_max_latency_us)
            s_engine.metrics.switch_max_latency_us = latency;
    }
    avatar_clip_map_schedule_hints(s_engine.current_clip, s_engine.pending.state,
                                   s_engine.pending.phase);
    if (strcmp(s_engine.current_clip->name, "ALERT") == 0 && s_engine.pending.phase == 2)
        s_engine.auto_return_due_us = now + 700000;
    ESP_LOGI(TAG, "switch complete clip=%s transition_done_ms=%.1f",
             s_engine.current_clip->name, (double)latency / 1000.0);
    s_engine.pending_valid = false;
    s_engine.pending_cached = false;
}

static void update_transition(int64_t now)
{
    uint32_t duration_us = (uint32_t)s_engine.pending.clip->transition_ms * 1000U;
    uint32_t elapsed = (uint32_t)(now - s_engine.transition_started_us);
    float t = duration_us ? (float)elapsed / duration_us : 1.0f;
    if (t > 1.0f) t = 1.0f;
    float eased = t * t * (3.0f - 2.0f * t);
    julia_ui_crossfade_rgb565_frames(s_engine.frames[s_engine.transition_old_slot],
                                     s_engine.frames[s_engine.transition_new_slot], FRAME_PIXELS,
                                     (uint8_t)(eased * 255.0f));
    s_engine.metrics.presented_frames++;
    if (t >= 1.0f) {
        uint8_t new_slot = s_engine.transition_new_slot;
        s_engine.transitioning = false;
        complete_switch(new_slot, now);
        s_engine.transition_old_slot = UINT8_MAX;
        s_engine.transition_new_slot = UINT8_MAX;
    }
}

static void start_transition(uint8_t target_slot, int64_t now)
{
    if (s_engine.displayed_slot >= 2) {
        complete_switch(target_slot, now);
        return;
    }
    s_engine.transitioning = true;
    s_engine.transition_old_slot = s_engine.displayed_slot;
    s_engine.transition_new_slot = target_slot;
    s_engine.transition_started_us = now;
    s_engine.states[target_slot] = SLOT_DISPLAYING;
    julia_ui_crossfade_rgb565_frames(s_engine.frames[s_engine.transition_old_slot],
                                     s_engine.frames[target_slot], FRAME_PIXELS, 0);
    uint32_t latency = (uint32_t)(now - s_engine.pending.requested_us);
    if (!s_engine.pending_latency_recorded) {
        s_engine.pending_latency_recorded = true;
        s_engine.metrics.switch_total_latency_us += latency;
        if (latency > s_engine.metrics.switch_max_latency_us)
            s_engine.metrics.switch_max_latency_us = latency;
    }
    ESP_LOGI(TAG, "switch first visible clip=%s latency_ms=%.1f",
             s_engine.pending.clip->name, (double)latency / 1000.0);
}

static uint8_t find_ready_current(void)
{
    for (uint8_t slot = 0; slot < 2; ++slot) {
        if (s_engine.states[slot] == SLOT_READY && !s_engine.slot_target[slot] &&
            s_engine.slot_clip[slot] == s_engine.current_clip &&
            s_engine.slot_generation[slot] == s_engine.current_generation) return slot;
    }
    return UINT8_MAX;
}

static void log_periodic_metrics(void)
{
    if (s_engine.metrics.presented_frames &&
        (s_engine.metrics.presented_frames % 100U) == 0U &&
        s_engine.metrics.presented_frames != s_engine.last_crc_log_frame &&
        s_engine.displayed_slot < 2) {
        s_engine.last_crc_log_frame = s_engine.metrics.presented_frames;
        uint8_t slot = s_engine.transitioning ? s_engine.transition_new_slot : s_engine.displayed_slot;
        uint32_t crc = esp_crc32_le(0, (const uint8_t *)s_engine.frames[slot], FRAME_BYTES);
        uint32_t expected = s_engine.slot_clip[slot]->decoded_crc32[s_engine.slot_frame[slot]];
        ESP_LOGI(TAG, "display CRC frame=%u clip=%s actual=%08lx expected=%08lx %s",
                 s_engine.metrics.presented_frames, s_engine.slot_clip[slot]->name,
                 (unsigned long)crc, (unsigned long)expected, crc == expected ? "PASS" : "FAIL");
    }
    if (!s_engine.metrics.presented_frames ||
        (s_engine.metrics.presented_frames % 200U) != 0U ||
        s_engine.metrics.presented_frames == s_engine.last_metrics_log_frame) return;
    s_engine.last_metrics_log_frame = s_engine.metrics.presented_frames;
    avatar_clip_cache_metrics_t cache = {0};
    avatar_clip_preload_metrics_t preload = {0};
    avatar_clip_cache_get_metrics(&cache);
    avatar_clip_preload_get_metrics(&preload);
    size_t free_heap = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    if (free_heap < 200U * 1024U || free_psram < 200U * 1024U)
        ESP_LOGW("JULIA_ANIM", "low memory heap=%u psram=%u threshold=204800",
                 (unsigned)free_heap, (unsigned)free_psram);
    uint64_t lookups = cache.hits + cache.misses;
    uint64_t readiness = s_engine.metrics.switch_cache_hits +
                         s_engine.metrics.switch_inflight_hits + s_engine.metrics.switch_misses;
    ESP_LOGI(TAG,
             "metrics: shown=%u decoded=%llu underrun=%u psram_min=%u lru_hit=%.1f%% evict=%u cache=%u preload(req=%llu done=%llu cancel=%llu fail=%llu bytes=%llu) switch(count=%u ready=%.1f%% cache=%u inflight=%u miss=%u fallback=%u avg=%.1fms max=%.1fms)",
             s_engine.metrics.presented_frames, s_engine.metrics.decoded_frames,
             s_engine.metrics.decode_underruns, (unsigned)cache.psram_min_free,
             lookups ? (double)cache.hits * 100.0 / lookups : 0.0, cache.evictions,
             (unsigned)cache.cached_bytes, preload.requests, preload.completed,
             preload.cancelled, preload.failed, preload.sd_bytes, s_engine.metrics.clip_switches,
             readiness ? (double)(s_engine.metrics.switch_cache_hits +
                 s_engine.metrics.switch_inflight_hits) * 100.0 / readiness : 0.0,
             s_engine.metrics.switch_cache_hits, s_engine.metrics.switch_inflight_hits,
             s_engine.metrics.switch_misses, s_engine.metrics.switch_fallbacks,
             s_engine.metrics.clip_switches ? (double)s_engine.metrics.switch_total_latency_us /
                 s_engine.metrics.clip_switches / 1000.0 : 0.0,
             (double)s_engine.metrics.switch_max_latency_us / 1000.0);
}

static void playback_timer(lv_timer_t *timer)
{
    (void)timer;
    if (lvgl_port_display_off() || s_playback_paused) return;
    int64_t now = esp_timer_get_time();
    for (uint8_t slot = 0; slot < 2; ++slot) {
        if (s_engine.states[slot] == SLOT_RETIRED &&
            now - s_engine.slot_retired_us[slot] >= 30000) {
            s_engine.states[slot] = SLOT_EMPTY;
            schedule_for_empty_slot(slot);
        }
    }
    consume_latest_command();
    if (s_engine.transitioning) {
        update_transition(now);
        log_periodic_metrics();
        return;
    }
    poll_pending_cache(now);

    if (!s_engine.pending_valid && s_engine.auto_return_due_us &&
        now >= s_engine.auto_return_due_us) {
        const avatar_clip_descriptor_t *thinking = avatar_clip_map_find("THINK");
        uint8_t state = 0, phase = 0;
        avatar_clip_map_get_context(&state, &phase);
        s_engine.auto_return_due_us = 0;
        avatar_anim_engine_request_clip(thinking, state, phase);
        consume_latest_command();
    } else if (!s_engine.pending_valid && !s_engine.thinking_nudge_done &&
               strcmp(s_engine.current_clip->name, "THINK") == 0 &&
               now - s_engine.current_started_us >= 8000000LL) {
        const avatar_clip_descriptor_t *alert = avatar_clip_map_find("ALERT");
        uint8_t state = 0, phase = 0;
        avatar_clip_map_get_context(&state, &phase);
        s_engine.thinking_nudge_done = true;
        avatar_anim_engine_request_clip(alert, state, phase);
        consume_latest_command();
    }

    if (s_engine.pending_valid && s_engine.pending_cached) {
        uint8_t target = find_ready_target();
        if (target < 2) {
            start_transition(target, now);
            log_periodic_metrics();
            return;
        }
        for (uint8_t slot = 0; slot < 2; ++slot) schedule_for_empty_slot(slot);
    }

    if (!s_engine.pending_valid && s_engine.current_clip->mode == AVATAR_CLIP_ONCE_HOLD &&
        s_engine.current_frame + 1U >= s_engine.current_clip->frame_count) {
        transition_director_on_clip_complete(s_engine.current_clip->name);
        log_periodic_metrics();
        return;
    }
    if (s_engine.displayed_slot < 2 &&
        !avatar_loop_player_due(&s_engine.loop, s_engine.current_clip, now)) {
        log_periodic_metrics();
        return;
    }
    uint8_t ready = find_ready_current();
    if (ready < 2) {
        uint8_t previous = s_engine.displayed_slot;
        s_engine.states[ready] = SLOT_DISPLAYING;
        s_engine.displayed_slot = ready;
        s_engine.current_frame = s_engine.slot_frame[ready];
        s_engine.loop.frame = s_engine.current_frame;
        julia_ui_bind_rgb565_frame(s_engine.frames[ready], FRAME_PIXELS);
        s_engine.metrics.presented_frames++;
#if JULIA_ANIM_LOG
        if (s_engine.current_clip->mode == AVATAR_CLIP_ONCE_HOLD)
            ESP_LOGI("JULIA_ANIM", "clip=%s frame=%u/%u", s_engine.current_clip->name,
                     (unsigned)s_engine.current_frame + 1U, s_engine.current_clip->frame_count);
#endif
        if (previous < 2 && previous != ready) {
            s_engine.states[previous] = SLOT_RETIRED;
            s_engine.slot_retired_us[previous] = now;
        }
        else if (previous >= 2) {
            uint8_t other = ready ^ 1U;
            schedule_for_empty_slot(other);
        }
    } else if (s_engine.metrics.presented_frames > 0 && s_engine.displayed_slot >= 2) {
        s_engine.metrics.decode_underruns++;
    }
    if ((s_engine.metrics.presented_frames % 20U) == 0U)
        avatar_clip_cache_maintain_watermark();
    log_periodic_metrics();
}

void avatar_anim_engine_set_paused(bool paused)
{
    s_playback_paused = paused;
}

void avatar_anim_engine_on_canvas_draw_complete(void)
{
    for (uint8_t slot = 0; slot < 2; ++slot) {
        if (s_engine.states[slot] == SLOT_RETIRED) {
            s_engine.states[slot] = SLOT_EMPTY;
            schedule_for_empty_slot(slot);
        }
    }
}

esp_err_t avatar_anim_engine_init(void)
{
    memset(&s_engine.metrics, 0, sizeof(s_engine.metrics));
    esp_err_t err = avatar_clip_cache_init();
    if (err != ESP_OK) return err;
    err = avatar_clip_cache_register_flash(BENCH_CLIP_NAME, rle_bench_start,
                                            (size_t)(rle_bench_end - rle_bench_start));
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    if ((err = register_transition_fallback("TR_S0_S1", transition_s0_s1_start,
                                            transition_s0_s1_end)) != ESP_OK ||
        (err = register_transition_fallback("TR_S1_S0", transition_s1_s0_start,
                                            transition_s1_s0_end)) != ESP_OK ||
        (err = register_transition_fallback("TR_S1_S3", transition_s1_s3_start,
                                            transition_s1_s3_end)) != ESP_OK) return err;
    avatar_clip_cache_set_current(BENCH_CLIP_NAME);
    esp_err_t map_err = avatar_clip_map_init();
    if (map_err != ESP_OK) ESP_LOGW(TAG, "SD clip map unavailable; Flash fallback active: %s",
                                    esp_err_to_name(map_err));
    err = avatar_clip_preload_init();
    if (err != ESP_OK) return err;
    transition_director_check_sd_assets();
    s_engine.decode_queue = xQueueCreate(2, sizeof(decode_request_t));
    s_engine.command_queue = xQueueCreate(16, sizeof(clip_command_t));
    if (!s_engine.decode_queue || !s_engine.command_queue) return ESP_ERR_NO_MEM;
    for (uint8_t slot = 0; slot < 2; ++slot) {
        s_engine.frames[slot] = heap_caps_malloc(FRAME_BYTES,
                                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_engine.frames[slot]) return ESP_ERR_NO_MEM;
        s_engine.states[slot] = SLOT_EMPTY;
    }
    s_engine.current_clip = &s_bench_clip;
    s_engine.current_generation = 1;
    avatar_loop_player_start(&s_engine.loop, s_engine.current_clip, esp_timer_get_time());
    s_engine.metrics.psram_min_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    if (xTaskCreatePinnedToCore(decode_task, "avatar_decode", 4096, NULL, 4, NULL, 1) != pdPASS)
        return ESP_ERR_NO_MEM;
    schedule_decode(0, s_engine.current_clip, 0, s_engine.current_generation, false);
    schedule_decode(1, s_engine.current_clip, 0, s_engine.current_generation, false);
    s_engine.timer = lv_timer_create(playback_timer, TIMER_PERIOD_MS, NULL);
    if (!s_engine.timer) return ESP_ERR_NO_MEM;
    ESP_LOGI(TAG, "clip player ready: clip-clock 4-15fps, crossfade=250ms, decoder core=1");
    avatar_clip_map_set_state(3);
    transition_director_preload_for(JULIA_MAIN_STATE_S1_STANDBY);
    return ESP_OK;
}

void avatar_anim_engine_request_clip(const avatar_clip_descriptor_t *clip,
                                     uint8_t state, uint8_t phase)
{
    if (!clip || !s_engine.command_queue) return;
    if (strcmp(clip->name, "THINK") == 0 &&
        (!s_engine.current_clip || strcmp(s_engine.current_clip->name, "ALERT") != 0))
        s_engine.thinking_nudge_done = false;
    avatar_clip_preload_cancel_except(clip->name);
    bool cached = cache_contains(clip->name);
    bool inflight = !cached && avatar_clip_preload_is_pending(clip->name);
    if (!cached && !inflight) {
        esp_err_t preload_err = avatar_clip_preload_request(
            clip->name, clip->path, clip->compressed_crc32, AVATAR_PRELOAD_EXPLICIT);
        if (preload_err != ESP_OK) {
            ESP_LOGE(TAG, "preload request failed state=%u phase=%u clip=%s: %s",
                     state, phase, clip->name, esp_err_to_name(preload_err));
        }
    }
    taskENTER_CRITICAL(&s_engine.lock);
    s_engine.metrics.switch_requests++;
    if (cached) s_engine.metrics.switch_cache_hits++;
    else if (inflight) s_engine.metrics.switch_inflight_hits++;
    else s_engine.metrics.switch_misses++;
    taskEXIT_CRITICAL(&s_engine.lock);
    clip_command_t command = {
        .clip = clip,
        .state = state,
        .phase = phase,
        .requested_us = esp_timer_get_time(),
    };
    if (xQueueSend(s_engine.command_queue, &command, 0) != pdTRUE) {
        clip_command_t discarded;
        xQueueReceive(s_engine.command_queue, &discarded, 0);
        xQueueSend(s_engine.command_queue, &command, 0);
    }
}

void avatar_anim_engine_get_metrics(avatar_anim_metrics_t *metrics)
{
    if (!metrics) return;
    taskENTER_CRITICAL(&s_engine.lock);
    *metrics = s_engine.metrics;
    taskEXIT_CRITICAL(&s_engine.lock);
}
