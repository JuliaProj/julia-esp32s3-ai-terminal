#include "sleep_sequence.h"

#include "avatar_eyes.h"
#include "avatar_face.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "esp_heap_caps.h"
#include "julia_backlight.h"
#include "transition_player.h"

#ifndef JULIA_SLEEP_LOG
#define JULIA_SLEEP_LOG 1
#endif

#define SEQUENCE_TICK_MS 50U
#define SLEEP_HALF_MS 0U
#define SLEEP_CLOSED_MS 400U
#define SLEEP_FADE_MS 800U
#define SLEEP_BLACK_MS 1500U
#define SLEEP_FADE_DURATION_MS 700U
#define SLEEP_HOLD_DEFAULT_MS 2000U
#define SLEEP_HOLD_MIN_MS 1000U
#define SLEEP_HOLD_MAX_MS 5000U

static TimerHandle_t s_timer;
static volatile bool s_running;
static uint32_t s_started_ms;
static uint32_t s_hold_ms = SLEEP_HOLD_DEFAULT_MS;
static uint8_t s_phase;
static sleep_sequence_done_cb_t s_done_cb;
static TaskHandle_t s_task;
static volatile bool s_trn_active;

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000ULL); }

static void log_phase(const char *phase, uint32_t elapsed)
{
#if JULIA_SLEEP_LOG
    ESP_LOGI("SLEEP_SEQUENCE", "phase=%s elapsed_ms=%lu", phase, (unsigned long)elapsed);
#else
    (void)phase;
    (void)elapsed;
#endif
}

static void advance_sequence(void)
{
    if (!s_running) return;
    uint32_t elapsed = now_ms() - s_started_ms;
    if (s_phase == 0) {
        avatar_eyes_show(AVATAR_EYES_HALF);
        log_phase("half-closed", elapsed);
        s_phase = 1;
    }
    if (s_phase == 1 && elapsed >= SLEEP_CLOSED_MS) {
        avatar_eyes_show(AVATAR_EYES_CLOSED);
        log_phase("closed", elapsed);
        s_phase = 2;
    }
    if (s_phase == 2 && elapsed >= SLEEP_FADE_MS) {
        esp_err_t err = julia_backlight_fade_to(0, SLEEP_FADE_DURATION_MS);
        ESP_LOGI("SLEEP_SEQUENCE", "phase=fade-to-black elapsed_ms=%lu result=%s",
                 (unsigned long)elapsed, esp_err_to_name(err));
        s_phase = 3;
    }
    if (s_phase == 3 && elapsed >= SLEEP_BLACK_MS) {
        julia_backlight_set(0);
        esp_err_t err = avatar_face_set_doze(true);
        ESP_LOGI("SLEEP_SEQUENCE", "phase=black-hold elapsed_ms=%lu hold_ms=%lu duty=%lu frame_sync=%s",
                 (unsigned long)elapsed, (unsigned long)s_hold_ms,
                 (unsigned long)julia_backlight_get_duty(), esp_err_to_name(err));
        if (err != ESP_OK) {
            sleep_sequence_done_cb_t callback = s_done_cb;
            sleep_sequence_stop();
            if (callback) callback(err);
            return;
        }
        s_phase = 4;
    }
    if (s_phase == 4 && elapsed >= SLEEP_BLACK_MS + s_hold_ms) {
        esp_err_t err = julia_backlight_breathe_start(0, 20, 8000);
        ESP_LOGI("SLEEP_SEQUENCE", "phase=doze-breathe elapsed_ms=%lu result=%s",
                 (unsigned long)elapsed, esp_err_to_name(err));
        sleep_sequence_done_cb_t callback = s_done_cb;
        sleep_sequence_stop();
        if (callback) callback(err);
    }
}

static void sequence_task(void *argument)
{
    (void)argument;
    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        advance_sequence();
    }
}

static void timer_callback(TimerHandle_t timer)
{
    (void)timer;
    if (s_task) xTaskNotifyGive(s_task);
}

esp_err_t sleep_sequence_init(void)
{
    if (s_timer) return ESP_OK;
    s_timer = xTimerCreate("sleep_seq", pdMS_TO_TICKS(SEQUENCE_TICK_MS), pdTRUE,
                           NULL, timer_callback);
    if (!s_timer || xTaskCreateWithCaps(sequence_task, "sleep_seq_worker", 4096, NULL, 4,
                                        &s_task, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS)
        return ESP_ERR_NO_MEM;
    return ESP_OK;
}

static void sleep_trn_complete(julia_main_state_t from, julia_main_state_t to,
                               esp_err_t result, void *context)
{
    (void)from; (void)to; (void)context;
    if (!s_running || !s_trn_active) return;
    s_trn_active = false;
    if (result != ESP_OK) {
        ESP_LOGW("SLEEP_SEQUENCE", "TRN fallback result=%s", esp_err_to_name(result));
        sleep_sequence_start_fallback(s_done_cb);
        return;
    }
    julia_backlight_set(0);
    esp_err_t sync_err = avatar_face_set_doze(true);
    if (sync_err != ESP_OK) {
        sleep_sequence_done_cb_t callback = s_done_cb;
        sleep_sequence_stop();
        if (callback) callback(sync_err);
        return;
    }
    s_phase = 4;
    s_started_ms = now_ms() - SLEEP_BLACK_MS;
    ESP_LOGI("SLEEP_SEQUENCE", "phase=TRN-black-hold hold_ms=%lu duty=%lu frame_sync=ESP_OK",
             (unsigned long)s_hold_ms, (unsigned long)julia_backlight_get_duty());
    if (xTimerStart(s_timer, 0) != pdPASS) {
        sleep_sequence_done_cb_t callback = s_done_cb;
        sleep_sequence_stop();
        if (callback) callback(ESP_ERR_TIMEOUT);
    }
}

esp_err_t sleep_sequence_start_fallback(sleep_sequence_done_cb_t done_cb)
{
    if (!s_timer) return ESP_ERR_INVALID_STATE;
    sleep_sequence_stop();
    s_done_cb = done_cb;
    s_phase = 0;
    s_started_ms = now_ms();
    s_running = true;
    avatar_eyes_set_sequence_active(true);
    return xTimerStart(s_timer, 0) == pdPASS ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t sleep_sequence_start(sleep_sequence_done_cb_t done_cb)
{
    if (!s_timer) return ESP_ERR_INVALID_STATE;
    sleep_sequence_stop();
    if (!transition_player_has(JULIA_MAIN_STATE_S1_STANDBY, JULIA_MAIN_STATE_S0_SLEEP))
        return sleep_sequence_start_fallback(done_cb);
    s_done_cb = done_cb;
    s_running = true;
    s_trn_active = true;
    avatar_eyes_set_sequence_active(true);
    julia_backlight_fade_to(0, SLEEP_BLACK_MS);
    esp_err_t err = transition_player_play(JULIA_MAIN_STATE_S1_STANDBY,
                                            JULIA_MAIN_STATE_S0_SLEEP,
                                            sleep_trn_complete, NULL);
    if (err == ESP_OK) {
        ESP_LOGI("SLEEP_SEQUENCE", "phase=TRN-start route=S1_S0");
        return ESP_OK;
    }
    s_trn_active = false;
    return sleep_sequence_start_fallback(done_cb);
}

void sleep_sequence_stop(void)
{
    s_running = false;
    if (s_trn_active) transition_player_stop();
    s_trn_active = false;
    if (s_timer) xTimerStop(s_timer, 0);
}

bool sleep_sequence_is_running(void) { return s_running; }

esp_err_t sleep_sequence_set_hold_ms(uint32_t hold_ms)
{
    if (hold_ms < SLEEP_HOLD_MIN_MS || hold_ms > SLEEP_HOLD_MAX_MS)
        return ESP_ERR_INVALID_ARG;
    s_hold_ms = hold_ms;
    return ESP_OK;
}

uint32_t sleep_sequence_get_hold_ms(void) { return s_hold_ms; }
