#pragma once

#include <stdbool.h>
#include "julia_fsm.h"

void julia_led_fsm_on_enter(julia_fsm_t *fsm, julia_sub_state_t state, fsm_event_t evt);
void julia_led_fsm_set_night(bool night);

