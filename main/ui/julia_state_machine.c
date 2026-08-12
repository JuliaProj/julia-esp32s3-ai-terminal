#include "julia_state_machine.h"

#include "avatar_eyes.h"
#include "avatar_face.h"
#include "avatar_mouth.h"
#include "idle_player.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "julia_backlight.h"
#include "julia_display_theme.h"
#include "julia_led_fsm_bridge.h"
#include "julia_sensors.h"
#include "julia_ui.h"
#include "transition_player.h"
#include "julia_tts.h"

#define SENSOR_EVALUATION_MS 5000U
#define S1_FAR_TIMEOUT_MS 60000ULL
#define S1_SLEEP_TIMEOUT_MS 120000ULL
#define S2_LEAVE_TIMEOUT_MS 600000ULL
#define S5_SILENCE_TIMEOUT_MS 1800000ULL

static SemaphoreHandle_t s_lock;
static julia_state_snapshot_t s_state;
static julia_transition_status_t s_transition = {.wait_complete = true, .debounce_ms = 500};
static TaskHandle_t s_transition_task;
static uint64_t s_last_transition_ms;
static const char *TAG = "JULIA_STATE";
static struct {
    uint64_t detected_ms;
    uint64_t first_frame_ms;
    uint64_t complete_ms;
    esp_err_t result;
} s_wake_trace;

static julia_state_t main_for(julia_sub_state_t state)
{
    if (state <= JULIA_SUB_STATE_S0_3_MANUAL_SLEEP) return JULIA_MAIN_STATE_S0_SLEEP;
    if (state <= JULIA_SUB_STATE_S1_3_CHARGING_STANDBY) return JULIA_MAIN_STATE_S1_STANDBY;
    if (state <= JULIA_SUB_STATE_S2_3_BEDTIME_COMPANION) return JULIA_MAIN_STATE_S2_COMPANION;
    if (state <= JULIA_SUB_STATE_S3_4_RECOVERY_PROBE) return JULIA_MAIN_STATE_S3_INITIATIVE;
    if (state <= JULIA_SUB_STATE_S4_4_INTERRUPT_HANDLE) return JULIA_MAIN_STATE_S4_DIALOG;
    return JULIA_MAIN_STATE_S5_SILENT;
}

static julia_sub_state_t default_for(julia_state_t state)
{
    static const julia_sub_state_t defaults[] = {
        JULIA_SUB_STATE_S0_1_NIGHT_SLEEP, JULIA_SUB_STATE_S1_1_NEAR_STANDBY,
        JULIA_SUB_STATE_S2_1_OBSERVE, JULIA_SUB_STATE_S3_1_EMOTION_TRIGGER,
        JULIA_SUB_STATE_S4_1_LIGHT_DIALOG, JULIA_SUB_STATE_S5_1_USER_REJECT,
    };
    return state < JULIA_MAIN_STATE_COUNT ? defaults[state] : JULIA_SUB_STATE_COUNT;
}

static void apply_backlight(julia_sub_state_t state)
{
    julia_backlight_breathe_stop();
    switch (state) {
    case JULIA_SUB_STATE_S0_1_NIGHT_SLEEP:
    case JULIA_SUB_STATE_S0_2_DAY_AWAY:
    case JULIA_SUB_STATE_S0_3_MANUAL_SLEEP: julia_backlight_fade_to(0, 500); break;
    case JULIA_SUB_STATE_S1_1_NEAR_STANDBY: julia_backlight_breathe_start(2, 8, 4000); break;
    case JULIA_SUB_STATE_S1_2_FAR_STANDBY: julia_backlight_alive_pulse_start(1, 30000, 500); break;
    case JULIA_SUB_STATE_S1_3_CHARGING_STANDBY: julia_backlight_breathe_start(10, 40, 3000); break;
    case JULIA_SUB_STATE_S2_1_OBSERVE: julia_backlight_breathe_start(18, 22, 4000); break;
    case JULIA_SUB_STATE_S2_2_SHARED_ACTIVITY: julia_backlight_fade_to(25, 400); break;
    case JULIA_SUB_STATE_S2_3_BEDTIME_COMPANION: julia_backlight_breathe_start(3, 8, 6000); break;
    case JULIA_SUB_STATE_S3_3_USER_CALL: julia_backlight_fade_to(60, 300); break;
    case JULIA_SUB_STATE_S3_1_EMOTION_TRIGGER:
    case JULIA_SUB_STATE_S3_2_ROUTINE_BREAK:
    case JULIA_SUB_STATE_S3_4_RECOVERY_PROBE: julia_backlight_fade_to(50, 400); break;
    case JULIA_SUB_STATE_S4_1_LIGHT_DIALOG:
    case JULIA_SUB_STATE_S4_2_DEEP_TALK:
    case JULIA_SUB_STATE_S4_3_MULTI_TURN: julia_backlight_fade_to(70, 300); break;
    case JULIA_SUB_STATE_S4_4_INTERRUPT_HANDLE: julia_backlight_fade_to(70, 500); break;
    default: julia_backlight_breathe_start(2, 10, 6000); break;
    }
}

static void apply_behavior(julia_sub_state_t state, bool defer_wake_backlight)
{
    julia_state_t main = main_for(state);
    mouth_force_idle();
    avatar_face_set_state((uint8_t)main);
    avatar_eyes_set_state((uint8_t)main);
    if (main == JULIA_MAIN_STATE_S0_SLEEP || state == JULIA_SUB_STATE_S1_2_FAR_STANDBY ||
        state == JULIA_SUB_STATE_S1_3_CHARGING_STANDBY) avatar_eyes_show(AVATAR_EYES_CLOSED);
    else if (main == JULIA_MAIN_STATE_S1_STANDBY ||
             state == JULIA_SUB_STATE_S2_3_BEDTIME_COMPANION) avatar_eyes_show(AVATAR_EYES_HALF);
    if (defer_wake_backlight) {
        julia_backlight_breathe_stop();
        ESP_LOGI("WAKE", "breathe stopped current_percent=%u",
                 julia_backlight_get_percent());
    } else {
        apply_backlight(state);
    }
    julia_fsm_t bridge = {.main_state = main, .sub_state = state};
    julia_led_fsm_on_enter(&bridge, state, EVT_NONE);
}

static esp_err_t do_substate_transition(julia_sub_state_t target,
                                        julia_transition_reason_t reason)
{
    if (!s_lock || target >= JULIA_SUB_STATE_COUNT) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    julia_sub_state_t previous = s_state.sub_state;
    if (previous == target) { xSemaphoreGive(s_lock); return ESP_OK; }
    uint64_t now = esp_timer_get_time() / 1000ULL;
    s_state.main_state = main_for(target);
    s_state.sub_state = target;
    s_state.reason = reason;
    s_state.entered_ms = now;
    s_last_transition_ms = now;
    xSemaphoreGive(s_lock);
    bool voice_wake = reason == JULIA_REASON_USER_CALL &&
                      target == JULIA_SUB_STATE_S3_3_USER_CALL;
    if (voice_wake) {
        julia_display_theme_prepare_voice_wake();
        s_wake_trace = (typeof(s_wake_trace)){
            .detected_ms = now, .result = ESP_ERR_INVALID_STATE};
        ESP_LOGI("WAKE", "wakeword detected");
    }
    idle_player_exit();
    julia_ui_set_state(target);
    apply_behavior(target, voice_wake);
    ESP_LOGI(TAG, "%s -> %s reason=%s", julia_fsm_sub_state_name(previous),
             julia_fsm_sub_state_name(target), julia_transition_reason_name(reason));
    return ESP_OK;
}

static void queue_transition(julia_sub_state_t target, julia_transition_reason_t reason)
{
    s_transition.target_main = main_for(target);
    s_transition.target_sub = target;
    s_transition.reason = reason;
    s_transition.pending = true;
    ESP_LOGI(TAG, "transition busy/debounce; queued latest target=%s reason=%s",
             julia_fsm_sub_state_name(target), julia_transition_reason_name(reason));
}

static esp_err_t transition_request(julia_sub_state_t target,
                                    julia_transition_reason_t reason, bool force)
{
    if (!s_lock || target >= JULIA_SUB_STATE_COUNT) return ESP_ERR_INVALID_ARG;
    if (force) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_transition.pending = false;
        xSemaphoreGive(s_lock);
        transition_player_stop();
        ESP_LOGW(TAG, "forced transition target=%s", julia_fsm_sub_state_name(target));
        return do_substate_transition(target, reason);
    }
    uint64_t now = esp_timer_get_time() / 1000ULL;
    bool player_busy = transition_player_is_playing();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool same = s_state.sub_state == target;
    bool debounce = s_last_transition_ms && now - s_last_transition_ms < s_transition.debounce_ms;
    if (!same && ((s_transition.wait_complete && player_busy) || debounce))
        queue_transition(target, reason);
    bool queued = s_transition.pending && !same;
    xSemaphoreGive(s_lock);
    if (queued) return ESP_OK;
    return same ? ESP_OK : do_substate_transition(target, reason);
}

esp_err_t julia_substate_transition(julia_sub_state_t target, julia_transition_reason_t reason)
{
    return transition_request(target, reason, false);
}

esp_err_t julia_substate_transition_force(julia_sub_state_t target,
                                          julia_transition_reason_t reason)
{
    return transition_request(target, reason, true);
}

esp_err_t julia_state_transition(julia_state_t target, julia_transition_reason_t reason)
{
    julia_sub_state_t sub = default_for(target);
    return sub == JULIA_SUB_STATE_COUNT ? ESP_ERR_INVALID_ARG :
           julia_substate_transition(sub, reason);
}

esp_err_t julia_state_transition_force(julia_state_t target, julia_transition_reason_t reason)
{
    julia_sub_state_t sub = default_for(target);
    return sub == JULIA_SUB_STATE_COUNT ? ESP_ERR_INVALID_ARG :
           julia_substate_transition_force(sub, reason);
}

void julia_state_transition_set_debounce(uint32_t debounce_ms)
{
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY); s_transition.debounce_ms = debounce_ms;
    xSemaphoreGive(s_lock);
}

void julia_state_transition_set_wait(bool enabled)
{
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY); s_transition.wait_complete = enabled;
    xSemaphoreGive(s_lock);
}

void julia_state_transition_get_status(julia_transition_status_t *status)
{
    if (!status || !s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY); *status = s_transition; xSemaphoreGive(s_lock);
    status->transition_playing = transition_player_is_playing();
    status->transition_elapsed_ms = transition_player_elapsed_ms();
}

static void player_complete(julia_main_state_t from, julia_main_state_t to, esp_err_t result)
{
    ESP_LOGI(TAG, "transition complete notification S%u->S%u result=%s", from, to,
             esp_err_to_name(result));
    if (to == JULIA_MAIN_STATE_S3_INITIATIVE && s_wake_trace.detected_ms) {
        s_wake_trace.complete_ms = esp_timer_get_time() / 1000ULL;
        s_wake_trace.result = result;
        if (!s_wake_trace.first_frame_ms) julia_backlight_fade_to(100, 300);
        ESP_LOGI("WAKE", "transition complete result=%s elapsed_ms=%llu eyes/mouth restored",
                 esp_err_to_name(result),
                 (unsigned long long)(s_wake_trace.complete_ms - s_wake_trace.detected_ms));
    }
    if (s_transition_task) xTaskNotifyGive(s_transition_task);
    if (result == ESP_OK)
        tts_on_transition_event(from == JULIA_MAIN_STATE_S3_INITIATIVE &&
                                to == JULIA_MAIN_STATE_S4_DIALOG);
}

void julia_state_machine_on_transition_first_frame(julia_main_state_t from,
                                                    julia_main_state_t to)
{
    if (to != JULIA_MAIN_STATE_S3_INITIATIVE || !s_wake_trace.detected_ms ||
        s_wake_trace.first_frame_ms) return;
    s_wake_trace.first_frame_ms = esp_timer_get_time() / 1000ULL;
    esp_err_t err = julia_backlight_fade_to(100, 300);
    ESP_LOGI("WAKE", "first_frame_ready latency_ms=%llu backlight_fade=100/300 result=%s",
             (unsigned long long)(s_wake_trace.first_frame_ms - s_wake_trace.detected_ms),
             esp_err_to_name(err));
}

void julia_state_machine_print_wake_trace(void)
{
    printf("WAKE_TRACE detected_ms=%llu first_frame_ms=%llu complete_ms=%llu "
           "first_frame_latency_ms=%llu result=%s\n",
           (unsigned long long)s_wake_trace.detected_ms,
           (unsigned long long)s_wake_trace.first_frame_ms,
           (unsigned long long)s_wake_trace.complete_ms,
           (unsigned long long)(s_wake_trace.first_frame_ms && s_wake_trace.detected_ms
               ? s_wake_trace.first_frame_ms - s_wake_trace.detected_ms : 0),
           esp_err_to_name(s_wake_trace.result));
}

static void transition_coordinator_task(void *argument)
{
    (void)argument;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(50));
        if (transition_player_is_playing()) continue;
        uint64_t now = esp_timer_get_time() / 1000ULL;
        julia_sub_state_t target = JULIA_SUB_STATE_COUNT;
        julia_transition_reason_t reason = JULIA_REASON_DEBUG;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (s_transition.pending &&
            (!s_last_transition_ms || now - s_last_transition_ms >= s_transition.debounce_ms)) {
            target = s_transition.target_sub;
            reason = s_transition.reason;
            s_transition.pending = false;
        }
        xSemaphoreGive(s_lock);
        if (target != JULIA_SUB_STATE_COUNT) {
            ESP_LOGI(TAG, "dequeue transition target=%s", julia_fsm_sub_state_name(target));
            do_substate_transition(target, reason);
        }
    }
}

void julia_state_note_interaction(void)
{
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_state.last_interaction_ms = esp_timer_get_time() / 1000ULL;
    xSemaphoreGive(s_lock);
    julia_display_theme_on_interaction();
}

void julia_state_machine_get(julia_state_snapshot_t *snapshot)
{
    if (!snapshot || !s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY); *snapshot = s_state; xSemaphoreGive(s_lock);
}

const char *julia_transition_reason_name(julia_transition_reason_t reason)
{
    static const char *names[] = {"boot", "activity", "call", "left", "timeout",
                                  "sensor", "charging", "bedtime", "rejection",
                                  "dialog", "debug"};
    return reason <= JULIA_REASON_DEBUG ? names[reason] : "unknown";
}

static void evaluation_task(void *argument)
{
    (void)argument;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(SENSOR_EVALUATION_MS));
        julia_sensor_status_t sensor; julia_sensor_get_status(&sensor);
        julia_state_snapshot_t state; julia_state_machine_get(&state);
        uint64_t idle = esp_timer_get_time() / 1000ULL - state.last_interaction_ms;
        if (state.main_state == JULIA_MAIN_STATE_S1_STANDBY) {
            if (sensor.charging && state.sub_state != JULIA_SUB_STATE_S1_3_CHARGING_STANDBY)
                julia_substate_transition(JULIA_SUB_STATE_S1_3_CHARGING_STANDBY, JULIA_REASON_CHARGING);
            else if (!sensor.charging && state.sub_state == JULIA_SUB_STATE_S1_3_CHARGING_STANDBY)
                julia_substate_transition(JULIA_SUB_STATE_S1_1_NEAR_STANDBY, JULIA_REASON_SENSOR);
            else if (idle >= S1_SLEEP_TIMEOUT_MS)
                julia_substate_transition(JULIA_SUB_STATE_S0_2_DAY_AWAY, JULIA_REASON_TIMEOUT);
            else if (idle >= S1_FAR_TIMEOUT_MS || (!sensor.user_nearby && !sensor.bluetooth_connected))
                julia_substate_transition(JULIA_SUB_STATE_S1_2_FAR_STANDBY, JULIA_REASON_USER_LEFT);
        } else if (state.main_state == JULIA_MAIN_STATE_S2_COMPANION) {
            if (sensor.user_asleep)
                julia_substate_transition(JULIA_SUB_STATE_S0_1_NIGHT_SLEEP, JULIA_REASON_SENSOR);
            else if (sensor.ambient_light_lux < 10 && sensor.imu_stationary &&
                     sensor.sound_class == JULIA_SOUND_QUIET)
                julia_substate_transition(JULIA_SUB_STATE_S2_3_BEDTIME_COMPANION, JULIA_REASON_SENSOR);
            else if (sensor.sound_class != JULIA_SOUND_QUIET)
                julia_substate_transition(JULIA_SUB_STATE_S2_2_SHARED_ACTIVITY, JULIA_REASON_SENSOR);
            else if (idle >= S2_LEAVE_TIMEOUT_MS)
                julia_substate_transition(JULIA_SUB_STATE_S1_1_NEAR_STANDBY, JULIA_REASON_TIMEOUT);
        } else if (state.main_state == JULIA_MAIN_STATE_S3_INITIATIVE && idle >= 10000)
            julia_substate_transition(JULIA_SUB_STATE_S1_1_NEAR_STANDBY, JULIA_REASON_TIMEOUT);
        else if (state.main_state == JULIA_MAIN_STATE_S5_SILENT && idle >= S5_SILENCE_TIMEOUT_MS)
            julia_substate_transition(JULIA_SUB_STATE_S1_1_NEAR_STANDBY, JULIA_REASON_TIMEOUT);
    }
}

esp_err_t julia_state_machine_init(void)
{
    if (s_lock) return ESP_OK;
    ESP_RETURN_ON_ERROR(julia_sensors_init(), TAG, "sensor init");
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;
    uint64_t now = esp_timer_get_time() / 1000ULL;
    s_state = (julia_state_snapshot_t){JULIA_MAIN_STATE_S1_STANDBY,
        JULIA_SUB_STATE_S1_1_NEAR_STANDBY, JULIA_REASON_BOOT, now, now};
    apply_behavior(s_state.sub_state, false);
    idle_player_init();
    idle_player_enter(s_state.sub_state);
    transition_player_set_observer(player_complete);
    if (xTaskCreate(transition_coordinator_task, "state_transition", 4096, NULL, 4,
                    &s_transition_task) != pdPASS) return ESP_ERR_NO_MEM;
    if (xTaskCreate(evaluation_task, "julia_state_eval", 4096, NULL, 3, NULL) != pdPASS)
        return ESP_ERR_NO_MEM;
    return ESP_OK;
}
