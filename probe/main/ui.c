/*
 * M3 — the main screen, LVGL 9, 240x320 portrait.
 *
 * Deliberately plain: labels refreshed from a snapshot, a canvas per animated
 * widget. No image assets, no extra fonts beyond montserrat — the 16 MB flash
 * is not the constraint, the internal RAM is (see the design doc §6.0).
 */
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"

#include "ui.h"

static const char *TAG = "ui";

#define W 240
#define H 320

#define Y_TOP    0
#define H_TOP    26
#define Y_FREQ   30
#define H_FREQ   56
#define Y_INFO   88
#define H_INFO   18
#define Y_SPEC   108
#define H_SPEC   92
#define Y_WF     202
#define H_WF     74
#define Y_BOT    278
#define H_BOT    22

static lv_obj_t *l_mode, *l_vfo, *l_batt;
static lv_obj_t *l_freq;
static lv_obj_t *l_smeter, *l_filter, *l_af;
static lv_obj_t *l_state, *l_link;
static lv_obj_t *cv_spec, *cv_wf;
static lv_obj_t *l_tx;

static uint8_t *spec_px;          /* 240x92 RGB565, PSRAM */
static uint8_t *wf_px;            /* 240x74, one row shifted per update */
static uint8_t wf_hist[H_WF];     /* ring of one-pixel rows is overkill; keep a scrollable image */
static int spec_len_last;

/* This panel interprets RGB565 as R(15-11) / B(10-5) / G(4-0) — measured with
 * the three-tag colour probe (R correct, G/B swapped). MADCTL cannot express
 * that, so twist the colour on the way in and let the panel untwist it. */
static inline uint32_t tw888(uint32_t rgb)
{
    uint32_t r = (rgb >> 16) & 0xFF, g = (rgb >> 8) & 0xFF, b = rgb & 0xFF;
    return (r << 16) | (b << 8) | g;
}

static inline uint16_t tw565(uint16_t c)
{
    uint16_t r = (c >> 11) & 0x1F, g = (c >> 5) & 0x3F, b = c & 0x1F;
    return (uint16_t)((r << 11) | (b << 6) | (g >> 1));
}

#define C(rgb) lv_color_hex(tw888(rgb))

static lv_obj_t *panel(int x, int y, int w, int h, uint32_t bg)
{
    lv_obj_t *o = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, C(bg), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_radius(o, 0, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, uint32_t colour, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, C(colour), 0);
    lv_label_set_text(l, text);
    return l;
}

void ui_init(esp_lcd_panel_io_handle_t io, esp_lcd_panel_handle_t panel_h, esp_lcd_touch_handle_t tp)
{
    const lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_ERROR_CHECK(lvgl_port_init(&port_cfg));

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = io,
        .panel_handle = panel_h,
        .buffer_size = W * 40,
        .double_buffer = true,
        .hres = W,
        .vres = H,
        .monochrome = false,
        .rotation = {.swap_xy = false, .mirror_x = false, .mirror_y = false},
        .flags = {.buff_dma = 0, .buff_spiram = 1},
    };
    if (!lvgl_port_add_disp(&disp_cfg)) {
        ESP_LOGE(TAG, "lvgl_port_add_disp failed");
        return;
    }
    if (tp) {
        const lvgl_port_touch_cfg_t touch_cfg = {.disp = lv_display_get_default(), .handle = tp};
        lvgl_port_add_touch(&touch_cfg);
    }

    if (lvgl_port_lock(0)) {
        lv_obj_t *scr = lv_screen_active();
        lv_obj_set_style_bg_color(scr, C(0x000000), 0);
        lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

        /* ── top bar ──────────────────────────────────────────────── */
        lv_obj_t *top = panel(0, Y_TOP, W, H_TOP, 0x101010);
        l_mode = label(top, &lv_font_montserrat_14, 0xFFB020, "LSB");
        lv_obj_align(l_mode, LV_ALIGN_LEFT_MID, 6, 0);
        l_vfo = label(top, &lv_font_montserrat_12, 0x9AA0A6, "VFO A");
        lv_obj_align(l_vfo, LV_ALIGN_CENTER, 0, 0);
        l_batt = label(top, &lv_font_montserrat_12, 0x7FA37F, "87%");
        lv_obj_align(l_batt, LV_ALIGN_RIGHT_MID, -6, 0);

        /* Colour probe: lv_color_hex() takes 0xRRGGBB (not RGB565, which is
         * what made the first attempt unreadable). Left to right these are
         * pure red, green, blue, each tagged with its expected initial. */
        static const uint32_t probe_rgb[3] = {0xFF0000, 0x00FF00, 0x0000FF};
        static const char *probe_tag[3] = {"R", "G", "B"};
        for (int i = 0; i < 3; i++) {
            lv_obj_t *b = panel(140 + i * 22, 7, 14, 14, probe_rgb[i]);
            lv_obj_set_style_radius(b, 3, 0);
            lv_obj_t *t = label(b, &lv_font_montserrat_12, 0xFFFFFF, probe_tag[i]);
            lv_obj_center(t);
        }

        /* ── frequency ────────────────────────────────────────────── */
        l_freq = label(scr, &lv_font_montserrat_28, 0xFFFFFF, "7.215.00");
        lv_obj_set_pos(l_freq, 0, Y_FREQ);
        lv_obj_set_size(l_freq, W, H_FREQ);
        lv_obj_set_style_text_align(l_freq, LV_TEXT_ALIGN_CENTER, 0);

        /* ── info row ─────────────────────────────────────────────── */
        l_smeter = label(scr, &lv_font_montserrat_12, 0x9FB3C8, "S9+10");
        lv_obj_set_pos(l_smeter, 8, Y_INFO);
        l_filter = label(scr, &lv_font_montserrat_12, 0x7FA37F, "FIL 2.4K");
        lv_obj_set_pos(l_filter, 78, Y_INFO);
        l_af = label(scr, &lv_font_montserrat_12, 0x7FA37F, "AF 128");
        lv_obj_set_pos(l_af, 172, Y_INFO);

        /* ── spectrum canvas ──────────────────────────────────────── */
        spec_px = lv_malloc(W * H_SPEC * 2);
        wf_px = lv_malloc(W * H_WF * 2);
        if (spec_px && wf_px) {
            memset(spec_px, 0, (size_t)W * H_SPEC * 2);
            memset(wf_px, 0, (size_t)W * H_WF * 2);
            lv_obj_t *spec_bg = panel(0, Y_SPEC, W, H_SPEC, 0x04140C);
            cv_spec = lv_canvas_create(spec_bg);
            lv_canvas_set_buffer(cv_spec, spec_px, W, H_SPEC, LV_COLOR_FORMAT_RGB565);
            lv_obj_set_pos(cv_spec, 0, 0);

            lv_obj_t *wf_bg = panel(0, Y_WF, W, H_WF, 0x081018);
            cv_wf = lv_canvas_create(wf_bg);
            lv_canvas_set_buffer(cv_wf, wf_px, W, H_WF, LV_COLOR_FORMAT_RGB565);
            lv_obj_set_pos(cv_wf, 0, 0);
        } else {
            ESP_LOGE(TAG, "no memory for canvas buffers");
        }

        /* ── status bar ───────────────────────────────────────────── */
        lv_obj_t *bot = panel(0, Y_BOT, W, H_BOT, 0x101010);
        l_state = label(bot, &lv_font_montserrat_12, 0x2E7D32, "[RX]");
        lv_obj_align(l_state, LV_ALIGN_LEFT_MID, 8, 0);
        l_tx = label(bot, &lv_font_montserrat_12, 0xFF4444, "");
        lv_obj_align(l_tx, LV_ALIGN_CENTER, 0, 0);
        l_link = label(bot, &lv_font_montserrat_12, 0x2E7D32, "LINK");
        lv_obj_align(l_link, LV_ALIGN_RIGHT_MID, -8, 0);

        lvgl_port_unlock();
    }
    ESP_LOGI(TAG, "main screen built");
}

static void format_freq(char *out, size_t n, int32_t hz)
{
    if (hz <= 0) {
        snprintf(out, n, "--.---.--");
        return;
    }
    int mhz = hz / 1000000;
    int khz = (hz / 1000) % 1000;
    int hz3 = hz % 1000;
    snprintf(out, n, "%d.%03d.%02d", mhz, khz, hz3 / 10);
}

static inline uint16_t heat(int v)
{
    /* noise floor at the bottom, green → yellow → red at the top */
    if (v < 0) v = 0;
    if (v > 255) v = 255;
    int r = v < 128 ? 0 : (v - 128) * 2;
    int g = v < 128 ? v * 2 : 255;
    int b = v > 96 ? 0 : (96 - v) * 2;
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

void ui_update(const mrrc_state_t *st, const uint8_t *spec, int spec_len)
{
    if (!lvgl_port_lock(0)) return;
    char b[32];

    if (st->freq_hz > 0) {
        format_freq(b, sizeof b, st->freq_hz);
        lv_label_set_text(l_freq, b);
    }
    if (st->mode[0]) lv_label_set_text(l_mode, st->mode);

    snprintf(b, sizeof b, "%d K", st->scope_span >= 0 ? st->scope_span : 0);
    lv_label_set_text(l_link, st->up ? "LINK" : "----");
    lv_obj_set_style_text_color(l_link, C(st->up ? 0x2E7D32 : 0x883333), 0);
    if (st->filter_hz > 0) {
        snprintf(b, sizeof b, "FIL %.1fk", st->filter_hz / 1000.0);
        lv_label_set_text(l_filter, b);
    }
    if (st->serial_connected) {
        lv_label_set_text(l_smeter, st->smeter > 0 ? "S" : "S0");
    }

    /* Spectrum: 850 bins -> 240 columns, max per bucket.
     *
     * Two things here are not optional (both measured on the FT-710 stream):
     *  1. subtract the noise floor — bins idle around 120..150, so a peak-only
     *     normalisation fills the whole canvas;
     *  2. draw upward from the bottom edge, height scaled to the full 92 px.
     */
    if (spec_px && cv_spec && spec_len > 0) {
        static int floor_v = 120, peak_v = 200;
        int mn = 255, mx = 0;
        for (int i = 0; i < spec_len; i++) {
            if (spec[i] < mn) mn = spec[i];
            if (spec[i] > mx) mx = spec[i];
        }
        floor_v = (floor_v * 7 + mn) / 8;          /* slow tracking */
        peak_v = (peak_v * 7 + mx) / 8;
        if (peak_v - floor_v < 24) peak_v = floor_v + 24;

        uint16_t *px = (uint16_t *)spec_px;
        for (int x = 0; x < W; x++) {
            int a = x * spec_len / W;
            int e = (x + 1) * spec_len / W;
            if (e <= a) e = a + 1;
            int v = 0;
            for (int i = a; i < e && i < spec_len; i++) {
                if (spec[i] > v) v = spec[i];
            }
            int h = (v - floor_v) * H_SPEC / (peak_v - floor_v);
            if (h < 0) h = 0;
            if (h > H_SPEC) h = H_SPEC;
            for (int y = 0; y < H_SPEC; y++) {
                int from_bottom = H_SPEC - 1 - y;   /* y=0 is the top row */
                px[y * W + x] = (from_bottom < h) ? tw565(heat(from_bottom * 255 / H_SPEC)) : 0x0000;
            }
        }
        lv_obj_invalidate(cv_spec);

        /* Waterfall: shift the image down one row, write the new row on top. */
        if (wf_px && cv_wf) {
            uint16_t *wf = (uint16_t *)wf_px;
            memmove(wf + W, wf, (size_t)(H_WF - 1) * W * sizeof(uint16_t));
            for (int x = 0; x < W; x++) {
                int a = x * spec_len / W;
                int e = (x + 1) * spec_len / W;
                if (e <= a) e = a + 1;
                int v = 0;
                for (int i = a; i < e && i < spec_len; i++) {
                    if (spec[i] > v) v = spec[i];
                }
                wf[x] = tw565(heat(v));
            }
            lv_obj_invalidate(cv_wf);
        }
    }
    lvgl_port_unlock();
    (void)wf_hist;
}

void ui_set_tx(int on)
{
    if (!lvgl_port_lock(0)) return;
    lv_obj_set_style_text_color(l_state, C(on ? 0xFF4444 : 0x2E7D32), 0);
    lv_label_set_text(l_state, on ? "[TX]" : "[RX]");
    lv_label_set_text(l_tx, on ? "F" : "");
    lvgl_port_unlock();
}
