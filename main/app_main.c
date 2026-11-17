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
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

extern TaskHandle_t s_audio_task_h;
#include "mrrc_audio.h"
#include "mrrc_net.h"
#include "mrrc_tx.h"
#include "mrrc_ui.h"
#include "nvs.h"
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

/* Local speaker volume lives in NVS: it is a property of this terminal, not of
 * the radio (the radio's AF gain is a separate server-side setting). */
#define SPK_STEP 5
#define VOL_DEFAULT 60
static int s_spk_vol = VOL_DEFAULT;
static ui_mode_t s_ui_mode = UI_MODE_FREQ;

static void vol_load(void)
{
    nvs_handle_t h;
    int32_t v = VOL_DEFAULT;
    if (nvs_open("mrrc", NVS_READONLY, &h) == ESP_OK) {
        (void)nvs_get_i32(h, "spk_vol", &v);
        nvs_close(h);
    }
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    s_spk_vol = (int)v;
}

static void vol_store(void)
{
    nvs_handle_t h;
    if (nvs_open("mrrc", NVS_READWRITE, &h) != ESP_OK) return;
    (void)nvs_set_i32(h, "spk_vol", s_spk_vol);
    (void)nvs_commit(h);
    nvs_close(h);
}

/* One step of whichever knob is active.  Shared by the single click and the
 * auto-repeat so both behave identically. */
static int s_chan_idx;

static void knob_step(bsp_btn_t btn)
{
    const int dir = (btn == BSP_BTN_UP) ? +1 : -1;

    if (s_ui_mode == UI_MODE_CHANNEL) {
        if (g_chan_count <= 0) { mrrc_ui_set_toast("no channels"); return; }
        s_chan_idx = (s_chan_idx + dir + g_chan_count) % g_chan_count;
        mrrc_net_set_chan(s_chan_idx);
        mrrc_ui_set_channel(g_chans[s_chan_idx].label);
        mrrc_ui_set_toast(g_chans[s_chan_idx].label);
        return;
    }

    if (s_ui_mode == UI_MODE_VOL) {
        s_spk_vol += dir * SPK_STEP;
        if (s_spk_vol < 0) s_spk_vol = 0;
        if (s_spk_vol > 100) s_spk_vol = 100;
        bsp_audio_set_volume((uint8_t)s_spk_vol);
        mrrc_ui_set_mode(s_ui_mode, s_spk_vol);
        return;
    }

    mrrc_state_lock();
    const int32_t cur = g_radio.freq_hz;
    mrrc_state_unlock();
    if (cur <= 0) return;
    mrrc_net_set_freq(cur + dir * 1000);
}

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

/* Key the radio and open the uplink.  Returns false if the radio refused, in
 * which case everything is already unwound. */
static bool start_tx(void)
{
    mrrc_audio_suspend();
    /* One socket: nothing to open, and no ownership ordering to obey. */
    mrrc_net_set_ptt(true);
    vTaskDelay(pdMS_TO_TICKS(200));
    if (!mrrc_tx_key(true)) {
        ESP_LOGE(TAG, "TX start failed");
        mrrc_net_set_ptt(false);
        mrrc_audio_resume();
        return false;
    }
    return true;
}

static void keys_task(void *arg)
{
    (void)arg;
    key_ev_t e;
    bool ptt_on = false;
    /* Auto-repeat: the first step lands on the press itself (see below), the
     * next after 350 ms, then one every 90 ms.  The handset's 450/120 felt
     * sticky when sweeper tuning. */
    bool repeat = false;
    /* iot_button's BUTTON_DOUBLE_CLICK never fires on this board: the BSP key
     * lock reports the release 120 ms late, so the second tap lands inside the
     * first tap's hold window and the driver sees one long press.  Count two
     * clicks ourselves; a single OK click has no other meaning. */
    int64_t ok_click_us = 0;
    /* iot_button only emits SINGLE_CLICK when the measured hold exceeds
     * short_press_time (180 ms) and drops anything shorter without a trace;
     * combined with the BSP lock's +40 ms press / +120 ms release skew, a light
     * tap falls into that hole and no CLICK ever arrives.  Measure the press
     * ourselves from PRESS/RELEASE instead. */
    int64_t ok_press_us = 0;
    /* A press must be held this long before it means "transmit".  Comfortably
     * longer than any tap (measured taps: 170-220 ms, worst seen 350 ms) and
     * short enough that push-to-talk still feels immediate. */
    /* 400 ms: above every measured tap (<=221 ms), below every measured
     * push-to-talk (>=569 ms), so it has 180 ms of margin on either side. */
    /* How long the key must have been untouched before a long press counts as
     * push-to-talk rather than the tail of a double tap.  Comfortably longer
     * than the 450 ms double-tap window, short enough that the operator does
     * not notice. */
    const int64_t PTT_IDLE_US = 900000;
    int64_t last_release_us = 0;       /* when any OK press was released */

    /* Double/triple tap arbitration.
     *
     * The old scheme waited out the full 600 ms window before acting on a
     * double tap, so every knob-mode change felt stuck, then landed.  Now the
     * double tap fires the moment the second tap is released — the action the
     * operator asked for happens immediately — and a third tap inside the
     * window is treated as "and also cycle the mode", which is exactly the
     * triple-tap job.  Nothing is latched, so a late third tap just starts a
     * new sequence. */
    int tap_count = 0;
    int64_t tap_deadline = 0;          /* only used to recognise a 3rd tap */
    bsp_btn_t repeat_btn = BSP_BTN_UP;
    int64_t repeat_at = 0;

    for (;;) {
        TickType_t wait = portMAX_DELAY;
        if (repeat) {
            const int64_t left = repeat_at - esp_timer_get_time();
            wait = left <= 0 ? 0 : pdMS_TO_TICKS((left + 999) / 1000);
        } else if (tap_deadline) {
            /* Just a timer to close the window; nothing is deferred any more. */
            const int64_t left = tap_deadline - esp_timer_get_time();
            wait = left <= 0 ? 0 : pdMS_TO_TICKS((left + 999) / 1000);
        }

        if (xQueueReceive(s_key_q, &e, wait) != pdTRUE) {
            if (repeat) {
                knob_step(repeat_btn);
                repeat_at = esp_timer_get_time() + 90000;
            }
            if (tap_deadline && esp_timer_get_time() > tap_deadline) {
                tap_deadline = 0;
                tap_count = 0;
            }
            continue;
        }

        if (e.btn == BSP_BTN_OK) {
            ESP_LOGI(TAG, "OK ev=%d held=%lldus window=%lldus taps=%d",
                     (int)e.ev, ok_press_us ? (long long)(esp_timer_get_time() - ok_press_us) : -1LL,
                     tap_deadline ? (long long)(tap_deadline - esp_timer_get_time()) : 0LL,
                     tap_count);
        }

        /* ── PTT ───────────────────────────────────────────────────────
         * Measured on this board (hardware log): deliberate taps release after
         * 169-221 ms, real push-to-talk after 569-580 ms, and nothing lands in
         * between.  BSP_BTN_LONG fires at 350 ms into *any* press, which sits in
         * exactly that empty gap, so it cannot tell the two intents apart: a
         * fast double tap whose first press was reported late produces a LONG
         * and keyed the radio.  The event is therefore ignored and PTT is keyed
         * from our own timer, at a point with margin on both sides.
         */
        if (e.btn == BSP_BTN_OK && e.ev == BSP_BTN_LONG) {
            /* Only a press that starts from a quiet key can transmit.
             *
             * Measured on this board: taps release after 169-246 ms, a real
             * push-to-talk after 569-580 ms, and a *fast double tap* is
             * collapsed by the ADC driver into a single press that is reported
             * as a long press releasing at 619-627 ms.  That last case is the
             * dangerous one — it is indistinguishable from PTT by duration
             * alone, and it is what kept keying the radio on quick double taps.
             *
             * The discriminator that does work: a merged double tap is always
             * preceded by a recent tap release, a genuine PTT is not.  So the
             * key must have been idle for PTT_IDLE_US before the press.  This
             * costs no latency on a real transmission. */
            if (last_release_us && esp_timer_get_time() - last_release_us < PTT_IDLE_US) {
                continue;
            }
            ptt_on = start_tx();
            continue;
        }
        if (e.ev == BSP_BTN_RELEASE && e.btn == BSP_BTN_OK && ptt_on) {
            ptt_on = false;
            last_release_us = esp_timer_get_time();
            mrrc_tx_key(false);
            mrrc_net_set_ptt(false);
            mrrc_audio_resume();
            continue;
        }

        /* ── knob ──────────────────────────────────────────────────────
         * Driven by PRESS, not CLICK.  The button component only emits CLICK
         * when the measured hold exceeds short_press_time (180 ms) and silently
         * drops anything shorter, and this board's ADC decoder plus the BSP key
         * lock make short taps fall into that hole — that is why UP/DOWN
         * sometimes did nothing at all.  PRESS is emitted on every press with no
         * such precondition and no latency, so the step happens immediately and
         * auto-repeat is timed from there.
         */
        if (e.btn == BSP_BTN_UP || e.btn == BSP_BTN_DOWN) {
            if (e.ev == BSP_BTN_PRESS) {
                knob_step(e.btn);
                if (s_ui_mode == UI_MODE_VOL) vol_store();
                repeat = true;
                repeat_btn = e.btn;
                repeat_at = esp_timer_get_time() + 350000;   /* then repeat */
            } else if (e.ev == BSP_BTN_RELEASE) {
                if (repeat) {
                    repeat = false;
                    if (s_ui_mode == UI_MODE_VOL) vol_store();
                }
            }
            continue;
        }

        /* ── OK press/release: short tap counts, long press was PTT ────── */
        if (e.btn == BSP_BTN_OK && e.ev == BSP_BTN_PRESS) {
            ok_press_us = esp_timer_get_time();
            continue;
        }
        if (e.btn == BSP_BTN_OK && e.ev == BSP_BTN_RELEASE && !ptt_on) {
            const int64_t now = esp_timer_get_time();
            last_release_us = now;
            const int64_t held = ok_press_us ? now - ok_press_us : 0;
            ok_press_us = 0;
            if (held > 0 && held < 500000) {          /* a tap, not a keying */
                tap_count = (now < tap_deadline) ? tap_count + 1 : 1;
                tap_deadline = now + 450000;          /* window for a further tap */
                ok_click_us = now;

                if (tap_count == 2) {
                    /* Act now — no waiting for the window to close. */
                    s_ui_mode = (ui_mode_t)((s_ui_mode + 1) % 3);
                    mrrc_ui_set_mode(s_ui_mode, s_spk_vol);
                    const char *nm = s_ui_mode == UI_MODE_CHANNEL ? "CH"
                                   : (s_ui_mode == UI_MODE_VOL ? "VOL" : "FREQ");
                    mrrc_ui_set_toast(nm);
                    ESP_LOGI(TAG, "mode -> %s", nm);
                } else if (tap_count >= 3) {          /* third tap: cycle mode too */
                    tap_count = 0;
                    tap_deadline = 0;
                    mrrc_net_next_mode();
                    mrrc_ui_set_toast("MODE");
                }
            }
            continue;
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

    vol_load();
    /* Create the audio task and its jitter-buffer mutex BEFORE the socket can
     * deliver anything: /WSterminal starts pushing audio as soon as it
     * connects, and feeding into an uncreated mutex asserts. */
    mrrc_audio_start();
    mrrc_ui_start();
    mrrc_ui_set_mode(s_ui_mode, s_spk_vol);
    bsp_audio_set_volume((uint8_t)s_spk_vol);
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
