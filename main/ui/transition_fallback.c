#include "transition_fallback.h"

#include "avatar_anim_engine.h"
#include "avatar_clip_cache.h"
#include "avatar_face.h"
#include "breathing_led.h"
#include "esp_log.h"
#include "generated/transitions/transition_clip_table.h"
#include "julia_ui.h"
#include "wake_reply.h"

static const char *TAG = "TRANSITION_FALLBACK";

bool transition_fallback_play_clip(const transition_script_t *script,
                                   julia_sub_state_t target)
{
    const avatar_clip_descriptor_t *clip = script ?
        transition_director_find_clip(script->clip_id) : NULL;
    if (!script || !clip || !avatar_clip_cache_contains(clip->name)) return false;
    julia_ui_set_transition_frame_mode(true);
    avatar_face_set_transition_active(true);
    led_transition_to((led_state_t)script->to_state, script->duration_ms);
    avatar_anim_engine_request_clip(clip, (uint8_t)target, 0);
    ESP_LOGI(TAG, "flash clip from=S%u to=S%u clip=%s", script->from_state,
             script->to_state, clip->name);
    if (script->reply_pcm_id) wake_reply_play();
    return true;
}

void transition_fallback_static(julia_main_state_t from, julia_main_state_t to)
{
    julia_ui_set_transition_frame_mode(false);
    avatar_face_set_transition_active(false);
    ESP_LOGW(TAG, "static from=S%u to=S%u", from, to);
}
