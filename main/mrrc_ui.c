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

#define REFRESH_MS 200

static lv_obj_t *s_freq;
static lv_obj_t *s_mode;
static lv_obj_t *s_band;
static lv_obj_t *s_sunit;
static lv_obj_t *s_sbar;
static lv_obj_t *s_status;
static lv_obj_t *s_link;
static lv_obj_t *s_batt;
static lv_obj_t *s_toast;

/* ── scope ─────────────────────────────────────────────────────────────
 * One 850-bin frame arrives ~5 times per second on /WSterminal.  Rendering is
 * done into two L8 (8-bit luminance) buffers: RGB565 for both would need 38 KB
 * and this board has ~36 KB of largest free block.
 *   spectrum: 240 x SCOPE_H    trend of the current sweep
 *   waterfall: 240 x WF_H      history, newest row on top
 */
#define SCOPE_W   240
#define SCOPE_H    84          /* lv_line: vertex range only, costs no RAM */
#define WF_H       30          /* RGB565: 240*30*2 = 14.4 KB */
#define BINS      850

/* Colours ported from the xz-02 firmware (mrrc-handheld/main/ui.c), which in
 * turn ports the web client's WF_PALETTES.jet.  The spectrum trace is that
 * firmware's C_BLUE (0x3C1E); the waterfall uses the 64-step Jet scale. */
#define C_SCOPE_BLUE  0x3C1E
#define C_SCOPE_BG    0x0000

static uint16_t s_heat[64];

/* Jet: v=0 is the palette's dark blue floor, not black — a black waterfall
 * reads as "no data". */
static void heat_init(void)
{
    for (int i = 0; i < 64; i++) {
        const float v = i / 63.0f;
        int r, g, b;
        if (v < 0.125f) {
            const float u = v / 0.125f;
            r = 0; g = 0; b = (int)(128 + u * 127);
        } else if (v < 0.375f) {
            const float u = (v - 0.125f) / 0.25f;
            r = 0; g = (int)(u * 255); b = 255;
        } else if (v < 0.625f) {
            const float u = (v - 0.375f) / 0.25f;
            r = (int)(u * 255); g = 255; b = (int)(255 * (1 - u));
        } else if (v < 0.875f) {
            const float u = (v - 0.625f) / 0.25f;
            r = 255; g = (int)(255 * (1 - u)); b = 0;
        } else {
            const float u = (v - 0.875f) / 0.125f;
            r = (int)(255 * (1 - u * 0.5f)); g = 0; b = 0;
        }
        s_heat[i] = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
    }
}

/* Fixed floor/ceiling, same as the browser defaults (Floor 71 / Ceil 233).
 * Adaptive AGC made the trace jump whenever a single carrier appeared. */
#define SCOPE_FLOOR 71
#define SCOPE_CEIL  233

#define BINS_TO_IDX(v) \
    ((int)((v) <= SCOPE_FLOOR ? 0 : ((v) >= SCOPE_CEIL ? 63 : \
     ((v) - SCOPE_FLOOR) * 63 / (SCOPE_CEIL - SCOPE_FLOOR))))

/* Spectrum: an lv_line stores only the 240 vertex coordinates (960 B) and LVGL
 * rasterises the stroke itself, so SCOPE_H costs nothing here. */
static lv_point_precise_t s_spec_pts[SCOPE_W];
static uint16_t s_wf_buf[SCOPE_W * WF_H];
static lv_obj_t *s_spec_img;
static lv_obj_t *s_wf_img;
static volatile uint8_t s_bins[BINS];
static volatile bool s_bins_new;
static uint8_t s_scope_cols[SCOPE_W];
/* Waterfall scrolling: frames arrive at ~5.3 Hz but the UI refreshes at 6.7 Hz.
 * Pushing a row only when data arrives makes the history jump; instead the
 * previous and current sweeps are interpolated once per refresh tick. */
static uint8_t s_prev_cols[SCOPE_W];
static uint8_t s_cur_cols[SCOPE_W];
static uint32_t s_wf_t;        /* 0..1000 progress from prev to cur */
static bool s_wf_started;
static uint8_t s_wf_div;      /* waterfall advances every other tick */

static uint32_t s_last_seq = 0xFFFFFFFF;
static uint32_t s_last_meter = 0xFFFFFFFF;
static int s_batt_pct = -1;
static ui_mode_t s_ui_mode = UI_MODE_FREQ;
static int s_spk_pct = 60;
static bool s_mode_dirty = true;

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

    s_mode = label(scr, &lv_font_montserrat_14, lv_color_hex(0xFFB300), 12, 2);
    lv_label_set_text(s_mode, "---");

    s_batt = label(scr, &lv_font_montserrat_14, lv_color_hex(0x9E9E9E), 166, 2);
    lv_label_set_text(s_batt, "BAT --");

    s_freq = label(scr, &lv_font_montserrat_28, lv_color_white(), 16, 30);
    /* 1.5x via transform: the LVGL Kconfig in this build does not expose a
     * 40 px Montserrat (confgen drops the symbol), and scaling the label keeps
     * the same glyph shapes anyway.  256 = 1.0x. */
    lv_obj_set_style_transform_scale(s_freq, 384, 0);
    lv_obj_set_style_transform_pivot_x(s_freq, 0, 0);
    lv_obj_set_style_transform_pivot_y(s_freq, 0, 0);
    lv_label_set_text(s_freq, "---.---");

    s_band = label(scr, &lv_font_montserrat_14, lv_color_hex(0x4FC3F7), 0, 2);
    lv_obj_align(s_band, LV_ALIGN_TOP_MID, 0, 2);
    lv_label_set_text(s_band, "----");

    s_sunit = label(scr, &lv_font_montserrat_20, lv_color_hex(0x8BC34A), 12, 86);
    lv_label_set_text(s_sunit, "S--");

    /* S-meter: 0..255 maps to the bar. */
    s_sbar = lv_bar_create(scr);
    lv_obj_set_size(s_sbar, 216, 8);
    lv_obj_set_pos(s_sbar, 12, 112);
    lv_bar_set_range(s_sbar, 0, 255);
    lv_obj_set_style_bg_color(s_sbar, lv_color_hex(0x263238), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_sbar, lv_color_hex(0x66BB6A), LV_PART_INDICATOR);
    lv_bar_set_value(s_sbar, 0, LV_ANIM_OFF);

    s_spec_img = lv_line_create(scr);
    lv_obj_set_pos(s_spec_img, 0, 124);
    lv_obj_set_size(s_spec_img, SCOPE_W, SCOPE_H);
    lv_obj_set_style_line_width(s_spec_img, 2, 0);
    lv_obj_set_style_line_color(s_spec_img, lv_color_hex(0x3B82F6), 0);
    lv_obj_set_style_line_rounded(s_spec_img, true, 0);
    for (int x = 0; x < SCOPE_W; x++) {
        s_spec_pts[x].x = x;
        s_spec_pts[x].y = SCOPE_H - 1;
    }
    lv_line_set_points(s_spec_img, s_spec_pts, SCOPE_W);

    /* lv_canvas rather than lv_image: a canvas owns its buffer and LVGL
     * re-reads it on invalidate, which is the supported path for pixels this
     * code writes directly. */
    s_wf_img = lv_canvas_create(scr);
    lv_canvas_set_buffer(s_wf_img, s_wf_buf, SCOPE_W, WF_H, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_pos(s_wf_img, 0, 212);

    s_status = label(scr, &lv_font_montserrat_14, lv_color_hex(0x4FC3F7), 12, 272);
    lv_label_set_text(s_status, "RX");
    s_link = label(scr, &lv_font_montserrat_14, lv_color_hex(0x9E9E9E), 168, 272);
    lv_label_set_text(s_link, "LINK --");
    /* dim grid so the empty scope does not read as "no data". */
    heat_init();
    for (int i = 0; i < SCOPE_W * WF_H; i++) s_wf_buf[i] = 0;

    s_toast = label(scr, &lv_font_montserrat_14, lv_color_hex(0xEF5350), 12, 294);
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

        /* UP/DOWN drive either the tuning knob or the speaker; show which. */
        if (s_ui_mode == UI_MODE_VOL) {
            snprintf(buf, sizeof buf, "SPK %d%%", s_spk_pct);
            lv_label_set_text(s_band, buf);
            lv_obj_set_style_text_color(s_mode, lv_color_hex(0x4FC3F7), 0);
        } else if (s_ui_mode == UI_MODE_CHANNEL) {
            extern int g_chan_count;
            lv_label_set_text(s_band, "CH");
            lv_obj_set_style_text_color(s_mode, lv_color_hex(0x66BB6A), 0);
            (void)g_chan_count;
        } else {
            lv_label_set_text(s_band, st.band_name[0] ? st.band_name : "----");
            lv_obj_set_style_text_color(s_mode, lv_color_hex(0xFFB300), 0);
        }
        const char *tx = st.tx_status == 1 ? "TX" : (st.tx_status == 2 ? "TUNE" : "RX");
        snprintf(buf, sizeof buf, "%s%s", tx, st.radio_online ? "" : " ?");
        lv_label_set_text(s_status, buf);
        snprintf(buf, sizeof buf, "LINK %s", mrrc_net_up() ? "up" : "--");
        lv_label_set_text(s_link, buf);
        lv_obj_set_style_text_color(s_link,
            mrrc_net_up() ? lv_color_hex(0x66BB6A) : lv_color_hex(0xEF5350), 0);
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

    if (s_bins_new) {
        s_bins_new = false;
        /* Downsample 850 bins to 240 columns by peak, which keeps narrow
         * signals visible instead of averaging them away. */
        for (int x = 0; x < SCOPE_W; x++) {
            const int lo = (int)((long)x * BINS / SCOPE_W);
            int hi = (int)((long)(x + 1) * BINS / SCOPE_W);
            if (hi <= lo) hi = lo + 1;
            uint8_t peak = 0;
            for (int i = lo; i < hi && i < BINS; i++)
                if (s_bins[i] > peak) peak = s_bins[i];
            s_scope_cols[x] = peak;
        }
        if (s_wf_started) memcpy(s_prev_cols, s_cur_cols, SCOPE_W);
        else { memcpy(s_prev_cols, s_scope_cols, SCOPE_W); s_wf_started = true; }
        memcpy(s_cur_cols, s_scope_cols, SCOPE_W);
        s_wf_t = 0;

        /* waterfall: shift up one row, push the newest through the Jet scale */
        /* The waterfall is a 25.9 KB RGB565 image; redrawing it every tick
         * starves taskLVGL.  Every other tick keeps the motion smooth and
         * halves the SPI + render load. */
        if (++s_wf_div < 2) { if (s_spec_img) lv_obj_invalidate(s_spec_img); return; }
        s_wf_div = 0;
        memmove(s_wf_buf, s_wf_buf + SCOPE_W,
                sizeof(s_wf_buf) - SCOPE_W * sizeof(uint16_t));
        if (s_spec_img) lv_obj_invalidate(s_spec_img);
    }

    /* Spectrum and waterfall both advance on this tick.  Frames land at
     * ~5.3 Hz; interpolating from the previous sweep to the current one makes
     * the trace move smoothly instead of snapping 5 times a second. */
    if (s_wf_started) {
        for (int x = 0; x < SCOPE_W; x++) {
            const int v = s_prev_cols[x] +
                          ((int)s_cur_cols[x] - (int)s_prev_cols[x]) * (int)s_wf_t / 1000;
            const int n = BINS_TO_IDX(v) * 255 / 63;
            s_spec_pts[x].y = (SCOPE_H - 1) - (n * (SCOPE_H - 1)) / 255;
        }
        /* Only invalidate: the point array is already registered with the
         * object and is read at draw time.  Calling lv_line_set_points() every
         * tick recomputes the line's bounding box and, at this rate, starves
         * the idle task (task watchdog). */
        if (s_spec_img) lv_obj_invalidate(s_spec_img);
        s_wf_t += (uint32_t)(REFRESH_MS * 1000u) * 1000u / 189000u;   /* ~5.3 Hz */
        if (s_wf_t > 1000) s_wf_t = 1000;
        /* Two rows per tick: at 5 ticks/s that is 10 rows/s, so the 54-row
         * history scrolls in ~5 s.  One row per tick looked frozen. */
        for (int r = 0; r < 2; r++) {
            memmove(s_wf_buf, s_wf_buf + SCOPE_W,
                    sizeof(s_wf_buf) - SCOPE_W * sizeof(uint16_t));
            uint16_t *last = s_wf_buf + (WF_H - 1) * SCOPE_W;
            const int t = ((int)s_wf_t * (r + 1)) / 2;
            for (int x = 0; x < SCOPE_W; x++) {
                const int v = s_prev_cols[x] +
                              ((int)s_cur_cols[x] - (int)s_prev_cols[x]) * t / 1000;
                const int idx = BINS_TO_IDX(v);
                last[x] = (idx > 0) ? s_heat[idx] : 0x0000;
            }
        }
        if (s_wf_img) lv_obj_invalidate(s_wf_img);
    }

    if (s_batt_pct >= 0) {
        char buf[16];
        snprintf(buf, sizeof buf, "BAT %d%%", s_batt_pct);
        lv_label_set_text(s_batt, buf);
    }
}

static void ui_timer_cb(lv_timer_t *t)
{
    (void)t;
    refresh();
}


void mrrc_ui_start(void)
{
    if (!bsp_lvgl_lock(1000)) return;
    build_screen();
    bsp_lvgl_unlock();
    /* Runs inside the LVGL task, in step with the renderer.  A separate
     * low-priority task lost the CPU to audio/Wi-Fi and made the scope visibly
     * stutter once per second. */
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
    /* Force the main-field branch on the next refresh. */
    s_last_seq = 0xFFFFFFFF;
    bsp_lvgl_unlock();
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
