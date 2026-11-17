// main/mrrc_ui.c — the xz-02 handheld layout, ported to this 240x320 panel.
//
// The reference is mrrc-handheld/main/ui.c: an ICOM-style fixed layout with a
// flat chrome bar top and bottom, a "field" convention behind every small
// label, and a spectrum + waterfall stack in the middle.  That firmware draws
// straight into a 240x240 framebuffer; this one keeps LVGL (the BSP has no
// second full framebuffer to give), so the same design is expressed with
// styled labels, one lv_line and one lv_canvas.  Layout rows are kept at the
// handheld's y offsets for the top block and given the extra 80 px of height
// this panel has.
//
//   y   0.. 21  chrome bar: [LSB]              VFO A   87%   ⣿      (+ accent rule)
//   y  30.. 82  frequency  "7.079.51"   always white (the handheld turns it
//               red on TX; on this panel the chips bar already says TX)
//   y  84..100  S-meter row:  S9+10 ▮▮▮▮▮▮░░   FIL 2.4K    AF 128
//   y 106..117  SPECTRUM SCOPE                                    100K
//   y 120..199  spectrum line plot (blue, RED on TX, centre marker)
//   y 200..259  waterfall (Jet, newest row on top; 30 rows = 14.4 KB)
//   y 262..273  scale: 7.029        7.079        7.129
//   y 276..295  chips bar: FREQ | RX / TX 3 | LINK
//   y 300..318  toast (overlaps nothing until it is needed)
//
// The waterfall is HALF the handheld'' height on purpose: that board has PSRAM,
// this ESP32-C3 does not, and 60 rows (28.8 KB of .bss) took the largest free
// internal block from 26 KB down to 9 KB — too little for TLS/cJSON on PTT.
//
// The refresh runs as an LVGL timer, not a separate task: LVGL is not thread
// safe and the WebSocket callback must never block on a render.
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "bsp_display.h"
#include "lvgl.h"
#include "mrrc_net.h"
#include "mrrc_ui.h"

#define REFRESH_MS 200

/* ── palette ────────────────────────────────────────────────────────────
 * THE VALUES BELOW ARE 24-BIT HEX, NOT RGB565.  They are handed to
 * lv_color_hex(), which takes 0xRRGGBB and converts at run time.  The
 * handheld's ui.c defines the same colours as pre-converted RGB565 literals
 * because it writes its own framebuffer; copying those literals into a
 * lv_color_hex() call re-interprets them as the low 16 bits of an RGB24
 * triple and silently shifts the whole palette — 0xFFFF ("white") becomes
 * 0x00FFFF cyan, and 0x3C1E (blue) becomes 0x003C1E dark green.  Both bugs
 * were visible on hardware before this note was written.
 *
 * Same source colours as the handheld / web client, expressed as they were
 * meant to read: #2a2a2a chrome, #ffa500 accent, #3a3a3a fields, #3b82f6 trace. */
#define C_BAR       0x2A2A2A      /* flat top and bottom bars */
#define C_ORANGE    0xFFA500      /* accent rule */
#define C_FIELD     0x3A3A3A      /* text field plate */
#define C_FIELD_D   0x212121      /* dimmer plate */
#define C_ACCENT    0xE87A00      /* bright orange, small accents only */
#define C_FAINT     0x6B4A00      /* ~25 % orange, barely-there marks */
#define C_TEXT      0xFFFFFF
#define C_RED       0xFF0000
#define C_GREEN     0x00FF00
#define C_BLUE      0x3B82F6      /* #3b82f6 — the spectrum trace */

/* ── layout ─────────────────────────────────────────────────────────────
 * This is a rounded-corner panel: the header content starts below the curve
 * and the frequency sits under the accent rule rather than overlapping it. */
#define SCR_W       240
#define SCR_H       320
#define H_HDR       26          /* chrome bar height; its text sits inside it */
#define Y_FREQ      46          /* below the accent rule, clear of the bar */
#define Y_INFO      88          /* row 1: band | FIL | AF | SPAN */
#define H_INFO      17
#define Y_SMET      107         /* row 2: the S-meter, on its own line */
#define H_SMET      18
#define Y_PLOT      136         /* pushed down: the gap above it read as dead space */
#define PLOT_H      62
#define Y_WF        202
#define WF_H        30                  /* RGB565: 240*30*2 = 14.4 KB */
#define Y_SCALE     236
#define Y_CHIPS     292
#define H_CHIPS     22
#define Y_TOAST     296

#define BINS        850

static lv_obj_t *s_bar_top, *s_mode, *s_vfo, *s_batt, *s_wifi;
static lv_obj_t *s_freq;
static lv_obj_t *s_sunit, *s_sbar, *s_fil, *s_af;
static lv_obj_t *s_bandplate, *s_spanplate;
static lv_obj_t *s_spec_line, *s_wf_img;
static lv_obj_t *s_sc_l, *s_sc_c, *s_sc_r;
static lv_obj_t *s_chip_mode, *s_chip_tx, *s_chip_link;
static lv_obj_t *s_toast;

/* Fixed floor/ceiling, same as the browser defaults (Floor 71 / Ceil 233).
 * Adaptive AGC made the trace jump whenever a single carrier appeared. */
#define SCOPE_FLOOR 71
#define SCOPE_CEIL  233
#define BINS_TO_IDX(v) \
    ((int)((v) <= SCOPE_FLOOR ? 0 : ((v) >= SCOPE_CEIL ? 63 : \
     ((v) - SCOPE_FLOOR) * 63 / (SCOPE_CEIL - SCOPE_FLOOR))))

static uint16_t s_heat[64];

/* Jet, ported from static/ft710_ui.js WF_PALETTES.jet.  v=0 is dark blue, not
 * black: a black waterfall reads as "no data". */
static void heat_init(void)
{
    for (int i = 0; i < 64; i++) {
        const float v = i / 63.0f;
        int r, g, b;
        if (v < 0.125f)      { const float u = v / 0.125f;
                               r = 0; g = 0; b = (int)(128 + u * 127); }
        else if (v < 0.375f) { const float u = (v - 0.125f) / 0.25f;
                               r = 0; g = (int)(u * 255); b = 255; }
        else if (v < 0.625f) { const float u = (v - 0.375f) / 0.25f;
                               r = (int)(u * 255); g = 255; b = (int)(255 * (1 - u)); }
        else if (v < 0.875f) { const float u = (v - 0.625f) / 0.25f;
                               r = 255; g = (int)(255 * (1 - u)); b = 0; }
        else                 { const float u = (v - 0.875f) / 0.125f;
                               r = (int)(255 * (1 - u * 0.5f)); g = 0; b = 0; }
        s_heat[i] = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
    }
}

/* Spectrum: lv_line stores only the 240 vertex coordinates (960 B) and LVGL
 * rasterises the stroke, so PLOT_H costs nothing in RAM. */
static lv_point_precise_t s_pts[BINS > SCR_W ? BINS : SCR_W];
static uint16_t s_wf_buf[SCR_W * WF_H];

/* Waterfall scrolling: sweeps arrive at ~5.3 Hz while the UI ticks at 5 Hz.
 * Pushing a row only when data lands makes the history jump, so the previous
 * and current sweeps are interpolated once per tick instead. */
static volatile uint8_t s_bins[BINS];
static volatile bool s_bins_new;
static uint8_t s_cols[SCR_W];
static uint8_t s_prev[SCR_W];
static uint8_t s_cur[SCR_W];
static uint32_t s_t;            /* 0..1000 progress from prev to cur */
static bool s_started;

/* Handheld trace model: 30 % new data per frame, then a 2x amplitude boost. */
#define FFT_ALPHA 0.30f
#define FFT_BOOST 2.0f
static float s_fft[SCR_W];
static bool s_fft_primed;

static uint32_t s_last_seq = 0xFFFFFFFF;
static uint32_t s_last_meter = 0xFFFFFFFF;
static int s_batt_pct = -1;
static bool s_batt_charging;
static ui_mode_t s_ui_mode = UI_MODE_FREQ;
static int s_spk_pct = 60;
static char s_chan_lbl[24];

/* SS05 index -> Hz, same table as the handheld.  The old code hardcoded 100 kHz
 * and never read scope_span, so the caption and the three axis labels stayed at
 * ±50 kHz no matter what the radio was showing. */
static const int32_t k_spans_hz[10] = {
    1000, 2000, 5000, 10000, 20000, 50000, 100000, 200000, 500000, 1000000
};

static int32_t span_hz(int idx)
{
    if (idx < 0 || idx > 9) idx = 6;      /* fall back to 100 kHz */
    return k_spans_hz[idx];
}

static void format_span(char *out, size_t n, int32_t hz)
{
    if (hz >= 1000000) snprintf(out, n, "%" PRId32 "M", hz / 1000000);
    else snprintf(out, n, "%" PRId32 "K", hz / 1000);
}

/* 7.079.51 — last two digits are Hz/10, matching the handheld. */
static void format_freq(char *out, size_t n, int32_t hz)
{
    if (hz <= 0) { snprintf(out, n, "----.--"); return; }
    snprintf(out, n, "%ld.%03ld.%02ld",
             (long)(hz / 1000000), (long)((hz / 1000) % 1000), (long)((hz % 1000) / 10));
}

static void format_mhz(char *out, size_t n, int32_t hz)
{
    if (hz < 0) hz = 0;
    snprintf(out, n, "%ld.%03ld", (long)(hz / 1000000), (long)((hz / 1000) % 1000));
}

/* ── widgets ────────────────────────────────────────────────────────── */

static lv_obj_t *bar(int y, int h)
{
    lv_obj_t *o = lv_obj_create(lv_screen_active());
    lv_obj_set_size(o, SCR_W, h);
    lv_obj_set_pos(o, 0, y);
    lv_obj_set_style_radius(o, 0, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(C_BAR), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

/* A field behind a run of text: dim grey plate, light glyphs.  Everything in
 * the info and scale strips uses this so it stays readable at 14 px. */
static lv_obj_t *field(const lv_font_t *font, int x, int y, int w, bool dim)
{
    lv_obj_t *l = lv_label_create(lv_screen_active());
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(C_TEXT), 0);
    lv_obj_set_style_bg_color(l, lv_color_hex(dim ? C_FIELD_D : C_FIELD), 0);
    lv_obj_set_style_bg_opa(l, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(l, 2, 0);
    lv_obj_set_style_pad_hor(l, 3, 0);
    lv_obj_set_style_pad_ver(l, 1, 0);
    if (w > 0) lv_obj_set_width(l, w);
    lv_obj_set_pos(l, x, y);
    lv_label_set_text(l, "");
    return l;
}

static lv_obj_t *plain(const lv_font_t *font, lv_color_t color, int x, int y)
{
    lv_obj_t *l = lv_label_create(lv_screen_active());
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_obj_set_pos(l, x, y);
    lv_label_set_text(l, "");
    return l;
}

/* Link strength as a dBm number.  The handheld draws four 2 px bars, which at
 * this font size is unreadable on our panel; the figure itself is unambiguous
 * and colour-codes the same three bands. */
static void draw_rssi(lv_obj_t *o, int rssi, bool up)
{
    /* Four blocks.  The bare dBm figure made the operator decode a
     * radio-engineering number at a glance; bar height says it instantly and
     * the colour carries good / fair / poor. */
    /* Ascending blocks, not equal ones: four identical squares read as an
     * unrelated UI widget, a rising staircase reads as signal strength. */
    static const char *const BAR[5] = {
        "", "\xe2\x96\x81", "\xe2\x96\x82\xe2\x96\x84",
        "\xe2\x96\x82\xe2\x96\x84\xe2\x96\x86",
        "\xe2\x96\x81\xe2\x96\x83\xe2\x96\x85\xe2\x96\x87" };
    if (!up || rssi == 0) {
        lv_label_set_text(o, "--");
        lv_obj_set_style_text_color(o, lv_color_hex(C_FAINT), 0);
        return;
    }
    int n = rssi > -55 ? 4 : (rssi > -65 ? 3 : (rssi > -75 ? 2 : 1));
    lv_label_set_text(o, BAR[n]);
    lv_obj_set_style_text_color(o,
        lv_color_hex(n >= 3 ? C_GREEN : (n == 2 ? C_ORANGE : C_RED)), 0);
}

/* Battery percentage — pushed out of refresh()'s hot path so it can share the
 * RSSI throttle. */
static void update_battery(void)
{
    if (s_batt_pct < 0 || !s_batt) return;
    char b[12];
    snprintf(b, sizeof b, "%d%%", s_batt_pct);
    lv_label_set_text(s_batt, b);
    lv_obj_set_style_text_color(s_batt,
        lv_color_hex(s_batt_charging ? C_GREEN : C_TEXT), 0);
}

static void build_screen(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_screen_load(scr);

    /* ── top chrome: one bar, one colour, plus the accent rule ── */
    s_bar_top = bar(0, H_HDR);
    /* Rounded-corner panel: glyphs at y=0 are clipped by the corner mask, so
     * the chrome row is centred inside a 26 px bar instead of hugging the top. */
    s_mode = plain(&lv_font_montserrat_14, lv_color_hex(C_TEXT), 8, 8);
    lv_label_set_text(s_mode, "---");
    /* Right-anchored, widest first: the battery sits against the panel edge,
     * RSSI bars to its left, VFO label left of those. */
    s_batt = plain(&lv_font_montserrat_14, lv_color_hex(C_TEXT), SCR_W - 44, 8);
    lv_obj_set_style_text_align(s_batt, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_size(s_batt, 40, 16);
    s_wifi = plain(&lv_font_montserrat_14, lv_color_hex(C_TEXT), SCR_W - 84, 8);
    lv_obj_set_style_text_align(s_wifi, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_size(s_wifi, 34, 16);
    s_vfo  = plain(&lv_font_montserrat_14, lv_color_hex(C_TEXT), SCR_W - 172, 8);
    lv_obj_set_style_text_align(s_vfo, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_width(s_vfo, 70);
    /* The bar is tall so its lower edge clears the corner mask, but the orange
     * accent rule still belongs at the chrome/frequency boundary — draw it as
     * its own line rather than the bar's bottom border. */
    lv_obj_set_style_border_width(s_bar_top, 0, 0);
    lv_obj_t *rule = lv_obj_create(lv_screen_active());
    lv_obj_set_size(rule, SCR_W, 1);
    lv_obj_set_pos(rule, 0, H_HDR);
    lv_obj_set_style_radius(rule, 0, 0);
    lv_obj_set_style_bg_color(rule, lv_color_hex(C_ORANGE), 0);
    lv_obj_set_style_bg_opa(rule, LV_OPA_COVER, 0);
    lv_obj_remove_flag(rule, LV_OBJ_FLAG_SCROLLABLE);

    /* ── frequency ── */
    s_freq = plain(&lv_font_montserrat_28, lv_color_hex(C_TEXT), 0, Y_FREQ);
    lv_obj_align(s_freq, LV_ALIGN_TOP_MID, 0, Y_FREQ);
    /* Montserrat 40 is not exposed by this build's Kconfig; scaling 28 -> 1.5x
     * gives the same glyphs at the handheld's numeral height. */
    lv_obj_set_style_transform_scale(s_freq, 384, 0);
    lv_obj_set_style_transform_pivot_x(s_freq, 50, 0);
    lv_obj_set_style_transform_pivot_y(s_freq, 0, 0);
    lv_label_set_text(s_freq, "---.---");

    /* ── row 1: band name (or FREQ / VOL / channel plate), then FIL, AF, SPAN ──
     * The band label used to sit in the scope caption row; the operator wants it
     * beside the readouts that share its timescale. */
    /* Four plates must fit 240 px.  "FIL 2.4K" at Montserrat 14 needs ~62 px and
     * the row wrapped onto the S-meter line; this build only compiles 14/20/28,
     * so the labels are shortened to bare values in the same 14 px face —
     * 2.4K / 0 / 200K are self-evident next to their own plates. */
    s_bandplate = field(&lv_font_montserrat_14, 2, Y_INFO, 54, false);
    lv_obj_set_style_text_color(s_bandplate, lv_color_hex(C_ACCENT), 0);
    lv_label_set_text(s_bandplate, "---");
    s_fil = field(&lv_font_montserrat_14, 59, Y_INFO, 62, false);
    s_af  = field(&lv_font_montserrat_14, 124, Y_INFO, 58, false);
    s_spanplate = field(&lv_font_montserrat_14, 185, Y_INFO, 53, true);
    lv_label_set_text(s_spanplate, "--");

    /* ── row 2: S-meter alone — unit text left, bar filling the rest ── */
    s_sunit = field(&lv_font_montserrat_14, 2, Y_SMET, 52, false);
    s_sbar  = lv_bar_create(lv_screen_active());
    lv_obj_set_size(s_sbar, SCR_W - 66, H_SMET - 4);
    lv_obj_set_pos(s_sbar, 62, Y_SMET + 2);
    lv_bar_set_range(s_sbar, 0, 255);
    lv_obj_set_style_bg_color(s_sbar, lv_color_hex(C_FIELD_D), LV_PART_MAIN);
    lv_obj_set_style_radius(s_sbar, 2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_sbar, lv_color_hex(C_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_sbar, 2, LV_PART_INDICATOR);
    lv_bar_set_value(s_sbar, 0, LV_ANIM_OFF);

    /* ── spectrum line ── */
    s_spec_line = lv_line_create(lv_screen_active());
    lv_obj_set_pos(s_spec_line, 0, Y_PLOT);
    lv_obj_set_size(s_spec_line, SCR_W, PLOT_H);
    /* Rounded joins matter here: with butt caps, LVGL's antialiased segments
     * leave visible gaps wherever the trace steps sharply, which reads as a
     * row of dots instead of one line. */
    lv_obj_set_style_line_width(s_spec_line, 3, 0);
    lv_obj_set_style_line_color(s_spec_line, lv_color_hex(C_BLUE), 0);
    /* This LVGL build has no separate cap style; rounded is what turns on
     * round joins and caps in the software renderer. */
    lv_obj_set_style_line_rounded(s_spec_line, true, 0);
    for (int x = 0; x < SCR_W; x++) { s_pts[x].x = x; s_pts[x].y = PLOT_H - 1; }
    lv_line_set_points(s_spec_line, s_pts, SCR_W);

    /* ── waterfall: lv_canvas owns its buffer and LVGL re-reads it on
     * invalidate, which is the supported path for pixels written here. ── */
    s_wf_img = lv_canvas_create(lv_screen_active());
    lv_canvas_set_buffer(s_wf_img, s_wf_buf, SCR_W, WF_H, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_pos(s_wf_img, 0, Y_WF);

    /* ── scale row: three fields like the handheld ── */
    s_sc_l = field(&lv_font_montserrat_14, 2, Y_SCALE, 54, true);
    s_sc_c = field(&lv_font_montserrat_14, 92, Y_SCALE, 56, false);
    s_sc_r = field(&lv_font_montserrat_14, 184, Y_SCALE, 54, true);

    /* ── bottom chips: mode | RX/TX | link ── */
    bar(Y_CHIPS, H_CHIPS);
    s_chip_mode = plain(&lv_font_montserrat_14, lv_color_hex(C_TEXT), 6, Y_CHIPS + 2);
    lv_label_set_text(s_chip_mode, "FREQ");
    s_chip_tx = plain(&lv_font_montserrat_14, lv_color_hex(C_TEXT), 0, Y_CHIPS + 2);
    lv_obj_align(s_chip_tx, LV_ALIGN_TOP_MID, 0, Y_CHIPS + 2);
    lv_label_set_text(s_chip_tx, "RX");
    s_chip_link = plain(&lv_font_montserrat_14, lv_color_hex(C_TEXT), SCR_W - 46, Y_CHIPS + 2);
    lv_label_set_text(s_chip_link, "LINK");

    s_toast = plain(&lv_font_montserrat_14, lv_color_hex(C_TEXT), 0, Y_TOAST);
    lv_obj_align(s_toast, LV_ALIGN_TOP_MID, 0, Y_TOAST);
    lv_obj_set_style_bg_color(s_toast, lv_color_hex(C_RED), 0);
    lv_obj_set_style_bg_opa(s_toast, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_toast, 3, 0);
    lv_label_set_text(s_toast, "");
    lv_obj_add_flag(s_toast, LV_OBJ_FLAG_HIDDEN);

    heat_init();
    memset(s_wf_buf, 0, sizeof s_wf_buf);
}

/* Mirrors the handheld's fft_update(): EMA over the peak-downsampled columns. */
static void fft_update(const uint8_t *cols)
{
    for (int x = 0; x < SCR_W; x++) {
        if (!s_fft_primed) s_fft[x] = (float)cols[x];
        else s_fft[x] = s_fft[x] * (1.0f - FFT_ALPHA) + (float)cols[x] * FFT_ALPHA;
    }
    s_fft_primed = true;
}

static void update_plot(bool tx)
{
    const int range = SCOPE_CEIL - SCOPE_FLOOR;
    for (int x = 0; x < SCR_W; x++) {
        float mapped = (s_fft[x] - (float)SCOPE_FLOOR) / (float)range * FFT_BOOST;
        if (mapped < 0.0f) mapped = 0.0f;
        if (mapped > 1.0f) mapped = 1.0f;
        s_pts[x].y = (PLOT_H - 1) - (int)(mapped * (PLOT_H - 1));
    }
    lv_obj_set_style_line_color(s_spec_line,
                                lv_color_hex(tx ? C_RED : C_BLUE), 0);
    lv_obj_invalidate(s_spec_line);
}

/* Scroll one row and paint the newest through Jet. */
static void wf_push(int t_permille)
{
    /* Newest row on top, older rows pushed down — time flows downward, matching
     * the handheld (whose ring buffer draws row 0 as the newest sample). */
    memmove(s_wf_buf + SCR_W, s_wf_buf, sizeof(s_wf_buf) - SCR_W * sizeof(uint16_t));
    uint16_t *last = s_wf_buf;
    for (int x = 0; x < SCR_W; x++) {
        const int v = s_prev[x] + ((int)s_cur[x] - (int)s_prev[x]) * t_permille / 1000;
        const int idx = BINS_TO_IDX(v);
        /* idx 0 is the palette floor, but a cold bin used to be painted pure
         * black, which reads as "no data"; keep the dark-blue floor visible. */
        last[x] = s_heat[idx];
    }
    lv_obj_invalidate(s_wf_img);
}

static void refresh(void)
{
    mrrc_state_t st;
    mrrc_state_lock();
    st = g_radio;
    mrrc_state_unlock();

    const bool main_changed = st.seq != s_last_seq;
    const bool meter_changed = st.meter_seq != s_last_meter;
    const bool tx = st.tx_status > 0;
    char b[24];

    if (main_changed) {
        format_freq(b, sizeof b, st.freq_hz);
        lv_label_set_text(s_freq, b);
        /* header: mode left, VFO / battery / wifi right */
        lv_label_set_text(s_mode, st.mode_name[0] ? st.mode_name : "---");
        snprintf(b, sizeof b, "VFO %c", st.vfo ? st.vfo : 'A');
        lv_label_set_text(s_vfo, b);
        /* Live scope span: drives this plate and the three axis labels below. */
        int32_t span = span_hz(st.scope_span);
        char spanb[16];
        format_span(spanb, sizeof spanb, span);
        lv_label_set_text(s_spanplate, spanb);

        /* Leftmost plate carries whatever the knob is pointed at — band name in
         * FREQ mode, channel name in CH, speaker level in VOL. */
        if (s_ui_mode == UI_MODE_VOL) {
            snprintf(b, sizeof b, "%d%%", s_spk_pct);
            lv_label_set_text(s_bandplate, b);
        } else if (s_ui_mode == UI_MODE_CHANNEL && s_chan_lbl[0]) {
            lv_label_set_text(s_bandplate, s_chan_lbl);
        } else if (st.band_name[0]) {
            lv_label_set_text(s_bandplate, st.band_name);
        }

        /* RSSI and battery do not move at 5 Hz — refresh them about every 2 s. */
        if ((lv_tick_get() / 200) % 10 == 0) {
            draw_rssi(s_wifi, mrrc_net_rssi(), mrrc_net_up());
            update_battery();
        }

        /* chips bar: knob mode | RX or TX seconds | link */
        const char *m = s_ui_mode == UI_MODE_VOL ? "VOL"
                      : (s_ui_mode == UI_MODE_CHANNEL ? "CH" : "FREQ");
        lv_label_set_text(s_chip_mode, m);
        if (tx) {
            snprintf(b, sizeof b, "TX%s", st.tx_status == 2 ? "T" : "");
            lv_label_set_text(s_chip_tx, b);
            lv_obj_set_style_text_color(s_chip_tx, lv_color_hex(C_RED), 0);
        } else {
            lv_label_set_text(s_chip_tx, "RX");
            lv_obj_set_style_text_color(s_chip_tx, lv_color_hex(C_TEXT), 0);
        }
        lv_label_set_text(s_chip_link, mrrc_net_up() ? "LINK" : "noSRV");
        lv_obj_set_style_text_color(s_chip_link,
                                    lv_color_hex(mrrc_net_up() ? C_TEXT : C_RED), 0);

        /* filter + AF live in the info row; SPK replaces AF in VOL mode */
        if (st.filter_hz > 0) {
            /* "FL" not "FIL": the full word pushes the plate past its 62 px and
             * the row wraps onto the S-meter, but a bare value is cryptic. */
            snprintf(b, sizeof b, "FL %d.%dK", st.filter_hz / 1000,
                     (st.filter_hz % 1000) / 100);
            lv_label_set_text(s_fil, b);
        } else {
            lv_label_set_text(s_fil, "");
        }
        snprintf(b, sizeof b, "AF %d", st.af_gain);
        lv_label_set_text(s_af, b);

        /* scale row around the VFO */
        const int32_t half = span / 2;
        format_mhz(b, sizeof b, st.freq_hz - half);  lv_label_set_text(s_sc_l, b);
        format_mhz(b, sizeof b, st.freq_hz);         lv_label_set_text(s_sc_c, b);
        format_mhz(b, sizeof b, st.freq_hz + half);  lv_label_set_text(s_sc_r, b);

        s_last_seq = st.seq;
    }

    if (meter_changed || main_changed) {
        /* S number and bar share the row: number is the server's s_unit
         * ("9", "+10", "5"), bar is the raw 0..255 level. */
        if (st.s_unit[0] == '+') snprintf(b, sizeof b, "S9%s", st.s_unit);
        else snprintf(b, sizeof b, "%s", st.s_unit[0] ? st.s_unit : "-");
        lv_label_set_text(s_sunit, b);
        lv_bar_set_value(s_sbar, st.s_meter, LV_ANIM_OFF);
        s_last_meter = st.meter_seq;
    }

    if (s_bins_new) {
        s_bins_new = false;
        /* Peak downsample 850 -> 240: averaging hides narrow signals. */
        for (int x = 0; x < SCR_W; x++) {
            const int lo = (int)((long)x * BINS / SCR_W);
            int hi = (int)((long)(x + 1) * BINS / SCR_W);
            if (hi <= lo) hi = lo + 1;
            uint8_t peak = 0;
            for (int i = lo; i < hi && i < BINS; i++)
                if (s_bins[i] > peak) peak = s_bins[i];
            s_cols[x] = peak;
        }
        if (s_started) memcpy(s_prev, s_cur, SCR_W);
        else { memcpy(s_prev, s_cols, SCR_W); s_started = true; }
        memcpy(s_cur, s_cols, SCR_W);
        s_t = 0;
        fft_update(s_cols);
    }

    if (s_started) {
        update_plot(tx);
        /* Two rows per tick at 5 ticks/s = 10 rows/s, so the WF_H-row history
         * scrolls in about six seconds.  One row per tick looked frozen. */
        for (int r = 0; r < 2; r++) {
            const int t = (int)s_t + (r * (1000 - (int)s_t)) / 2;
            wf_push(t);
        }
        s_t += (uint32_t)(REFRESH_MS * 1000u) * 1000u / 189000u;   /* ~5.3 Hz */
        if (s_t > 1000) s_t = 1000;
    }

}

static void ui_timer_cb(lv_timer_t *t) { (void)t; refresh(); }

void mrrc_ui_start(void)
{
    if (!bsp_lvgl_lock(1000)) return;
    build_screen();
    bsp_lvgl_unlock();
    /* Runs inside the LVGL task, in step with the renderer.  A separate
     * low-priority task lost the CPU to audio/Wi-Fi and made the scope stutter
     * once per second. */
    lv_timer_create(ui_timer_cb, REFRESH_MS, NULL);
}

void mrrc_ui_spectrum(const uint8_t *bins, size_t n)
{
    if (!bins || n < BINS) return;
    memcpy((void *)s_bins, bins, BINS);
    s_bins_new = true;
}

void mrrc_ui_set_mode(ui_mode_t mode, int volume_pct)
{
    if (s_ui_mode == mode && s_spk_pct == volume_pct) return;
    s_ui_mode = mode;
    s_spk_pct = volume_pct;
    if (!bsp_lvgl_lock(100)) return;
    s_last_seq = 0xFFFFFFFF;      /* force the field branch next refresh */
    bsp_lvgl_unlock();
}

void mrrc_ui_set_channel(const char *label)
{
    snprintf(s_chan_lbl, sizeof s_chan_lbl, "%s", label ? label : "");
    if (!bsp_lvgl_lock(100)) return;
    s_last_seq = 0xFFFFFFFF;
    bsp_lvgl_unlock();
}

void mrrc_ui_set_battery(int percent, bool charging)
{
    s_batt_pct = percent;
    s_batt_charging = charging;
}

void mrrc_ui_set_toast(const char *text)
{
    if (!bsp_lvgl_lock(100)) return;
    lv_label_set_text(s_toast, text ? text : "");
    /* An empty label with a filled background still paints a small solid
     * square at the bottom of the screen — hide it until there is something to
     * say. */
    if (text && text[0]) lv_obj_clear_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
    bsp_lvgl_unlock();
}
