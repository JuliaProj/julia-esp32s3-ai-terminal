#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "fsm/julia_fsm.h"

struct avatar_clip_descriptor;

typedef enum {
    TRANSITION_BACKLIGHT_HOLD = 0,
    TRANSITION_BACKLIGHT_RISE,
    TRANSITION_BACKLIGHT_FALL,
    TRANSITION_BACKLIGHT_FADE_OUT,
    TRANSITION_BACKLIGHT_PREHEAT,
} transition_backlight_curve_t;

typedef enum {
    TRANSITION_RGB_HOLD = 0,
    TRANSITION_RGB_WARM_RISE,
    TRANSITION_RGB_EMOTION_SOLID,
    TRANSITION_RGB_COLD_EBB,
    TRANSITION_RGB_PROBE,
    TRANSITION_RGB_FADE_OUT,
    TRANSITION_RGB_CONCERN,
} transition_rgb_curve_t;

typedef struct {
    julia_main_state_t from_state;
    julia_main_state_t to_state;
    const char *clip_id;
    uint8_t frame_count;
    uint8_t fps;
    uint16_t duration_ms;
    transition_backlight_curve_t backlight_curve;
    transition_rgb_curve_t rgb_curve;
    const char *reply_pcm_id;
} transition_script_t;

typedef struct {
    julia_sub_state_t substate;
    const char *primary_path;
    const char *secondary_path;
} idle_script_t;

typedef void (*transition_director_done_cb_t)(julia_sub_state_t target);

void transition_director_init(transition_director_done_cb_t done_cb);
void transition_director_check_sd_assets(void);
const transition_script_t *transition_director_find(julia_main_state_t from,
                                                    julia_main_state_t to);
bool transition_director_play(julia_main_state_t from, julia_main_state_t to,
                              julia_sub_state_t target);
void transition_director_on_clip_complete(const char *clip_id);
void transition_director_preload_for(julia_main_state_t state);
size_t transition_director_script_count(void);
const struct avatar_clip_descriptor *transition_director_find_clip(const char *name);
const idle_script_t *transition_director_idle_for(julia_sub_state_t substate);
