#pragma once

#include <stdbool.h>
#include "fsm/julia_fsm.h"
#include "transition_director.h"

bool transition_fallback_play_clip(const transition_script_t *script,
                                   julia_sub_state_t target);
void transition_fallback_static(julia_main_state_t from, julia_main_state_t to);

