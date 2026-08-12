#include <stdio.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "esp_heap_caps.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_io_interface.h"
#include "julia_backlight.h"
#include "wake_word_config.h"
#include "esp_lcd_st77916.h"
#include "I2C_Driver.h"
#include "TCA9554PWR.h"
#include "julia_fsm.h"
#include "julia_ui.h"
#include "julia_ui_showcase.h"
#include "avatar_anim_engine.h"
#include "avatar_micro_action.h"
#include "idle_player.h"
#include "breathing_led.h"
#include "julia_led.h"
#include "julia_led_fsm_bridge.h"
#include "julia_audio.h"
#include "julia_network.h"
#include "julia_ai_client.h"
#include "julia_home.h"
#include "julia_power.h"
#include "julia_sd.h"
#include "julia_voice.h"
#include "julia_lipsync.h"
#include "julia_speech_cloud.h"
#include "julia_context.h"
#include "julia_memory.h"
#include "julia_routine.h"
#include "julia_system.h"
#include "cJSON.h"
#include "sdkconfig.h"
#if CONFIG_JULIA_SOAK_TEST
#include "julia_soak_test.h"
#include "julia_display_theme.h"
#endif

#ifndef CONFIG_JULIA_AI_TEST_ON_BOOT
#define CONFIG_JULIA_AI_TEST_ON_BOOT 0
#endif
#include "lvgl_port.h"
#include "lvgl.h"

#define TAG "JULIA"

#if WAKE_REPLY_ENABLE_ONLINE_CACHE
#define ONLINE_REPLY_ROW(id, group, text, filename, embedded) text,
static const char *const s_online_reply_texts[] = {WAKE_REPLY_CATALOG(ONLINE_REPLY_ROW)};
#undef ONLINE_REPLY_ROW
#define ONLINE_REPLY_ROW(id, group, text, filename, embedded) \
    WAKE_REPLY_SD_DIRECTORY "/" filename,
static const char *const s_online_reply_paths[] = {WAKE_REPLY_CATALOG(ONLINE_REPLY_ROW)};
#undef ONLINE_REPLY_ROW
#endif
#define NETWORK_PROMPT_PATH "/sdcard/julia/netmsg.pcm"

#define LCD_WIDTH                    360
#define LCD_HEIGHT                   360
#define LCD_COLOR_BITS               16

#define LCD_HOST                     SPI2_HOST
#define LCD_SPI_MODE                 0
#define LCD_SPI_READ_HZ              (3 * 1000 * 1000)
#define LCD_SPI_WRITE_HZ             (40 * 1000 * 1000)
#define LCD_SPI_QUEUE_DEPTH          1
#define LCD_SPI_CMD_BITS             32
#define LCD_SPI_PARAM_BITS           8
/* LVGL uses a 360 x 36 RGB565 draw buffer.  The SPI bus must accept one
 * complete flush area; 2048 bytes only covers two full display rows and
 * caused the lower/right part of larger updates to remain white. */
#define LCD_SPI_MAX_TRANSFER_SIZE    (LVGL_PORT_BUFFER_PIXELS * sizeof(lv_color_t))

#define LCD_TE_GPIO                  18
#define LCD_SCK_GPIO                 40
#define LCD_DATA0_GPIO               46
#define LCD_DATA1_GPIO               45
#define LCD_DATA2_GPIO               42
#define LCD_DATA3_GPIO               41
#define LCD_CS_GPIO                  21
#define LCD_RST_GPIO                 (-1)
#define LCD_BL_GPIO                  5

#define LCD_OPCODE_READ_CMD          0x0BULL

#define BACKLIGHT_MAX                100

static esp_lcd_panel_handle_t s_panel_handle = NULL;

static const st77916_lcd_init_cmd_t vendor_specific_init_new[] = {
    {0xF0, (uint8_t[]){0x28}, 1, 0},
    {0xF2, (uint8_t[]){0x28}, 1, 0},
    {0x73, (uint8_t[]){0xF0}, 1, 0},
    {0x7C, (uint8_t[]){0xD1}, 1, 0},
    {0x83, (uint8_t[]){0xE0}, 1, 0},
    {0x84, (uint8_t[]){0x61}, 1, 0},
    {0xF2, (uint8_t[]){0x82}, 1, 0},
    {0xF0, (uint8_t[]){0x00}, 1, 0},
    {0xF0, (uint8_t[]){0x01}, 1, 0},
    {0xF1, (uint8_t[]){0x01}, 1, 0},
    {0xB0, (uint8_t[]){0x56}, 1, 0},
    {0xB1, (uint8_t[]){0x4D}, 1, 0},
    {0xB2, (uint8_t[]){0x24}, 1, 0},
    {0xB4, (uint8_t[]){0x87}, 1, 0},
    {0xB5, (uint8_t[]){0x44}, 1, 0},
    {0xB6, (uint8_t[]){0x8B}, 1, 0},
    {0xB7, (uint8_t[]){0x40}, 1, 0},
    {0xB8, (uint8_t[]){0x86}, 1, 0},
    {0xBA, (uint8_t[]){0x00}, 1, 0},
    {0xBB, (uint8_t[]){0x08}, 1, 0},
    {0xBC, (uint8_t[]){0x08}, 1, 0},
    {0xBD, (uint8_t[]){0x00}, 1, 0},
    {0xC0, (uint8_t[]){0x80}, 1, 0},
    {0xC1, (uint8_t[]){0x10}, 1, 0},
    {0xC2, (uint8_t[]){0x37}, 1, 0},
    {0xC3, (uint8_t[]){0x80}, 1, 0},
    {0xC4, (uint8_t[]){0x10}, 1, 0},
    {0xC5, (uint8_t[]){0x37}, 1, 0},
    {0xC6, (uint8_t[]){0xA9}, 1, 0},
    {0xC7, (uint8_t[]){0x41}, 1, 0},
    {0xC8, (uint8_t[]){0x01}, 1, 0},
    {0xC9, (uint8_t[]){0xA9}, 1, 0},
    {0xCA, (uint8_t[]){0x41}, 1, 0},
    {0xCB, (uint8_t[]){0x01}, 1, 0},
    {0xD0, (uint8_t[]){0x91}, 1, 0},
    {0xD1, (uint8_t[]){0x68}, 1, 0},
    {0xD2, (uint8_t[]){0x68}, 1, 0},
    {0xF5, (uint8_t[]){0x00, 0xA5}, 2, 0},
    {0xDD, (uint8_t[]){0x4F}, 1, 0},
    {0xDE, (uint8_t[]){0x4F}, 1, 0},
    {0xF1, (uint8_t[]){0x10}, 1, 0},
    {0xF0, (uint8_t[]){0x00}, 1, 0},
    {0xF0, (uint8_t[]){0x02}, 1, 0},
    {0xE0, (uint8_t[]){0xF0, 0x0A, 0x10, 0x09, 0x09, 0x36, 0x35, 0x33, 0x4A, 0x29, 0x15, 0x15, 0x2E, 0x34}, 14, 0},
    {0xE1, (uint8_t[]){0xF0, 0x0A, 0x0F, 0x08, 0x08, 0x05, 0x34, 0x33, 0x4A, 0x39, 0x15, 0x15, 0x2D, 0x33}, 14, 0},
    {0xF0, (uint8_t[]){0x10}, 1, 0},
    {0xF3, (uint8_t[]){0x10}, 1, 0},
    {0xE0, (uint8_t[]){0x07}, 1, 0},
    {0xE1, (uint8_t[]){0x00}, 1, 0},
    {0xE2, (uint8_t[]){0x00}, 1, 0},
    {0xE3, (uint8_t[]){0x00}, 1, 0},
    {0xE4, (uint8_t[]){0xE0}, 1, 0},
    {0xE5, (uint8_t[]){0x06}, 1, 0},
    {0xE6, (uint8_t[]){0x21}, 1, 0},
    {0xE7, (uint8_t[]){0x01}, 1, 0},
    {0xE8, (uint8_t[]){0x05}, 1, 0},
    {0xE9, (uint8_t[]){0x02}, 1, 0},
    {0xEA, (uint8_t[]){0xDA}, 1, 0},
    {0xEB, (uint8_t[]){0x00}, 1, 0},
    {0xEC, (uint8_t[]){0x00}, 1, 0},
    {0xED, (uint8_t[]){0x0F}, 1, 0},
    {0xEE, (uint8_t[]){0x00}, 1, 0},
    {0xEF, (uint8_t[]){0x00}, 1, 0},
    {0xF8, (uint8_t[]){0x00}, 1, 0},
    {0xF9, (uint8_t[]){0x00}, 1, 0},
    {0xFA, (uint8_t[]){0x00}, 1, 0},
    {0xFB, (uint8_t[]){0x00}, 1, 0},
    {0xFC, (uint8_t[]){0x00}, 1, 0},
    {0xFD, (uint8_t[]){0x00}, 1, 0},
    {0xFE, (uint8_t[]){0x00}, 1, 0},
    {0xFF, (uint8_t[]){0x00}, 1, 0},
    {0x60, (uint8_t[]){0x40}, 1, 0},
    {0x61, (uint8_t[]){0x04}, 1, 0},
    {0x62, (uint8_t[]){0x00}, 1, 0},
    {0x63, (uint8_t[]){0x42}, 1, 0},
    {0x64, (uint8_t[]){0xD9}, 1, 0},
    {0x65, (uint8_t[]){0x00}, 1, 0},
    {0x66, (uint8_t[]){0x00}, 1, 0},
    {0x67, (uint8_t[]){0x00}, 1, 0},
    {0x68, (uint8_t[]){0x00}, 1, 0},
    {0x69, (uint8_t[]){0x00}, 1, 0},
    {0x6A, (uint8_t[]){0x00}, 1, 0},
    {0x6B, (uint8_t[]){0x00}, 1, 0},
    {0x70, (uint8_t[]){0x40}, 1, 0},
    {0x71, (uint8_t[]){0x03}, 1, 0},
    {0x72, (uint8_t[]){0x00}, 1, 0},
    {0x73, (uint8_t[]){0x42}, 1, 0},
    {0x74, (uint8_t[]){0xD8}, 1, 0},
    {0x75, (uint8_t[]){0x00}, 1, 0},
    {0x76, (uint8_t[]){0x00}, 1, 0},
    {0x77, (uint8_t[]){0x00}, 1, 0},
    {0x78, (uint8_t[]){0x00}, 1, 0},
    {0x79, (uint8_t[]){0x00}, 1, 0},
    {0x7A, (uint8_t[]){0x00}, 1, 0},
    {0x7B, (uint8_t[]){0x00}, 1, 0},
    {0x80, (uint8_t[]){0x48}, 1, 0},
    {0x81, (uint8_t[]){0x00}, 1, 0},
    {0x82, (uint8_t[]){0x06}, 1, 0},
    {0x83, (uint8_t[]){0x02}, 1, 0},
    {0x84, (uint8_t[]){0xD6}, 1, 0},
    {0x85, (uint8_t[]){0x04}, 1, 0},
    {0x86, (uint8_t[]){0x00}, 1, 0},
    {0x87, (uint8_t[]){0x00}, 1, 0},
    {0x88, (uint8_t[]){0x48}, 1, 0},
    {0x89, (uint8_t[]){0x00}, 1, 0},
    {0x8A, (uint8_t[]){0x08}, 1, 0},
    {0x8B, (uint8_t[]){0x02}, 1, 0},
    {0x8C, (uint8_t[]){0xD8}, 1, 0},
    {0x8D, (uint8_t[]){0x04}, 1, 0},
    {0x8E, (uint8_t[]){0x00}, 1, 0},
    {0x8F, (uint8_t[]){0x00}, 1, 0},
    {0x90, (uint8_t[]){0x48}, 1, 0},
    {0x91, (uint8_t[]){0x00}, 1, 0},
    {0x92, (uint8_t[]){0x0A}, 1, 0},
    {0x93, (uint8_t[]){0x02}, 1, 0},
    {0x94, (uint8_t[]){0xDA}, 1, 0},
    {0x95, (uint8_t[]){0x04}, 1, 0},
    {0x96, (uint8_t[]){0x00}, 1, 0},
    {0x97, (uint8_t[]){0x00}, 1, 0},
    {0x98, (uint8_t[]){0x48}, 1, 0},
    {0x99, (uint8_t[]){0x00}, 1, 0},
    {0x9A, (uint8_t[]){0x0C}, 1, 0},
    {0x9B, (uint8_t[]){0x02}, 1, 0},
    {0x9C, (uint8_t[]){0xDC}, 1, 0},
    {0x9D, (uint8_t[]){0x04}, 1, 0},
    {0x9E, (uint8_t[]){0x00}, 1, 0},
    {0x9F, (uint8_t[]){0x00}, 1, 0},
    {0xA0, (uint8_t[]){0x48}, 1, 0},
    {0xA1, (uint8_t[]){0x00}, 1, 0},
    {0xA2, (uint8_t[]){0x05}, 1, 0},
    {0xA3, (uint8_t[]){0x02}, 1, 0},
    {0xA4, (uint8_t[]){0xD5}, 1, 0},
    {0xA5, (uint8_t[]){0x04}, 1, 0},
    {0xA6, (uint8_t[]){0x00}, 1, 0},
    {0xA7, (uint8_t[]){0x00}, 1, 0},
    {0xA8, (uint8_t[]){0x48}, 1, 0},
    {0xA9, (uint8_t[]){0x00}, 1, 0},
    {0xAA, (uint8_t[]){0x07}, 1, 0},
    {0xAB, (uint8_t[]){0x02}, 1, 0},
    {0xAC, (uint8_t[]){0xD7}, 1, 0},
    {0xAD, (uint8_t[]){0x04}, 1, 0},
    {0xAE, (uint8_t[]){0x00}, 1, 0},
    {0xAF, (uint8_t[]){0x00}, 1, 0},
    {0xB0, (uint8_t[]){0x48}, 1, 0},
    {0xB1, (uint8_t[]){0x00}, 1, 0},
    {0xB2, (uint8_t[]){0x09}, 1, 0},
    {0xB3, (uint8_t[]){0x02}, 1, 0},
    {0xB4, (uint8_t[]){0xD9}, 1, 0},
    {0xB5, (uint8_t[]){0x04}, 1, 0},
    {0xB6, (uint8_t[]){0x00}, 1, 0},
    {0xB7, (uint8_t[]){0x00}, 1, 0},
    {0xB8, (uint8_t[]){0x48}, 1, 0},
    {0xB9, (uint8_t[]){0x00}, 1, 0},
    {0xBA, (uint8_t[]){0x0B}, 1, 0},
    {0xBB, (uint8_t[]){0x02}, 1, 0},
    {0xBC, (uint8_t[]){0xDB}, 1, 0},
    {0xBD, (uint8_t[]){0x04}, 1, 0},
    {0xBE, (uint8_t[]){0x00}, 1, 0},
    {0xBF, (uint8_t[]){0x00}, 1, 0},
    {0xC0, (uint8_t[]){0x10}, 1, 0},
    {0xC1, (uint8_t[]){0x47}, 1, 0},
    {0xC2, (uint8_t[]){0x56}, 1, 0},
    {0xC3, (uint8_t[]){0x65}, 1, 0},
    {0xC4, (uint8_t[]){0x74}, 1, 0},
    {0xC5, (uint8_t[]){0x88}, 1, 0},
    {0xC6, (uint8_t[]){0x99}, 1, 0},
    {0xC7, (uint8_t[]){0x01}, 1, 0},
    {0xC8, (uint8_t[]){0xBB}, 1, 0},
    {0xC9, (uint8_t[]){0xAA}, 1, 0},
    {0xD0, (uint8_t[]){0x10}, 1, 0},
    {0xD1, (uint8_t[]){0x47}, 1, 0},
    {0xD2, (uint8_t[]){0x56}, 1, 0},
    {0xD3, (uint8_t[]){0x65}, 1, 0},
    {0xD4, (uint8_t[]){0x74}, 1, 0},
    {0xD5, (uint8_t[]){0x88}, 1, 0},
    {0xD6, (uint8_t[]){0x99}, 1, 0},
    {0xD7, (uint8_t[]){0x01}, 1, 0},
    {0xD8, (uint8_t[]){0xBB}, 1, 0},
    {0xD9, (uint8_t[]){0xAA}, 1, 0},
    {0xF3, (uint8_t[]){0x01}, 1, 0},
    {0xF0, (uint8_t[]){0x00}, 1, 0},
    {0x21, (uint8_t[]){0x00}, 1, 0},
    {0x11, (uint8_t[]){0x00}, 1, 120},
    {0x29, (uint8_t[]){0x00}, 1, 0},
};

static int lcd_pack_read_cmd(uint8_t cmd)
{
    return ((int)LCD_OPCODE_READ_CMD << 24) | (cmd << 8);
}

static volatile bool s_boot_reveal_started;

static void lcd_backlight_set(uint8_t light)
{
    if (light > BACKLIGHT_MAX) {
        light = BACKLIGHT_MAX;
    }
    julia_backlight_set(light);
}

static void lcd_backlight_init(void)
{
    ESP_ERROR_CHECK(julia_backlight_init());
    julia_backlight_set(0);
}

#ifdef BOOT_DEBUG
void boot_debug_log_stage(const char *stage_name)
{
    ESP_LOGI("BOOT_STAGE", "%s backlight=%u duty=%u", stage_name,
             julia_backlight_get_percent(), (unsigned)julia_backlight_get_duty());
}
#define BOOT_STAGE(name) boot_debug_log_stage(name)
#else
#define BOOT_STAGE(name) do { } while (0)
#endif

void system_boot_sequence(void)
{
    BOOT_STAGE("fade_in_start");
    avatar_show_all();
    led_set_state(LED_S1_DIM_WARM);
    display_fade_in(800);
    s_boot_reveal_started = true;
}

static void lcd_reset_via_exio(void)
{
    Set_EXIO(TCA9554_EXIO2, false);
    vTaskDelay(pdMS_TO_TICKS(10));
    Set_EXIO(TCA9554_EXIO2, true);
    vTaskDelay(pdMS_TO_TICKS(50));
}

static esp_lcd_panel_io_handle_t lcd_new_io(uint32_t pclk_hz)
{
    esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num = LCD_CS_GPIO,
        .dc_gpio_num = -1,
        .spi_mode = LCD_SPI_MODE,
        .pclk_hz = pclk_hz,
        .trans_queue_depth = LCD_SPI_QUEUE_DEPTH,
        .on_color_trans_done = lvgl_port_color_trans_done,
        .user_ctx = NULL,
        .lcd_cmd_bits = LCD_SPI_CMD_BITS,
        .lcd_param_bits = LCD_SPI_PARAM_BITS,
        .flags = {
            .dc_low_on_data = 0,
            .octal_mode = 0,
            .quad_mode = 1,
            .sio_mode = 0,
            .lsb_first = 0,
            .cs_high_active = 0,
        },
    };

    esp_lcd_panel_io_handle_t io_handle = NULL;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_config, &io_handle));
    return io_handle;
}

static void lcd_init_panel(void)
{
    static const spi_bus_config_t bus_config = {
        .data0_io_num = LCD_DATA0_GPIO,
        .data1_io_num = LCD_DATA1_GPIO,
        .sclk_io_num = LCD_SCK_GPIO,
        .data2_io_num = LCD_DATA2_GPIO,
        .data3_io_num = LCD_DATA3_GPIO,
        .data4_io_num = -1,
        .data5_io_num = -1,
        .data6_io_num = -1,
        .data7_io_num = -1,
        .max_transfer_sz = LCD_SPI_MAX_TRANSFER_SIZE,
        .flags = SPICOMMON_BUSFLAG_MASTER,
        .intr_flags = 0,
    };

    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &bus_config, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_handle_t io_handle = lcd_new_io(LCD_SPI_READ_HZ);

    uint8_t register_data[4] = {0};
    ESP_ERROR_CHECK(esp_lcd_panel_io_rx_param(io_handle, lcd_pack_read_cmd(0x04), register_data, sizeof(register_data)));
    ESP_LOGI(TAG, "LCD ID(0x04): %02X %02X %02X %02X", register_data[0], register_data[1], register_data[2], register_data[3]);

    st77916_vendor_config_t vendor_config = {
        .flags = {
            .use_qspi_interface = 1,
        },
    };
    if (register_data[0] == 0x00 && register_data[1] == 0x02 && register_data[2] == 0x7F && register_data[3] == 0x7F) {
        vendor_config.init_cmds = vendor_specific_init_new;
        vendor_config.init_cmds_size = sizeof(vendor_specific_init_new) / sizeof(vendor_specific_init_new[0]);
        ESP_LOGI(TAG, "Using vendor_specific_init_new");
    } else {
        vendor_config.init_cmds = vendor_specific_init_new;
        vendor_config.init_cmds_size = sizeof(vendor_specific_init_new) / sizeof(vendor_specific_init_new[0]);
        ESP_LOGI(TAG, "Using vendor init table for detected panel");
    }

    io_handle = lcd_new_io(LCD_SPI_WRITE_HZ);

    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = LCD_RST_GPIO,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = LCD_COLOR_BITS,
        .flags = {
            .reset_active_high = 0,
        },
        .vendor_config = &vendor_config,
    };

    ESP_ERROR_CHECK(esp_lcd_new_panel_st77916(io_handle, &panel_config, &s_panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel_handle, true));
}

static void fsm_ui_on_enter(julia_fsm_t *fsm, julia_sub_state_t state, fsm_event_t evt)
{
    (void)evt;
    static const expr_t expression_for_state[JULIA_MAIN_STATE_COUNT] = {
        [JULIA_MAIN_STATE_S0_SLEEP] = JULIA_EXPR_SLEEP,
        [JULIA_MAIN_STATE_S1_STANDBY] = JULIA_EXPR_SLEEP,
        [JULIA_MAIN_STATE_S2_COMPANION] = JULIA_EXPR_WATCHING,
        [JULIA_MAIN_STATE_S3_INITIATIVE] = JULIA_EXPR_HAPPY,
        [JULIA_MAIN_STATE_S4_DIALOG] = JULIA_EXPR_SPEAKING,
        [JULIA_MAIN_STATE_S5_SILENT] = JULIA_EXPR_CONFUSED,
    };

    julia_ui_set_expression(expression_for_state[fsm->main_state], 75);
    julia_ui_set_state(state);
    julia_led_fsm_on_enter(fsm, state, evt);
}

static void run_fsm_demo(void)
{
    julia_fsm_t fsm;
    julia_fsm_init(&fsm);
    fsm.on_enter = fsm_ui_on_enter;
    fsm.on_enter(&fsm, fsm.sub_state, EVT_NONE);

    ESP_LOGI(TAG, "FSM demo: S1.1 -> S1.2 -> S1.1 -> S3.3 -> S4.1 -> S5.2 -> S1.1");

    const fsm_event_t demo_events[] = {
        EVT_USER_LEAVE,        // 用户离开 10 分钟，进入远场待机 S1.2
        EVT_USER_RETURN,       // 用户返回，恢复近场待机 S1.1
        EVT_USER_CALL,         // 用户呼唤，进入主动搭话 S3.3
        EVT_START_DIALOG,      // 开始对话，进入 S4.1
        EVT_USER_PERFUNCTORY,  // 用户敷衍，进入 S5.2
        EVT_SILENCE_TIMEOUT,   // 静默超时，回到 S1.1
    };

    for (size_t i = 0; i < sizeof(demo_events) / sizeof(demo_events[0]); i++) {
        julia_fsm_handle_event(&fsm, demo_events[i], NULL);
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

static void expression_demo_task(void *arg)
{
    (void)arg;
    static const expr_t expressions[] = {
        JULIA_EXPR_SLEEP,
        JULIA_EXPR_WATCHING,
        JULIA_EXPR_HAPPY,
        JULIA_EXPR_SPEAKING,
        JULIA_EXPR_CONFUSED,
    };

    size_t index = 0;
    while (1) {
        julia_ui_set_expression(expressions[index], 80);
        if (expressions[index] == JULIA_EXPR_SPEAKING) {
            julia_ui_speak("Hello, I'm Julia. How was your day?");
        }
        index = (index + 1) % (sizeof(expressions) / sizeof(expressions[0]));
        vTaskDelay(pdMS_TO_TICKS(2500));
    }
}

static void led_demo_task(void *arg)
{
    (void)arg;
    static const julia_sub_state_t states[] = {
        JULIA_SUB_STATE_S0_1_NIGHT_SLEEP,
        JULIA_SUB_STATE_S1_1_NEAR_STANDBY,
        JULIA_SUB_STATE_S2_1_OBSERVE,
        JULIA_SUB_STATE_S3_1_EMOTION_TRIGGER,
        JULIA_SUB_STATE_S4_1_LIGHT_DIALOG,
        JULIA_SUB_STATE_S5_1_USER_REJECT,
    };
    static const julia_main_state_t main_states[] = {
        JULIA_MAIN_STATE_S0_SLEEP, JULIA_MAIN_STATE_S1_STANDBY,
        JULIA_MAIN_STATE_S2_COMPANION, JULIA_MAIN_STATE_S3_INITIATIVE,
        JULIA_MAIN_STATE_S4_DIALOG, JULIA_MAIN_STATE_S5_SILENT,
    };
    julia_fsm_t demo = {0};
    size_t index = 0;
    while (1) {
        demo.main_state = main_states[index];
        demo.sub_state = states[index];
        julia_led_fsm_on_enter(&demo, demo.sub_state, EVT_NONE);
        index = (index + 1) % (sizeof(states) / sizeof(states[0]));
        vTaskDelay(pdMS_TO_TICKS(6000));
    }
}

static void audio_echo_test_task(void *arg)
{
    (void)arg;
    const size_t echo_bytes = JULIA_AUDIO_SAMPLE_RATE * sizeof(int16_t) * 3;
    uint8_t *echo_buffer = heap_caps_malloc(echo_bytes, MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM);
    if (echo_buffer == NULL) {
        echo_buffer = heap_caps_malloc(echo_bytes, MALLOC_CAP_8BIT);
    }
    if (echo_buffer == NULL) {
        ESP_LOGE(TAG, "Audio echo buffer allocation failed");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Audio echo test: recording 3 seconds (%u bytes)", (unsigned)echo_bytes);
    if (julia_audio_record_start(echo_buffer, echo_bytes) == ESP_OK) {
        ESP_ERROR_CHECK(julia_audio_record_stop());
        ESP_LOGI(TAG, "Audio echo test: playback started");
        ESP_ERROR_CHECK(julia_audio_play_start(echo_buffer, echo_bytes));
        ESP_ERROR_CHECK(julia_audio_play_stop());
        ESP_LOGI(TAG, "Audio echo test: complete");
    } else {
        ESP_LOGE(TAG, "Audio echo test: recording failed");
    }
    heap_caps_free(echo_buffer);
    vTaskDelete(NULL);
}

static void network_monitor_task(void *arg)
{
    (void)arg;
    bool cloud_voice_checked = false;
    if (CONFIG_JULIA_WIFI_SSID[0] == '\0') {
        ESP_LOGW(TAG, "WiFi test skipped: configure Julia network configuration in menuconfig");
        vTaskDeleteWithCaps(NULL);
        return;
    }

    esp_err_t err = julia_wifi_connect(CONFIG_JULIA_WIFI_SSID, CONFIG_JULIA_WIFI_PASSWORD);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Initial WiFi connection pending automatic reconnect: %s", esp_err_to_name(err));
        /* Keep STA reconnecting. Starting APSTA here used to race a late STA
         * association under low internal-RAM conditions and crashed inside
         * ieee80211_hostap_attach(). Provisioning remains an explicit action. */
    }
    while (true) {
        if (julia_wifi_is_connected()) {
            esp_ip4_addr_t ip;
            int8_t rssi = julia_wifi_get_rssi();
            if (julia_wifi_get_ip(&ip) == ESP_OK) {
            ESP_LOGI(TAG, "WiFi IP=" IPSTR " RSSI=%d dBm", IP2STR(&ip), rssi);
            if (!cloud_voice_checked && !julia_voice_is_busy()) {
                int16_t *pcm = NULL;
                size_t samples = 0;
                ESP_LOGI(TAG, "Preparing offline network prompt cache");
                esp_err_t voice_err = julia_speech_tts("网络不稳定，请稍后再试", &pcm, &samples);
                if (voice_err != ESP_OK && voice_err != ESP_ERR_TIMEOUT &&
                    voice_err != ESP_ERR_NOT_FOUND) {
                    ESP_LOGW(TAG, "Cloud TTS self-test first attempt: %s", esp_err_to_name(voice_err));
                    vTaskDelay(pdMS_TO_TICKS(2000));
                    voice_err = julia_speech_tts("网络不稳定，请稍后再试", &pcm, &samples);
                }
                if (voice_err == ESP_OK) {
                    voice_err = julia_audio_save_pcm(NETWORK_PROMPT_PATH, pcm, samples);
                }
#if CONFIG_JULIA_VOICE_PLAYBACK_SELF_TEST
                if (voice_err == ESP_OK) {
                    ESP_LOGI(TAG, "Cloud voice playback self-test: %.2f seconds",
                             (double)samples / JULIA_AUDIO_SAMPLE_RATE);
                    julia_lipsync_begin();
                    voice_err = julia_lipsync_play(pcm, samples);
                    esp_err_t stop_err = julia_lipsync_end();
                    if (voice_err == ESP_OK) voice_err = stop_err;
                    ESP_LOGI(TAG, "Cloud voice playback self-test: %s",
                             esp_err_to_name(voice_err));
                }
#endif
                free(pcm);
                pcm = NULL;
                samples = 0;
#if WAKE_REPLY_ENABLE_ONLINE_CACHE
                ESP_LOGI(TAG, "Optional online wake-reply refresh enabled for %s",
                         WAKE_WORD_DISPLAY_TEXT);
                esp_err_t wake_voice_err = ESP_OK;
                for (size_t i = 0; i < sizeof(s_online_reply_texts) /
                                           sizeof(s_online_reply_texts[0]); ++i) {
                    pcm = NULL;
                    samples = 0;
                    wake_voice_err = julia_speech_tts(s_online_reply_texts[i], &pcm, &samples);
                    if (wake_voice_err == ESP_OK)
                        wake_voice_err = julia_audio_save_pcm(s_online_reply_paths[i], pcm, samples);
                    free(pcm);
                    if (wake_voice_err != ESP_OK) break;
                }
                ESP_LOGI(TAG, "Online wake-reply refresh: %s",
                         esp_err_to_name(wake_voice_err));
#endif
                cloud_voice_checked = true;
                ESP_LOGI(TAG, "Offline network prompt cache: %s", esp_err_to_name(voice_err));
            }
            }
        } else {
            ESP_LOGW(TAG, "WiFi disconnected; reconnect manager active");
        }
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

static void ai_test_task(void *arg)
{
    (void)arg;
    if (!CONFIG_JULIA_AI_TEST_ON_BOOT) {
        vTaskDeleteWithCaps(NULL);
        return;
    }
    while (!julia_wifi_is_connected()) vTaskDelay(pdMS_TO_TICKS(500));
    /* Let the network monitor finish its one-time TTS cache download first;
     * mbedTLS uses substantial internal heap during certificate verification. */
    vTaskDelay(pdMS_TO_TICKS(30000));
    if (CONFIG_JULIA_AI_API_KEY[0] == '\0') {
        ESP_LOGW(TAG, "AI test skipped: API key is empty");
        vTaskDeleteWithCaps(NULL);
        return;
    }
    ESP_LOGI(TAG, "Qwen test: sending greeting with model %s", CONFIG_JULIA_AI_MODEL);
    ESP_ERROR_CHECK(julia_ai_send_message("你好", "你是Julia，一个温柔、简洁的中文陪伴助手。"));
    char chunk[256];
    while (julia_ai_receive_chunk(chunk, sizeof(chunk)) == ESP_OK) {
        ESP_LOGI(TAG, "AI chunk: %s", chunk);
        julia_ui_speak(chunk);
    }
    ESP_LOGI(TAG, "AI stream ended or timed out");
    vTaskDeleteWithCaps(NULL);
}

static void home_control_test_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(9000));
    const char *simulated_ai =
        "{\"action\":\"light_on\",\"device\":\"living_room\","
        "\"brightness\":75,\"color_temp\":3200}";
    ESP_LOGI(TAG, "Simulated AI home command: %s", simulated_ai);
    esp_err_t err = julia_home_handle_ai_command(simulated_ai);
    ESP_LOGI(TAG, "Simulated home command result: %s", esp_err_to_name(err));
    vTaskDelete(NULL);
}

static void power_screen_control(bool on)
{
    if (!s_boot_reveal_started) {
        lcd_backlight_set(0);
        return;
    }
    lcd_backlight_set(on ? 70 : 0);
}

static void power_test_task(void *arg)
{
    (void)arg;
    julia_fsm_t fsm;
    julia_fsm_init(&fsm);

    float voltage = julia_power_get_voltage();
    ESP_LOGI(TAG, "Battery: %.3f V, %u%%, charging=%s", voltage,
             julia_power_get_battery_percent(), julia_power_is_charging() ? "yes" : "no");

    julia_power_set_simulated_voltage(3.60f);
    ESP_LOGW(TAG, "Power test: simulated %.2f V, %u%%", julia_power_get_voltage(),
             julia_power_get_battery_percent());
    ESP_ERROR_CHECK(julia_power_update(&fsm, false));
    ESP_LOGI(TAG, "Power test FSM result: %s", julia_fsm_sub_state_name(fsm.sub_state));
    julia_power_set_simulated_voltage(-1.0f);
    vTaskDelete(NULL);
}

static void ui_state_test_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(2000));
    ESP_LOGI(TAG, "UI test: cycling all %d states", JULIA_SUB_STATE_COUNT);
    for (julia_sub_state_t state = 0; state < JULIA_SUB_STATE_COUNT; state++) {
        ESP_LOGI(TAG, "UI test: displaying %s (%d/%d)",
                 julia_fsm_sub_state_name(state), state + 1, JULIA_SUB_STATE_COUNT);
        julia_ui_set_state(state);
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
    julia_ui_set_state(JULIA_SUB_STATE_S1_1_NEAR_STANDBY);
    ESP_LOGI(TAG, "UI test complete; returned to S1.1 standby");
    vTaskDelete(NULL);
}

void app_main(void)
{
    lcd_backlight_init();
    BOOT_STAGE("backlight_init_0");
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    gpio_config_t te_gpio_conf = {
        .pin_bit_mask = 1ULL << LCD_TE_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&te_gpio_conf));

    I2C_Init();
    esp_err_t sd_err = julia_sd_init(true);
    if (sd_err != ESP_OK) ESP_LOGW(TAG, "SD unavailable: %s", esp_err_to_name(sd_err));
    ESP_ERROR_CHECK(EXIO_Init());
    lcd_reset_via_exio();
    lcd_init_panel();
    ESP_ERROR_CHECK(julia_power_init());
    julia_power_set_screen_callback(power_screen_control);
    ESP_ERROR_CHECK(lvgl_port_init(s_panel_handle));
    BOOT_STAGE("lvgl_init_done");
    ESP_ERROR_CHECK(julia_led_init());
    ESP_ERROR_CHECK(julia_audio_init());
    ESP_ERROR_CHECK(julia_audio_play_tone(660, 140, 45));
    ESP_LOGI(TAG, "Speaker self-test tone complete");
    ESP_ERROR_CHECK(julia_wifi_init());
    ESP_ERROR_CHECK(julia_system_init());
    ESP_ERROR_CHECK(julia_home_init());
    esp_err_t memory_err = julia_memory_init();
    if (memory_err != ESP_OK)
        ESP_LOGW(TAG, "Long-term memory unavailable: %s", esp_err_to_name(memory_err));
    else {
        esp_err_t routine_err = julia_routine_init();
        if (routine_err != ESP_OK)
            ESP_LOGW(TAG, "Routine baseline unavailable: %s", esp_err_to_name(routine_err));
    }
    /* Keep LVGL from starting a competing flush while the static boot portrait
     * is prepared and transferred directly to the panel. */
    ESP_LOGI(TAG, "Preparing static standby portrait");
    julia_ui_init();
    BOOT_STAGE("layers_created_hidden");
    ESP_ERROR_CHECK(julia_ui_draw_standby_direct(s_panel_handle));
    /* Direct drawing owns the panel during boot.  Hand it back to LVGL so
     * layered avatar invalidations and micro motions can reach the LCD. */
    /* State and transition animation is streamed by idle_player/transition_player.
     * The legacy clip preloader exhausts the internal DMA pool used by SDMMC. */
    ESP_ERROR_CHECK(julia_display_theme_init(lcd_backlight_set));
    BOOT_STAGE("resources_loaded");
    ESP_LOGI(TAG, "Static standby portrait transferred");
    BaseType_t network_task = xTaskCreateWithCaps(
        network_monitor_task, "network_monitor", 16384, NULL, 4, NULL,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (network_task != pdPASS) ESP_LOGE(TAG, "Failed to start network monitor");
    ESP_ERROR_CHECK(julia_voice_init());
#if CONFIG_JULIA_SOAK_TEST
    ESP_ERROR_CHECK(julia_soak_test_init());
#endif
    esp_err_t context_err = julia_context_init();
    if (context_err != ESP_OK)
        ESP_LOGE(TAG, "Context sensing unavailable; continuing safely: %s",
                 esp_err_to_name(context_err));
    system_boot_sequence();
    ESP_ERROR_CHECK(julia_ui_showcase_init());
    if (CONFIG_JULIA_AI_TEST_ON_BOOT)
        xTaskCreateWithCaps(ai_test_task, "ai_test", 8192, NULL, 3, NULL,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    ESP_LOGI(TAG, "ST77916 + LVGL initialized");

    int counter = 0;
    uint32_t motion_ticks = 0;
    uint64_t disp_last_flush = 0;
    while (1) {
        julia_system_watchdog_feed();
        update_avatar((uint32_t)(esp_timer_get_time() / 1000ULL));
        breathing_led_update((uint32_t)(esp_timer_get_time() / 1000ULL));
        julia_display_theme_update((uint32_t)(esp_timer_get_time() / 1000ULL));
        idle_player_tick();
        if (++motion_ticks >= 25) {
            motion_ticks = 0;
            uint64_t flush_count = 0;
            lvgl_port_get_flush_metrics(&flush_count, NULL, NULL);
            if (!lvgl_port_display_off()) {
                ESP_LOGI("DISP", "tick: led_update=%d, lvgl_flush=%d, state=%d",
                         breathing_led_transition_active() ? 1 : 0,
                         flush_count != disp_last_flush ? 1 : 0,
                         julia_ui_current_state());
                ESP_LOGI(TAG, "heartbeat [%d]", counter++);
            }
            disp_last_flush = flush_count;
        }
        vTaskDelay(pdMS_TO_TICKS(40));
    }
}
