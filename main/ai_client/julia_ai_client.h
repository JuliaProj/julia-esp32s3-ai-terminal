#pragma once

#include <stddef.h>
#include "esp_err.h"

typedef esp_err_t (*julia_ai_function_cb_t)(const char *json_arguments, void *user_ctx);

#define JULIA_QWEN_CHAT_COMPLETIONS_URL \
    "https://dashscope.aliyuncs.com/compatible-mode/v1/chat/completions"

esp_err_t julia_ai_init(const char *api_url, const char *api_key);
esp_err_t julia_ai_init_qwen(const char *api_key);
esp_err_t julia_ai_chat_start(void);
esp_err_t julia_ai_send_message(const char *user_text, const char *system_prompt);
esp_err_t julia_ai_receive_chunk(char *buffer, size_t buf_len);
esp_err_t julia_ai_cancel_request(void);
esp_err_t julia_ai_register_function(const char *name, julia_ai_function_cb_t callback,
                                     void *user_ctx);
void julia_ai_close(void);
