#include "julia_led_fsm_bridge.h"
#include "julia_led.h"

static bool s_night;
void julia_led_fsm_set_night(bool night) { s_night = night; }

void julia_led_fsm_on_enter(julia_fsm_t *fsm, julia_sub_state_t state, fsm_event_t evt)
{
    (void)evt;
    switch (state) {
    case JULIA_SUB_STATE_S0_1_NIGHT_SLEEP:
    case JULIA_SUB_STATE_S0_2_DAY_AWAY:
    case JULIA_SUB_STATE_S0_3_MANUAL_SLEEP:
        julia_led_set_off(); break;
    case JULIA_SUB_STATE_S1_1_NEAR_STANDBY:
        julia_led_set_breathing(5, 15, 4000, 0xFFF1D6); break;
    case JULIA_SUB_STATE_S1_2_FAR_STANDBY:
        julia_led_set_breathing(0, 3, 10000, 0xFFF1D6); break;
    case JULIA_SUB_STATE_S1_3_CHARGING_STANDBY:
        julia_led_set_breathing(10, 40, 3000, 0x55DD66); break;
    case JULIA_SUB_STATE_S2_1_OBSERVE:
        julia_led_set_breathing(18, 22, 4000, s_night ? 0xFFD27A : 0xFFF1D6); break;
    case JULIA_SUB_STATE_S2_2_SHARED_ACTIVITY:
        julia_led_set_breathing(20, 25, 1800, 0xFFD27A); break;
    case JULIA_SUB_STATE_S2_3_BEDTIME_COMPANION:
        julia_led_set_breathing(3, 8, 6000, 0xFFD27A); break;
    case JULIA_SUB_STATE_S3_1_EMOTION_TRIGGER:
        julia_led_set_breathing(10, 50, 700, 0xFF8A35); break;
    case JULIA_SUB_STATE_S3_2_ROUTINE_BREAK:
        julia_led_set_breathing(5, 40, 1800, 0xFFE36E); break;
    case JULIA_SUB_STATE_S3_3_USER_CALL:
        julia_led_set_solid(60, 0xFFF1D6); break;
    case JULIA_SUB_STATE_S3_4_RECOVERY_PROBE:
        julia_led_set_breathing(5, 15, 3000, 0xC8A8FF); break;
    case JULIA_SUB_STATE_S4_1_LIGHT_DIALOG:
        julia_led_set_solid(50, 0xFFF1D6); break;
    case JULIA_SUB_STATE_S4_2_DEEP_TALK:
        julia_led_set_breathing(65, 70, 4000, 0x6495D8); break;
    case JULIA_SUB_STATE_S4_3_MULTI_TURN:
        julia_led_set_breathing(75, 80, 1800, 0xFFE36E); break;
    case JULIA_SUB_STATE_S4_4_INTERRUPT_HANDLE:
        julia_led_set_breathing(20, 30, 5000, 0x8A9BAB); break;
    case JULIA_SUB_STATE_S5_1_USER_REJECT:
        julia_led_set_breathing(2, 10, 6000, 0x69849E); break;
    case JULIA_SUB_STATE_S5_2_USER_PERFUNCTORY:
        julia_led_set_breathing(5, 15, 6000, 0xC8A8FF); break;
    case JULIA_SUB_STATE_S5_3_USER_LEFT:
        julia_led_set_breathing(2, 10, 8000, 0x69849E); break;
    default:
        julia_led_set_off(); break;
    }
}
