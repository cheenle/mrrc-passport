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

#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"

#include "mrrc_link.h"
#include "secrets.h"

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

#define PIN_I2S_MCLK 4
#define PIN_I2S_BCLK 5
#define PIN_I2S_WS   7
#define PIN_I2S_DOUT 8
#define PIN_I2S_DIN  6
#define PIN_PA_EN    1            /* FM8002E, active low */

#define AUDIO_RATE   16000

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

/* ── audio (M1b) ─────────────────────────────────────────────────────────── */
static esp_codec_dev_handle_t s_codec;

/* Task stacks in PSRAM: with four TLS sockets the internal heap bottoms out
 * around 4 KB, and any xTaskCreate() after that fails. */
static TaskHandle_t task_with_psram_stack(TaskFunction_t fn, const char *name, uint32_t stack,
                                         UBaseType_t prio, BaseType_t core)
{
    StaticTask_t *tcb = heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    StackType_t *stk = heap_caps_malloc(stack, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!tcb || !stk) {
        ESP_LOGE("probe", "no memory for task %s", name);
        return NULL;
    }
    return xTaskCreateStaticPinnedToCore(fn, name, stack / sizeof(StackType_t), NULL, prio, stk, tcb, core);
}

static volatile bool s_ptt;
static volatile uint32_t s_tx_frames;
static i2s_chan_handle_t s_tx, s_rx;

static void audio_init(i2c_master_bus_handle_t i2c)
{
    /* PA enable is driven by the codec's gpio_if (pa_pin + pa_reverted), but we
     * also park it low up-front so the amp never pops on boot. */
    gpio_config_t pa = {
        .pin_bit_mask = 1ULL << PIN_PA_EN,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&pa));
    gpio_set_level(PIN_PA_EN, 0);

    i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&chan, &s_tx, &s_rx));

    i2s_std_config_t std = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = PIN_I2S_MCLK,
            .bclk = PIN_I2S_BCLK,
            .ws = PIN_I2S_WS,
            .dout = PIN_I2S_DOUT,
            .din = PIN_I2S_DIN,
            .invert_flags = {.mclk_inv = false, .bclk_inv = false, .ws_inv = false},
        },
    };
    std.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_tx, &std));
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_rx, &std));
    /* Do NOT enable the channels here: esp_codec_dev's I2S data_if reconfigures
     * the slot and enables the channels inside esp_codec_dev_open(). Enabling
     * first makes that reconfiguration fail with ESP_ERR_INVALID_ARG. */

    audio_codec_i2c_cfg_t i2c_cfg = {
        .addr = ES8311_CODEC_DEFAULT_ADDR,
        .bus_handle = i2c,
    };
    const audio_codec_ctrl_if_t *ctrl = audio_codec_new_i2c_ctrl(&i2c_cfg);
    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = I2S_NUM_0,
        .rx_handle = s_rx,
        .tx_handle = s_tx,
    };
    const audio_codec_data_if_t *data = audio_codec_new_i2s_data(&i2s_cfg);
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();

    es8311_codec_cfg_t es = {
        .ctrl_if = ctrl,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH,
        .pa_pin = PIN_PA_EN,
        .pa_reverted = true,            /* active low */
        .master_mode = false,           /* ESP32 is the I2S master */
        .use_mclk = true,
        .hw_gain = {.pa_voltage = 5.0, .codec_dac_voltage = 3.3},
    };
    const audio_codec_if_t *codec = es8311_codec_new(&es);
    if (!codec) {
        ESP_LOGE(TAG, "es8311_codec_new failed");
        return;
    }
    esp_codec_dev_cfg_t dev = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN_OUT,   /* without this esp_codec_dev_new() returns NULL */
        .codec_if = codec,
        .data_if = data,
    };
    s_codec = esp_codec_dev_new(&dev);
    if (!s_codec) {
        ESP_LOGE(TAG, "esp_codec_dev_new failed");
        return;
    }
    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16,
        .channel = 1,
        .sample_rate = AUDIO_RATE,
        .mclk_multiple = 0,
    };
    esp_err_t err = esp_codec_dev_open(s_codec, &fs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "codec open failed: %s", esp_err_to_name(err));
        return;
    }
    esp_codec_dev_set_out_vol(s_codec, 60);
    esp_codec_dev_set_in_gain(s_codec, 30.0);
    ESP_LOGI(TAG, "es8311 up (%d Hz mono, pa=%d active-low)", AUDIO_RATE, PIN_PA_EN);
}

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

/* Single audio task: reads the mic, keys the uplink when PTT is on, and plays
 * whatever the socket queued. PCM16 both ways — no codec on the wire, so
 * nothing here touches libopus and the whole thread-safety trap disappears. */
static void audio_task(void *arg)
{
    (void)arg;
    static int16_t mic[320];          /* 20 ms @ 16 kHz */
    static int16_t pcm[2880];
    static uint8_t frame[2048];
    bool tx_prev = false;

    ESP_LOGI(TAG, "audio task up (PCM16 16 kHz both ways, 20 ms frames)");

    while (true) {
        const bool tx = s_ptt && mrrc_link_up();

        if (tx && !tx_prev) {
            tx_prev = true;
            /* Half duplex: feeding our own uplink back through the speaker is a
             * howl, not audio. Mute the amp for the whole transmission. */
            esp_codec_dev_set_out_mute(s_codec, true);
            /* Server keys the radio then reopens its TX stream: let that land
             * before we push samples, and drop whatever was captured during it. */
            vTaskDelay(pdMS_TO_TICKS(200));
            esp_codec_dev_read(s_codec, mic, sizeof mic);
            while (mrrc_link_take_audio(frame, sizeof frame) > 0) { }
        } else if (!tx && tx_prev) {
            tx_prev = false;
            /* Drop the downlink that piled up while we were keyed, then unmute. */
            while (mrrc_link_take_audio(frame, sizeof frame) > 0) { }
            esp_codec_dev_set_out_mute(s_codec, false);
        }

        if (esp_codec_dev_read(s_codec, mic, sizeof mic) != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        if (tx) {
            if (mrrc_link_send_pcm(mic, 320)) s_tx_frames++;
            /* Keep the queue drained so the backlog cannot overflow (drop=25
             * earlier) and so release does not replay stale audio. */
            while (mrrc_link_take_audio(frame, sizeof frame) > 0) { }
        } else {
            int len;
            while ((len = mrrc_link_take_audio(frame, sizeof frame)) > 0) {
                const int16_t *in = (const int16_t *)(frame + 2);
                if (frame[1] == 0x02) {                   /* pcm16 @ 16 kHz */
                    int samples = (len - 2) / 2;
                    if (samples > 0) esp_codec_dev_write(s_codec, (void *)in, samples * 2);
                } else if (frame[1] == 0x00) {            /* pcm48 -> 16 kHz */
                    int samples = ((len - 2) / 2) / 3;
                    for (int i = 0; i < samples; i++) pcm[i] = in[i * 3];
                    if (samples > 0) esp_codec_dev_write(s_codec, (void *)pcm, samples * 2);
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

/* Keep the server's TX liveness gate fed while keyed. Independent task on
 * purpose: the encode loop saturates core 0 and would starve anything parked
 * on the main loop (handheld measured "TX dies at 5.6 s" from exactly that). */
static void tx_hb_task(void *arg)
{
    (void)arg;
    while (true) {
        if (s_ptt) mrrc_link_send_ptt_heartbeat();
        vTaskDelay(pdMS_TO_TICKS(1500));
    }
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

    audio_init(i2c);

    static const mrrc_cfg_t cfg = {
        .ssid = PROBE_WIFI_SSID,
        .wifi_pass = PROBE_WIFI_PASS,
        .host = PROBE_SERVER_HOST,
        .port = PROBE_SERVER_PORT,
        .server_pass = PROBE_SERVER_PASS,
    };
    /* BOOT (GPIO0) is the only physical key: long-press >= 600 ms keys the radio. */
    gpio_config_t btn = {
        .pin_bit_mask = 1ULL << 0,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&btn);

    mrrc_link_start(&cfg);
    /* Stacks stay in internal RAM: PSRAM stacks crashed at boot on this board
     * (i2s_channel_disable spam then a reset loop at ~945 ms). */
    xTaskCreatePinnedToCore(audio_task, "audio", 10240, NULL, 7, NULL, 0);
    xTaskCreatePinnedToCore(tx_hb_task, "txhb", 2560, NULL, 10, NULL, 1);

    int hits = 0;
    int tick = 0;
    int held_ms = 0;
    while (true) {
        /* BOOT key: 600 ms to key, release to unkey. A short tap never keys. */
        const int lvl = gpio_get_level(0);
        if (lvl == 0) {
            held_ms += 50;
            if (held_ms >= 600 && !s_ptt) {
                s_ptt = true;
                mrrc_link_set_ptt(true);
            }
        } else {
            if (s_ptt) {
                s_ptt = false;
                mrrc_link_set_ptt(false);
            }
            held_ms = 0;
        }

        if ((tick++ % 20) == 0) {
            const mrrc_state_t *st = mrrc_state();
            ESP_LOGI(TAG, "link: up=%d %ldHz %s %s state=%lu spec=%lu audio=%lu drop=%lu ptt=%d tx=%lu",
                     (int)st->up, (long)st->freq_hz, st->mode, st->band,
                     (unsigned long)st->state_updates, (unsigned long)st->spectrum_frames,
                     (unsigned long)st->audio_frames, (unsigned long)st->audio_drops,
                     (int)s_ptt, (unsigned long)s_tx_frames);
        }
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
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
