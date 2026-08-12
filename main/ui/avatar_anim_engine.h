#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

struct avatar_clip_descriptor;

typedef struct {
    uint64_t decoded_frames;
    uint64_t decode_total_us;
    uint32_t decode_max_us;
    uint32_t decode_underruns;
    uint32_t presented_frames;
    size_t psram_min_free;
    uint64_t lru_hits;
    uint64_t lru_misses;
    uint32_t lru_evictions;
    uint32_t switch_requests;
    uint32_t clip_switches;
    uint32_t switch_cache_hits;
    uint32_t switch_inflight_hits;
    uint32_t switch_misses;
    uint32_t switch_fallbacks;
    uint64_t switch_total_latency_us;
    uint32_t switch_max_latency_us;
} avatar_anim_metrics_t;

esp_err_t avatar_anim_engine_init(void);
void avatar_anim_engine_get_metrics(avatar_anim_metrics_t *metrics);
void avatar_anim_engine_on_canvas_draw_complete(void);
void avatar_anim_engine_request_clip(const struct avatar_clip_descriptor *clip,
                                     uint8_t state, uint8_t phase);
void avatar_anim_engine_set_paused(bool paused);
