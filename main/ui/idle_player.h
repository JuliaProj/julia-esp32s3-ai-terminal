#pragma once

#include <stdbool.h>
#include "fsm/julia_fsm.h"

void idle_player_init(void);
void idle_player_enter(julia_sub_state_t substate);
void idle_player_enter_variant(julia_sub_state_t substate, uint8_t variant);
void idle_player_play_showcase_path(julia_sub_state_t substate, const char *path);
void idle_player_exit(void);
void idle_player_tick(void);
bool idle_player_is_active(void);
