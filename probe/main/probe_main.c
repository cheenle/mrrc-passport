/*
 * M1a 硬件探针：ES3C28P 板（ESP32-S3 N16R8 · 2.8" ILI9341 240×320 · FT6336G 触摸）。
 *
 * 目的只有一个：在真机上证明「屏能点亮 + 触摸有坐标」，把两件事从设计文档里的
 * "照抄上游" 变成 "本板已验证"。验证通过后本工程整体删除，常量搬进 bsp_pins.h。
 *
 * 引脚来源（三方一致，见 docs/superpowers/specs/2026-11-18-mrrc-touch-handheld-design.md）：
 *   lcdwiki ES3C28P 文档 / xiaozhi quandong/esp32-s3-spi-2.8 板级 / 我们自己的 USB-JTAG 实测
 */
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"

#include "esp_heap_caps.h"
#include "esp_lcd_ili9341.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_touch.h"
#include "esp_lcd_touch_ft5x06.h"
#include "esp_log.h"

#define LCD_W 240
#define LCD_H 320

#define PIN_LCD_CS   10
#define PIN_LCD_DC   46
#define PIN_LCD_SCK  12
#define PIN_LCD_MOSI 11
#define PIN_LCD_BL   45
#define PIN_I2C_SCL  15
#define PIN_I2C_SDA  16
#define PIN_TP_INT   17
#define PIN_TP_RST   18

static const char *TAG = "probe";

/* 厂家初始化序列（gamma / power 时序），照抄 xiaozhi quandong 板级。
 * 少这一组会出现「能亮但偏色 / 局部花屏」。 */
static const ili9341_lcd_init_cmd_t vendor_init[] = {
    {0xCF, (uint8_t[]){0x00, 0xC1, 0x30}, 3, 0},
    {0xED, (uint8_t[]){0x64, 0x03, 0x12, 0x81}, 4, 0},
    {0xE8, (uint8_t[]){0x85, 0x00, 0x78}, 3, 0},
    {0xCB, (uint8_t[]){0x39, 0x2C, 0x00, 0x34, 0x02}, 5, 0},
    {0xF7, (uint8_t[]){0x20}, 1, 0},
    {0xEA, (uint8_t[]){0x00, 0x00}, 2, 0},
    {0xC0, (uint8_t[]){0x13}, 1, 0},
    {0xC1, (uint8_t[]){0x13}, 1, 0},
    {0xC5, (uint8_t[]){0x22, 0x35}, 2, 0},
    {0xC7, (uint8_t[]){0xBD}, 1, 0},
    {0x36, (uint8_t[]){0x08}, 1, 0},          /* MADCTL: BGR, portrait */
    {0xB6, (uint8_t[]){0x0A, 0xA2}, 2, 0},
    {0x3A, (uint8_t[]){0x55}, 1, 0},          /* 16 bit / pixel */
    {0xF6, (uint8_t[]){0x01, 0x30}, 2, 0},
    {0xB1, (uint8_t[]){0x00, 0x1B}, 2, 0},
    {0xF2, (uint8_t[]){0x00}, 1, 0},
    {0x26, (uint8_t[]){0x01}, 1, 0},
    {0xE0, (uint8_t[]){0x0F, 0x35, 0x31, 0x0B, 0x0E, 0x06, 0x49, 0xA7, 0x33, 0x07, 0x0F, 0x03, 0x0C, 0x0A, 0x00}, 15, 0},
    {0xE1, (uint8_t[]){0x00, 0x0A, 0x0F, 0x04, 0x11, 0x08, 0x36, 0x58, 0x4D, 0x07, 0x10, 0x0C, 0x32, 0x34, 0x0F}, 15, 0},
};

static void backlight_set(uint32_t percent)
{
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, (1023 * percent) / 100);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

static void backlight_init(void)
{
    ledc_timer_config_t t = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&t));
    ledc_channel_config_t c = {
        .gpio_num = PIN_LCD_BL,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,                 /* dark until the panel is initialised */
        .hpoint = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&c));
}

static esp_lcd_panel_handle_t panel;

static void display_init(void)
{
    spi_bus_config_t bus = {
        .mosi_io_num = PIN_LCD_MOSI,
        .miso_io_num = -1,         /* write-only panel */
        .sclk_io_num = PIN_LCD_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_W * 64 * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = PIN_LCD_CS,
        .dc_gpio_num = PIN_LCD_DC,
        .spi_mode = 0,
        .pclk_hz = 40 * 1000 * 1000,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_cfg, &io));

    const ili9341_vendor_config_t vendor_cfg = {
        .init_cmds = &vendor_init[0],
        .init_cmds_size = sizeof(vendor_init) / sizeof(ili9341_lcd_init_cmd_t),
    };
    esp_lcd_panel_dev_config_t dev = {
        .reset_gpio_num = -1,      /* panel RST shares the ESP32 EN pin */
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = (void *)&vendor_cfg,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_ili9341(io, &dev, &panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel, false));      /* portrait */
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel, false, false));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));
    ESP_LOGI(TAG, "ili9341 up (%dx%d, SPI2, 40 MHz)", LCD_W, LCD_H);
}

static esp_lcd_touch_handle_t tp;

static void touch_init(i2c_master_bus_handle_t i2c)
{
    esp_lcd_panel_io_handle_t tp_io = NULL;
    esp_lcd_panel_io_i2c_config_t io_cfg = ESP_LCD_TOUCH_IO_I2C_FT5x06_CONFIG();
    io_cfg.scl_speed_hz = 400 * 1000;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(i2c, &io_cfg, &tp_io));

    esp_lcd_touch_config_t cfg = {
        .x_max = LCD_W,
        .y_max = LCD_H,
        .rst_gpio_num = PIN_TP_RST,
        .int_gpio_num = PIN_TP_INT,
        .levels = {.reset = 0, .interrupt = 0},
        .flags = {.swap_xy = 0, .mirror_x = 0, .mirror_y = 0},
    };
    ESP_ERROR_CHECK(esp_lcd_touch_new_i2c_ft5x06(tp_io, &cfg, &tp));
    ESP_LOGI(TAG, "ft6336g up (I2C 0x38, int=%d rst=%d)", PIN_TP_INT, PIN_TP_RST);
}

static uint16_t *fb;

static uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

static void paint_bars(void)
{
    static const uint16_t colours[8] = {
        0xF800, 0x07E0, 0x001F, 0xFFFF, 0x0000, 0xFFE0, 0x07FF, 0xF81F};
    for (int y = 0; y < LCD_H; y++) {
        uint16_t c = colours[y / (LCD_H / 8)];
        for (int x = 0; x < LCD_W; x++) {
            fb[y * LCD_W + x] = c;
        }
    }
    ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(panel, 0, 0, LCD_W, LCD_H, fb));
}

static void paint_dot(int cx, int cy)
{
    enum { R = 8 };
    uint16_t block[R * 2][R * 2];
    for (int i = 0; i < R * 2; i++) {
        for (int j = 0; j < R * 2; j++) {
            block[i][j] = rgb565(255, 255, 255);
        }
    }
    int x0 = cx - R < 0 ? 0 : cx - R;
    int y0 = cy - R < 0 ? 0 : cy - R;
    ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(panel, x0, y0, x0 + R * 2, y0 + R * 2, &block[0][0]));
}

void app_main(void)
{
    backlight_init();

    i2c_master_bus_config_t i2c_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = PIN_I2C_SDA,
        .scl_io_num = PIN_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags = {.enable_internal_pullup = 1},
    };
    i2c_master_bus_handle_t i2c = NULL;
    ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_cfg, &i2c));

    display_init();
    touch_init(i2c);

    fb = heap_caps_malloc(LCD_W * LCD_H * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    if (!fb) {
        ESP_LOGE(TAG, "no PSRAM for framebuffer");
        return;
    }
    paint_bars();
    backlight_set(80);

    int hits = 0;
    while (true) {
        esp_lcd_touch_read_data(tp);
        uint16_t x = 0, y = 0, strength = 0;
        uint8_t n = 1;
        bool pressed = esp_lcd_touch_get_coordinates(tp, &x, &y, &strength, &n, 1);
        if (pressed && n > 0) {
            ESP_LOGI(TAG, "touch (%u, %u) strength=%u", x, y, strength);
            paint_dot(x, y);
            hits++;
        }
        if (hits > 0 && (hits % 32) == 0) {
            paint_bars();          /* wipe the scribble every 32 taps */
        }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}
