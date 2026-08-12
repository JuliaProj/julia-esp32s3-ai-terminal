#include "julia_power.h"

#include <math.h>
#include "driver/gpio.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define BATTERY_ADC_CHANNEL       ADC_CHANNEL_7 /* Waveshare board battery divider: GPIO8 */
#define BATTERY_ADC_ATTEN         ADC_ATTEN_DB_12
#define BATTERY_DIVIDER_RATIO     (3.0f / 0.9945f)
#define BATTERY_SAMPLES           16
#define CHARGE_STATUS_GPIO        (-1) /* Board does not route charger status to ESP32-S3. */
#define CHARGE_STATUS_ACTIVE      0
#define VOICE_WAKE_GPIO           GPIO_NUM_3
#define DEFAULT_DEEP_WAKE_US      (30ULL * 60ULL * 1000000ULL)

static const char *TAG = "JULIA_POWER";
static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_cali;
static bool s_calibrated;
static bool s_initialized;
static float s_simulated_voltage = -1.0f;
static julia_power_screen_cb_t s_screen_cb;
static bool s_last_charging;
static bool s_low_reported;

esp_err_t julia_power_init(void)
{
    if (s_initialized) return ESP_OK;

    adc_oneshot_unit_init_cfg_t unit_cfg = {.unit_id = ADC_UNIT_1};
    ESP_RETURN_ON_ERROR(adc_oneshot_new_unit(&unit_cfg, &s_adc), TAG, "ADC init failed");
    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = BATTERY_ADC_ATTEN,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    ESP_RETURN_ON_ERROR(adc_oneshot_config_channel(s_adc, BATTERY_ADC_CHANNEL, &chan_cfg), TAG,
                        "ADC channel config failed");

    adc_cali_curve_fitting_config_t cal_cfg = {
        .unit_id = ADC_UNIT_1,
        .chan = BATTERY_ADC_CHANNEL,
        .atten = BATTERY_ADC_ATTEN,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    esp_err_t cal_err = adc_cali_create_scheme_curve_fitting(&cal_cfg, &s_cali);
    s_calibrated = (cal_err == ESP_OK);
    if (!s_calibrated) ESP_LOGW(TAG, "ADC eFuse calibration unavailable; using raw conversion");

#if CHARGE_STATUS_GPIO >= 0
    {
        gpio_config_t charge_cfg = {
            .pin_bit_mask = 1ULL << CHARGE_STATUS_GPIO,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ESP_RETURN_ON_ERROR(gpio_config(&charge_cfg), TAG, "charge GPIO config failed");
    }
#else
    ESP_LOGW(TAG, "charger status is unavailable on this board revision");
#endif

#if !CONFIG_PM_ENABLE
    ESP_LOGW(TAG, "CONFIG_PM_ENABLE is disabled; automatic frequency scaling is unavailable");
#endif
    s_last_charging = julia_power_is_charging();
    s_initialized = true;
    return ESP_OK;
}

float julia_power_get_voltage(void)
{
    if (s_simulated_voltage >= 0.0f) return s_simulated_voltage;
    if (!s_adc) return 0.0f;
    int raw_sum = 0;
    for (int i = 0; i < BATTERY_SAMPLES; ++i) {
        int raw = 0;
        if (adc_oneshot_read(s_adc, BATTERY_ADC_CHANNEL, &raw) == ESP_OK) raw_sum += raw;
    }
    int raw = raw_sum / BATTERY_SAMPLES;
    int mv = 0;
    if (!s_calibrated || adc_cali_raw_to_voltage(s_cali, raw, &mv) != ESP_OK) {
        mv = raw * 3300 / 4095;
    }
    return (mv / 1000.0f) * BATTERY_DIVIDER_RATIO;
}

uint8_t julia_power_get_battery_percent(void)
{
    static const float volts[] = {3.20f, 3.50f, 3.65f, 3.75f, 3.85f, 3.95f, 4.05f, 4.20f};
    static const uint8_t pct[] = {0, 5, 10, 25, 50, 70, 85, 100};
    float v = julia_power_get_voltage();
    if (v <= volts[0]) return 0;
    for (unsigned i = 1; i < sizeof(volts) / sizeof(volts[0]); ++i) {
        if (v <= volts[i]) {
            float f = (v - volts[i - 1]) / (volts[i] - volts[i - 1]);
            return pct[i - 1] + (uint8_t)lroundf(f * (pct[i] - pct[i - 1]));
        }
    }
    return 100;
}

bool julia_power_is_charging(void)
{
#if CHARGE_STATUS_GPIO >= 0
    return gpio_get_level((gpio_num_t)CHARGE_STATUS_GPIO) == CHARGE_STATUS_ACTIVE;
#else
    return false;
#endif
}

void julia_power_set_screen_callback(julia_power_screen_cb_t callback) { s_screen_cb = callback; }
void julia_power_set_simulated_voltage(float voltage) { s_simulated_voltage = voltage; }

esp_err_t julia_power_enter_light_sleep(void)
{
    if (s_screen_cb) s_screen_cb(false);
    ESP_RETURN_ON_ERROR(esp_sleep_enable_timer_wakeup(1000000ULL), TAG, "timer wake config failed");
    /* ESP32-S3 retains Wi-Fi state and RAM across explicit light sleep. */
    vTaskDelay(pdMS_TO_TICKS(100));
    return esp_light_sleep_start();
}

void julia_power_enter_deep_sleep(uint64_t wakeup_time_us)
{
    if (s_screen_cb) s_screen_cb(false);
    if (!wakeup_time_us) wakeup_time_us = DEFAULT_DEEP_WAKE_US;
    ESP_ERROR_CHECK(esp_sleep_enable_timer_wakeup(wakeup_time_us));
    uint64_t wake_mask = 1ULL << VOICE_WAKE_GPIO;
#if CHARGE_STATUS_GPIO >= 0
    wake_mask |= 1ULL << CHARGE_STATUS_GPIO;
#endif
    ESP_ERROR_CHECK(esp_sleep_enable_ext1_wakeup_io(wake_mask, ESP_EXT1_WAKEUP_ANY_LOW));
    ESP_LOGI(TAG, "deep sleep: timer=%llu us, voice GPIO=%d, charge GPIO=%d",
             wakeup_time_us, VOICE_WAKE_GPIO, CHARGE_STATUS_GPIO);
    esp_deep_sleep_start();
}

esp_err_t julia_power_update(julia_fsm_t *fsm, bool allow_sleep)
{
    ESP_RETURN_ON_FALSE(fsm, ESP_ERR_INVALID_ARG, TAG, "fsm is null");
    bool charging = julia_power_is_charging();
    uint8_t percent = julia_power_get_battery_percent();
    if (charging && (!s_last_charging || fsm->main_state == JULIA_MAIN_STATE_S0_SLEEP)) {
        julia_fsm_handle_event(fsm, EVT_CHARGE_START, NULL);
    }
    if (!charging && s_last_charging) julia_fsm_handle_event(fsm, EVT_CHARGE_DONE, NULL);
    s_last_charging = charging;

    if (percent < 10 && !charging && !s_low_reported) {
        ESP_LOGW(TAG, "Battery low (%u%%), please connect charger", percent);
        julia_fsm_handle_event(fsm, EVT_LOW_BATTERY, NULL);
        s_low_reported = true;
    } else if (percent >= 12 || charging) {
        s_low_reported = false;
    }

    if (!allow_sleep) return ESP_OK;
    if (fsm->main_state == JULIA_MAIN_STATE_S0_SLEEP) julia_power_enter_deep_sleep(0);
    if (fsm->main_state == JULIA_MAIN_STATE_S1_STANDBY) return julia_power_enter_light_sleep();
    return ESP_OK;
}
