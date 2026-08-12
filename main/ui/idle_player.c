#include "idle_player.h"

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "avatar_anim_engine.h"
#include "julia_ui.h"
#include "transition_director.h"
#include "transition_player.h"

#define IDLE_SWITCH_MIN_MS 30000U
#define IDLE_SWITCH_SPAN_MS 30001U

static bool s_initialized;
static bool s_active;
static julia_sub_state_t s_substate = JULIA_SUB_STATE_COUNT;
static uint8_t s_variant;
static int64_t s_switch_due_us;
static const char *TAG = "IDLE_PLAYER";

static void schedule_variant_switch(void)
{
    s_switch_due_us = esp_timer_get_time() +
        (int64_t)(IDLE_SWITCH_MIN_MS + esp_random() % IDLE_SWITCH_SPAN_MS) * 1000LL;
}

static void start_current(void)
{
    const idle_script_t *script = transition_director_idle_for(s_substate);
    if (!script || !script->primary_path) { s_active = false; return; }
    const char *path = s_variant && script->secondary_path ?
                       script->secondary_path : script->primary_path;
    esp_err_t err = transition_player_play_path_for(path, UINT32_MAX, NULL, NULL);
    s_active = err == ESP_OK;
    if (script->secondary_path) schedule_variant_switch();
    ESP_LOGI(TAG, "enter substate=%u variant=%u path=%s result=%s", s_substate,
             s_variant, path, esp_err_to_name(err));
}

void idle_player_init(void)
{
    if (s_initialized) return;
    esp_err_t err = transition_player_init();
    s_initialized = err == ESP_OK;
    ESP_LOGI(TAG, "init result=%s", esp_err_to_name(err));
}

void idle_player_enter(julia_sub_state_t substate)
{
    idle_player_enter_variant(substate, 0);
}

void idle_player_enter_variant(julia_sub_state_t substate, uint8_t variant)
{
    if (!s_initialized || substate >= JULIA_SUB_STATE_COUNT) return;
    idle_player_exit();
    s_substate = substate;
    const idle_script_t *script = transition_director_idle_for(substate);
    s_variant = variant && script && script->secondary_path ? 1U : 0U;
    julia_ui_set_program_blink_enabled(false);
    julia_ui_set_idle_frame_mode(true, substate);
    avatar_anim_engine_set_paused(true);
    start_current();
}

void idle_player_play_showcase_path(julia_sub_state_t substate, const char *path)
{
    if (!s_initialized || substate >= JULIA_SUB_STATE_COUNT || !path) return;
    idle_player_exit();
    s_substate = substate;
    julia_ui_set_program_blink_enabled(false);
    julia_ui_set_idle_frame_mode(true, substate);
    avatar_anim_engine_set_paused(true);
    esp_err_t err = transition_player_play_path_for(path, UINT32_MAX, NULL, NULL);
    s_active = err == ESP_OK;
    ESP_LOGI(TAG, "showcase substate=%u path=%s result=%s", substate, path,
             esp_err_to_name(err));
}

void idle_player_exit(void)
{
    if (s_initialized) transition_player_stop();
    avatar_anim_engine_set_paused(false);
    julia_ui_set_idle_frame_mode(false, JULIA_SUB_STATE_COUNT);
    s_active = false;
    s_substate = JULIA_SUB_STATE_COUNT;
    s_switch_due_us = 0;
    julia_ui_set_program_blink_enabled(true);
}

void idle_player_tick(void)
{
    if (!s_active || !s_switch_due_us || esp_timer_get_time() < s_switch_due_us) return;
    const idle_script_t *script = transition_director_idle_for(s_substate);
    if (!script || !script->secondary_path) return;
    s_variant ^= 1U;
    transition_player_stop();
    start_current();
}

bool idle_player_is_active(void) { return s_active; }
