#include "julia_dialog_manager.h"

#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "julia_audio_player.h"
#include "julia_llm_client.h"
#include "julia_state_machine.h"
#include "julia_tts.h"
#include "julia_network.h"
#include "sdkconfig.h"

#define TAG "JULIA_DIALOG"
#define DIALOG_TIMEOUT_US (30LL * 1000LL * 1000LL)

static julia_dialog_context_t s_dialog;
static char s_context[2560];
static SemaphoreHandle_t s_lock;
static volatile int64_t s_idle_deadline;

static void append_history(const char *role, const char *text)
{
    if (!text) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_dialog.turn_count == 10) {
        memmove(s_dialog.history, s_dialog.history + 1,
                sizeof(s_dialog.history) - sizeof(s_dialog.history[0]));
        s_dialog.turn_count--;
    }
    snprintf(s_dialog.history[s_dialog.turn_count++], 256, "%s: %.246s", role, text);
    s_context[0] = '\0';
    for (uint8_t i = 0; i < s_dialog.turn_count; ++i) {
        strlcat(s_context, s_dialog.history[i], sizeof(s_context));
        strlcat(s_context, "\n", sizeof(s_context));
    }
    xSemaphoreGive(s_lock);
}

static bool contains(const char *text, const char *needle)
{
    return text && needle && strstr(text, needle) != NULL;
}

static void playback_complete(esp_err_t result)
{
    if (result == ESP_OK) {
        s_idle_deadline = esp_timer_get_time() + DIALOG_TIMEOUT_US;
        ESP_LOGI(TAG, "reply complete; dialog timeout armed");
    }
}

static void audio_ready(const uint8_t *pcm, uint32_t len)
{
    esp_err_t err = audio_player_play(pcm, len);
    if (err != ESP_OK) ESP_LOGE(TAG, "audio play failed: %s", esp_err_to_name(err));
}

static void llm_response(const char *text, uint16_t len)
{
    (void)len;
    dialog_manager_llm_response(text);
}

static void timeout_task(void *argument)
{
    (void)argument;
    while (true) {
        int64_t deadline = s_idle_deadline;
        if (deadline && esp_timer_get_time() >= deadline) {
            s_idle_deadline = 0;
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_dialog.in_conversation = false;
            xSemaphoreGive(s_lock);
            julia_substate_transition(JULIA_SUB_STATE_S1_1_NEAR_STANDBY,
                                      JULIA_REASON_TIMEOUT);
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void dialog_manager_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    llm_client_on_response(llm_response);
    tts_on_audio_ready(audio_ready);
    audio_player_on_complete(playback_complete);
    if (xTaskCreate(timeout_task, "dialog_timeout", 3072, NULL, 3, NULL) != pdPASS)
        ESP_LOGE(TAG, "failed to create timeout task");
}

void dialog_manager_input(const char *user_text)
{
    if (!user_text || !user_text[0] || !s_lock) return;
    s_idle_deadline = 0;
    tts_note_asr_input();
    llm_client_cancel();
    tts_cancel();
    audio_player_stop();
    julia_state_note_interaction();

    if (contains(user_text, "\xe6\x83\xb3\xe9\x9d\x99\xe9\x9d\x99")) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_dialog.in_conversation = false;
        xSemaphoreGive(s_lock);
        julia_substate_transition_force(JULIA_SUB_STATE_S5_1_USER_REJECT,
                                        JULIA_REASON_REJECTION);
        audio_player_play_resource("sorry");
        return;
    }
    if (contains(user_text, "\xe7\xad\x89\xe4\xb8\x80\xe4\xb8\x8b") ||
        contains(user_text, "\xe5\x81\x9c\xe6\xad\xa2")) {
        julia_substate_transition_force(JULIA_SUB_STATE_S4_4_INTERRUPT_HANDLE,
                                        JULIA_REASON_DIALOG);
        s_idle_deadline = esp_timer_get_time() + 3LL * 1000LL * 1000LL;
        return;
    }

    char prior[sizeof(s_context)];
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(prior, s_context, sizeof(prior));
    s_dialog.in_conversation = true;
    xSemaphoreGive(s_lock);
    append_history("user", user_text);
    julia_substate_transition(JULIA_SUB_STATE_S3_3_USER_CALL, JULIA_REASON_USER_CALL);
    if (!julia_wifi_is_connected() && !CONFIG_JULIA_LLM_MOCK_MODE) {
        julia_substate_transition(JULIA_SUB_STATE_S4_1_LIGHT_DIALOG, JULIA_REASON_DIALOG);
        audio_player_play_resource("net_offline");
        ESP_LOGW(TAG, "offline resource=net_offline");
        return;
    }
    esp_err_t err = llm_client_send(user_text, prior);
    if (err != ESP_OK) dialog_manager_llm_response("Network unavailable, please try later.");
}

void dialog_manager_llm_response(const char *ai_text)
{
    if (!ai_text || !ai_text[0]) return;
    append_history("assistant", ai_text);
    julia_substate_transition(JULIA_SUB_STATE_S4_1_LIGHT_DIALOG, JULIA_REASON_DIALOG);
    esp_err_t err = tts_synthesize(ai_text);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "TTS start failed: %s", esp_err_to_name(err));
        s_idle_deadline = esp_timer_get_time() + DIALOG_TIMEOUT_US;
    }
}

const char *dialog_manager_get_context(void) { return s_context; }

void dialog_manager_reset(void)
{
    if (!s_lock) return;
    llm_client_cancel(); tts_cancel(); audio_player_stop(); s_idle_deadline = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memset(&s_dialog, 0, sizeof(s_dialog)); memset(s_context, 0, sizeof(s_context));
    xSemaphoreGive(s_lock);
}

bool dialog_manager_is_in_conversation(void) { return s_dialog.in_conversation; }

void dialog_manager_get_status(julia_dialog_context_t *status)
{
    if (!status || !s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY); *status = s_dialog; xSemaphoreGive(s_lock);
}
