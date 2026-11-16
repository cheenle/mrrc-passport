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

extern TaskHandle_t s_audio_task_h;
#include "mrrc_audio.h"
#include "mrrc_net.h"
#include "mrrc_tx.h"
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
    bool ptt_on = false;
    for (;;) {
        if (xQueueReceive(s_key_q, &e, portMAX_DELAY) != pdTRUE) continue;

        /* PTT: key only after the long-press threshold, never on a short
         * press, so a double-tap can never leak carrier. */
        if (e.ev == BSP_BTN_LONG && e.btn == BSP_BTN_OK && !ptt_on) {
            ptt_on = true;
            /* Order matters:
             *   1. free the downlink socket (room for one extra TLS session)
             *   2. open /WSaudioTX and wait until it is accepted
             *   3. only then send ptt:true, so server.py's owner claim finds
             *      our uplink socket in audio_tx_clients
             * Sending ptt first makes the radio key with no modulation. */
            mrrc_audio_suspend();
            if (!mrrc_tx_key(true)) {
                ESP_LOGE(TAG, "TX start failed");
                mrrc_net_set_ptt(false);
                mrrc_audio_resume();
                ptt_on = false;
                continue;
            }
            mrrc_net_set_ptt(true);
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        if (e.ev == BSP_BTN_RELEASE && e.btn == BSP_BTN_OK && ptt_on) {
            ptt_on = false;
            mrrc_tx_key(false);
            mrrc_net_set_ptt(false);
            mrrc_audio_resume();
            continue;
        }

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
        if (bsp_audio_set_format(CONFIG_MRRC_AUDIO_HZ, 16, 1) != ESP_OK)
            ESP_LOGE(TAG, "codec open failed at %d Hz", CONFIG_MRRC_AUDIO_HZ);
        bsp_audio_set_volume(60);
    } else {
        ESP_LOGE(TAG, "audio init failed");
    }
    mrrc_audio_prewarm();   /* reserve Opus memory before Wi-Fi fragments the heap */
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
        vTaskDelay(pdMS_TO_TICKS(60000));
        const int soc = bsp_battery_soc();
        if (soc >= 0) mrrc_ui_set_battery(soc, false);
        uint32_t af = 0, ae = 0, ad = 0;
        mrrc_audio_stats(&af, &ae, &ad);
        ESP_LOGI(TAG, "idle free=%uK largest=%uK freq=%ld s=%d glitch=%u "
                      "audio(frames=%u err=%u drop=%u)",
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                 (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024),
                 (long)g_radio.freq_hz, g_radio.s_meter,
                 (unsigned)bsp_button_glitch_count(),
                 (unsigned)af, (unsigned)ae, (unsigned)ad);
#if CONFIG_MRRC_PROFILE
        /* ── profile dump ─────────────────────────────────────────────── */
        {
            static char buf[1024];
            uint32_t afr = 0;
            int64_t dus = 0;
            mrrc_audio_profile(&afr, &dus);
            ESP_LOGI(TAG, "mem: free=%uK largest=%uK min=%uK dma_free=%uK",
                     (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                     (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024),
                     (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024),
                     (unsigned)(heap_caps_get_free_size(MALLOC_CAP_DMA) / 1024));
            if (afr)
                ESP_LOGI(TAG, "decode: %lld us/frame avg over %u frames "
                              "(%.1f%% of a 20 ms frame)",
                         (long long)(dus / afr), (unsigned)afr,
                         100.0 * (double)dus / (double)afr / 20000.0);
            vTaskGetRunTimeStats(buf);
            ESP_LOGI(TAG, "cpu:\n%s", buf);
        }
#endif
        unsigned k = 0, kf = 0, kfr = 0, ksf = 0, krf = 0, kld = 0;
        mrrc_tx_stats(&k, &kf, &kfr, &ksf, &krf, &kld);
        if (s_audio_task_h)
            ESP_LOGI(TAG, "stack: audio min free=%u B",
                     (unsigned)(uxTaskGetStackHighWaterMark(s_audio_task_h)));
        ESP_LOGI(TAG, "tx: keys=%u fails=%u frames=%u sendfail=%u readfail=%u "
                      "linkdowns=%u last=%s",
                 k, kf, kfr, ksf, krf, kld, mrrc_tx_last_event());
    }
}
