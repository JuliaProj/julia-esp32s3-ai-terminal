#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

void tts_init(void);
esp_err_t tts_synthesize(const char *text);
void tts_on_audio_ready(void (*callback)(const uint8_t *pcm, uint32_t len));
bool tts_is_busy(void);
void tts_cancel(void);
void tts_note_asr_input(void);
void tts_on_transition_event(bool speaking_ready);
bool tts_audio_is_staged(void);
