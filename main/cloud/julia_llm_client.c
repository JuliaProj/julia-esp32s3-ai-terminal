#include "julia_llm_client.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "julia_ai_client.h"
#include "julia_system.h"
#include "sdkconfig.h"
#include "esp_timer.h"

#define TAG "JULIA_LLM_CLIENT"
#define RESPONSE_BYTES 4096

typedef struct { char *prompt; char *context; uint32_t generation; } request_t;
static void (*s_callback)(const char *, uint16_t);
static char s_system_prompt[1024] = JULIA_SYSTEM_PROMPT;
static volatile bool s_busy;
static volatile uint32_t s_generation;

static void request_task(void *argument)
{
    request_t *request = argument;
#if CONFIG_JULIA_LLM_MOCK_MODE
    const char *reply = "我听见了。";
    if (strstr(request->prompt, "你好")) reply = "你好呀，今天过得怎么样？";
    else if (strstr(request->prompt, "hello")) reply = "你好呀，今天过得怎么样？";
    else if (strstr(request->prompt, "天气")) reply = "今天天气不错，适合出去走走。";
    else if (strstr(request->prompt, "睡觉")) reply = "早点休息，晚安哦。";
    else if (strstr(request->prompt, "静静")) reply = "好的，我安静待着。";
    vTaskDelay(pdMS_TO_TICKS(120));
    if (request->generation == s_generation && s_callback)
        s_callback(reply, (uint16_t)strlen(reply));
    ESP_LOGI(TAG, "[TIME] llm_mock_first_byte_ms=120 reply=%s", reply);
    uint32_t mock_generation = request->generation;
    free(request->prompt); free(request->context); free(request);
    if (mock_generation == s_generation) s_busy = false;
    vTaskDeleteWithCaps(NULL);
    return;
#endif
    char *response = heap_caps_calloc(1, RESPONSE_BYTES,
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    char system[2048];
    strlcpy(system, s_system_prompt, sizeof(system));
    if (request->context && request->context[0]) {
        strlcat(system, "\n最近对话：\n", sizeof(system));
        strlcat(system, request->context, sizeof(system));
    }
    esp_err_t err = response ? julia_ai_send_message(request->prompt, system)
                             : ESP_ERR_NO_MEM;
    size_t used = 0;
    bool first = true;
    while (err == ESP_OK && request->generation == s_generation) {
        char chunk[256];
        err = julia_ai_receive_chunk_timeout(chunk, sizeof(chunk), first ? 5000 : 20000);
        if (err == ESP_ERR_NOT_FINISHED) { err = ESP_OK; break; }
        if (err != ESP_OK) break;
        first = false;
        size_t bytes = strnlen(chunk, sizeof(chunk));
        if (used + bytes >= RESPONSE_BYTES) bytes = RESPONSE_BYTES - used - 1;
        memcpy(response + used, chunk, bytes); used += bytes; response[used] = '\0';
        if (used == RESPONSE_BYTES - 1) break;
    }
    if (request->generation == s_generation && s_callback) {
        const char *text = used ? response : "我好像断网了，稍后再试";
        s_callback(text, (uint16_t)strlen(text));
    }
    ESP_LOGI(TAG, "complete bytes=%u result=%s", (unsigned)used, esp_err_to_name(err));
    uint32_t generation = request->generation;
    free(response); free(request->prompt); free(request->context); free(request);
    if (generation == s_generation) s_busy = false;
    vTaskDeleteWithCaps(NULL);
}

void llm_client_init(void)
{
    char configured[sizeof(s_system_prompt)] = {0};
    if (julia_system_config_get("system_prompt", configured, sizeof(configured)) == ESP_OK &&
        configured[0]) strlcpy(s_system_prompt, configured, sizeof(s_system_prompt));
}

esp_err_t llm_client_send(const char *prompt, const char *context)
{
    if (!prompt || !prompt[0]) return ESP_ERR_INVALID_ARG;
    if (s_busy) return ESP_ERR_INVALID_STATE;
    request_t *request = calloc(1, sizeof(*request));
    if (!request) return ESP_ERR_NO_MEM;
    request->prompt = strdup(prompt);
    request->context = strdup(context ? context : "");
    request->generation = ++s_generation;
    if (!request->prompt || !request->context) {
        free(request->prompt); free(request->context); free(request); return ESP_ERR_NO_MEM;
    }
    s_busy = true;
    if (xTaskCreateWithCaps(request_task, "mock_llm", 12288, request, 5, NULL,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        s_busy = false; free(request->prompt); free(request->context); free(request);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void llm_client_on_response(void (*callback)(const char *, uint16_t)) { s_callback = callback; }

esp_err_t llm_client_set_system_prompt(const char *prompt)
{
    if (!prompt || !prompt[0] || strlen(prompt) >= sizeof(s_system_prompt))
        return ESP_ERR_INVALID_ARG;
    strlcpy(s_system_prompt, prompt, sizeof(s_system_prompt));
    return julia_system_config_set("system_prompt", prompt, false);
}

void llm_client_get_config(char *url, uint16_t url_size, char *model, uint16_t model_size)
{
    if (url && url_size) strlcpy(url, JULIA_QWEN_CHAT_COMPLETIONS_URL, url_size);
    if (model && model_size) strlcpy(model, CONFIG_JULIA_AI_MODEL, model_size);
}

void llm_client_cancel(void) { ++s_generation; julia_ai_cancel_request(); s_busy = false; }
bool llm_client_is_busy(void) { return s_busy; }
