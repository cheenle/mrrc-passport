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
/* Screen backlight, 0..100 %.  The panel's BL pin is on a LEDC channel
 * (bsp_display.c: backlight_init), so this is real PWM, not a fake dim.  Kept in
 * NVS like volume: a display that has to be re-dimmed after every battery change
 * is a display nobody dims. */
static int s_bright = 70;
#define BRIGHT_MIN 5
#define BRIGHT_STEP 5
static ui_mode_t s_ui_mode = UI_MODE_FREQ;

/* Knob settings that outlive a reboot.  STEP is what FREQ mode adds per click;
 * FILTER is the IF width asked of the server. */
#define STEP_COUNT   3
#define FILTER_COUNT 4
static const int STEP_HZ[STEP_COUNT]     = { 1000, 5000, 25000 };
/* What the plate shows.  The radio's ladder is an index, not a frequency:
 *   voice  (SSB/AM/FM) FILTER_WIDTHS_VOICE  -> 1800/2400/2700/3000 Hz at 9/13/17/20
 *   narrow (CW/RTTY/DATA/PSK) FILTER_WIDTHS_NARROW -> 1700/2400/3000/4000 at 15/17/18/21
 * Sending Hz itself does nothing: `filter_hz` is derived server-side. */
static const int FILTER_HZ[FILTER_COUNT]       = { 1800, 2400, 2700, 3000 };
static const int FILTER_IDX_VOICE[FILTER_COUNT]  = { 9, 13, 17, 20 };
static const int FILTER_IDX_NARROW[FILTER_COUNT] = { 15, 17, 18, 21 };
static bool mode_is_narrow(void)
{
    const char *m = g_radio.mode_name;
    return !strcmp(m, "CW-U") || !strcmp(m, "CW-L") || !strcmp(m, "RTTY-L") ||
           !strcmp(m, "RTTY-U") || !strcmp(m, "DATA-L") || !strcmp(m, "DATA-U") ||
           !strcmp(m, "PSK");
}
static int filter_index_for(int slot)
{
    return (mode_is_narrow() ? FILTER_IDX_NARROW : FILTER_IDX_VOICE)[slot];
}
static int s_step_hz   = 1000;
static int s_step_idx  = 0;
static int s_filter_hz = 2400;
static int s_filter_idx = 1;
static bool s_filter_sent;

/* Two knob families with their own last-used position.
 *
 *   local  (OK double-tap):   VOL  <-> MIC          — this device's own audio
 *   radio  (UP double-tap):   FREQ -> CH -> STEP -> FIL — things the radio owns
 *
 * Splitting them means switching the speaker volume never costs a trip through
 * four radio modes, and the radio group keeps its place across local changes. */
/* The two families are still useful as documentation of what belongs where,
 * even though one OK double-tap now walks all six positions in order. */
static int s_local_idx;   /* kept: remembers the last local position */

static const char *mode_name(ui_mode_t m)
{
    switch (m) {
        case UI_MODE_CHANNEL: return "CH";
        case UI_MODE_VOL:     return "VOL";
        case UI_MODE_MIC:     return "MIC";
        case UI_MODE_STEP:    return "STEP";
        case UI_MODE_FILTER:  return "FIL";
        case UI_MODE_BRIGHT:  return "BL";
        default:              return "FREQ";
    }
}

/* Value the UI should show for a mode: percent, dB, Hz. */
static int mode_value(ui_mode_t m)
{
    switch (m) {
        case UI_MODE_MIC:    return bsp_audio_get_mic_gain_db();
        case UI_MODE_STEP:   return s_step_hz;
        case UI_MODE_FILTER: return s_filter_hz;
        case UI_MODE_BRIGHT: return s_bright;
        default:             return s_spk_vol;
    }
}

static void enter_mode(ui_mode_t m)
{
    s_ui_mode = m;
    mrrc_ui_set_mode(m, mode_value(m));
    mrrc_ui_set_toast(mode_name(m));
    ESP_LOGI(TAG, "mode -> %s", mode_name(m));
}

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

    int32_t br = 70;
    if (nvs_open("mrrc", NVS_READONLY, &h) == ESP_OK) {
        (void)nvs_get_i32(h, "bright", &br);
        nvs_close(h);
    }
    if (br < BRIGHT_MIN) br = BRIGHT_MIN;
    if (br > 100) br = 100;
    s_bright = (int)br;
    bsp_display_backlight((uint8_t)s_bright);

    /* Microphone PGA survives a reboot too: an operator who raised it to be
     * heard on a weak signal should not have to rediscover the setting. */
    int32_t g = bsp_audio_get_mic_gain_db();
    if (nvs_open("mrrc", NVS_READONLY, &h) == ESP_OK) {
        (void)nvs_get_i32(h, "mic_gain", &g);
        nvs_close(h);
    }
    bsp_audio_set_mic_gain_db((int)g);

    /* Step and filter width likewise survive: an operator who set 25 kHz for
     * band sweeping does not want 1 kHz again after a battery change. */
    int32_t st = s_step_hz;
    int32_t fi = s_filter_hz;
    if (nvs_open("mrrc", NVS_READONLY, &h) == ESP_OK) {
        (void)nvs_get_i32(h, "step_hz", &st);
        (void)nvs_get_i32(h, "filter_hz", &fi);
        nvs_close(h);
    }
    for (int i = 0; i < STEP_COUNT; i++)   if (STEP_HZ[i] == st)   { s_step_idx = i;   s_step_hz = STEP_HZ[i]; }
    for (int i = 0; i < FILTER_COUNT; i++) if (FILTER_HZ[i] == fi) { s_filter_idx = i; s_filter_hz = FILTER_HZ[i]; }
    ESP_LOGI(TAG, "knob: vol=%d%% mic=%d dB step=%d Hz filter=%d Hz bright=%d%%",
             s_spk_vol, bsp_audio_get_mic_gain_db(), s_step_hz, s_filter_hz, s_bright);
}

static void settings_store(void)
{
    nvs_handle_t h;
    if (nvs_open("mrrc", NVS_READWRITE, &h) != ESP_OK) return;
    (void)nvs_set_i32(h, "spk_vol", s_spk_vol);
    (void)nvs_set_i32(h, "bright", s_bright);
    (void)nvs_set_i32(h, "mic_gain", bsp_audio_get_mic_gain_db());
    (void)nvs_set_i32(h, "step_hz", s_step_hz);
    (void)nvs_set_i32(h, "filter_hz", s_filter_hz);
    (void)nvs_commit(h);
    nvs_close(h);
}

/* Older callers used the volume-only name. */
static void vol_store(void) { settings_store(); }

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

    if (s_ui_mode == UI_MODE_MIC) {
        /* The ES8311 input PGA only has 0/6/12/18/24/30 dB steps, so the knob
         * moves one step at a time.  Asking for +1 dB used to land on the same
         * register value and nothing audibly changed. */
        const int g = bsp_audio_set_mic_gain_db(bsp_audio_get_mic_gain_db() + dir * 6);
        mrrc_ui_set_mode(s_ui_mode, g);
        ESP_LOGI(TAG, "mic gain -> %d dB", g);
        return;
    }

    if (s_ui_mode == UI_MODE_STEP) {
        s_step_idx = (s_step_idx + dir + STEP_COUNT) % STEP_COUNT;
        s_step_hz = STEP_HZ[s_step_idx];
        mrrc_ui_set_mode(s_ui_mode, s_step_hz);
        ESP_LOGI(TAG, "step -> %d Hz", s_step_hz);
        settings_store();
        return;
    }

    if (s_ui_mode == UI_MODE_FILTER) {
        s_filter_idx = (s_filter_idx + dir + FILTER_COUNT) % FILTER_COUNT;
        s_filter_hz = FILTER_HZ[s_filter_idx];
        mrrc_ui_set_mode(s_ui_mode, s_filter_hz);
        const int fi = filter_index_for(s_filter_idx);
        mrrc_net_set_filter_width(fi);
        ESP_LOGI(TAG, "filter -> %d Hz (index %d%s)", s_filter_hz, fi,
                 mode_is_narrow() ? ", narrow" : "");
        settings_store();
        return;
    }

    if (s_ui_mode == UI_MODE_BRIGHT) {
        s_bright += dir * BRIGHT_STEP;
        if (s_bright < BRIGHT_MIN) s_bright = BRIGHT_MIN;
        if (s_bright > 100) s_bright = 100;
        bsp_display_backlight((uint8_t)s_bright);
        mrrc_ui_set_mode(s_ui_mode, s_bright);
        settings_store();            /* one NVS commit per notch, as for volume */
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
    mrrc_net_set_freq(cur + dir * s_step_hz);
}

/* Heap by capability, not just a single total.
 *
 * "free" and "largest" hide what matters here: this part has no PSRAM, DMA
 * descriptors and the LVGL draw buffer must live in DMA-capable internal RAM,
 * and the TLS handshake needs ~16 KB *contiguous*.  Printing the split per
 * stage shows which subsystem ate the block when a number drops. */
static void mem_report(const char *stage)
{
    ESP_LOGW(TAG, "HEAP %-14s free=%uK largest=%uK | dma=%uK/%uK | min=%uK",
             stage,
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_DMA) / 1024),
             (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_DMA) / 1024),
             (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024));
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

#if CONFIG_MRRC_PTT_SELFTEST
#define PTT_SELFTEST_STACK 3072
#define PTT_SELFTEST_KEYED_MS 5000

/* Key the transmitter once, without a human, for measurement runs.
 *
 * This exists because the Kconfig option promised it and no code implemented it
 * (found 2026-11-23 while trying to measure NFR-004: the switch was on the menu
 * and did nothing).  It runs the same sequence as the OK long-press —
 * start_tx() -> wait -> unkey -> resume — so the numbers it produces belong to
 * the real path, not to a harness.
 *
 * Default n, and it keys on boot: never enable it in a shipped build. */
static void ptt_selftest_task(void *arg)
{
    (void)arg;
    for (int i = 0; i < 120 && !mrrc_net_up(); i++) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    if (!mrrc_net_up()) {
        ESP_LOGW(TAG, "selftest: no control link after 120 s, not keying");
        vTaskDelete(NULL);
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(3000));          /* let the first frames settle */
    if (!start_tx()) {
        ESP_LOGE(TAG, "selftest: key refused");
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGW(TAG, "selftest: keyed %d ms", PTT_SELFTEST_KEYED_MS);
    vTaskDelay(pdMS_TO_TICKS(PTT_SELFTEST_KEYED_MS));
    mrrc_tx_key(false);
    mrrc_net_set_ptt(false);
    mrrc_audio_resume();
    ESP_LOGW(TAG, "selftest: unkeyed");
    vTaskDelete(NULL);
}
#endif

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
                const int64_t now_us = esp_timer_get_time();
                knob_step(e.btn);
                repeat = true;
                repeat_btn = e.btn;
                repeat_at = now_us + 350000;   /* then repeat */
            } else if (e.ev == BSP_BTN_RELEASE) {
                if (repeat) {
                    repeat = false;
                    settings_store();
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
                    /* OK double-tap walks the LOCAL family only: the speaker and
                     * the microphone are this device's business, and an operator
                     * changing volume should not have to pass through four
                     * radio-side modes to get back.  Radio modes live on the
                     * UP double-tap (see below). */
                    /* One cycle over everything the knob can point at.  The
                     * radio group used to live on the arrow keys, but a
                     * double-tap there fights the tuning taps — the operator
                     * sweeps with the same key.  Keep mode changes on OK and
                     * leave the arrows for tuning only. */
                    const int all = (int)UI_MODE_COUNT;
                    int next = ((int)s_ui_mode + 1) % all;
                    enter_mode((ui_mode_t)next);
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

#if CONFIG_MRRC_PTT_SELFTEST
    if (xTaskCreate(ptt_selftest_task, "pttsel", PTT_SELFTEST_STACK, NULL, 4, NULL) != pdPASS)
        ESP_LOGE(TAG, "selftest task create failed");
#endif

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
        if (!s_filter_sent && mrrc_net_up()) {
            /* The server keeps the filter; push ours once the link is up so the
             * knob and the radio agree from the first second. */
            s_filter_sent = true;
            mrrc_net_set_filter_width(filter_index_for(s_filter_idx));
        }
        uint32_t dl = 0, df = 0, wf = 0, un = 0; uint64_t wsk = 0;
        uint32_t ins_n = 0, trim_n = 0;
        mrrc_audio_governor(&ins_n, &trim_n);
        size_t ru = 0, rumax = 0; bool mu = false, txf = false;
        mrrc_audio_state(&un, &ru, &rumax, &mu, &txf);
        mrrc_audio_drop_split(&dl, &df);
        mrrc_audio_written(&wf, &wsk);
        static uint32_t p_wf = 0; static uint64_t p_wsk = 0;
        ESP_LOGI(TAG, "idle free=%uK largest=%uK freq=%ld s=%d glitch=%u "
                      "audio(frames=%u err=%u drop=%u lock=%u full=%u) "
                      "i2s(%.1f f/s) ring(%u/%u max %u) un=%u flush=%u mute=%d tx=%d "
                      "gov(ins=%u trim=%u) batt=%d%% %dmV",
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                 (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024),
                 (long)g_radio.freq_hz, g_radio.s_meter,
                 (unsigned)bsp_button_glitch_count(),
                 (unsigned)af, (unsigned)ae, (unsigned)ad,
                 (unsigned)dl, (unsigned)df,
                 (double)(wf - p_wf) / 60.0,
                 (unsigned)ru, (unsigned)AUD_RB_BYTES_CONST, (unsigned)rumax,
                 (unsigned)un, (unsigned)mrrc_audio_stall_flushes(), (int)mu, (int)txf,
                 (unsigned)ins_n, (unsigned)trim_n,
                 bsp_battery_soc(), bsp_battery_mv());
        p_wf = wf; p_wsk = wsk;
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
            uint32_t lrx = 0, lun = 0, lwr = 0;
            size_t lused = 0;
            uint32_t wf_now = 0; uint64_t wsk_now = 0;
            mrrc_audio_written(&wf_now, &wsk_now);
            mrrc_audio_link_stats(&lrx, &lun, &lwr, &lused);
            /* Deltas, not totals: the interesting value is this minute's rate. */
            static uint32_t p_rx, p_un, p_wrx; static uint64_t p_wsk2; static int64_t p_us;
            const int64_t now_us = esp_timer_get_time();
            const double dt = p_us ? (double)(now_us - p_us) / 1e6 : 0.0;
            if (dt > 1.0) {
                ESP_LOGI(TAG, "link: rx=%.1f KB/s underruns=%u/s wr_max=%u us "
                              "rb_used_max=%u/%u rebuf=%u rssi=%d sps=%.0f",
                         (double)(lrx - p_rx) / dt / 1024.0,
                         (unsigned)((double)(lun - p_un) / dt),
                         (unsigned)lwr, (unsigned)lused, (unsigned)16384,
                         (unsigned)mrrc_audio_rebuffers(), mrrc_net_rssi(),
                         (double)(wsk_now - p_wsk2) / dt);
            }
            const uint32_t wrx = mrrc_net_ws_rx_bytes();
            unsigned tsm = 0, tslow = 0, tloop = 0, tstall = 0;
            mrrc_tx_link_stats(&tsm, &tslow, &tloop, &tstall);
            static uint32_t p_fb, p_fc;
            uint32_t fb = 0, fc = 0;
            bsp_display_flush_stats(&fb, &fc);
            ESP_LOGI(TAG, "flush: %.0f KB/s, %.1f screens/s, %u flushes/s",
                     (double)(fb - p_fb) / dt / 1024.0,
                     (double)(fb - p_fb) / dt / 153600.0,
                     (unsigned)((double)(fc - p_fc) / dt));
            {
                static uint32_t p_band[11];
                uint32_t band[11];
                bsp_display_flush_bands(band, 11);
                double tot = 0;
                for (int i = 0; i < 11; i++) tot += (double)(band[i] - p_band[i]);
                if (tot > 0) {
                    char line[192]; int n = 0;
                    for (int i = 0; i < 11; i++) {
                        const double pct = 100.0 * (double)(band[i] - p_band[i]) / tot;
                        n += snprintf(line + n, sizeof line - n, "y%03d:%2.0f%% ",
                                      i * 32, pct);
                        p_band[i] = band[i];
                    }
                    ESP_LOGI(TAG, "redraw by band: %s", line);
                }
            }
            p_fb = fb; p_fc = fc;
            {
                static uint32_t pa, ps, pj, na, ns, nj;
                uint32_t ca, cs, cj, qa, qs, qj;
                mrrc_net_ws_cost(&ca, &cs, &cj, &qa, &qs, &qj);
                ESP_LOGI(TAG, "ws cost/msg: audio %.0f us (%u)  spec %.0f us (%u)  json %.0f us (%u)",
                         (double)(ca - pa) / (double)((qa - na) ? (qa - na) : 1),
                         (unsigned)(qa - na),
                         (double)(cs - ps) / (double)((qs - ns) ? (qs - ns) : 1),
                         (unsigned)(qs - ns),
                         (double)(cj - pj) / (double)((qj - nj) ? (qj - nj) : 1),
                         (unsigned)(qj - nj));
                pa = ca; ps = cs; pj = cj; na = qa; ns = qs; nj = qj;
            }
            ESP_LOGI(TAG, "ws: total=%.1f KB/s (audio above is a subset)",
                     (double)(wrx - p_wrx) / dt / 1024.0);
            ESP_LOGI(TAG, "txlink: send_max=%u us slow=%u loop_max=%u us stalls=%u",
                     (unsigned)tsm, (unsigned)tslow, (unsigned)tloop, (unsigned)tstall);
            p_rx = lrx; p_un = lun; p_wrx = wrx; p_wsk2 = wsk_now; p_us = now_us;
            /* Per-task CPU needs the tick hook; the link counters above do not,
             * so keep the two switches independent. */
#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
            vTaskGetRunTimeStats(buf);
            ESP_LOGI(TAG, "cpu:\n%s", buf);
#else
            (void)buf;
#endif
        }
#endif
        /* Audio health decides whether the link is worth staying on.  A hotspot
         * can stay associated while delivering 10% loss and multi-second stalls;
         * esp_wifi never calls that a disconnect, so the stream has to judge. */
        {
            static uint32_t p_drop, p_un2;
            static int bad_windows;
            const uint32_t ddrop = ad - p_drop;
            const uint32_t dun = un - p_un2;
            p_drop = ad; p_un2 = un;
            const bool bad = (ddrop > 60 || dun > 3000);   /* >2% frames or >50 underruns/s */
            if (bad) bad_windows++; else bad_windows = 0;
            /* Bandwidth triage.  The terminal has no audio tier below PCM16
             * (AD-002), so when the path cannot carry audio + a full spectrum
             * frame the only thing it can give back is the waterfall: 18 KB/s
             * of "high" frames is more than half of a starved WAN link, while
             * the operator is listening, not watching.  Demote on the second
             * bad minute, restore after two clean ones — hysteresis, because a
             * flapping tier costs a round trip each way. */
            {
                static int good_windows;
                const char *tier = mrrc_net_link_tier();
                if (bad && bad_windows >= 2 && strcmp(tier, "low") != 0) {
                    mrrc_net_spectrum_tier("low");
                    good_windows = 0;
                } else if (!bad) {
                    good_windows++;
                    if (good_windows >= 2 && strcmp(tier, "low") == 0 && !txf) {
                        mrrc_net_spectrum_tier("high");
                        good_windows = 0;
                    }
                } else {
                    good_windows = 0;
                }
            }
            if (bad && bad_windows >= 2) {
                bad_windows = 0;
                mrrc_net_wifi_kick(ddrop > 60 ? "frame drops" : "underruns");
            }
        }
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
