#include "julia_real_asr.h"
#include "julia_voice.h"

static asr_provider_t s_provider = ASR_PROVIDER_QWEN;
static uint32_t s_timeout_ms = 6000;

esp_err_t real_asr_init(asr_provider_t provider)
{
    s_provider = provider;
    return provider == ASR_PROVIDER_QWEN ? ESP_OK : ESP_ERR_NOT_SUPPORTED;
}
esp_err_t real_asr_start_listening(void) { return julia_voice_start_listening(); }
void real_asr_stop_listening(void) { julia_voice_stop_listening(); }
bool real_asr_is_listening(void) { return julia_voice_is_listening(); }
const char *real_asr_get_result(void) { return julia_voice_last_result(); }
void real_asr_set_timeout_ms(uint32_t timeout_ms) { s_timeout_ms = timeout_ms; }
asr_provider_t real_asr_get_provider(void) { return s_provider; }
uint32_t real_asr_get_timeout_ms(void) { return s_timeout_ms; }
