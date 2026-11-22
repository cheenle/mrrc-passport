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

#include "bsp_audio.h"
#include "bsp_display.h"
#include "lvgl.h"
#include "mrrc_net.h"
#include "mrrc_ui.h"

/* 190 ms (5.26 Hz) — matched to the radio's own sweep rate.
 *
 * The "high" spectrum profile delivers one frame per radio sweep, measured at
 * ~5.3 frames/s.  The tick was 300 ms (3.3 Hz), which meant two or three frames
 * arrived between renders and s_bins_new — a bool — kept only the last one:
 * about 38% of the sweeps were dropped, and the waterfall filled the gap by
 * interpolating between the previous and current frame, so most of its rows
 * were synthesised rather than measured.
 *
 * At 190 ms each real sweep gets its own render and its own waterfall row.
 * CPU cost measured separately; the 24-line draw buffer leaves enough headroom
 * for it. */
#define REFRESH_MS 190

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
#define Y_INFO     102          /* row 1: band | FL | AF | SPAN — two blank lines
                                 * under the frequency, which also pulls the
                                 * bottom of the layout up */
#define H_INFO      17
#define Y_SMET      121         /* row 2: the S-meter, on its own line */
#define H_SMET      18
#define Y_PLOT      146
#define PLOT_H      45                  /* 0.8x of 56: the trace is a convenience,
                                         * the history is what gets read */
/* 196, not 192: LVGL merges dirty areas that are adjacent or nearly so, so
 * with the waterfall starting one row below the trace every update repainted
 * the two together as one 146..232 block (~40 KB) instead of each on its own.
 * A visible gap keeps them separate, which is the whole point of measuring
 * whether the trace or the history is the expensive one. */
#define Y_WF        196
#define WF_H        40                  /* 240*40*2 = 18.8 KB of .bss.  60 rows was
                                         * tried and took the low-water heap mark
                                         * to 3 KB, too little for the TLS
                                         * handshake PTT needs. */
#define Y_SCALE     254
#define Y_CHIPS     272
#define H_CHIPS     22
#define Y_TOAST     272   /* its own line above the scale row */

#define BINS        850

static lv_obj_t *s_bar_top, *s_mode, *s_vfo, *s_batt;
static lv_obj_t *s_bar[4];   /* RSSI meter: four rising bars */
static lv_obj_t *s_freq;
static lv_obj_t *s_sunit, *s_sbar, *s_fil, *s_af;
static lv_obj_t *s_bandplate, *s_spanplate;
static lv_obj_t *s_spec_line, *s_wf_img;
static lv_obj_t *s_sc_l, *s_sc_c, *s_sc_r;
static lv_obj_t *s_chip_mode, *s_chip_tx, *s_chip_link;

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
/* Only SCR_W vertices are ever handed to lv_line — the 850 bins are
 * peak-downsampled to one per column before this.  The array was sized
 * max(BINS, SCR_W) = 850, which wasted 4880 bytes of .bss for points that were
 * never passed to the renderer. */
static lv_point_precise_t s_pts[SCR_W];
static uint16_t s_wf_buf[SCR_W * WF_H];

/* Waterfall scrolling: sweeps arrive at ~5.3 Hz while the UI ticks at 5 Hz.
 * Pushing a row only when data lands makes the history jump, so the previous
 * and current sweeps are interpolated once per tick instead. */
static volatile uint8_t s_bins[BINS];
static volatile bool s_bins_new;
static uint8_t s_cols[SCR_W];
static uint8_t s_cur[SCR_W];
static bool s_started;

/* Handheld trace model: 30 % new data per frame, then a 2x amplitude boost.
 *
 * The EMA runs in Q8 fixed point.  This part has no FPU, so 0.30f costs a
 * software float multiply-add per column per frame; 77/256 = 0.30078 is within
 * 0.3 % of the same factor in one integer multiply and a shift.  (The absolute
 * saving is small — 240 columns at 3.3 Hz — but it is free and keeps float out
 * of the render loop.)
 *
 * The input must be shifted into the same Q8 scale: adding the raw value was
 * the first version's bug, and it made the accumulator decay to zero, so the
 * trace drew as a flat line at the bottom. */
#define FFT_ALPHA_Q8   77       /* 77/256  = 0.30078 */
#define FFT_ONE_Q8    256
#define FFT_BOOST_X2    2
static uint16_t s_fft[SCR_W];   /* EMA accumulator, Q8 */
static bool s_fft_primed;

static uint32_t s_last_seq = 0xFFFFFFFF;
static uint32_t s_last_meter = 0xFFFFFFFF;
static int s_batt_pct = -1;
static bool s_batt_charging;
static ui_mode_t s_ui_mode = UI_MODE_FREQ;
static int s_spk_pct = 60;
/* Generic knob position: speaker % in VOL mode, microphone dB in MIC mode. */
static int s_mode_value = 60;

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
/* Signal strength as four vertical bars of rising height, in plain white —
 * the familiar phone/desktop Wi-Fi meter.  Replaces the coloured block glyphs:
 * green/orange/red made a status icon look like an alarm, and block glyphs are
 * not aligned on a common baseline so they never read as a meter.
 *
 * Drawn as real rectangles rather than font glyphs so the bars share a baseline
 * and their widths stay equal at any font scale. */
static void draw_rssi(int rssi, bool up)
{
    const int level = (!up || rssi == 0) ? 0
                    : (rssi > -55 ? 4 : (rssi > -65 ? 3 : (rssi > -75 ? 2 : 1)));

    for (int i = 0; i < 4; i++) {
        /* Lit bars are white; unlit ones stay the field grey so the meter's full
         * extent is still readable as a scale. */
        lv_obj_set_style_bg_color(s_bar[i],
            i < level ? lv_color_hex(C_TEXT) : lv_color_hex(C_FIELD), 0);
    }
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
    s_mode = plain(&lv_font_montserrat_12, lv_color_hex(C_ORANGE), 8, 8);
    lv_label_set_text(s_mode, "---");
    /* Right-anchored, widest first: the battery sits against the panel edge,
     * RSSI bars to its left, VFO label left of those. */
    s_batt = plain(&lv_font_montserrat_12, lv_color_hex(C_ORANGE), SCR_W - 44, 8);
    lv_obj_set_style_text_align(s_batt, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_size(s_batt, 40, 16);
    /* Four bars, 3 px wide with 2 px gaps, bottoms aligned at a common
     * baseline — a real meter rather than coloured font glyphs. */
    for (int i = 0; i < 4; i++) {
        s_bar[i] = lv_obj_create(lv_screen_active());
        /* Height grows left to right, bottoms on a shared baseline at y=20:
         * y = baseline - height, so all four line up along the bottom edge. */
        const int h = 4 + i * 3;
        lv_obj_set_size(s_bar[i], 3, h);
        lv_obj_set_pos(s_bar[i], SCR_W - 82 + i * 5, 20 - h);
        lv_obj_set_style_radius(s_bar[i], 1, 0);
        lv_obj_set_style_border_width(s_bar[i], 0, 0);
        lv_obj_set_style_bg_opa(s_bar[i], LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(s_bar[i], lv_color_hex(C_FIELD), 0);
        lv_obj_remove_flag(s_bar[i], LV_OBJ_FLAG_SCROLLABLE);
    }
    s_vfo  = plain(&lv_font_montserrat_12, lv_color_hex(C_ORANGE), SCR_W - 172, 8);
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

    /* Mirror of the header rule just above the chips bar, so the two chrome
     * rows are enclosed the same way. */
    lv_obj_t *rule_bot = lv_obj_create(lv_screen_active());
    lv_obj_set_size(rule_bot, SCR_W, 1);
    lv_obj_set_pos(rule_bot, 0, Y_CHIPS - 2);
    lv_obj_set_style_radius(rule_bot, 0, 0);
    lv_obj_set_style_bg_color(rule_bot, lv_color_hex(C_ORANGE), 0);
    lv_obj_set_style_bg_opa(rule_bot, LV_OPA_COVER, 0);
    lv_obj_remove_flag(rule_bot, LV_OBJ_FLAG_SCROLLABLE);

    /* ── frequency ── */
    s_freq = plain(&lv_font_montserrat_44, lv_color_hex(C_TEXT), 0, Y_FREQ);
    lv_obj_align(s_freq, LV_ALIGN_TOP_MID, 0, Y_FREQ);
    /* Native Montserrat 44, no scaling.
     *
     * Scaling 28 through transform_scale 384 did not grow the object, so 42 px
     * of glyph drew into a 28 px box and LVGL clipped it to the top half;
     * enlarging the box then blanked it entirely against this display's
     * rounded-corner mask.  A real 44 px face is stable and crisp; 28 is no
     * longer used anywhere, so one face replaces the other. */
    lv_obj_set_width(s_freq, SCR_W);
    lv_obj_set_height(s_freq, 52);
    lv_obj_set_style_text_align(s_freq, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_freq, "---.---");

    /* ── row 1: band name (or FREQ / VOL / channel plate), then FIL, AF, SPAN ──
     * The band label used to sit in the scope caption row; the operator wants it
     * beside the readouts that share its timescale. */
    /* One row, Montserrat 12 — the smallest compiled face.  The row must not
     * wrap, so the face is chosen for fit, not for stroke weight; plate widths
     * below total 238 of 240 px. */
    s_bandplate = field(&lv_font_montserrat_12, 2, Y_INFO, 52, false);
    lv_obj_set_style_text_color(s_bandplate, lv_color_hex(C_ACCENT), 0);
    lv_label_set_text(s_bandplate, "---");
    s_fil = field(&lv_font_montserrat_12, 57, Y_INFO, 58, false);
    /* Speaker level of THIS device, not the radio's own AF gain: af_gain is a
     * rig setting the terminal cannot change, so showing it was misleading —
     * the number never responded to the VOL knob. */
    s_af  = field(&lv_font_montserrat_12, 118, Y_INFO, 62, false);
    s_spanplate = field(&lv_font_montserrat_12, 183, Y_INFO, 55, true);
    lv_label_set_text(s_spanplate, "--");

    /* ── row 2: S-meter alone — unit text left, bar filling the rest ── */
    s_sunit = field(&lv_font_montserrat_12, 2, Y_SMET, 52, false);
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
    s_sc_l = field(&lv_font_montserrat_12, 2, Y_SCALE, 54, true);
    s_sc_c = field(&lv_font_montserrat_12, 92, Y_SCALE, 56, false);
    s_sc_r = field(&lv_font_montserrat_12, 184, Y_SCALE, 54, true);

    /* ── bottom chips: mode | RX/TX | link ── */
    bar(Y_CHIPS, H_CHIPS);
    /* Three fixed, non-overlapping slots: knob mode left, RX/TX centred,
     * link state right.  Mixing an absolute x with LV_ALIGN_TOP_MID made the
     * mode text and the RX/TX text fight over the same place. */
    s_chip_mode = plain(&lv_font_montserrat_12, lv_color_hex(C_ORANGE), 8, Y_CHIPS + 2);
    lv_obj_set_width(s_chip_mode, 44);
    lv_label_set_text(s_chip_mode, "FREQ");

    s_chip_tx = plain(&lv_font_montserrat_12, lv_color_hex(C_ORANGE), 96, Y_CHIPS + 2);
    lv_obj_set_width(s_chip_tx, 48);
    lv_obj_set_style_text_align(s_chip_tx, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_chip_tx, "RX");

    s_chip_link = plain(&lv_font_montserrat_12, lv_color_hex(C_ORANGE), SCR_W - 52, Y_CHIPS + 2);
    lv_obj_set_width(s_chip_link, 52);
    lv_obj_set_style_text_align(s_chip_link, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_text(s_chip_link, "LINK");

    heat_init();
    memset(s_wf_buf, 0, sizeof s_wf_buf);
}

/* Mirrors the handheld's fft_update(): EMA over the peak-downsampled columns. */
static void fft_update(const uint8_t *cols)
{
    const uint32_t keep = FFT_ONE_Q8 - FFT_ALPHA_Q8;   /* 179/256 */
    for (int x = 0; x < SCR_W; x++) {
        const uint32_t in = (uint32_t)cols[x] << 8;    /* into Q8 */
        if (!s_fft_primed) s_fft[x] = (uint16_t)in;
        else s_fft[x] = (uint16_t)(((uint32_t)s_fft[x] * keep
                                    + in * FFT_ALPHA_Q8) >> 8);
    }
    s_fft_primed = true;
}

static void update_plot(bool tx)
{
    /* Integer form of the original float mapping:
     *   mapped = (v - FLOOR) / (CEIL - FLOOR) * 2, clamped to 0..1
     *   y      = (PLOT_H-1) - mapped * (PLOT_H-1)
     * `t` holds the clamped position scaled by 65535 so both divisions stay
     * exact without float. */
    const int32_t range = SCOPE_CEIL - SCOPE_FLOOR;
    for (int x = 0; x < SCR_W; x++) {
        const int32_t v = (int32_t)s_fft[x] >> 8;      /* Q8 -> whole units */
        int32_t t = 0;
        if (v > SCOPE_FLOOR) {
            int64_t q = (int64_t)(v - SCOPE_FLOOR) * FFT_BOOST_X2 * 65535 / range;
            if (q > 65535) q = 65535;
            t = (int32_t)q;
        }
        s_pts[x].y = (PLOT_H - 1) - (int32_t)((int64_t)t * (PLOT_H - 1) / 65535);
    }
    lv_obj_set_style_line_color(s_spec_line,
                                lv_color_hex(tx ? C_RED : C_BLUE), 0);
    lv_obj_invalidate(s_spec_line);
}

/* Scroll one row and paint the newest through Jet. */
static void wf_push(void)
{
    /* Newest row on top, older rows pushed down — time flows downward, matching
     * the handheld (whose ring buffer draws row 0 as the newest sample). */
    memmove(s_wf_buf + SCR_W, s_wf_buf, sizeof(s_wf_buf) - SCR_W * sizeof(uint16_t));
    uint16_t *last = s_wf_buf;
    /* One measured sweep per row.  The tick matches the radio's sweep rate, so
     * there is nothing to interpolate between — the old code blended s_prev and
     * s_cur to fake the rows the slow tick could not produce. */
    for (int x = 0; x < SCR_W; x++)
        last[x] = s_heat[BINS_TO_IDX(s_cur[x])];
    lv_obj_invalidate(s_wf_img);
}

/* Setting a label's text invalidates it even when the string is identical, and
 * the diag line showed the frequency and the info row together accounting for
 * 40% of every repaint.  Compare first: a label that did not change costs
 * nothing.  Same rule the chip row already followed. */
static void label_set_if_changed(lv_obj_t *lbl, const char *text)
{
    if (!lbl || !text) return;
    const char *cur = lv_label_get_text(lbl);
    if (cur && strcmp(cur, text) == 0) return;
    lv_label_set_text(lbl, text);
}

static void refresh(void)
{
    /* May write to the socket, so it runs before any LVGL work. */
    mrrc_net_chan_pump();
    mrrc_net_audio_pump();

    mrrc_state_t st;
    mrrc_state_lock();
    st = g_radio;
    mrrc_state_unlock();

    const bool main_changed = st.seq != s_last_seq;
    const bool meter_changed = st.meter_seq != s_last_meter;
    const bool tx = st.tx_status > 0;
    char b[24];

    /* The knob mode is ours, not the radio's: set it outside the main_changed
     * branch so it can never lag behind the keys by a server frame. */
    {
        const char *m = s_ui_mode == UI_MODE_VOL   ? "VOL"
                      : s_ui_mode == UI_MODE_MIC   ? "MIC"
                      : s_ui_mode == UI_MODE_CHANNEL ? "CH" : "FREQ";
        if (strcmp(m, lv_label_get_text(s_chip_mode)) != 0) {
            lv_label_set_text(s_chip_mode, m);
        }
    }

    if (main_changed) {
        format_freq(b, sizeof b, st.freq_hz);
        label_set_if_changed(s_freq, b);
        /* header: mode left, VFO / battery / wifi right */
        label_set_if_changed(s_mode, st.mode_name[0] ? st.mode_name : "---");
        snprintf(b, sizeof b, "VFO %c", st.vfo ? st.vfo : 'A');
        label_set_if_changed(s_vfo, b);
        /* Live scope span: drives this plate and the three axis labels below. */
        int32_t span = span_hz(st.scope_span);
        char spanb[16];
        format_span(spanb, sizeof spanb, span);
        label_set_if_changed(s_spanplate, spanb);

        /* Leftmost plate carries whatever the knob is pointed at — band name in
         * FREQ mode, channel name in CH, speaker level in VOL. */
        if (s_ui_mode == UI_MODE_VOL) {
            snprintf(b, sizeof b, "%d%%", s_mode_value);
            label_set_if_changed(s_bandplate, b);
        } else if (s_ui_mode == UI_MODE_MIC) {
            snprintf(b, sizeof b, "%d", s_mode_value);
            label_set_if_changed(s_bandplate, b);
        } else if (s_ui_mode == UI_MODE_CHANNEL && s_chan_lbl[0]) {
            label_set_if_changed(s_bandplate, s_chan_lbl);
        } else if (st.band_name[0]) {
            label_set_if_changed(s_bandplate, st.band_name);
        }

        /* RSSI and battery do not move at 5 Hz — refresh them about every 2 s. */
        if ((lv_tick_get() / 200) % 10 == 0) {
            draw_rssi(mrrc_net_rssi(), mrrc_net_up());
            update_battery();
        }

        /* chips bar: knob mode | RX or TX seconds | link */
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
        if (tx) {
            /* On TX this plate shows SWR instead of the filter width: it is the
             * reading that protects the final, and the filter is not being
             * adjusted mid-transmission. */
            if (st.swr > 0.0f) snprintf(b, sizeof b, "SW %.1f", (double)st.swr);
            else               snprintf(b, sizeof b, "SW --");
            lv_label_set_text(s_fil, b);
        } else if (st.filter_hz > 0) {
            /* "FL" not "FIL": the full word pushes the plate past its 62 px and
             * the row wraps onto the S-meter, but a bare value is cryptic. */
            snprintf(b, sizeof b, "FL %d.%dK", st.filter_hz / 1000,
                     (st.filter_hz % 1000) / 100);
            lv_label_set_text(s_fil, b);
        } else {
            lv_label_set_text(s_fil, "");
        }
        /* The right-hand plate shows the audio level the knob is NOT
         * currently moving, so both are always visible at a glance. */
        if (s_ui_mode == UI_MODE_MIC) {
            snprintf(b, sizeof b, "SPK %d%%", s_spk_pct);
        } else {
            /* No "dB" suffix: the plate is 62 px wide and "MIC 24dB" wrapped
             * onto the S-meter row.  The number alone fits and the mode chip
             * already says MIC. */
            snprintf(b, sizeof b, "MIC %d", bsp_audio_get_mic_gain_db());
        }
        lv_label_set_text(s_af, b);

        /* scale row around the VFO */
        const int32_t half = span / 2;
        format_mhz(b, sizeof b, st.freq_hz - half);  lv_label_set_text(s_sc_l, b);
        format_mhz(b, sizeof b, st.freq_hz);         lv_label_set_text(s_sc_c, b);
        format_mhz(b, sizeof b, st.freq_hz + half);  lv_label_set_text(s_sc_r, b);

        s_last_seq = st.seq;
    }

    if (meter_changed || main_changed) {
        /* This row carries whichever meter matters right now: the S-meter while
         * receiving, and power / SWR while transmitting.  The bar is filled from
         * a scaled version of that same quantity, so it stays meaningful in
         * both directions.
         *
         * SWR is the number that matters on TX — an operator watching power
         * alone can burn a final by feeding a mismatched antenna. */
        if (tx) {
            if (st.power_w >= 0.0f) snprintf(b, sizeof b, "%dW", (int)(st.power_w + 0.5f));
            else                    snprintf(b, sizeof b, "--W");
            lv_label_set_text(s_sunit, b);

            /* Bar shows forward power on a 0..150 W scale, which covers this
             * radio's 100 W and still leaves headroom to spot an overshoot. */
            int pw = (int)(st.power_w < 0.0f ? 0.0f : st.power_w);
            if (pw > 150) pw = 150;
            lv_bar_set_value(s_sbar, pw * 255 / 150, LV_ANIM_OFF);
            lv_obj_set_style_bg_color(s_sbar, lv_color_hex(C_RED), LV_PART_INDICATOR);
        } else {
            /* S number and bar share the row: number is the server's s_unit
             * ("9", "+10", "5"), bar is the raw 0..255 level. */
            if (st.s_unit[0] == '+') snprintf(b, sizeof b, "S9%s", st.s_unit);
            else snprintf(b, sizeof b, "%s", st.s_unit[0] ? st.s_unit : "-");
            lv_label_set_text(s_sunit, b);
            lv_bar_set_value(s_sbar, st.s_meter, LV_ANIM_OFF);
            lv_obj_set_style_bg_color(s_sbar, lv_color_hex(C_ACCENT), LV_PART_INDICATOR);
        }
        s_last_meter = st.meter_seq;
    }

    bool got_sweep = false;
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
        memcpy(s_cur, s_cols, SCR_W);
        s_started = true;
        fft_update(s_cols);
        got_sweep = true;
    }

    /* Skip the scope while transmitting.
     *
     * The radio is not receiving, and the spectrum tier is dropped to "low" for
     * the duration, so there is nothing worth drawing.  LVGL is the largest CPU
     * consumer on this chip (measured 49% at 10 Hz ticks, 27% at 5 Hz); every
     * cycle it takes is one the WebSocket task does not get, and starving that
     * task is what dropped the link mid-transmission. */
    /* Only redraw when a sweep actually arrived.  The tick and the sweep rate
     * are matched now, so this is normally once per tick; the guard just avoids
     * repainting the same data when they drift out of phase. */
    if (got_sweep && !tx) {
        update_plot(tx);
        /* One row per sweep, measured. */
        wf_push();
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

/* `value` is the knob position for whichever mode is active: speaker percent
 * in VOL, microphone dB in MIC (the refresh() branch decides how to label it). */
void mrrc_ui_set_mode(ui_mode_t mode, int value)
{
    if (s_ui_mode == mode && s_mode_value == value) return;
    s_ui_mode = mode;
    s_mode_value = value;
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

/* Kept for callers, but the mode readout now lives solely in the chips bar:
 * showing it twice (orange text plus the bar) meant the two could disagree for
 * a frame after a mode change, which read as a glitch. */
void mrrc_ui_set_toast(const char *text)
{
    (void)text;
}
