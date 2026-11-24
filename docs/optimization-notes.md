<p align="right">
  <a href="optimization-notes.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Performance work and debugging notes

Measured data and conclusions for the MRRC Passport (ESP32-C3 terminal): audio
path, UI rendering, key handling. **The point is hindsight** — when a symptom
comes back, check here first for what was verified, what was a guess, and what
was refuted, so the same detour is not walked twice.

Labelling:

- **measured** — backed by hardware logs or timings
- **hypothesis** — unverified; do not treat as a conclusion
- **refuted** — once believed, later contradicted by data

> Current values live in §13 here and in [transport tuning](transport-tuning.md).
> Earlier sections are the record as it stood on the day, including numbers that
> have since moved.

---

## 0. Baseline

`1a99a2f` is the last commit where receive worked completely:

```
memChannels: 6 slots, 6 usable
audio profile pcm16 accepted (16000 Hz)
idle free=78K largest=52K  audio(frames=2512 err=0 drop=0)
```

**When a large change goes wrong, come back here first.**

---

## 1. Hardware and configuration limits

| Item | Value | Consequence |
|---|---|---|
| SoC | ESP32-C3, single core at 160 MHz | no second core to carry the network or the UI |
| PSRAM | none | every buffer comes out of internal RAM |
| Display | 240×320 ST7789, SPI 80 MHz | big-endian panel; needs `swap_bytes` |
| Codec | ES8311, 16 kHz | the microphone path is effectively dead at 48 kHz |
| `FREERTOS_HZ` | 1000 | 1 tick = 1 ms (this sets what every delay actually means) |
| Heap | ~400 KB SRAM, measured `largest` 40–60 KB | TLS needs ~16 KB **contiguous** |

### Fonts and memory

Each Montserrat face is a few KB of glyph bitmaps **in flash, not on the heap**.
Three faces were compiled at the time (12 / 20 / 44); there was once a six-face
build (12/14/16/20/28/40) whose `bin` was 37 KB larger — that is flash, and it
**does not change `largest`**. (Refuted: the belief that dropping font faces
would improve heap headroom.)

---

## 2. Audio path (PCM16, no Opus)

### 2.1 Why libopus went away (measured)

Removing it took `bin` from `0x186ce0` to `0x16f4b0` (about 92 KB) and moved the
heap from `free 44K / largest 17K` to `free 80K / largest 58K`. The C3 has no
headroom for Opus plus TLS plus LVGL.

### 2.2 Uplink frame format (important, cost us once)

Every binary frame on `/WSterminal` carries a **channel byte**:

```
[0x01][0x02][int16 PCM ...]
 ^chan  ^codec tag
```

- `0x01` = TX audio (uplink)
- `0x00` = RX audio (downlink)
- `0x02` = spectrum

**The bug**: the firmware sent `[0x02][PCM]` directly, the server read `0x02` as
the **spectrum channel** and discarded the PCM — heard as "transmits with no power
and no audio". Adding the channel byte fixed it immediately.

### 2.3 PCM16 profile negotiation

On connect it sends:

```json
{"type":"audioCaps","profile":"pcm16"}
```

The server answers `accepted:true` plus `rate:16000`. The field is **`profile`**,
not `rx`.

**Note**: the `/WSterminal` endpoint must support `audioCaps`; an early version
only honoured `spectrumCaps` and silently ignored the request (symptom: the reply
never arrives).

### 2.4 The three places downlink frames are lost

`rb_put()` in `mrrc_audio.c`:

| Site | Meaning | How to see it |
|---|---|---|
| `lock` | the mutex could not be taken (network callback against audio task) | `audio drops: lock=N` |
| `full` | the ring buffer is full | `audio drops: full=N` |

Logged once a minute:

```
audio drops: lock=0 full=26
```

**Measured conclusion**: nearly all drops were `full`, not `lock`.

### 2.5 Tried and not worth repeating

| Attempt | Result |
|---|---|
| 8 KB → 16 KB ring | **useless**. `full` kept growing and the drop rate did not move — not a capacity problem |
| spin + yield around `rb_put` | **harmful**. Drops went up (11% → 13%) |
| batched frame reads in `rb_get` | one run showed 10% → 2.3%, but it was mixed with other changes and never verified alone |

---

## 3. UI and LVGL (the main CPU consumer)

### 3.1 Typical measured CPU split

```
taskLVGL       35-44%     <- the largest consumer
IDLE           40-54%
esp_timer       2-3%
websocket       1-4%
wifi            1-2%
audio          <1%
```

`refresh()` — our own UI code — is only **3-9%**; the rest is inside LVGL.

### 3.2 Ruled out (measured)

Three control experiments, each disabling one redraw and reading `taskLVGL`:

| Experiment | taskLVGL |
|---|---|
| waterfall redraw disabled | 49% |
| spectrum redraw disabled | 43% |
| both disabled | 45% |

**Conclusion: CPU cost is essentially independent of what is drawn**; it is LVGL's
fixed per-run overhead. "Draw less" is therefore a dead end.

(The diagnostic switches `CONFIG_MRRC_UI_NO_WF` / `MRRC_UI_NO_PLOT` existed
temporarily and are gone.)

### 3.3 Ruled out after being believed

- **"`lv_timer_handler()` runs 1000 times a second" — refuted.** There is a
  `vTaskDelay(1)` inside `esp_lvgl_port`, which looked like a 1 ms loop. The loop
  already sleeps through `xEventGroupWaitBits(..., task_delay_ms)` as LVGL
  requires; the `vTaskDelay(1)` only adds one more millisecond. The real rate is
  about **20 per second**, not 1000.
- **"Trace and waterfall adjacency merges dirty areas" — partly true, not the
  main cause.** Moving `Y_WF` from 192 to 196 (5 rows of black gap) took
  `taskLVGL` from 44% to 35%: eight points, not decisive.

### 3.4 Draw buffer lines — the single biggest win

**Espressif's performance guide states the threshold**
(`esp_lvgl_port/docs/performance.md`):

> - Frame buffer size >25% of the screen does not bring relevant performance boost
> - Frame buffer size <10% will have **severe negative effect** on performance

240×320 = 76800 px, so 10% = 7680 px = **32 lines**.

We had been running at 3.8%–5.6%, exactly in the range the guide calls severe —
**that was the real reason `taskLVGL` ate half the CPU.**

| Lines | Share of screen | taskLVGL | largest |
|---|---|---|---|
| 8 | 3.8% | 58-60% | 46K |
| 12 | 5.6% | 43-49% | 44K |
| 32 + FAST_MEM_IRAM | 10.0% | 20-26% | 18K ← not enough headroom |
| **24 + FAST_MEM_IRAM (current)** | **7.5%** | **23%** | **24K** |

It settled at 24 lines: 32 is slightly better for CPU but leaves `largest` at
18 KB, only 2 KB above the ~16 KB PTT's TLS handshake needs — not safe on a chip
also running Wi-Fi, audio, and a renderer. Three CPU points bought back 6 KB of
contiguous memory.

This was paired with `CONFIG_LV_ATTRIBUTE_FAST_MEM_USE_IRAM=y` (Espressif's
separate acceleration item: the hottest LVGL functions go to IRAM and skip the
flash-cache wait).

⚠️ **Watch the cost**: `largest` fell from 38K to 24K and the boot-time minimum
reached `min=9K`. The PTT TLS handshake needs ~16 KB contiguous — 24K works but
without much margin. If transmission fails, step the buffer back first.

### 3.4b Configuration items already ruled out

| Item | Conclusion |
|---|---|
| all 12 pixel formats enabled | **done**: RGB565 only, `bin` 114 KB smaller. No CPU change (not on the hot path) |
| `LV_DRAW_SW_ASM_NONE` | **cannot change**: it is a four-way choice whose alternatives need NEON / Helium / RISC-V **Vector**. The ESP32-C3 is RV32IMC, no V extension |
| `swap_bytes = true` | **turned off**: the panel is told little-endian via RAMCTRL(0xB0). No CPU change — `lv_draw_sw_rgb565_swap` was never the bottleneck; it only saved 20k iterations per frame |
| `LV_USE_FLEX` / `GRID` | **cannot disable**: `lv_list`, `lv_menu`, `lv_calendar` and even bundled LVGL examples depend on them |
| `double_buffer` | **cannot enable**: doubling the buffer is 30 KB contiguous, competing with PTT's TLS |
| CPU at 240 MHz | not applicable: the C3 tops out at 160 MHz |

### 3.5 Changing draw buffer lines needs care

`BSP_LVGL_DRAW_BUFFER_LINES` from 8 to 20 (9.4 KB) reduces SPI transactions, but
it is **contiguous internal RAM**. Once combined with other memory changes it
pushed `largest` to 9 KB and the PTT TLS handshake failed — heard as
"transmission drops itself".

**Change one thing at a time and measure memory after each.**

### 3.6 UI refresh rate

The refresh period settled at **190 ms (5.26 Hz)**, matched to the radio's sweep
rate.

History: 200 ms → 100 ms doubled the CPU and was reverted; later it was dropped
to 300 ms (3.3 Hz) to save CPU. But 300 ms had a hidden cost: the data source
delivers 5.3 frames/s while the tick runs 3.3 times/s, and `s_bins_new` is a
single bool, so of the ~1.6 frames arriving between ticks only the last survived
— **about 38% of sweep frames were silently discarded**. To hide the mismatch the
waterfall interpolated two rows per tick from `s_prev`/`s_cur`, so most rows in
the history were synthesised.

| Tick | Rate | Data match | Waterfall content | taskLVGL |
|---|---|---|---|---|
| 300 ms | 3.3 Hz | 38% lost | 2 interpolated rows | 23% |
| **190 ms** | **5.26 Hz** | **nothing lost** | **every row measured** | **33%** |

The cost of 190 ms is CPU going 23% → 33% and `audio drops` starting to grow, but
listening showed no break-up (`err=0`), and it buys a trace that follows the knob
and an honest waterfall history. `update_plot`/`wf_push` now run only on
`got_sweep`, so old data is no longer repainted.

Spectrum tiers (`spectrumCaps`):

| Tier | Frame size | Divider | Frame rate |
|---|---|---|---|
| `high` (Full) | 1701 B | 1 | ~5.3 frames/s |
| `mid` | 851 B | 2 | ~2.7 frames/s |
| `low` | 426 B | 4 | ~1.3 frames/s |

Receive uses `high` and switches to `low` while transmitting (nobody watches the
screen then, and the bandwidth goes to the audio uplink).

**Trap**: early firmware hard-coded `mid` and the waterfall looked like it was
crawling — each frame is one row of history, so half the rate is half the row
rate.

---

## 4. Keys (PTT and double-click)

### 4.1 Real ADC key timing (measured)

| Action | Hold time reported by the driver |
|---|---|
| normal single tap | 169–246 ms |
| genuine long press (PTT) | 569–580 ms |
| **fast double tap (merged by the driver)** | **619–627 ms** |

The driver **merges the two taps of a fast double-click into one press** and
reports it as a long press — 40 ms away from a genuine PTT, **indistinguishable
by duration**. That is the root of "a double-click keys the transmitter".

### 4.2 Discrimination schemes that were tried

| Scheme | Result |
|---|---|
| check whether the previous tap's window is still open | unreliable: the window is set on release and may not be set yet when the second tap lands |
| decide by hold duration | unreliable: see the table, 619 vs 569 |
| require 900 ms of silence before PTT | works, but feels bad (after a double tap you must wait to talk) |
| uplink queue with a single writer | **introduced new problems, fully rolled back** |

### 4.3 Key events must use PRESS

`iot_button` emits `CLICK` only when the hold exceeds `short_press_time`
(180 ms); anything shorter is **silently dropped**. This board's ADC decoding
plus the BSP key lock put fast taps into that hole, which looked like "the button
does nothing".

**UP/DOWN must use `BSP_BTN_PRESS`** (fires on press, unconditional, no delay),
not `CLICK`.

### 4.4 Key polling period

`CONFIG_BUTTON_PERIOD_TIME_MS = 5` — `iot_button` polls every 5 ms, 200 times a
second. Measured `esp_timer` at 2–3% CPU: not the main cost, but a constant wake
source. Halving it to 10 ms is possible; note that `SHORT_TICKS`/`LONG_TICKS` are
computed from this value, so key decisions must be re-verified after the change.

---

## 5. Display and fonts

### 5.1 `transform_scale` is unreliable on this panel

`lv_obj_set_style_transform_scale(s_freq, 384, 0)` to scale a 28 px face by 1.5×:

- the glyph rendered at 42 px into a 28 px object → clipped to the **top half**
- making the object taller → **completely black screen** (conflicts with the
  rounded-mask callback)

**Use a native 44 px face**; it is stable and crisp. Do not revisit scaling.

### 5.2 Rounded panel

The panel corners are round, so glyphs at y=0 get clipped. Header text sits around
y=8 inside `H_HDR`.

### 5.3 Font size and plate width

Four plates in one row must be budgeted. `FIL 2.4K` needs ~62 px at 14 px; with
56 px plates it **wrapped and overlapped the S-meter below**. At 12 px,
`FL 2.4K` is about 55 px and fits.

---

## 6. Traps in the server interaction

### 6.1 What a disconnect really means

In the server log (`/opt/mrrc_modern/logs/server-stdout.log`):

```
TX audio ownership → PTT client
Terminal client disconnected during TX! Forcing RX.
Terminal client connected (2 total, ...)
```

**The device's WebSocket dropped; the server merely forced RX as its rule
requires.** It was not the server kicking us. Look at the firmware side (a starved
network task), not the server's PTT watchdog.

### 6.2 Use the right server

- **right**: the W103D box `w103d.vlsc.net`, service under `/opt/mrrc_modern`,
  log as above
- **wrong**: the local `~/HAM/mrrc/` (the old ALS-7400l stack, whose log says
  `rigctld not ready` — unrelated to the device under test)

### 6.3 Send failure codes

Return values of `esp_websocket_client_send_bin()`:

| Value | Meaning |
|---|---|
| `>0` | bytes actually written |
| `0` | nothing written (socket busy) |
| `-1` | could not take the client lock (**cross-task contention**) |
| `-2` | socket not connected |

`-1` means several tasks are writing the same WebSocket. An uplink queue was
introduced for this once and made things **worse** (full queue, fragmentation,
confused state machine) — **fully rolled back**.

---

## 7. Method (the traps worth remembering)

This section matters more than the numbers above.

1. **Change one thing at a time.** Once the draw buffer, the audio ring, and the
   locking strategy changed together; they interacted, nothing could be
   attributed, and all three were rolled back.

2. **Measure before changing.** Enlarging buffers, adding queues and rewriting
   locks on intuition all failed. Adding `audio drops: lock=/full=` counters
   showed in one glance that it was `full`, not `lock`.

3. **Distrust "saves memory" intuition.** Fonts live in flash, not the heap;
   removing a background colour saves nothing (it is only a style field); and
   `largest` is about **contiguous** space, so only shrinking a large `.bss`
   helps.

4. **A failed build must stop the chain.** In an `&&` chain, if `grep` finds an
   error but returns 0, the previous firmware gets flashed. Confirm
   `Project build complete` actually appeared before flashing.

5. **A vanished device is not always a code problem.** When `/dev/cu.usbmodem*`
   disappeared after "plugging it in", it was USB enumeration; replugging fixed
   it.

6. **Never edit `managed_components/`.** It is re-fetched. Write a
   `tools/patch_*.sh`, run it before the build, keep it idempotent.

---

## 8. Configuration that was known good at the time

| Item | Value |
|---|---|
| `REFRESH_MS` | 300 (3.3 Hz) |
| `WF_H` | 40 rows (18.8 KB) |
| `PLOT_H` | 45 px |
| `Y_WF` | 196 (5 rows of black gap below the trace) |
| `AUD_RB_BYTES` | 8192 (~250 ms) |
| `BSP_LVGL_DRAW_BUFFER_LINES` | 8 |
| Fonts | 12 / 20 / 44 |
| Spectrum tier | `high` receiving, `low` transmitting |
| TX frame | 20 ms at 16 kHz (320 samples) |

Measured:

```
free=67K  largest=44K  min=43K
IDLE 53%  taskLVGL 36%
audio drops: lock=0 full=5    (0.46%)
```

---

## 9. Next candidates (lowest risk first)

1. `CONFIG_BUTTON_PERIOD_TIME_MS` 5 → 10 (halve key polling)
2. disable `LV_DRAW_SW_ASM_NONE` so LVGL takes the optimised path
3. keep RGB565 as the only pixel format
4. draw buffer 8 → 12 or 16 (**measure `largest` afterwards**)
5. manual waterfall flush, bypassing LVGL's dirty-area mechanism (large change)

---

## 11. Build and statistics switches

| Item | Value | Conclusion |
|---|---|---|
| `CONFIG_COMPILER_OPTIMIZATION_PERF` | `-O2` | **keep**. Flash is 83% free, space is not the constraint; CPU is, and `-Os` costs 5-15% while `-O3` gains 2-5% with more aggressive-optimisation risk |
| `CONFIG_MRRC_PROFILE` | **off** | was on, dumping CPU/heap/decode once a minute |
| `CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS` | **off** | was on — **a timing hook on every FreeRTOS tick**, the largest standing cost during investigation |
| `OPTIMIZATION_ASSERTIONS_ENABLE` | on | keep; useful while debugging |
| `STACK_CHECK_MODE_NONE` | off | correct; saves overhead |

Diagnostic code added during investigation and since removed:

- the lock/full classification in `mrrc_audio_drop_detail()`
- LVGL event registration counting
- the `plot:` spectrum dump in `update_plot()`
- an unused `esp_log.h` in `mrrc_ui.c`

Kept: five `HEAP <stage>` lines during boot, and one
`idle free=... audio(frames=... err=... drop=...)` line per 60 s.

## 12. Approaches abandoned

### I8 palette waterfall (Stage 3 of the historical plan)

The goal was to halve the buffer (40 rows 18.8 KB → 9.4 KB), in theory allowing
60 rows again. LVGL's implementation does not permit it:

```c
void lv_draw_buf_set_palette(lv_draw_buf_t *draw_buf, uint8_t index, lv_color32_t color)
{
    lv_color32_t *palette = (lv_color32_t *)draw_buf->data;
    palette[index] = color;        /* the palette is written at the start of the pixel buffer */
}
```

**Palette and pixel data share one buffer**: the palette occupies the first 256
bytes while the waterfall writes indices from offset 0, so they overwrite each
other — the waterfall simply disappears. Supporting it would mean reserving space
for the palette and pointing the canvas at the shifted pixel area: more change
than gain.

### A Q8 fixed-point EMA mistake

The first version added raw values into the Q8 accumulator (forgetting `<< 8`);
the accumulator decayed to zero and the trace drew flat along the bottom. A
diagnostic print saved it:

```
plot: v=0..0  y=44..44  raw0=109 raw1=115    <- raw has values, all zero after >>8
```

## 13. Final configuration at a glance

```
BSP_LVGL_DRAW_BUFFER_LINES   24           draw buffer lines
CONFIG_LV_ATTRIBUTE_FAST_MEM_USE_IRAM  y LVGL hot functions into IRAM
FFT_ALPHA_Q8                 77 (=0.30)  Q8 fixed-point EMA
REFRESH_MS                   190         5.26 Hz (see §3.6)
WF_H                         40          waterfall rows (RGB565 18.8 KB)
PLOT_H                       45          trace height
AUD_RB_BYTES                 16384       ~0.51 s, plus a 160 ms pre-buffer (§16)
CONFIG_LWIP_TCP_WND_DEFAULT           11520  8 MSS, with the queues behind it (§16)
CONFIG_LWIP_TCP_SND_BUF_DEFAULT       11520  uplink, 8 MSS (§16)
CONFIG_LWIP_TCP_RECVMBOX_SIZE         8      was 6, the queue that actually filled
CONFIG_LWIP_TCP_OOSEQ_MAX_PBUFS       8      was 4
CONFIG_ESP_WIFI_RX_BA_WIN             16     was 6
swapp_bytes                  false       panel-side RAMCTRL(0xB0) little-endian
CONFIG_COMPILER_OPTIMIZATION_PERF      y  -O2
CONFIG_MRRC_PROFILE                     n
CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS n
```

Steady-state measurement at that point: `taskLVGL 23%`, `IDLE 56%`,
`free 55K / largest 25K`, `err=0`, drops stable and not growing.

> The values above are the current ones. §8 and the earlier sections keep the
> numbers from the day they were written.

---

## 14. Verification of external optimisation suggestions

An outside review made four suggestions. Each was checked; the results are here
so they are not debated twice.

### "Delete `s_prev` / `s_cur` / `s_t` and push one row per frame"

**Partly right.** `s_t` was already deleted with the 190 ms change and the
interpolation branch is bypassed by `t_permille >= 1000`. But the `s_prev` /
`s_cur` members remained, and were cleaned up:

- `s_prev[SCR_W]` deleted (240 bytes)
- `wf_push(int t_permille)` became `wf_push(void)`; the interpolation branch is gone
- `s_pts` went from `max(BINS, SCR_W)` = 850 points to `SCR_W` = 240 points

That last one was the **real waste** found: `lv_line_set_points()` only ever
passed 240 points, but the array was sized for 850 at 8 bytes per vertex — 4880
bytes of `.bss` for nothing.

### "Downsample the 850-point polyline, or every rasterisation draws 849 lines"

**Not true.** The code already peak-downsamples 850 bins to 240 columns and then:

```c
lv_line_set_points(s_spec_line, s_pts, SCR_W);   /* 240, not 850 */
```

LVGL rasterises 239 segments. The suggestion was based on the assumption that 850
points were passed.

### "Floating-point smoothing must become integer Q8" / "line_width=1, line_rounded=false"

**The floating-point part is done**: `FFT_ALPHA_Q8 = 77` (77/256 ≈ 0.30) and the
mapping in `update_plot` uses Q16 integer arithmetic — see §3.

**`line_width` / `line_rounded` should not change**: width 3 with rounded joints
is a visual choice measured on this panel. With butt caps LVGL's anti-aliased
segments leave gaps at steep transitions and the whole trace reads as **a row of
dots** rather than a line. That appearance was found by trying it.

### "Switch the waterfall to LV_COLOR_FORMAT_I8; 60 rows would be 15.4 KB"

**Tried and abandoned.** See §12: LVGL stores the palette **at the start of the
pixel buffer** (`lv_draw_buf_set_palette` writes `draw_buf->data` directly) while
the waterfall writes indices from offset 0, so the two overwrite each other and
the waterfall disappears. Supporting it needs reserved palette space and a canvas
pointing at the shifted pixels — more change than gain.

The suggestion's arithmetic ("240*60 + 1024 = 15.4 KB") also underestimates the
need: the palette is not a separate 1024-byte header but shares the same buffer.

### "Raise the audio task priority"

**Already the case.** `audio` runs at priority 8, the LVGL task at 4 (the
`ESP_LVGL_PORT_INIT_CONFIG` default). Audio has always been above rendering.

---

## 15. Before and after, across the whole optimisation

### CPU (steady state, sampled with `CONFIG_MRRC_PROFILE` temporarily on)

| Stage | taskLVGL | IDLE | Note |
|---|---|---|---|
| **start** | **58%** | **14%** | 8-line draw buffer, 10 Hz tick, floating-point EMA |
| draw buffer 12 lines | 43–49% | 24% | |
| draw buffer 32 lines + IRAM | 20–26% | 58% | not enough headroom (`largest` 18K) |
| draw buffer 24 lines | 23% | 56% | 3 CPU points for 6K of memory |
| **end (190 ms tick)** | **33–42%** | **32–45%** | 5.26 Hz, matched to the radio's sweep |

**Final CPU is 16–25 points below the start**, while the refresh rate is 1.6×
higher (the start had a 10 Hz tick but discarded 38% of the data and interpolated
the waterfall; the end is 5.26 Hz with every frame real).

### Memory

| Stage | free | largest | min |
|---|---|---|---|
| start | 49–55K | 44K | 21K |
| draw buffer 32 lines | 49K | **18K** | 15K |
| draw buffer 24 lines | 52K | 24K | 21K |
| **end** | **58–60K** | **27–28K** | 24K |

`largest` is the PTT metric (TLS needs ~16 KB contiguous); the end is 16K below
the start but still has **1.7× margin**, while `free` ended 5–9K *higher* than the
start (the 4880-byte `s_pts` waste, the 850-point array and `s_prev` are gone).

### Other

| Item | Start | End |
|---|---|---|
| bin size | 0x177de0 | 0x15b250 (114 KB smaller, pixel formats trimmed) |
| font faces | 3 (12/14/28) | 2 (12/44) |
| spectrum data used | 62% (38% discarded) | **100%** |
| waterfall content | 60% interpolated | **100% measured** |
| standing statistics cost | tick hook per tick | disabled |

### Audio quality at that point

| Item | State |
|---|---|
| RX gain clipping | server side +18 dB → lowered (operator action) |
| RX sample rate | 16 kHz; the server resamples 44.1→48→16 (could be improved, small gain) |
| TX sample rate | **16 kHz** (the codec's microphone fails at 48k, so 16k it is) |
| Audio drops | `err=0`; `drop` grows slightly at 190 ms, not audible as break-up |

---

## 16. The transport round (2026-11-21)

The last round of stutter work was not about CPU, memory, or rendering at all: it
was the transport. Full measurements, including the reference-client method and
the server-side counters, are in [transport tuning](transport-tuning.md). Summary
of what changed and what it bought:

| Cause | Fix | Effect |
|---|---|---|
| receive queues behind the TCP window were full (`RECVMBOX` 6, `OOSEQ` 4, `RX_BA_WIN` 6) | 8 / 8 / 16, window 8 MSS | device receives `31 KB/s` audio, `49 KB/s` total — the same as a reference client on the same Wi-Fi |
| playback started from an empty ring, so every network gap starved the DMA | 160 ms pre-buffer, re-buffer after 200 ms dry | ring-empty wakeups 3–5/s → 0–2/s |
| the uplink ran under Nagle and a 4-MSS send buffer | `tools/patch_ws_nodelay.sh` plus `SND_BUF` 11520 | sends over 30 ms: 31% → 0; worst send 89 ms → 9–17 ms |
| a software TX gain stacked on the analog PGA | server `tx_mic_db` 12 → 0 dB | clipped samples 5% → 0, `peak` 100% → 40% |

The server's own counters agree: `frames == fed == written`, `queue_drops=0`,
`write_err=0` per transmission, and `max pump stall` fell from 5791 ms to 3 ms.

Two traps from this round, worth more than the numbers:

1. **A counter without a reference lies.** Three earlier rounds tuned buffers
   because the device "only received 26 KB/s". It was measured against nothing.
   Once a second `/WSterminal` client ran on the same Wi-Fi, it was obvious
   whether the server was sending less or the device was receiving less. Always
   bring a reference client.
2. **Lowering a gain does not lower clipping if the peaks are upstream.** The
   server's gain went +12 → +3 dB and the clipping rate went *up* (5.0% → 5.8%),
   which is what proved the peaks were already at the ADC. A negative result that
   pointed straight at the real cause.

---

## 17. Where the repaint actually goes (2026-11-22)

The CPU table had taskLVGL at 56–61% with IDLE at 7–14% — far more than a
240×320 panel at 5.26 Hz should cost. Two instruments answered it, and both are
worth keeping:

1. **`bsp_display_flush_stats()`** — bytes and flushes handed to the panel. A full
   screen is 153,600 B, so the number converts directly into "screens per second".
2. **`bsp_display_flush_bands()`** — the same pixels bucketed into 32-row bands,
   which says *where* the repainting happens rather than only how much.

With the UI timer stopped, flush fell to **0 B/s**: nothing repaints
unconditionally, so every byte was the cost of live data. The band histogram then
named the culprit:

```
y000: 2%  y032:10%  y064:10%  y096:20%  y128:10%
y160:16%  y192:16%  y224: 9%  y256: 4%  y288: 3%
```

The frequency (y032 + y064) and the info row (y096) were 40% of every repaint.
`refresh()` rewrote those labels whenever `st.seq` moved, and `seq` also moves for
changes those labels do not show — `lv_label_set_text()` invalidates even when the
string is identical.

Fix: `label_set_if_changed()` compares first, the rule the chip row already
followed. Ten lines, no layout change:

| | before | after |
|---|---:|---:|
| flush | 526 KB/s (3.5 screens/s) | **344 KB/s (2.3 screens/s)** |
| flushes | 85/s | **56/s** |
| taskLVGL | 56–61% | **43%** |
| IDLE | 7–14% | **28%** |
| frequency bands | 20% of repaint | **0%** |

What is left is genuinely live: the trace (240×45) and the waterfall (the whole
canvas, because every row shifts down when a new one arrives) are about two thirds
of the remaining 344 KB/s, and the S-meter bar is most of the rest. Cutting further
means design changes — a rotating-row waterfall so only the newest row is
invalidated, or a slower tick — not more guards.

---

## 18. The trace, not the waterfall (2026-11-22)

After §17 halved the repaint volume, taskLVGL was still 42%. Two control
experiments said where the rest goes — freeze one widget, measure the CPU table:

| Frozen | taskLVGL | IDLE | flush |
|---|---:|---:|---:|
| nothing (baseline) | 42% | 29% | 335 KB/s |
| waterfall | 36–39% | 33–37% | 229 KB/s |
| **spectrum trace** | **15–17%** | **55–57%** | 223 KB/s |

The trace costs **25 CPU points while accounting for only a quarter of the
flushed area**. That is the signature of per-segment cost, not per-pixel: LVGL
builds an anti-aliased mask for each of the 239 segments of a 240-vertex
polyline.

| Change | taskLVGL | flush |
|---|---:|---:|
| 240 vertices, width 3 (old) | 42% | 335 KB/s |
| 240 vertices, width 2 | 37–39% | 320 KB/s |
| **120 vertices (2 px apart), width 3** | **28–30%** | 329 KB/s |
| 120 vertices, width 2 | 28–30% | 329 KB/s |

So vertex count is the cost and line width barely matters once segments are
longer. The firmware now draws one vertex every 2 columns (`PLOT_STEP_PX 2`):
flush volume unchanged, taskLVGL 42% → 28–30%, IDLE 29% → 42–45%, and the line
keeps its original weight. Neighbouring FFT bins are correlated, so a 2 px step
is not visible on a 240×45 trace; going further would start to show corners.

**A dead end worth recording:** the plan for this round was to convert the
waterfall to a rotating-row buffer so only the newest row would be invalidated.
It does not work. When one row arrives and the picture scrolls, *every* visible
row changes content, so LVGL must repaint the whole band no matter how the buffer
is stored — the cost is in the redraw, not the memmove. The waterfall's whole
share is 4–5 points (36–39% with it frozen), fifth of the trace's.

## 19. Where the network CPU goes (2026-11-22)

Once the trace was cheap, `websocket_task` at 14–15% became the second largest
consumer. Rather than guess, the three payload kinds are now timed separately
inside the event handler (`mrrc_net_ws_cost()`, behind `CONFIG_MRRC_PROFILE`):

| Payload | Per message | Messages/min | CPU share |
|---|---:|---:|---:|
| audio (642 B) | 70 µs | 2985 | 0.35% |
| spectrum (1701 B) | 53 µs | 642 | 0.06% |
| control JSON | 897 µs | 437 | 0.65% |
| **our handler, total** | | | **≈1.1%** |

So almost none of that 15% is ours: the remaining ~14% is inside
`esp_websocket_client` — TLS record decryption, frame reassembly, socket reads.
Reducing it would mean replacing the client or changing the protocol, which is not
worth it for this much.

Two observations worth keeping:

- **Control JSON is expensive per byte.** 437 messages a minute at 897 µs each is
  0.65% CPU from ~1 KB/s of payload, because cJSON allocates a node per key. It is
  also the main source of heap fragmentation (a 5.5 KB `fullState` allocated and
  freed whole, repeatedly — §4.3/§13). Parsing only the fields the UI shows would
  cut both; it was not done here.
- **Audio costs almost nothing per message** (70 µs for 642 B including the ring
  copy), so PCM16 at 50 messages/s is not what makes the network task busy. TLS at
  49 KB/s is.

## 20. Waterfall storage: 8-bit indices (2026-11-22)

The waterfall buffer was the largest single `.bss` block in the application:
`uint16_t s_wf_buf[240 × 40]` = **19,200 B**. It is now one byte per pixel, an
index into a 256-entry palette:

| | before | after |
|---|---:|---:|
| waterfall buffer | 19,200 B | **10,624 B** (1,024 B palette + 9,600 B indices) |
| steady-state `free` | 48 KB | **57–59 KB** |
| steady-state `largest` | 27–42 KB | **38–48 KB** |
| taskLVGL | 28–30% | **30–31%** |

The cost is 1–2 CPU points: LVGL now looks each pixel up in the palette while
blending instead of copying RGB565. With IDLE still at 40%, that is a good trade —
`largest` is what the PTT TLS handshake depends on, and it gained 11 KB.

**§12 said this was a dead end; it was not.** The conflict there was
`lv_draw_buf_set_palette()` writing at `draw_buf->data` while the pixels were also
written from offset 0. LVGL actually requires the palette **in front of** the
pixels — `lv_draw_buf_goto_xy()` skips `palette_size * 4` bytes before addressing
pixels — so the buffer simply has to be allocated with that space reserved. The
canvas then works with `LV_COLOR_FORMAT_I8` and no custom draw callback at all.

Trap worth recording: the palette must be filled **after** `heat_init()`. Setting
it at canvas-creation time, before the Jet table existed, produced a waterfall of
zeros — a completely black waterfall with no error anywhere. Look at the screen
when changing anything palette-related.

## 21. The steady 2% of drops was lock contention (2026-11-22)

A 13-minute soak with everything else idle showed only one number moving: the
drop counter crept up at ~57 per minute (≈2% of frames) while `free` sat at 56–58
KB, `largest` at 42–44 KB, `glitch=0`, and there were zero reconnects, zero
watchdog events and zero aborts. A ring that is 2% short every minute with no
memory pressure is the signature of the *other* drop path: `rb_put()` took the
ring mutex with a **zero** timeout, so any overlap with the consumer's frame copy
(~10 µs) discarded the frame.

Taking it with a 2 ms timeout — 100× the hold time, far below one 20 ms frame —
removed the steady loss. The same soak afterwards:

```
minute   0  2  3    4    5  6  7  8  9   10  11
drop     7 26 395  452  455 455 455 455 529 529 529
per min    19 369   57   3   0   0   0  74  0   0
```

Drops are now **bursty instead of continuous**: minutes at zero, then a spike of
50–370 when the network delivers a clump the 0.5 s ring cannot hold. Those are
genuine overruns, and the remaining lever for them is latency (a larger
pre-buffer) rather than CPU or memory.

Both drop paths still incremented one counter, so it was split — and the answer
was immediate: **`lock=0`, everything is `full`**. The zero-timeout mutex was a
latent fault, not the cause of these drops.

With the split in place, the consumer was measured too (`i2s out`, frames and
samples written per minute). Over a minute with continuous supply it is **exactly
50.0 frames/s and 16,000 Hz** — the codec is not slow, and the producer is not
fast. The remaining drops are therefore pure burst overruns: minutes with 0, then
a spike of a few hundred when the network hands over a clump larger than the ring's
slack (0.5 s ring minus the 160 ms pre-buffer). The lever for those is latency, not
CPU, and they are not audible as continuous stutter.

## 22. Surviving a mobile link (2026-11-22)

Measured on a train, tethered to a phone: **6.5–12% packet loss, RTT peaks of
6–10 s, 26–41 pings over one second in four minutes**. The firmware cannot buffer
that — ten seconds of 32 KB/s audio is 320 KB, and the whole SRAM is 400 KB — so
the goal is not continuity but *graceful* degradation. Two policies got there.

**1. A stall means the queue is history.** If no audio arrives for a second
(`AUD_STALL_FLUSH_US`), the frames already queued are stale by definition. Throw
them away and start a fresh pre-buffer rather than replaying half a second of
history and then fighting the recovery burst. Printed as `flush=` in the diag
line; it fired 5–16 times per four minutes, matching the ping record.

**2. Which end to drop depends on why the ring is full.** The ring used to drop
the *newest* frame, which is right in steady state (one 20 ms frame lost, stream
intact) and wrong during recovery: after a stall the server hands over seconds of
backlog, and dropping the newest keeps the *oldest* 16 KB — stale audio, and the
ring then sits at `15408/16384` minute after minute. Once the ring is over 75%
full the oldest frames are discarded instead, so what survives is the freshest.

Result on the same train link, seven minutes, 6.5% loss, 29 spikes over one second:

| | before | after |
|---|---:|---:|
| drops (`full`) | +200/min | **0** |
| `i2s` | 1.9–26 f/s | 26–47 f/s |
| `flush` | — | 16 |

Price: while recovering, the ring sits full, so playback is up to 0.5 s behind
live. Trimming back to the 160 ms floor after a recovery would fix the latency at
the cost of an audible skip; not done.

What remains is underruns (`un`) climbing during the stalls themselves — the
honest answer for a link that disappears for seconds.

## 23. Several networks, and switching on audio health (2026-11-22)

ESP-IDF configures one AP per station interface, so a network list is an
application concern. The firmware now holds up to three (SSID, password) pairs
from Kconfig — the credentials stay in the gitignored `sdkconfig.defaults.local`
— and at boot:

1. scans once (~1–2 s, before the first connect),
2. connects to the **strongest configured SSID that the scan can see**,
3. falls back to the first entry when none is visible, so a single-network setup
   behaves exactly as before,
4. rescans and moves on after three consecutive connection failures, which is what
   happens when the operator physically moves between sites.

Separately, the link can be bad while `esp_wifi` never reports a disconnect: a
phone hotspot stays associated through 10% loss and multi-second stalls. So the
once-a-minute audio counters now decide too — two consecutive minutes with more
than 2% frame drops or 50 underruns per second trigger `mrrc_net_wifi_kick()`,
which rescans and moves to a different visible candidate (and does nothing when
there is none).

Measured on a train, same device and firmware, only the AP changed:

| | `cnham-MM` | `iPhone` hotspot |
|---|---:|---:|
| frame drops | +200/min | **0** |
| frames written to I2S | 1.9–26 /s | 44–47 /s |
| ring occupancy | pinned full | drains back to 0 |
| stall recoveries (`flush`) | — | ~1/min |

Both are mobile links: `i2s` at 44–47 instead of 50 and `un` still climbing means
second-long stalls remain, and no ring covers those. What changed is that the
failure is now graceful — no drop storms, no stutter bursts from a permanently
full ring.

## 24. Acceptance run against the SDD targets (2026-11-23)

The first run measured against `SDD/05`'s numbers instead of against an
impression. Conditions, because a number without them is a rumour (SDD/00 §V6):

| | |
| --- | --- |
| Build | `v1.3.0-1-g933a4ae` + `CONFIG_MRRC_PROFILE=y` + `CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS=y` |
| Unit | ESP32-C3, `98:c3:77:f5:4b:2c`, battery 94% (4 109 mV) |
| Server | `w103d.vlsc.net`, live session, `rx 31.2 KB/s` |
| Duration | 12 minutes continuous receive, then one 5 s automatic transmission |
| Instrument | the device's own profile lines (SDD/00 §0.3, instrument 1) — **not** a reference client |

| NFR | Target | Measured | Verdict |
| --- | --- | --- | --- |
| NFR-001 | underruns ≤2/s, `rebuf` not growing | **0–18/s** (11 samples, mean 6.5/s), `rebuf` 2 → 9 | **FAIL** |
| NFR-002 | 50.0 f/s, `drop=0` | 49.4–50.0 f/s, `drop=0 err=0 full=0 lock=0` on every sample | PASS |
| NFR-003 | ≥31.3 KB/s audio, ≥49 KB/s total | 31.1–31.3 KB/s audio, 49.2–49.3 KB/s total | marginal |
| NFR-004 | 50 frames/s, `sendfail=0`, `readfail=0` | 256 frames in 5 s = **51.2/s**, both 0 | PASS (5 s window, not 30 s) |
| NFR-005 | worst send ≤50 ms | `send_max=19 095 µs`, `slow=0` | PASS |
| NFR-006 | `peak` 20 000–29 000 | **835** | not assessable — see below |
| NFR-018 | `largest` ≥24 KB | 28 KB (steady, every sample) | PASS |
| NFR-019 | `free` ≥49 KB, no downward trend | 49–52 KB, flat over 12 min | PASS |
| NFR-020 | `taskLVGL` ≤30% | **36%** | **FAIL** |
| NFR-021 | `IDLE` ≥40% | **33%** | **FAIL** |
| NFR-022 | waterfall ≤11 KB of `.bss` | 10 624 B (by construction) | PASS |
| NFR-027 | 16 kHz, ≤0.2% drift, no audio lost to it | 16 kHz, no drift symptom in 12 min | PASS (12 min is short) |
| NFR-031 | one serial line diagnoses the fault | this run is the example | PASS |

Supporting numbers from the same run, not in the table because no NFR owns them:
`decode 0 µs/frame` (PCM16, no Opus), `flush 346 KB/s / 2.3 screens/s /
68 flushes/s`, websocket cost `audio 84 µs`, `spectrum 52 µs`, `json 761 µs` per
message, DMA free 41–44 KB, audio task stack high-water 7 744 B free.

### What the three failures actually mean

**NFR-001 (underruns).** Not a link problem: `rx` held 31.2 KB/s and `drop=0`.
The ring oscillates between roughly 13 KB and its 98% mark (16 050/16 384) and the
cushion does not survive the gaps — 0 to 18 wakeups per second depending on the
minute, and `rebuf` climbed 2 → 9 across the run. The 160 ms pre-buffer is the
thing under test here, and at this window it is not holding.

**NFR-020/NFR-021 (CPU).** 36% for `taskLVGL` and 33% `IDLE` are 6–8 points worse
than the 28–30% / 42–45% recorded on 2026-11-22 (§15, §17, §18). Two explanations
are possible and this run does not separate them: the profile build measures
itself (`GENERATE_RUN_TIME_STATS` charges every tick, and the flush/band/ws-cost
sampling runs once a minute), or the UI has genuinely grown since — the chips bar,
the 1 px red centre line and the edge-hugging scale row all landed after those
numbers were taken. SDD/00 §V7 says a changed instrument invalidates history, so
the next step is one A/B: same build with only the run-time stats off.

**NFR-006 (level).** `peak=835` is not a clipper and not a level fault: the run was
unattended and nobody spoke, so the peak over 5 s of silence is silence. This is a
hole in the *method*, not in the firmware — the automated self-test can prove the
path (256 frames, no failures) but cannot prove the level. Chapter 15 §15.5 now
says so; a level measurement needs speech at the microphone and a human.

## 25. The jitter governor: bound the ring instead of growing it (2026-11-23)

§24's acceptance run failed NFR-001 — underruns 0–18/s (mean 6.5/s), `rebuf`
2→9 — while `drop=0 err=0` and `rx` held 31.2 KB/s. The first instinct was
"the 160 ms pre-buffer is too shallow, grow the ring." The ring itself said the
opposite: `ring(15408/16384)` minute after minute, i.e. 98% full.

Two consequences of a ring pinned near full, both measured in that run:

1. **Latency.** 15 408 B ≈ 0.48 s — three times the cushion the design assumed
   and more than a half-duplex radio exchange can tolerate.
2. **The free-run cascade.** When the DMA finally starves, `bsp_audio_write()`
   stops pacing the consumer loop; the next `rb_get` returns immediately and the
   whole 14 KB drains in a burst, so *one* network gap becomes hundreds of
   underruns and a rebuffer. This is the mechanism behind the 0–18/s spread.

The fix is a **jitter governor** in `main/mrrc_audio.c`, not a bigger buffer:

- **High-water trim** (`rb_put`): whenever a recovery burst pushes the ring past
  `AUD_HI_BYTES` (12 288 B ≈ 384 ms), drop the *oldest* frames down to that mark.
  What survives is the freshest audio, and latency is bounded above.
- **Low-water repeat** (`audio_task`): when the ring runs dry, write the last
  played frame again instead of handing the DMA an empty buffer. `bsp_audio_write`
  keeps pacing at 20 ms/frame, so the cascade cannot start. The 200 ms dry rule
  still triggers a rebuffer, and the 1 s stall-flush still discards stale audio,
  so a repeat can never mask a dead link — it only bridges a gap the cushion can
  absorb.

The floor is deliberately the ring itself: it drains one 20 ms frame per write,
so a "repeat below N bytes" threshold would only start earlier without buying
slack. The pre-buffer (5 120 B ≈ 160 ms) is unchanged.

New instruments, both behind `CONFIG_MRRC_PROFILE`:

- idle line gains `gov(ins=… trim=…)` — frames repeated vs frames trimmed.
- the `link:` line gains `sps=…` — samples written to I2S per second, the
  consumer's real drain rate (≈16 000 at 16 kHz).

Status: **implemented, not yet field-exercised** (SDD/00 §V5). The §24 table has
not been re-run on a board. NFR-001 stays open until a live session shows
`err=0 drop=0` with `sps≈16000` and `gov` accounting for the gaps; if underruns
move after this change, `AUD_HI_BYTES` (384 ms) is the single knob, not the ring
size.

## 25. Stutter round 2: the codec was playing 2.4% slow (2026-11-23)

The acceptance run (§24) left NFR-001 failing: 0–18 underruns/s and `rebuf` climbing. The
summary everyone reaches for is "mobile link"; the measurement said otherwise.

**What was measured, on the LAN, RSSI −29 dBm, `rx=31.2 KB/s`:**

| | before | after |
| --- | ---: | ---: |
| samples written to I2S (per second) | **15 600** | **16 002** |
| `i2s` frames/s | 46.8 | 50.0 |
| underruns/s | 0–18 | **0** |
| ring occupancy | pinned 13.5–15.4 KB (98%) | 2.5–12.8 KB, bounded |
| `rebuf` (re-buffers) | +7 per 12 min | +1…3 per 7 min |
| `taskLVGL` / `IDLE` | 36% / 33% | 27% / 46% |
| `largest` | 28 KB | 38 KB |

**Root cause.** `I2S_CLK_SRC_DEFAULT` on the C3 resolves to the **XTAL** source. With
MCLK = 256·fs, the divider is `40 MHz / (256 × 16 000) = 9.77 → 10`, so the codec really
ran at **15.625 kHz**. The stream arrives at 16 kHz, so playback was 2.4% slow: the ring
filled to 98% and stayed there, and every "network gap" was really the consumer falling
behind. Two things had been hiding it:

- the old ring comment ("a 24 KB experiment changed nothing: the ring is permanently full,
  so the bottleneck is playback draining slower than the stream arrives — do not grow this
  until that is fixed") had already named the symptom and left it unfixed;
- `design-v1.md` §3.2 recorded the clock as 16.03 kHz (+0.16%) from the PLL divider
  arithmetic, without anyone measuring the *actual* drain rate. A design document that
  computes a clock instead of measuring it is how a 2.4% error survives for days.

Fix: `clk_src = I2S_CLK_SRC_PLL_160M` → `160 MHz/(256×16 000) = 39.06 → 39` → **16.026 kHz**,
the +0.16% the jitter buffer is sized for.

**Second fix (this round): the pre-buffer became a controller.** It used to gate only the
start, then let the ring drift. Measured consequences of a 98%-full ring: half a second of
latency, and when the DMA finally starved, `bsp_audio_write()` stopped pacing the loop and
the consumer free-ran through 14 KB — one gap became a burst of underruns plus a re-buffer.
The governor now keeps the cushion inside a band: trim the front above 384 ms, repeat the
last frame (20 ms of held audio) below 128 ms instead of handing the DMA an empty buffer.
`un=0` on every sample since.

**Third fix: the waterfall gives back bandwidth.** The terminal asked for spectrum `high`
(1701 B × ~11 fps ≈ 18 KB/s) unconditionally, on a path where audio alone needs 32 KB/s.
There is no lower audio tier (AD-002, PCM16 only), so the only thing that can be given back
is the waterfall: two consecutive bad minutes demote it to `low`, two clean minutes restore
`high`, and the PTT path already drops it while keyed.

**What this round did *not* fix, measured:** the terminal and the server are on different
ISPs (`2408:8916:…` versus `2409:8a00:…`), so every frame crosses the public internet. In
one five-minute window the downlink collapsed 31.2 → 10.6 KB/s with the device **54% idle**
and the server reporting `max pump stall 4817 ms` and 440 dropped audio frames. That is a
path limit, not a device limit, and it is what AD-007 predicted: the goal is graceful
degradation, not continuity.

## 26. Transmit died 2 s in: a fatal write timeout, not a slow link (2026-11-24)

Reproduced with the automatic self-test and a serial capture, so both ends were
visible in the same window. Device side:

```
selftest: keyed 5000 ms
transport_base: Poll timeout or error, timeout_ms=100
websocket_client: esp_transport_write() returned 0
net: /WSterminal error -> disconnected -> Failed to send the buffer
tx done: frames=104 sendfail=146 readfail=0 peak=18633 send_max=109805 us
```

Server side, same transmission: `Terminal client disconnected during TX! Forcing
RX`, and no session record at all.

**Two faults, both self-inflicted.**

1. **A missed capability ack tripled the uplink.** With no `audioCaps` reply within
   3 s the firmware fell back to 48 kHz PCM, whose transmit path upsamples 3:1 —
   32 KB/s becomes ~96 KB/s, on the link that had just failed to deliver one small
   text frame. Measured consequence: `sendfail=146` within 2 s. The fallback is
   gone: the declaration is re-sent (5 tries, 3 s apart) and the wire format stays
   16 kHz, because every reachable server speaks pcm16 (AD-038) and a dropped ack
   says nothing about that.

2. **The 100 ms write timeout was sized for the wrong failure.** It was chosen on
   2026-11-22 to stop a congested uplink from turning the transmit loop into
   2 frames/s, and it does that. But `esp_websocket_client` serves TLS in *both*
   directions from one task, so while it drains a downlink burst an uplink write
   can wait ~105 ms — and the client treats a failed write as **fatal**, tearing
   the session down mid-transmission. The timeout is now 400 ms: the worst case is
   ~20 dropped frames of audio, against losing the whole transmission plus a ~5 s
   reconnect.

Same self-test after both changes:

```
selftest: keyed 5000 ms
tx done: frames=256 sendfail=0 readfail=0 send_max=24078 us slow=0
selftest: unkeyed          (no /WSterminal error, no disconnect)
```

256 frames in 5 s = 51.2/s, worst write 24 ms, and the session survived.

**Still open:** the capability ack is not landing at all in these runs — the device
re-sends five times and then keeps 16 kHz by design (device log:
`no audioCaps reply after 5 asks; staying 16k`). Audio works, but a handshake that
nobody completes is a bug on one side of the wire, not a setting.

## 27. The cushion adapts itself: trims went to zero (2026-11-24)

The "occasional stutter" in receive had a number after all. Six minutes of the
idle line, differenced per minute:

| minute | `i2s` | ring | `ins` | `trim` |
| --- | ---: | ---: | ---: | ---: |
| 1 | 40.7 | 3 210 | +2 | +0 |
| 2 | 49.8 | 10 914 | +13 | +1 |
| 3 | 49.5 | 12 198 | +26 | +32 |
| 4 | 49.3 | 9 630 | +39 | +40 |
| 5 | 47.5 | 11 556 | +57 | +153 |
| 6 | 46.9 | 10 272 | +146 | +167 |

1.3 trims and 0.9 repeats per second — **4.4 % of the stream tampered with 20 ms
at a time**. The ring never sat mid-band; it sat *at* the 12 288 B ceiling
(9.6–12.2 KB), so every fan-out tick overflowed by construction.

Cause: the server sends audio in ticks (measured `blocks/s=10, offered/s=50` — five
20 ms blocks per 100 ms). A ring that is AT the ceiling has no room for the next
tick, so it trims on every tick; then it drains below the floor between ticks and
repeats.

Two changes in `mrrc_audio.c`:

1. **Trim with headroom.** On overflow, come down to `ceiling - 3 584 B` (one tick
   plus margin) instead of stopping one frame below the ceiling.
2. **Adapt the ceiling.** Once a minute: trims > repeats and trims > 20 → widen by
   1 KB (max 14 336); repeats > 20 → narrow by 512 B (min 8 192). Every change is
   logged, because an adaptive threshold nobody can reconstruct afterwards is a
   rumour machine.

After, same measurement (three minutes):

```
i2s(49.9 f/s)  ring(6420-7704/16384 max 8988)  un=0  gov(ins=19 trim=0)
i2s(49.9 f/s)  ring(7704/16384 max 8988)       un=0  gov(ins=25 trim=4)
```

`trim` ≈ 0 and `ins` ≈ 0.1/s: 0.3 % of frames, down from 4.4 %. The ring now sits
mid-band instead of pinned, which is what the design always claimed it did.

**Boundary.** This did not change the *supply*: one 531-second server session in
the same period reported `dropped 1217 (24.3 s)` — 4.6 % of audio never left the
box, which the cushion cannot invent back. With the cushion fixed, that number is
now the only remaining explanation for a stutter, and it lives on the server side.
