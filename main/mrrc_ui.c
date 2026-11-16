// main/mrrc_ui.c — 240x320 ICOM-style layout.
//
// Layout (y in px, screen is 240x320, round panel — the BSP masks the corners):
//     4.. 34   mode badge (left)              battery (right)
//    44..100   frequency, Montserrat 28
//   104..124   band + filter
//   140..170   S-meter bar + s_unit
//   190..230   status chips: RX/TX | server link
//   250..300   toast line
//
// The refresh task polls seq/meter_seq instead of being called from the WS
// callback: LVGL is not thread safe, and the WebSocket task must never block
// on a render.
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "bsp_display.h"
#include "lvgl.h"
#include "mrrc_net.h"
#include "mrrc_ui.h"

#define REFRESH_MS 150

static lv_obj_t *s_freq;
static lv_obj_t *s_mode;
static lv_obj_t *s_band;
static lv_obj_t *s_sunit;
static lv_obj_t *s_sbar;
static lv_obj_t *s_status;
static lv_obj_t *s_batt;
static lv_obj_t *s_toast;

static uint32_t s_last_seq = 0xFFFFFFFF;
static uint32_t s_last_meter = 0xFFFFFFFF;
static int s_batt_pct = -1;

/* 7.215.00 — the last three digits are Hz/10, matching the xz-02 layout. */
static void format_freq(char *out, size_t n, int32_t hz)
{
    if (hz <= 0) {
        snprintf(out, n, "----.--");
        return;
    }
    const int32_t mhz = hz / 1000000;
    const int32_t khz = (hz % 1000000) / 1000;
    const int32_t units = (hz % 1000) / 10;
    snprintf(out, n, "%ld.%03ld.%02ld", (long)mhz, (long)khz, (long)units);
}

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color, int x, int y)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_obj_set_pos(l, x, y);
    return l;
}

static void build_screen(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    s_mode = label(scr, &lv_font_montserrat_14, lv_color_hex(0xFFB300), 14, 6);
    lv_label_set_text(s_mode, "---");

    s_batt = label(scr, &lv_font_montserrat_14, lv_color_hex(0x9E9E9E), 168, 6);
    lv_label_set_text(s_batt, "BAT --");

    s_freq = label(scr, &lv_font_montserrat_28, lv_color_white(), 26, 44);
    lv_label_set_text(s_freq, "---.---");

    s_band = label(scr, &lv_font_montserrat_14, lv_color_hex(0x4FC3F7), 14, 84);
    lv_label_set_text(s_band, "----");

    s_sunit = label(scr, &lv_font_montserrat_20, lv_color_hex(0x8BC34A), 14, 112);
    lv_label_set_text(s_sunit, "S--");

    /* S-meter: 0..255 maps to the bar. */
    s_sbar = lv_bar_create(scr);
    lv_obj_set_size(s_sbar, 212, 10);
    lv_obj_set_pos(s_sbar, 14, 142);
    lv_bar_set_range(s_sbar, 0, 255);
    lv_obj_set_style_bg_color(s_sbar, lv_color_hex(0x263238), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_sbar, lv_color_hex(0x66BB6A), LV_PART_INDICATOR);
    lv_bar_set_value(s_sbar, 0, LV_ANIM_OFF);

    s_status = label(scr, &lv_font_montserrat_14, lv_color_hex(0x4FC3F7), 14, 170);
    lv_label_set_text(s_status, "RX   LINK --");

    s_toast = label(scr, &lv_font_montserrat_14, lv_color_hex(0xEF5350), 14, 250);
    lv_label_set_text(s_toast, "");

    lv_screen_load(scr);
}

static void refresh(void)
{
    mrrc_state_t st;
    mrrc_state_lock();
    st = g_radio;
    mrrc_state_unlock();

    const bool main_changed = st.seq != s_last_seq;
    const bool meter_changed = st.meter_seq != s_last_meter;

    if (main_changed) {
        char buf[24];
        format_freq(buf, sizeof buf, st.freq_hz);
        lv_label_set_text(s_freq, buf);

        snprintf(buf, sizeof buf, "%s %c", st.mode_name[0] ? st.mode_name : "---", st.vfo);
        lv_label_set_text(s_mode, buf);

        snprintf(buf, sizeof buf, "%s  %s",
                 st.band_name[0] ? st.band_name : "----",
                 st.filter_hz ? "" : "");
        lv_label_set_text(s_band, buf);

        const char *tx = st.tx_status == 1 ? "TX" : (st.tx_status == 2 ? "TUNE" : "RX");
        snprintf(buf, sizeof buf, "%s%s  LINK %s", tx,
                 st.radio_online ? "" : " ?", mrrc_net_up() ? "up" : "--");
        lv_label_set_text(s_status, buf);
        lv_obj_set_style_text_color(s_status,
                                    st.tx_status == 0 ? lv_color_hex(0x4FC3F7)
                                                      : lv_color_hex(0xEF5350), 0);
        lv_obj_set_style_text_color(s_freq,
                                    st.tx_status == 0 ? lv_color_white()
                                                      : lv_color_hex(0xEF5350), 0);
        s_last_seq = st.seq;
    }

    if (meter_changed || main_changed) {
        char buf[20];
        lv_bar_set_value(s_sbar, st.s_meter, LV_ANIM_OFF);
        snprintf(buf, sizeof buf, "%s", st.s_unit[0] ? st.s_unit : "S--");
        lv_label_set_text(s_sunit, buf);
        s_last_meter = st.meter_seq;
    }

    if (s_batt_pct >= 0) {
        char buf[16];
        snprintf(buf, sizeof buf, "BAT %d%%", s_batt_pct);
        lv_label_set_text(s_batt, buf);
    }
}

static void ui_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (bsp_lvgl_lock(100)) {
            refresh();
            bsp_lvgl_unlock();
        }
        vTaskDelay(pdMS_TO_TICKS(REFRESH_MS));
    }
}

void mrrc_ui_start(void)
{
    if (!bsp_lvgl_lock(1000)) return;
    build_screen();
    bsp_lvgl_unlock();
    if (xTaskCreate(ui_task, "ui", 4096, NULL, 4, NULL) != pdPASS)
        printf("ui task failed\n");
}

void mrrc_ui_set_battery(int percent, bool charging)
{
    (void)charging;
    s_batt_pct = percent;
}

void mrrc_ui_set_toast(const char *text)
{
    if (!bsp_lvgl_lock(100)) return;
    lv_label_set_text(s_toast, text);
    bsp_lvgl_unlock();
}
