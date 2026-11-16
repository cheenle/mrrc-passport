// main/app_main.c — MRRC terminal on FoloToy AI Passport (ESP32-C3).
//
// Stage S1: control channel.  Wi-Fi -> login -> /WSradio -> the screen shows
// live frequency / mode / S-meter.  Audio (S2) and PTT (S3) are deliberately
// absent; see docs/design-v1.md.
//
// Key facts this file relies on (measured, see the design doc):
//   * no PSRAM: everything comes out of 400 KB internal SRAM, but Wi-Fi still
//     leaves ~154 KB free / 112 KB largest block, so LVGL stays;
//   * the panel is round: the BSP masks the corners during flush;
//   * codec is opened once at 48 kHz and never reconfigured at runtime --
//     capture and playback share MCLK/BCLK/WS, and reopening costs a click.
#include <stdio.h>
#include <string.h>

#include "bsp_audio.h"
#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_i2c.h"
#include "bsp_pins.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "mrrc_net.h"
#include "mrrc_ui.h"
#include "nvs_flash.h"

static const char *TAG = "mrrc";

#define KEY_QUEUE_DEPTH 8
#define KEYS_TASK_STACK 3072
#define KEYS_TASK_PRIO  6

typedef struct {
    bsp_btn_t btn;
    bsp_btn_ev_t ev;
} key_ev_t;

static QueueHandle_t s_key_q;

static void mem_report(const char *stage)
{
    ESP_LOGW(TAG, "HEAP %-14s free=%5uK largest=%5uK",
             stage,
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024));
}

/* Runs on the shared esp_timer task: enqueue and return.  Nothing else. */
static void on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user)
{
    (void)user;
    const key_ev_t e = { btn, ev };
    if (s_key_q) (void)xQueueSend(s_key_q, &e, 0);
}

static void keys_task(void *arg)
{
    (void)arg;
    key_ev_t e;
    for (;;) {
        if (xQueueReceive(s_key_q, &e, portMAX_DELAY) != pdTRUE) continue;

        /* S1: only tuning.  PTT / volume switching arrive in S3. */
        if (e.ev == BSP_BTN_CLICK && (e.btn == BSP_BTN_UP || e.btn == BSP_BTN_DOWN)) {
            mrrc_state_lock();
            const int32_t cur = g_radio.freq_hz;
            mrrc_state_unlock();
            if (cur <= 0) continue;
            const int32_t next = cur + (e.btn == BSP_BTN_UP ? 1000 : -1000);
            mrrc_net_set_freq(next);
            ESP_LOGI(TAG, "tune -> %ld", (long)next);
        } else if (e.ev == BSP_BTN_LONG && e.btn == BSP_BTN_OK) {
            mrrc_ui_set_toast("PTT: not in S1 yet");
        }
    }
}

void app_main(void)
{
    esp_err_t nvs = nvs_flash_init();
    if (nvs == ESP_ERR_NVS_NO_FREE_PAGES || nvs == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs = nvs_flash_init();
    }
    ESP_LOGI(TAG, "reset reason %d", (int)esp_reset_reason());
    mem_report("boot");

    ESP_ERROR_CHECK(bsp_i2c_init());
    bsp_i2c_scan();

    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "display/LVGL init failed (MOSI=%d SCLK=%d CS=%d DC=%d BL=%d)",
                 BSP_LCD_MOSI, BSP_LCD_SCLK, BSP_LCD_CS, BSP_LCD_DC, BSP_LCD_BL);
        return;
    }
    bsp_display_backlight(70);
    mem_report("display");

    if (bsp_audio_init() == ESP_OK) {
        /* One rate for both directions, forever.  See docs/design-v1.md 3.1. */
        if (bsp_audio_set_format(CONFIG_MRRC_PROBE_AUDIO_HZ, 16, 1) != ESP_OK)
            ESP_LOGE(TAG, "codec open failed at %d Hz", CONFIG_MRRC_PROBE_AUDIO_HZ);
        bsp_audio_set_volume(60);
    } else {
        ESP_LOGE(TAG, "audio init failed");
    }
    mem_report("audio");

    if (bsp_battery_init() == ESP_OK)
        ESP_LOGI(TAG, "battery %d mV %d%%", bsp_battery_mv(), bsp_battery_soc());
    else
        ESP_LOGW(TAG, "battery init failed (CW2017)");

    s_key_q = xQueueCreate(KEY_QUEUE_DEPTH, sizeof(key_ev_t));
    if (!s_key_q || xTaskCreate(keys_task, "keys", KEYS_TASK_STACK, NULL,
                                KEYS_TASK_PRIO, NULL) != pdPASS) {
        ESP_LOGE(TAG, "key dispatch failed");
    } else if (bsp_button_init(on_key, NULL) != ESP_OK) {
        ESP_LOGE(TAG, "button init failed (ADC ladder on GPIO%d)", BSP_BTN_GPIO);
    }
    mem_report("buttons");

    mrrc_ui_start();
    mrrc_net_start();
    mem_report("net started");

    /* Battery is polled slowly; the CW2017 sits on the shared I2C bus with the
     * codec, so there is no reason to hammer it. */
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(30000));
        const int soc = bsp_battery_soc();
        if (soc >= 0) mrrc_ui_set_battery(soc, false);
        ESP_LOGI(TAG, "idle free=%uK largest=%uK freq=%ld s=%d glitch=%u",
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                 (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024),
                 (long)g_radio.freq_hz, g_radio.s_meter,
                 (unsigned)bsp_button_glitch_count());
    }
}
