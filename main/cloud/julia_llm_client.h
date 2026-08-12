#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define JULIA_SYSTEM_PROMPT "你是Julia，一个温柔、简洁的中文AI陪伴助手。回答适合语音朗读，通常不超过两句话，不使用Markdown。"

void llm_client_init(void);
esp_err_t llm_client_send(const char *prompt, const char *context);
void llm_client_on_response(void (*callback)(const char *text, uint16_t len));
esp_err_t llm_client_set_system_prompt(const char *prompt);
void llm_client_get_config(char *url, uint16_t url_size,
                           char *model, uint16_t model_size);
void llm_client_cancel(void);
bool llm_client_is_busy(void);
