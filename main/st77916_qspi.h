#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "esp_lcd_panel_io.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ST77916_QSPI_LCD_WIDTH 360
#define ST77916_QSPI_LCD_HEIGHT 360

typedef struct {
    int dc_gpio_num;
    int cs_gpio_num;
    int sck_gpio_num;
    int data0_gpio_num;
    int data1_gpio_num;
    int data2_gpio_num;
    int data3_gpio_num;
    int spi_host;
    uint32_t pclk_hz;
    uint16_t width;
    uint16_t height;
    int i2c_port;
    int i2c_scl_gpio_num;
    int i2c_sda_gpio_num;
    uint8_t exio_i2c_addr;
    uint8_t lcd_rst_exio_bit;
} st77916_qspi_config_t;

esp_err_t st77916_qspi_new(const st77916_qspi_config_t *config, esp_lcd_panel_io_handle_t *ret_io);
esp_err_t st77916_qspi_reset(const st77916_qspi_config_t *config);
esp_err_t st77916_qspi_init(esp_lcd_panel_io_handle_t io);
esp_err_t st77916_qspi_fill_color(esp_lcd_panel_io_handle_t io, uint16_t width, uint16_t height, uint16_t color);

#ifdef __cplusplus
}
#endif
