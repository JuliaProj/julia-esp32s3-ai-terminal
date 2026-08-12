#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "fsm/julia_fsm.h"

#define TRANSITION_ROOT "/sdcard/julia/transitions"

typedef void (*transition_player_done_cb_t)(julia_main_state_t from,
                                            julia_main_state_t to,
                                            esp_err_t result, void *context);
typedef void (*transition_player_list_cb_t)(julia_main_state_t from,
                                            julia_main_state_t to,
                                            const char *path, void *context);
typedef void (*transition_player_observer_cb_t)(julia_main_state_t from,
                                                julia_main_state_t to,
                                                esp_err_t result);

esp_err_t transition_player_init(void);
esp_err_t transition_player_rescan(void);
bool transition_player_has(julia_main_state_t from, julia_main_state_t to);
esp_err_t transition_player_play(julia_main_state_t from, julia_main_state_t to,
                                 transition_player_done_cb_t callback, void *context);
esp_err_t transition_player_play_path(const char *path,
                                      transition_player_done_cb_t callback, void *context);
esp_err_t transition_player_play_path_for(const char *path, uint32_t loop_ms,
                                          transition_player_done_cb_t callback, void *context);
void transition_player_stop(void);
bool transition_player_is_playing(void);
uint32_t transition_player_elapsed_ms(void);
void transition_player_set_observer(transition_player_observer_cb_t callback);
void transition_player_list(transition_player_list_cb_t callback, void *context);
void transition_player_cache_print(void);
void transition_player_set_fallback_enabled(bool enabled);
bool transition_player_fallback_enabled(void);
