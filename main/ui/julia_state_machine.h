#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "julia_fsm.h"

typedef julia_main_state_t julia_state_t;

typedef enum {
    JULIA_REASON_BOOT = 0,
    JULIA_REASON_USER_ACTIVITY,
    JULIA_REASON_USER_CALL,
    JULIA_REASON_USER_LEFT,
    JULIA_REASON_TIMEOUT,
    JULIA_REASON_SENSOR,
    JULIA_REASON_CHARGING,
    JULIA_REASON_BEDTIME,
    JULIA_REASON_REJECTION,
    JULIA_REASON_DIALOG,
    JULIA_REASON_DEBUG,
} julia_transition_reason_t;

typedef struct {
    julia_state_t main_state;
    julia_sub_state_t sub_state;
    julia_transition_reason_t reason;
    uint64_t entered_ms;
    uint64_t last_interaction_ms;
} julia_state_snapshot_t;

typedef struct {
    julia_state_t target_main;
    julia_sub_state_t target_sub;
    julia_transition_reason_t reason;
    bool pending;
    bool transition_playing;
    bool wait_complete;
    uint32_t transition_elapsed_ms;
    uint32_t debounce_ms;
} julia_transition_status_t;

esp_err_t julia_state_machine_init(void);
esp_err_t julia_state_transition(julia_state_t target, julia_transition_reason_t reason);
esp_err_t julia_substate_transition(julia_sub_state_t target, julia_transition_reason_t reason);
esp_err_t julia_state_transition_force(julia_state_t target, julia_transition_reason_t reason);
esp_err_t julia_substate_transition_force(julia_sub_state_t target,
                                          julia_transition_reason_t reason);
void julia_state_transition_set_debounce(uint32_t debounce_ms);
void julia_state_transition_set_wait(bool enabled);
void julia_state_transition_get_status(julia_transition_status_t *status);
void julia_state_machine_on_transition_first_frame(julia_main_state_t from,
                                                    julia_main_state_t to);
void julia_state_machine_print_wake_trace(void);
void julia_state_note_interaction(void);
void julia_state_machine_get(julia_state_snapshot_t *snapshot);
const char *julia_transition_reason_name(julia_transition_reason_t reason);
