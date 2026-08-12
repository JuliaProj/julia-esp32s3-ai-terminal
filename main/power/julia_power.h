#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "fsm/julia_fsm.h"

typedef void (*julia_power_screen_cb_t)(bool on);

esp_err_t julia_power_init(void);
float julia_power_get_voltage(void);
uint8_t julia_power_get_battery_percent(void);
bool julia_power_is_charging(void);

void julia_power_set_screen_callback(julia_power_screen_cb_t callback);
esp_err_t julia_power_enter_light_sleep(void);
void julia_power_enter_deep_sleep(uint64_t wakeup_time_us);

/* Call periodically. It emits FSM events and enters the mode required by S0/S1. */
esp_err_t julia_power_update(julia_fsm_t *fsm, bool allow_sleep);

/* Test hook: a negative value restores real ADC readings. */
void julia_power_set_simulated_voltage(float voltage);

