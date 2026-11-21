<p align="right">
  <a href="UI.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# UI layout and styling

How the MRRC Passport screen (240×320, ST7789, rounded panel) is built today.
Every coordinate, font size, and colour here matches `main/mrrc_ui.c`; when you
change the code, change this document too.

---

## 1. Canvas and refresh

| Item | Value | Note |
|---|---|---|
| Resolution | 240 × 320 | portrait |
| Colour format | RGB565 | `LV_COLOR_FORMAT_RGB565` |
| Refresh period | `REFRESH_MS = 190` (5.26 Hz) | see below |
| LVGL draw buffer | 24 lines (11,520 B) | `BSP_LVGL_DRAW_BUFFER_LINES` |
| Double buffering | off | not enough RAM; rendering and SPI are serialised |

**Why 5.26 Hz**: it matches the radio's own sweep rate, so every waterfall row is
measured data instead of an interpolation. The cost is CPU: `taskLVGL` went from
23% (at 300 ms) to 33–42%. 200 ms had been tried earlier and was too aggressive —
`taskLVGL` took 47–55% and the audio task, which must feed a blocking I2S write
every 20 ms, got under 1%, so the ring overflowed and RX broke up.

---

## 2. Palette

**Every colour constant is a 24-bit `0xRRGGBB`, not RGB565.** They are handed to
`lv_color_hex()` and converted at runtime.

> Trap we hit: RGB565 literals copied straight out of the handheld's `ui.c` were
> fed to `lv_color_hex()`, which read them as the low 16 bits of an RGB24 value.
> The whole palette shifted: `0xFFFF` (white) became `0x00FFFF` (cyan) and
> `0x3C1E` (blue) became `0x003C1E` (dark green).

| Macro | Value | Use |
|---|---|---|
| `C_BAR` | `0x2A2A2A` | top/bottom chrome bar background |
| `C_ORANGE` | `0xFFA500` | rules and top/bottom row text |
| `C_FIELD` | `0x3A3A3A` | field plate (light) |
| `C_FIELD_D` | `0x212121` | field plate (dark) |
| `C_ACCENT` | `0xE87A00` | S-meter bar |
| `C_FAINT` | `0x6B4A00` | very faint marks |
| `C_TEXT` | `0xFFFFFF` | primary text (frequency) |
| `C_RED` | `0xFF0000` | TX state |
| `C_GREEN` | `0x00FF00` | charging, "good" signal tier |
| `C_BLUE` | `0x3B82F6` | spectrum trace |

---

## 3. Vertical layout

```
y=  0  ┌────────────────────────────────┐
       │ 26 px chrome bar (text at y=8)  │
y= 26  ├────────────────────────────────┤ <- orange rule (rule)
y= 46  │        7.079.51   <- 44 px white│
y=102  │ [40m] [FL 2.4K] [SPK 60%] [200K]│  <- 12 px
y=121  │ [S 9+20][██████████░░░░░░░░░░]  │  <- S-meter, a row of its own
       │         (5 rows of black gap)    │
y=146  │  ╱╲    ╱╲╱╲    ╱╲    <- trace    │  PLOT_H=45
y=191  │        (5 rows of black gap)     │
y=196  │ ▒▓█ Jet waterfall, 40 rows       │  WF_H=40
y=236  │                                 │
y=254  │ [7.029]     [7.079]     [7.129] │  <- scale
y=270  ├────────────────────────────────┤ <- orange rule (rule_bot)
y=272  │ FREQ          RX          LINK  │  <- chips bar
y=294  └────────────────────────────────┘
```

### Constants

| Macro | Value | Content |
|---|---|---|
| `SCR_W` / `SCR_H` | 240 / 320 | |
| `H_HDR` | 26 | header height |
| `Y_FREQ` | 46 | frequency |
| `Y_INFO` | 102 | row 1: band / FL / SPK / SPAN |
| `H_INFO` | 17 | |
| `Y_SMET` | 121 | row 2: S-meter |
| `H_SMET` | 18 | |
| `Y_PLOT` | 146 | trace origin |
| `PLOT_H` | 45 | trace height |
| `Y_WF` | 196 | waterfall origin |
| `WF_H` | 40 | waterfall rows (18.8 KB of `.bss`) |
| `Y_SCALE` | 254 | scale row |
| `Y_CHIPS` | 272 | bottom status bar |
| `H_CHIPS` | 22 | |

### Why there is a 5-row black gap between trace and waterfall

`Y_PLOT + PLOT_H = 191` and `Y_WF = 196`. LVGL **merges adjacent dirty areas**:
with the waterfall flush against the trace both are repainted as one block from
146 to 232 (about 40 KB). With the gap they repaint independently.

Measured gain: `taskLVGL` 44% → 35%. Helpful, not decisive.

---

## 4. Fonts

Only **two** Montserrat faces are compiled: **12** and **44** (12 is the default).

The configuration (`sdkconfig.defaults` and the build machine's `sdkconfig` must
agree):

```
CONFIG_LV_FONT_MONTSERRAT_12=y
CONFIG_LV_FONT_MONTSERRAT_44=y
CONFIG_LV_FONT_DEFAULT_MONTSERRAT_12=y
```

> We once built five faces (12/14/20/28/44). After moving to 12/44 nothing
> referenced 14 or 28, and 20 was never used at all. Each extra face is a few KB
> of flash — it does not affect `largest` because glyph bitmaps live in flash —
> but there is no reason to keep them.

| Use | Size | Note |
|---|---|---|
| Header (mode / VFO / battery) | 12 | orange |
| **Frequency** | **44** | white, native face |
| Four plates in row 1 | 12 | |
| S-meter digits | 12 | |
| Scale row | 12 | |
| Bottom chips | 12 | orange |
| Waterfall "SPAN" label | — | removed |

### Why the frequency uses a native 44 instead of scaling

We once used `lv_obj_set_style_transform_scale(s_freq, 384, 0)` to enlarge a 28 px
face by 1.5×. The result:

- `transform_scale` **does not enlarge the object itself**, so a 42 px glyph drew
  into a 28 px box and only the top half survived
- Making the box taller gave a **completely black screen** (it conflicts with this
  panel's rounded-mask callback, `rounded_flush_event`)

A native 44 px face is stable and crisp. The relevant settings:

```c
lv_obj_set_width(s_freq, SCR_W);
lv_obj_set_height(s_freq, 52);          /* line box, or descenders get clipped */
lv_obj_set_style_text_align(s_freq, LV_TEXT_ALIGN_CENTER, 0);
lv_obj_align(s_freq, LV_ALIGN_TOP_MID, 0, Y_FREQ);
```

**Order matters**: set the size before `lv_obj_align`, or the anchor is computed
from the old size.

---

## 5. Region by region

### 5.1 Header (y=8, orange 12 px)

| Element | x | Content |
|---|---|---|
| `s_mode` | 8 | `LSB` / `USB` … the radio mode |
| `s_vfo` | 172 | `VFO A` |
| RSSI bars (four) | from 158 | 3 px wide, 5 px pitch, bottom-aligned to y=20 |
| `s_batt` | 196 | `87%` |

**RSSI is rectangles, not text**: four `lv_obj`s of increasing height
(4/7/10/13 px), bottom-aligned, bright white against `C_FIELD` grey.

> We used `▂▄▆█` glyphs once, but their baseline is inconsistent and it did not
> read as a meter; colouring them made the status icon look like an alarm. They
> are white now.

### 5.2 Frequency (y=46)

Montserrat 44, white, centred horizontally. Format `7.079.51` (`format_freq`).

### 5.3 Row 1 (y=102, four plates, 12 px)

| Element | x | Width | Content |
|---|---|---|---|
| `s_bandplate` | 2 | 52 | band name / channel name / volume, depending on the knob mode |
| `s_fil` | 57 | 58 | `FL 2.4K` |
| `s_af` | 118 | 62 | `SPK 60%` |
| `s_spanplate` | 183 | 55 | `200K` (actual span) |

238 of 240 px used, no overlap.

> Width lesson: `FIL 2.4K` needs about 62 px at 14 px, so a 56 px plate **wrapped**
> and the second line landed on the S-meter row. At 12 px, `FL 2.4K` needs about
> 55 px and fits.

`s_bandplate` follows the knob mode:

| Mode | Shows |
|---|---|
| FREQ | band name (e.g. `40m`) |
| CHANNEL | channel name |
| VOL | volume percentage |

### 5.4 S-meter (y=121, a row of its own)

- `s_sunit`: x=2, width 52, the text `S 9+20` (12 px)
- `s_sbar`: x=62, width `SCR_W-66`, height `H_SMET-4`, range 0..255, bar colour `C_ACCENT`

> The user explicitly asked for "the S-meter on its own row, digits and bar on the
> same line".

### 5.5 Spectrum trace (y=146, height 45)

`lv_line`, `C_BLUE` (`C_RED` while transmitting), width 3, rounded joints.

> Rounding matters: with butt caps LVGL's anti-aliased segments leave gaps at
> steep transitions and the trace looks like **a row of dots**. This LVGL version
> has no separate cap style, so `line_rounded` enables both.

Data: 850 bins peak-downsampled to 240 columns (**peak, not average** — averaging
hides narrow signals). The mapping is fixed at `SCOPE_FLOOR=71` /
`SCOPE_CEIL=233`; no adaptive AGC, which would make the whole trace jump when a
single carrier appears.

### 5.6 Waterfall (y=196, 40 rows)

`lv_canvas` with an RGB565 buffer (`240×40×2 = 18.8 KB` of static `.bss`).

- **Newest row on top**, history scrolls down (same as the handheld's ring buffer)
- One row per real sweep frame (**no interpolation**)
- Jet palette, 64 entries; `idx 0` is deep blue rather than pure black (pure black
  reads as "no data")

> We tried 60 rows: it pushed `largest` to 3 KB, the PTT TLS handshake could not
> get contiguous memory, and the transmission dropped itself. 40 rows = 18.8 KB is
> the current balance.

### 5.7 Scale row (y=254)

Three plates at x=2 / 92 / 184, widths 54 / 56 / 54, showing `VFO-span/2`, `VFO`,
and `VFO+span/2`.

### 5.8 Bottom chips (y=272, orange 12 px)

Three fixed slots (so the layout methods cannot fight each other):

| Element | x | Width | Align | Content |
|---|---|---|---|---|
| `s_chip_mode` | 8 | 44 | left | `FREQ` / `CH` / `VOL` |
| `s_chip_tx` | 96 | 48 | centre | `RX` / `TX` / `TXT` (red) |
| `s_chip_link` | 188 | 52 | right | `LINK` / `noSRV` (red) |

> We once mixed absolute x with `LV_ALIGN_TOP_MID`, and the mode text fought the
> RX/TX text for the same position.

### 5.9 Rules

Two 1 px orange horizontal lines:

- `rule`: y=`H_HDR` (26), under the header
- `rule_bot`: y=`Y_CHIPS-2` (270), above the chips bar

---

## 6. Rounded panel

The panel corners are round. `rounded_flush_event` (`bsp_display_lvgl.c`) paints
pixels outside the corner radius black on `LV_EVENT_FLUSH_START`.

Consequence: **glyphs near y=0 get clipped**, which is why the header text sits at
y=8. `bsp_display_rounded_row_span()` returns the full width for interior rows and
only computes a span for the edge rows.

---

## 7. Knob modes and prompts

OK double-tap cycles `FREQ → CHANNEL → VOL → FREQ`, shown in exactly one place:
`s_chip_mode` in the bottom chips.

> An orange toast once showed the mode as well; the two write paths ran out of
> sync and one frame after a mode change showed contradictory text. The toast is
> gone (`mrrc_ui_set_toast()` is kept as an empty function).

---

## 8. Spectrum tiers (server negotiation)

On connect it sends `{"type":"spectrumCaps","profile":"high"}`.

| Tier | Frame size | Divider | Frame rate | Bandwidth |
|---|---|---|---|---|
| `high` (Full) | 1701 B | 1 | ~5.3 frames/s | ~73 kbit/s |
| `mid` | 851 B | 2 | ~2.7 frames/s | ~18 kbit/s |
| `low` | 426 B | 4 | ~1.3 frames/s | ~5 kbit/s |

Receive uses `high` and **switches to `low` while transmitting** (nobody is
watching the screen then; the bandwidth goes to the audio uplink).

> Early firmware hard-coded `mid` and the waterfall looked like it was crawling —
> each frame is one row of history, so half the rate is half the row rate.

---

## 9. Checklist when changing the UI

1. **Changing colours**: make sure the value handed to `lv_color_hex()` is 24-bit,
   not RGB565
2. **Changing text width**: keep the plate total ≤ 240 px with ≥ 2 px to spare, or
   it wraps onto the next row
3. **Changing vertical coordinates**: check the neighbours do not overlap and that
   the trace/waterfall gap survives
4. **Changing waterfall rows**: `WF_H × 240 × 2` is `.bss`, and check that
   `largest` stays above 30 KB
5. **Adding a font face**: each Montserrat face is a few KB of flash (not heap),
   but `sdkconfig` and `sdkconfig.defaults` must both be updated
6. **Changing the refresh rate**: the higher it is the more CPU LVGL takes, which
   directly causes audio stutter
7. **Do not use `transform_scale`**: on this panel it clips or blacks the screen

---

## 10. Related files

| File | Content |
|---|---|
| `main/mrrc_ui.c` | all layout, drawing, and refresh logic |
| `main/mrrc_ui.h` | public interface |
| `components/bsp/src/bsp_display_lvgl.c` | display init, draw buffer size, rounded mask |
| `components/bsp/src/bsp_display_rounding.c` | rounded-corner geometry |
| `docs/optimization-notes.md` | measured performance data and refuted hypotheses |
