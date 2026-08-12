#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "fsm/julia_fsm.h"
#include "transition_player.h"

typedef enum {
    TRANSITION_SOURCE_NONE = 0,
    TRANSITION_SOURCE_PSRAM,
    TRANSITION_SOURCE_SD,
    TRANSITION_SOURCE_FLASH_CLIP,
} transition_source_t;

esp_err_t transition_loader_init(void);
transition_source_t transition_loader_source(julia_main_state_t from, julia_main_state_t to);
esp_err_t transition_loader_play(julia_main_state_t from, julia_main_state_t to,
                                 transition_player_done_cb_t callback, void *context);
void transition_loader_stop(void);

