#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

void audio_player_init(void);
esp_err_t audio_player_play(const uint8_t *pcm, uint32_t len);
void audio_player_stop(void);
bool audio_player_is_playing(void);
uint32_t audio_player_get_position_ms(void);
uint16_t audio_player_get_rms(void);
void audio_player_on_complete(void (*callback)(esp_err_t result));
esp_err_t audio_player_stream_begin(void);
esp_err_t audio_player_stream_write(const uint8_t *pcm, uint32_t len);
void audio_player_stream_end(void);
esp_err_t audio_player_play_file(const char *path);
void audio_player_stop_file(void);
bool audio_player_is_file_playing(void);
esp_err_t audio_player_play_resource(const char *name);
esp_err_t audio_player_stage_resource(const char *name);
bool audio_player_has_pending(void);
esp_err_t audio_player_play_staged(void);
void audio_player_clear_staged(void);
