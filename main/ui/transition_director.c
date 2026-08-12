#include "transition_director.h"

#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include "avatar_anim_engine.h"
#include "avatar_clip_cache.h"
#include "avatar_clip_preload.h"
#include "breathing_led.h"
#include "esp_log.h"
#include "esp_crc.h"
#include "julia_sd.h"
#include "julia_ui.h"
#include "avatar_face.h"
#include "wake_reply.h"
#include "generated/transitions/transition_clip_table.h"

static const transition_script_t s_scripts[] = {
    {JULIA_MAIN_STATE_S1_STANDBY, JULIA_MAIN_STATE_S2_COMPANION, "TR_S1_S2", 7, 6, 1167, TRANSITION_BACKLIGHT_RISE, TRANSITION_RGB_PROBE, NULL},
    {JULIA_MAIN_STATE_S2_COMPANION, JULIA_MAIN_STATE_S1_STANDBY, "TR_S2_S1", 7, 6, 1167, TRANSITION_BACKLIGHT_FALL, TRANSITION_RGB_PROBE, NULL},
    {JULIA_MAIN_STATE_S1_STANDBY, JULIA_MAIN_STATE_S3_INITIATIVE, "TR_S1_S3", 7, 6, 1167, TRANSITION_BACKLIGHT_RISE, TRANSITION_RGB_WARM_RISE, "wake"},
    {JULIA_MAIN_STATE_S3_INITIATIVE, JULIA_MAIN_STATE_S4_DIALOG, "TR_S3_S4", 7, 6, 1167, TRANSITION_BACKLIGHT_HOLD, TRANSITION_RGB_EMOTION_SOLID, NULL},
    {JULIA_MAIN_STATE_S4_DIALOG, JULIA_MAIN_STATE_S1_STANDBY, "TR_S4_S1", 7, 6, 1167, TRANSITION_BACKLIGHT_FALL, TRANSITION_RGB_PROBE, NULL},
    {JULIA_MAIN_STATE_S4_DIALOG, JULIA_MAIN_STATE_S5_SILENT, "TR_S4_S5", 7, 6, 1167, TRANSITION_BACKLIGHT_FALL, TRANSITION_RGB_COLD_EBB, NULL},
    {JULIA_MAIN_STATE_S5_SILENT, JULIA_MAIN_STATE_S1_STANDBY, "TR_S5_S1", 7, 6, 1167, TRANSITION_BACKLIGHT_HOLD, TRANSITION_RGB_PROBE, NULL},
    {JULIA_MAIN_STATE_S1_STANDBY, JULIA_MAIN_STATE_S0_SLEEP, "TR_S1_S0", 7, 6, 1167, TRANSITION_BACKLIGHT_FADE_OUT, TRANSITION_RGB_FADE_OUT, NULL},
    {JULIA_MAIN_STATE_S0_SLEEP, JULIA_MAIN_STATE_S1_STANDBY, "TR_S0_S1", 7, 6, 1167, TRANSITION_BACKLIGHT_PREHEAT, TRANSITION_RGB_WARM_RISE, NULL},
    {JULIA_MAIN_STATE_S2_COMPANION, JULIA_MAIN_STATE_S3_INITIATIVE, "TR_S2_S3", 7, 6, 1167, TRANSITION_BACKLIGHT_RISE, TRANSITION_RGB_CONCERN, NULL},
    {JULIA_MAIN_STATE_S5_SILENT, JULIA_MAIN_STATE_S2_COMPANION, "TR_S5_S2", 7, 6, 1167, TRANSITION_BACKLIGHT_RISE, TRANSITION_RGB_PROBE, NULL},
    {JULIA_MAIN_STATE_S5_SILENT, JULIA_MAIN_STATE_S4_DIALOG, "TR_S5_S4", 7, 6, 1167, TRANSITION_BACKLIGHT_RISE, TRANSITION_RGB_EMOTION_SOLID, NULL},
};

#define IDLE_ROOT "/sdcard/julia/idle/"
static const idle_script_t s_idle_scripts[] = {
    {JULIA_SUB_STATE_S0_1_NIGHT_SLEEP, IDLE_ROOT "S0_1_sleep_breathing.trn", NULL},
    {JULIA_SUB_STATE_S0_2_DAY_AWAY, IDLE_ROOT "S0_2_sleep_waiting.trn", NULL},
    {JULIA_SUB_STATE_S0_3_MANUAL_SLEEP, IDLE_ROOT "S0_3_sleep_goodnight.trn", NULL},
    {JULIA_SUB_STATE_S1_1_NEAR_STANDBY, IDLE_ROOT "S1_1_standby_peek.trn", NULL},
    {JULIA_SUB_STATE_S1_2_FAR_STANDBY, IDLE_ROOT "S1_2_standby_deep.trn", NULL},
    {JULIA_SUB_STATE_S1_3_CHARGING_STANDBY, IDLE_ROOT "S1_3_standby_charge.trn", NULL},
    {JULIA_SUB_STATE_S2_1_OBSERVE, IDLE_ROOT "S2_1_companion_read.trn", IDLE_ROOT "S2_1_companion_tea.trn"},
    {JULIA_SUB_STATE_S2_2_SHARED_ACTIVITY, IDLE_ROOT "S2_2_companion_activity.trn", NULL},
    {JULIA_SUB_STATE_S2_3_BEDTIME_COMPANION, IDLE_ROOT "S2_3_companion_sleepy.trn", NULL},
    {JULIA_SUB_STATE_S3_1_EMOTION_TRIGGER, IDLE_ROOT "S3_1_approach_concern.trn", NULL},
    {JULIA_SUB_STATE_S3_2_ROUTINE_BREAK, IDLE_ROOT "S3_2_approach_worry.trn", NULL},
    {JULIA_SUB_STATE_S3_3_USER_CALL, IDLE_ROOT "S3_3_approach_happy.trn", NULL},
    {JULIA_SUB_STATE_S3_4_RECOVERY_PROBE, IDLE_ROOT "S3_4_approach_careful.trn", NULL},
    {JULIA_SUB_STATE_S4_1_LIGHT_DIALOG, IDLE_ROOT "S4_1_chat_natural.trn", NULL},
    {JULIA_SUB_STATE_S4_2_DEEP_TALK, IDLE_ROOT "S4_2_chat_listen.trn", NULL},
    {JULIA_SUB_STATE_S4_3_MULTI_TURN, IDLE_ROOT "S4_3_chat_playful.trn", NULL},
    {JULIA_SUB_STATE_S4_4_INTERRUPT_HANDLE, IDLE_ROOT "S4_4_chat_confused.trn", NULL},
    {JULIA_SUB_STATE_S5_1_USER_REJECT, IDLE_ROOT "S5_1_reject_sad.trn", NULL},
    {JULIA_SUB_STATE_S5_2_USER_PERFUNCTORY, IDLE_ROOT "S5_2_reject_hurt.trn", NULL},
};

static transition_director_done_cb_t s_done_cb;
static const transition_script_t *s_active;
static julia_sub_state_t s_target;
static const char *TAG = "TRANSITION";

const avatar_clip_descriptor_t *transition_director_find_clip(const char *name)
{
    for (size_t i = 0; i < TRANSITION_CLIP_COUNT; ++i)
        if (!strcmp(g_transition_clips[i].name, name)) return &g_transition_clips[i];
    return NULL;
}

const idle_script_t *transition_director_idle_for(julia_sub_state_t substate)
{
    for (size_t i = 0; i < sizeof(s_idle_scripts) / sizeof(s_idle_scripts[0]); ++i)
        if (s_idle_scripts[i].substate == substate) return &s_idle_scripts[i];
    return NULL;
}

void transition_director_init(transition_director_done_cb_t done_cb) { s_done_cb = done_cb; }

void transition_director_check_sd_assets(void)
{
    unsigned valid = 0, missing = 0, bad_crc = 0, fallback = 0;
    if (!julia_sd_is_mounted()) {
        ESP_LOGW(TAG, "SD clip self-check skipped: card unavailable; flash fallback=%u", 3U);
        return;
    }
    for (size_t i = 0; i < TRANSITION_CLIP_COUNT; ++i) {
        const avatar_clip_descriptor_t *clip = &g_transition_clips[i];
        uint32_t crc = 0;
        size_t bytes = 0;
        bool exists = false;
        if (julia_sd_lock(pdMS_TO_TICKS(1500))) {
            FILE *file = fopen(clip->path, "rb");
            if (file) {
                exists = true;
                uint8_t buffer[1024];
                size_t count;
                while ((count = fread(buffer, 1, sizeof(buffer), file)) > 0) {
                    crc = esp_crc32_le(crc, buffer, count);
                    bytes += count;
                }
                fclose(file);
            }
            julia_sd_unlock();
        }
        bool cached = avatar_clip_cache_contains(clip->name);
        if (exists && crc == clip->compressed_crc32) {
            ++valid;
            ESP_LOGI(TAG, "SD clip valid id=%s bytes=%u crc=%08lx", clip->name,
                     (unsigned)bytes, (unsigned long)crc);
        } else {
            if (!exists) ++missing; else ++bad_crc;
            if (cached) ++fallback;
            ESP_LOGW(TAG, "SD clip unavailable id=%s reason=%s crc=%08lx fallback=%s",
                     clip->name, exists ? "crc" : "missing", (unsigned long)crc,
                     cached ? "flash" : "direct");
        }
    }
    ESP_LOGI(TAG, "SD clip self-check valid=%u missing=%u bad_crc=%u flash_fallback=%u",
             valid, missing, bad_crc, fallback);
}

size_t transition_director_script_count(void) { return sizeof(s_scripts) / sizeof(s_scripts[0]); }

const transition_script_t *transition_director_find(julia_main_state_t from,
                                                    julia_main_state_t to)
{
    for (size_t i = 0; i < transition_director_script_count(); ++i)
        if (s_scripts[i].from_state == from && s_scripts[i].to_state == to) return &s_scripts[i];
    return NULL;
}

void transition_director_preload_for(julia_main_state_t state)
{
    for (size_t i = 0; i < transition_director_script_count(); ++i) {
        if (s_scripts[i].from_state != state) continue;
        const avatar_clip_descriptor_t *clip = transition_director_find_clip(s_scripts[i].clip_id);
        if (clip) avatar_clip_preload_request(clip->name, clip->path,
                                              clip->compressed_crc32, AVATAR_PRELOAD_PREDICTED);
    }
}

bool transition_director_play(julia_main_state_t from, julia_main_state_t to,
                              julia_sub_state_t target)
{
    const transition_script_t *script = transition_director_find(from, to);
    const avatar_clip_descriptor_t *clip = script ? transition_director_find_clip(script->clip_id) : NULL;
    if (!script || !clip || !avatar_clip_cache_contains(clip->name)) {
        julia_ui_set_transition_frame_mode(false);
        avatar_face_set_transition_active(false);
        return false;
    }
    s_active = script;
    s_target = target;
    julia_ui_set_transition_frame_mode(true);
    avatar_face_set_transition_active(true);
    led_transition_to((led_state_t)to, script->duration_ms);
    avatar_anim_engine_request_clip(clip, (uint8_t)target, 0);
    ESP_LOGI(TAG, "start from=S%u to=S%u clip=%s frames=%u fps=%u duration_ms=%u",
             from, to, clip->name, script->frame_count, script->fps, script->duration_ms);
    if (script->reply_pcm_id) wake_reply_play();
    return true;
}

void transition_director_on_clip_complete(const char *clip_id)
{
    if (!s_active || strcmp(s_active->clip_id, clip_id)) return;
    julia_sub_state_t target = s_target;
    ESP_LOGI(TAG, "complete clip=%s target=%u static_flush_expected=0", clip_id, target);
    s_active = NULL;
    julia_ui_set_transition_frame_mode(false);
    avatar_face_set_transition_active(false);
    if (s_done_cb) s_done_cb(target);
}
