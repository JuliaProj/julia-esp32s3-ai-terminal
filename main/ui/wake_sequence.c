#include "wake_sequence.h"

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

#ifndef JULIA_WAKE_SYNC_LOG
#define JULIA_WAKE_SYNC_LOG 1
#endif

#define SEQUENCE_TICK_MS 50U
#define WAKE_HALF_MS 300U
#define WAKE_OPEN_MS 500U
#define WAKE_DONE_MS 700U
#define WAKE_FADE_MS 300U

static TimerHandle_t s_timer;
static volatile bool s_running;
static uint32_t s_started_ms;
static uint8_t s_phase;
static wake_sequence_done_cb_t s_done_cb;
static TaskHandle_t s_task;
static volatile bool s_trn_active;

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000ULL); }

static void advance_sequence(void)
{
    if (!s_running) return;
    uint32_t elapsed = now_ms() - s_started_ms;
    if (s_phase == 0 && elapsed >= WAKE_HALF_MS) {
        esp_err_t err = avatar_face_set_doze(false);
        avatar_eyes_show(AVATAR_EYES_HALF);
        ESP_LOGI("WAKE_SEQUENCE", "phase=half-open elapsed_ms=%lu frame_sync=%s",
                 (unsigned long)elapsed, esp_err_to_name(err));
        s_phase = 1;
    }
    if (s_phase == 1 && elapsed >= WAKE_OPEN_MS) {
        avatar_eyes_show(AVATAR_EYES_OPEN);
        ESP_LOGI("WAKE_SEQUENCE", "phase=open elapsed_ms=%lu", (unsigned long)elapsed);
        s_phase = 2;
    }
    if (s_phase == 2 && elapsed >= WAKE_DONE_MS) {
        julia_backlight_set(100);
        uint32_t sync_us = 0;
        esp_err_t sync_err = avatar_face_commit_standby_sync(&sync_us);
#if JULIA_WAKE_SYNC_LOG
        if (sync_us > 20000U) {
            ESP_LOGW("WAKE_SEQUENCE", "wake sync slow elapsed_us=%lu result=%s",
                     (unsigned long)sync_us, esp_err_to_name(sync_err));
        } else {
            ESP_LOGI("WAKE_SEQUENCE", "wake sync elapsed_us=%lu result=%s",
                     (unsigned long)sync_us, esp_err_to_name(sync_err));
        }
#endif
        if (sync_err != ESP_OK) {
            wake_sequence_done_cb_t callback = s_done_cb;
            wake_sequence_stop();
            if (callback) callback(sync_err);
            return;
        }
        avatar_eyes_set_sequence_active(false);
        ESP_LOGI("WAKE_SEQUENCE", "phase=complete elapsed_ms=%lu duty=%lu frame_sync=ESP_OK",
                 (unsigned long)elapsed, (unsigned long)julia_backlight_get_duty());
        wake_sequence_done_cb_t callback = s_done_cb;
        wake_sequence_stop();
        if (callback) callback(ESP_OK);
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

esp_err_t wake_sequence_init(void)
{
    if (s_timer) return ESP_OK;
    s_timer = xTimerCreate("wake_seq", pdMS_TO_TICKS(SEQUENCE_TICK_MS), pdTRUE,
                           NULL, timer_callback);
    if (!s_timer || xTaskCreateWithCaps(sequence_task, "wake_seq_worker", 4096, NULL, 4,
                                        &s_task, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS)
        return ESP_ERR_NO_MEM;
    return ESP_OK;
}

esp_err_t wake_sequence_start_fallback(wake_sequence_done_cb_t done_cb)
{
    if (!s_timer) return ESP_ERR_INVALID_STATE;
    wake_sequence_stop();
    julia_backlight_breathe_stop();
    esp_err_t err = julia_backlight_fade_to(100, WAKE_FADE_MS);
    if (err != ESP_OK) return err;
    s_done_cb = done_cb;
    s_phase = 0;
    s_started_ms = now_ms();
    s_running = true;
    avatar_eyes_set_sequence_active(true);
    ESP_LOGI("WAKE_SEQUENCE", "phase=fade-up elapsed_ms=0 duration_ms=%u", WAKE_FADE_MS);
    return xTimerStart(s_timer, 0) == pdPASS ? ESP_OK : ESP_ERR_TIMEOUT;
}

static void wake_trn_complete(julia_main_state_t from, julia_main_state_t to,
                              esp_err_t result, void *context)
{
    (void)from; (void)to; (void)context;
    if (!s_running || !s_trn_active) return;
    s_trn_active = false;
    if (result != ESP_OK) {
        ESP_LOGW("WAKE_SEQUENCE", "TRN fallback result=%s", esp_err_to_name(result));
        wake_sequence_start_fallback(s_done_cb);
        return;
    }
    julia_backlight_set(100);
    uint32_t sync_us = 0;
    esp_err_t sync_err = avatar_face_commit_standby_sync(&sync_us);
#if JULIA_WAKE_SYNC_LOG
    if (sync_us > 20000U)
        ESP_LOGW("WAKE_SEQUENCE", "wake TRN sync slow elapsed_us=%lu result=%s",
                 (unsigned long)sync_us, esp_err_to_name(sync_err));
    else
        ESP_LOGI("WAKE_SEQUENCE", "wake TRN sync elapsed_us=%lu result=%s",
                 (unsigned long)sync_us, esp_err_to_name(sync_err));
#endif
    avatar_eyes_set_sequence_active(false);
    wake_sequence_done_cb_t callback = s_done_cb;
    s_running = false;
    if (callback) callback(sync_err);
}

esp_err_t wake_sequence_start(wake_sequence_done_cb_t done_cb)
{
    if (!s_timer) return ESP_ERR_INVALID_STATE;
    wake_sequence_stop();
    julia_backlight_breathe_stop();
    esp_err_t fade_err = julia_backlight_fade_to(100, WAKE_FADE_MS);
    if (fade_err != ESP_OK) return fade_err;
    if (!transition_player_has(JULIA_MAIN_STATE_S0_SLEEP, JULIA_MAIN_STATE_S1_STANDBY))
        return wake_sequence_start_fallback(done_cb);
    s_done_cb = done_cb;
    s_running = true;
    s_trn_active = true;
    avatar_eyes_set_sequence_active(true);
    esp_err_t err = transition_player_play(JULIA_MAIN_STATE_S0_SLEEP,
                                            JULIA_MAIN_STATE_S1_STANDBY,
                                            wake_trn_complete, NULL);
    if (err == ESP_OK) {
        ESP_LOGI("WAKE_SEQUENCE", "phase=TRN-start route=S0_S1");
        return ESP_OK;
    }
    s_trn_active = false;
    return wake_sequence_start_fallback(done_cb);
}

void wake_sequence_stop(void)
{
    s_running = false;
    if (s_trn_active) transition_player_stop();
    s_trn_active = false;
    if (s_timer) xTimerStop(s_timer, 0);
}

bool wake_sequence_is_running(void) { return s_running; }
