#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef enum {
    ASR_PROVIDER_XUNFEI,
    ASR_PROVIDER_ALI,
    ASR_PROVIDER_BAIDU,
    ASR_PROVIDER_LOCAL,
    ASR_PROVIDER_QWEN,
} asr_provider_t;

esp_err_t real_asr_init(asr_provider_t provider);
esp_err_t real_asr_start_listening(void);
void real_asr_stop_listening(void);
bool real_asr_is_listening(void);
const char *real_asr_get_result(void);
void real_asr_set_timeout_ms(uint32_t timeout_ms);
asr_provider_t real_asr_get_provider(void);
uint32_t real_asr_get_timeout_ms(void);
