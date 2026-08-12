#include "julia_ui_showcase.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "julia_fsm.h"
#include "idle_player.h"
#include "julia_ui.h"

#define POWER_KEY_GPIO GPIO_NUM_6
#define KEY_POLL_MS 20U
#define KEY_DEBOUNCE_MS 60U
#define RECORD_LEAD_MS 5000U
#define TRANSITION_HOLD_MS 2200U
#define STATE_HOLD_MS 4200U

static const char *TAG = "UI_SHOWCASE";
static TaskHandle_t s_showcase_task;
static volatile bool s_running;

/* This route covers every directed transition exactly as registered by the
 * stable state machine. Repeated states are intentional route connectors. */
static const julia_sub_state_t s_route[] = {
    JULIA_SUB_STATE_S1_1_NEAR_STANDBY,
    JULIA_SUB_STATE_S2_1_OBSERVE,             /* S1 -> S2 */
    JULIA_SUB_STATE_S1_1_NEAR_STANDBY,        /* S2 -> S1 */
    JULIA_SUB_STATE_S3_1_EMOTION_TRIGGER,     /* S1 -> S3 */
    JULIA_SUB_STATE_S4_1_LIGHT_DIALOG,        /* S3 -> S4 */
    JULIA_SUB_STATE_S5_1_USER_REJECT,         /* S4 -> S5 */
    JULIA_SUB_STATE_S1_1_NEAR_STANDBY,        /* S5 -> S1 */
    JULIA_SUB_STATE_S0_1_NIGHT_SLEEP,         /* S1 -> S0 */
    JULIA_SUB_STATE_S1_1_NEAR_STANDBY,        /* S0 -> S1 */
    JULIA_SUB_STATE_S2_1_OBSERVE,
    JULIA_SUB_STATE_S3_1_EMOTION_TRIGGER,     /* S2 -> S3 */
    JULIA_SUB_STATE_S4_1_LIGHT_DIALOG,
    JULIA_SUB_STATE_S5_1_USER_REJECT,
    JULIA_SUB_STATE_S2_1_OBSERVE,             /* S5 -> S2 */
    JULIA_SUB_STATE_S3_1_EMOTION_TRIGGER,
    JULIA_SUB_STATE_S4_1_LIGHT_DIALOG,
    JULIA_SUB_STATE_S5_1_USER_REJECT,
    JULIA_SUB_STATE_S4_1_LIGHT_DIALOG,        /* S5 -> S4 */
    JULIA_SUB_STATE_S1_1_NEAR_STANDBY,        /* S4 -> S1 */
};

typedef struct {
    julia_sub_state_t state;
    uint8_t variant;
} showcase_idle_t;

static const showcase_idle_t s_idles[] = {
    {JULIA_SUB_STATE_S0_1_NIGHT_SLEEP, 0},
    {JULIA_SUB_STATE_S0_2_DAY_AWAY, 0},
    {JULIA_SUB_STATE_S0_3_MANUAL_SLEEP, 0},
    {JULIA_SUB_STATE_S1_1_NEAR_STANDBY, 0},
    {JULIA_SUB_STATE_S1_2_FAR_STANDBY, 0},
    {JULIA_SUB_STATE_S1_3_CHARGING_STANDBY, 0},
    {JULIA_SUB_STATE_S2_1_OBSERVE, 0},
    {JULIA_SUB_STATE_S2_1_OBSERVE, 1},
    {JULIA_SUB_STATE_S2_2_SHARED_ACTIVITY, 0},
    {JULIA_SUB_STATE_S2_3_BEDTIME_COMPANION, 0},
    {JULIA_SUB_STATE_S3_1_EMOTION_TRIGGER, 0},
    {JULIA_SUB_STATE_S3_2_ROUTINE_BREAK, 0},
    {JULIA_SUB_STATE_S3_3_USER_CALL, 0},
    {JULIA_SUB_STATE_S3_4_RECOVERY_PROBE, 0},
    {JULIA_SUB_STATE_S4_1_LIGHT_DIALOG, 0},
    {JULIA_SUB_STATE_S4_2_DEEP_TALK, 0},
    {JULIA_SUB_STATE_S4_3_MULTI_TURN, 0},
    {JULIA_SUB_STATE_S4_4_INTERRUPT_HANDLE, 0},
    {JULIA_SUB_STATE_S5_1_USER_REJECT, 0},
    {JULIA_SUB_STATE_S5_2_USER_PERFUNCTORY, 0},
};

static const char *s_standby_actions[] = {
    "/sdcard/julia/idle/S1_idle_stretch.trn",
    "/sdcard/julia/idle/S1_idle_drink.trn",
    "/sdcard/julia/idle/S1_idle_read.trn",
    "/sdcard/julia/idle/S1_idle_daze.trn",
};

static void showcase_task(void *argument)
{
    (void)argument;
    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        ESP_LOGI(TAG, "start transitions=12 transition_steps=%u idles=19 actions=%u lead_ms=%u",
                 (unsigned)(sizeof(s_route) / sizeof(s_route[0])),
                 (unsigned)(sizeof(s_idles) / sizeof(s_idles[0])), RECORD_LEAD_MS);
        julia_ui_set_state(JULIA_SUB_STATE_S1_1_NEAR_STANDBY);
        vTaskDelay(pdMS_TO_TICKS(RECORD_LEAD_MS));
        for (size_t i = 0; i < sizeof(s_standby_actions) / sizeof(s_standby_actions[0]); ++i) {
            ESP_LOGI(TAG, "standby_action=%u/%u path=%s", (unsigned)(i + 1),
                     (unsigned)(sizeof(s_standby_actions) / sizeof(s_standby_actions[0])),
                     s_standby_actions[i]);
            idle_player_play_showcase_path(JULIA_SUB_STATE_S1_1_NEAR_STANDBY,
                                           s_standby_actions[i]);
            vTaskDelay(pdMS_TO_TICKS(STATE_HOLD_MS));
        }
        for (size_t i = 0; i < sizeof(s_route) / sizeof(s_route[0]); ++i) {
            julia_sub_state_t state = s_route[i];
            ESP_LOGI(TAG, "step=%u/%u state=%s", (unsigned)(i + 1),
                     (unsigned)(sizeof(s_route) / sizeof(s_route[0])),
                     julia_fsm_sub_state_name(state));
            julia_ui_set_state(state);
            vTaskDelay(pdMS_TO_TICKS(TRANSITION_HOLD_MS));
        }
        for (size_t i = 0; i < sizeof(s_idles) / sizeof(s_idles[0]); ++i) {
            const showcase_idle_t *idle = &s_idles[i];
            ESP_LOGI(TAG, "idle=%u/%u state=%s variant=%u", (unsigned)(i + 1),
                     (unsigned)(sizeof(s_idles) / sizeof(s_idles[0])),
                     julia_fsm_sub_state_name(idle->state), idle->variant);
            julia_ui_set_state(idle->state);
            vTaskDelay(pdMS_TO_TICKS(1600));
            idle_player_enter_variant(idle->state, idle->variant);
            vTaskDelay(pdMS_TO_TICKS(STATE_HOLD_MS));
        }
        julia_ui_set_state(JULIA_SUB_STATE_S1_1_NEAR_STANDBY);
        s_running = false;
        ESP_LOGI(TAG, "complete returned=S1.1");
    }
}

static void power_key_task(void *argument)
{
    (void)argument;
    int stable = gpio_get_level(POWER_KEY_GPIO);
    int sampled = stable;
    int64_t changed_us = esp_timer_get_time();
    while (true) {
        int level = gpio_get_level(POWER_KEY_GPIO);
        int64_t now = esp_timer_get_time();
        if (level != sampled) {
            sampled = level;
            changed_us = now;
        } else if (sampled != stable &&
                   now - changed_us >= (int64_t)KEY_DEBOUNCE_MS * 1000) {
            stable = sampled;
            if (stable == 0) {
                bool started = julia_ui_showcase_start();
                ESP_LOGI(TAG, "power key pressed action=%s",
                         started ? "start" : "ignored-running");
            }
        }
        vTaskDelay(pdMS_TO_TICKS(KEY_POLL_MS));
    }
}

esp_err_t julia_ui_showcase_init(void)
{
    gpio_config_t config = {
        .pin_bit_mask = 1ULL << POWER_KEY_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&config);
    if (err != ESP_OK) return err;
    if (xTaskCreateWithCaps(showcase_task, "ui_showcase", 3072, NULL, 3,
                            &s_showcase_task,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS)
        return ESP_ERR_NO_MEM;
    if (xTaskCreateWithCaps(power_key_task, "power_key", 2048, NULL, 2, NULL,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS)
        return ESP_ERR_NO_MEM;
    ESP_LOGI(TAG, "ready power_key_gpio=%d active_low=1 lead_ms=%u transition_hold_ms=%u state_hold_ms=%u",
             POWER_KEY_GPIO, RECORD_LEAD_MS, TRANSITION_HOLD_MS, STATE_HOLD_MS);
    return ESP_OK;
}

bool julia_ui_showcase_start(void)
{
    if (!s_showcase_task || s_running) return false;
    s_running = true;
    xTaskNotifyGive(s_showcase_task);
    return true;
}

bool julia_ui_showcase_is_running(void) { return s_running; }

bool julia_ui_showcase_allows_state_change(void)
{
    return !s_running || xTaskGetCurrentTaskHandle() == s_showcase_task;
}
