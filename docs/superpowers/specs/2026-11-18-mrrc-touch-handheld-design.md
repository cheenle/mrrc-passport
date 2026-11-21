<p align="right">
  <a href="2026-11-18-mrrc-touch-handheld-design.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# MRRC Touch handheld design (ESP32-S3 · 2.8" ILI9341 · capacitive touch)

> Date: 2026-11-18  Branch: `feat/s3-touch-2.8`  Baseline: `mrrc-passport` v1.1.0 (cc924f4)
>
> In one sentence: move the mechanisms the passport already worked out (protocol
> timing, audio constraints, PTT state machine, heartbeat) onto a portrait S3 board
> with a touchscreen, and replace "no screen, three ADC keys" with "one permanent
> screen plus three mutually exclusive overlays".

---

## 1. Goals and non-goals

**Goals**

1. Touch tuning (drag or long-press for exact entry), channels, settings (10 items)
   and volume, all on screen.
2. 240×320 portrait; the main screen is always visible (frequency + spectrum +
   waterfall + status), with three mutually exclusive overlays.
3. Uplink Opus at 16 kbps (16 kHz mono / 60 ms frames / FEC); downlink accepts both
   PCM16 and Opus.
4. Keep every hard constraint and measured conclusion from the passport; do not
   re-derive them.

**Non-goals (explicitly out of V1)**

- A Wi-Fi provisioning UI (reuse the credentials in NVS; changing networks is done
  over serial or by reflashing).
- SD-card recording, FT8 decoding, two VFOs shown at once, spectrum zoom animation.
- Local playback of recordings (anything beyond `/WSaudioRX`).
- Fast-charge protocol detection (only the IO9 divider reading and a battery
  percentage in the header).

---

## 2. Hardware facts

**Board**: LCD Wiki **ES3C28P** (with touch) / ES3N28P (without). Also sold as
Hosyond 2.8" ESP32-S3 Display. Upstream board reference:
`main/boards/quandong/esp32-s3-spi-2.8/` in `78/xiaozhi-esp32`.

| Item | Fact | Source |
|---|---|---|
| SoC | ESP32-S3 QFN56 rev v0.2, dual core 240 MHz, FPU, Wi-Fi/BLE5 | esptool `chip_id` |
| Flash | 16 MB, Puya (`0x5E`), eFuse: quad / 3.3 V | esptool `flash_id` |
| PSRAM | 8 MB **octal**, occupying GPIO33–37 | `MCU_SEL=4` read over JTAG |
| Display | **ILI9341V**, 240×320, 4-wire SPI, RGB565 | lcdwiki documentation |
| Touch | **FT6336G**, I2C 0x38, INT=17, RST=18 | lcdwiki plus GPIO18 measured as an output |
| Audio | **ES8311** (I2C 0x18 + I2S full duplex) + **FM8002E** amplifier (active low) | firmware strings plus lcdwiki |
| Microphone | on-board MEMS (LMA2718B381), analog, into the ES8311 | lcdwiki |
| LED | WS2812B ×1, **IO42** | lcdwiki |
| SD | SDIO 4-bit: CLK=38 / CMD=40 / D0=39 / D1=41 / D2=48 / D3=47 | lcdwiki |
| Battery | TP4054 charging + **ADC on IO9** divider | lcdwiki |
| Keys | BOOT=IO0 (the only physical key), RESET=EN | lcdwiki |

**Pin table (single source of truth, goes into `bsp_pins.h`)**

```
LCD   : CS=10  DC=46  SCK=12  MOSI=11  MISO=13  RST=shared with EN (soft reset)  BL=45(PWM)
TOUCH : SDA=16 SCL=15 (shared with codec)  INT=17  RST=18  address 0x38
CODEC : SDA=16 SCL=15  address 0x18
I2S   : MCLK=4  BCLK=5  WS=7  DOUT=8  DIN=6
PA    : IO1, active low
BAT   : IO9 (ADC1_CH8)
LED   : IO42
BUS   : I2C0 / SPI3 / I2S0
```

> Cross-check: I2C 15/16, SPI 11/12, and GPIO10/GPIO18 as outputs all agree with
> the `IO_MUX` plus GPIO matrix read from this board over USB-JTAG.

---

## 3. Architecture

```
┌──────────────────────────── core 1 ────────────────────────────┐
│  LVGL 9.5 (: 30 fps) ─ main screen / three overlays / touch indev │
│      ↑ state snapshot              ↑ touch events                │
│  ui_state.c (lock-free snapshot)  ⇄  mrrc_ui.c (read-only render) │
│      ↑                                                           │
│  mrrc_net.c  (WebSocket: WSradio / WSspectrum / WSaudioTX/RX)     │
└──────────────────────────────┬──────────────────────────────────┘
                               │ ringbuf (frame-aligned)
┌──────────────────────────── core 0 ─────────────────────────────┐
│  audio_task (prio 7) ← one task owns both directions (Opus is not │
│      thread-safe)                                                 │
│      16 kHz → Opus 16 kbps → [0x01][opus]                        │
│      [0x00][codec][payload] → Opus dec 48 kHz → ES8311           │
│  tx_hb      (prio 10) one txhb every 1.5 s — its own task, never  │
│      inside the LVGL loop                                        │
└──────────────────────────────────────────────────────────────────┘
```

**Why the heartbeat must be separate** (measured on the handheld): while
transmitting, the encoder saturates core 0; a heartbeat hung off the main loop
(priority 1) gets starved for seconds, and the server's 5 s liveness gate unkeys
us — heard as "transmission dies after 5.6 s".

**Why encode and decode must be the same task** (measured on the handheld): GCC
for the ESP32-S3 downgrades `_Thread_local` to a plain global (no `.tbss`), so
micro-opus's "thread-safe pseudostack" is not actually per-thread; two tasks
entering libopus at once corrupt `global_stack/scratch_ptr` and panic with
`Cache disabled but cached memory region accessed`. PTT is half duplex, so encode
and decode never need to run in parallel — serialising them costs nothing.

---

## 4. BSP: same interfaces, new implementations

Keep the passport's `components/bsp` interfaces (`bsp_display_*` / `bsp_audio_*` /
`bsp_button_*` / `bsp_i2c_*` / `bsp_lvgl_lock`) so `main/` does not change when the
board does.

| Module | passport (C3) | this board (S3) |
|---|---|---|
| `bsp_display` | ST7789P3, SPI2 (9/8/1/20/21) | **ILI9341**, SPI3 (11/12/10/46/45), `esp_lcd_ili9341` plus the vendor's 20-entry init sequence; **round-screen row masking removed** |
| `bsp_audio` | ES8311 capture 16 kHz / playback 48 kHz | same, only the I2S pins, I2C (15/16) and PA (IO1, active low) change |
| `bsp_button` | three keys on an ADC ladder (GPIO0) | **single digital input** (GPIO0); long press = PTT, tap = nothing |
| `bsp_i2c` | its own pins | 15=SCL / 16=SDA at 400 kHz |
| `bsp_battery` | CW2017 (I2C 0x63) | **ADC divider on IO9** (CW2017 path deleted) |
| `bsp_touch` | none | **new**: `esp_lcd_touch_ft5x06` plus `lvgl_port_add_touch` |
| `bsp_lvgl` | LVGL 9.5 + esp_lvgl_port 2.9 + round-screen mask | LVGL 9.5, square panel, buffers in PSRAM |

**Dependencies** (`idf_component.yml`, all Apache-2.0):
`espressif/esp_lcd_ili9341 ^2.1.0`, `espressif/esp_lcd_touch_ft5x06 ^1.1.1`.

---

## 5. Audio path

```
capture  ES8311 @16 kHz mono ──► Opus enc 16 kHz / 60 ms / complexity 0 / 16 kbps
                              ──► [0x01][opus]  ──► /WSaudioTX
playback /WSaudioRX ──► [0x00][codec][payload] ──► Opus dec 48 kHz ──► ES8311 @48 kHz
```

Hard constraints (measured on the passport, not negotiable):

1. **The codec stays at 16 kHz.** The ES8311 is nearly deaf capturing at 48 kHz
   (peak 468 versus 12708).
2. **Do not declare `audioCaps`.** The server has only `legacy / pcm16 / pcm48`
   profiles, and `legacy` means Opus 48 kHz decode; the handheld uses exactly that
   and it works.
3. **micro-opus must be patched**: pseudostack 16 KB plus `NONTHREADSAFE` (on the
   passport's C3, RISC-V GCC emits no `.tbss` and thread-safe mode writes NULL into
   `celt_decoder`). Xtensa GCC for the S3 also emits no `.tbss`, so **only one task
   in the whole firmware may call libopus** (see above). Dependency:
   `esphome/micro-opus ^0.4.1`.

The downlink payload's codec tag must be dispatched by value (`0x00` PCM48 /
`0x01` Opus / `0x02` PCM16) — **never assume one**: a browser joining the same
session may push PCM48.

---

## 6. Network and protocol

### 6.0 Single `/WSterminal` connection (corrected by measurement, 2026-11-19)

The original design copied the handheld: four independent WebSockets
(`/WSradio` `/WSpectrum` `/WSaudioRX` `/WSaudioTX`) plus Opus. **The board refuted
it**:

| Observation | Number |
|---|---|
| internal RAM with four connections | `free=2451 B, largest=2048 B` |
| task for the fourth socket | `websocket_client: Error create websocket task` |
| mbedTLS record buffer squeezed to 4 KB | `/WSradio` reports `-0x7100` (`fullState` is 5.5 KB, it does not fit one TLS record) |
| task stacks moved into PSRAM | crashes at boot (`i2s_channel_disable` floods, then a reset loop at 945 ms) |
| after returning to one connection | `free=15799 B, largest=8704 B`; control, spectrum and audio all work on one socket |

**Conclusion**: the passport's single connection is not a compromise forced by the
poor C3 but the only shape that fits this class of chip (400–512 KB of internal
RAM). This project therefore **returns to `/WSterminal` + PCM16** and does not use
Opus.

### 6.1 `/WSterminal` frame formats

```
uplink    [0x01][0x02][PCM16 16 kHz mono]   channel=TX audio, tag=pcm16
downlink  [0x00][tag][payload]              channel=RX audio, tag: 0x02=pcm16 / 0x00=pcm48
spectrum  [0x02][850 bins]                  channel=spectrum
```

**The channel byte cannot be omitted**: without it the server reads the codec tag
`0x02` as the spectrum channel and discards the PCM — the radio keys but nothing
is transmitted, ATR-1000 reports `TX silence` and drops out by itself, heard as
"transmits with no power" (measured 2026-11-19).

Handshake: immediately after connecting, send
`{"type":"audioCaps","profile":"pcm16"}` (the server answers
`accepted:true, rate:16000`) and `{"type":"spectrumCaps","profile":"mid"}`.

### 6.2 Half duplex

While transmitting, the output must be muted with
`esp_codec_dev_set_out_mute(true)` and the downlink queue **drained and discarded**:
otherwise our own uplink comes back through the server as a howl, and after
unkeying the backlog of stale audio is played out.

Reuse the passport's `mrrc_net.c` structure (login → `/WSradio` / `/WSspectrum` /
`/WSaudio*`) and port the Opus codec and spectrum downsampling from the handheld:

| Capability | Source |
|---|---|
| login / token / reconnect / `fullState` parsing | passport (already working) |
| PTT state machine (`ptt:true` → frames → `"s:"` → `ptt:false`), `txhb` | passport + handheld |
| spectrum 850 bins → 240 columns (declares the `mid` tier, about 15 Hz) | handheld |
| Opus encode/decode with 60 ms frame alignment | handheld |
| channels: recall with `{"mem": n}` plus subscribing to `memChannels` broadcasts | already in the server (6 slots of `freq/mode/label`) |
| settings: 10 of 51 fields pushed down | server `/WSradio` |

**Channels are not stored locally**: the server's `mem_channels.json` is the single
source of truth; recall uses the server's atomic path (frequency first, then mode,
then frequency again plus a 2 s poll), and the handheld only sends an index.

---

## 7. UI

### 7.1 Main screen (240×320, permanent)

```
y   0.. 26   header   [LSB] ─── VFO A (tap: A/B) ─── battery
y  30.. 86   frequency "7.215.00" 24 px; drag up/down = tune, long press = keypad
y  86..104   info row S9+10 │ FIL 2.4K (tap) │ AF 128 (tap)
y 104..200   spectrum  240×96; drag left/right = continuous tune, tap = move there
y 200..276   waterfall 240×76, 40 rows of history ≈ 5 s; swipe up/down = span
y 278..300   footer    `FREQ/1k` (tap: step) │ [RX/TX] │ [LINK]
```

### 7.2 Three mutually exclusive overlays

| Overlay | Content | Opened by |
|---|---|---|
| volume | AF slider + local speaker slider (two independent) | tapping `AF` in the info row |
| settings | 10 items: filter width / RF power / preamp·attenuator / noise blanker·reducer / squelch / mic gain / tuner / AGC speed / spectrum span / channels | long-press the footer or the header icon |
| channels | list of 6 slots (label + frequency + mode) | long-press `FREQ` in the footer |

Rule: **only one overlay exists at a time**, dismissed by tapping the scrim or by
a BOOT tap; no nesting.

### 7.3 Gestures

| Region | Gesture | Action |
|---|---|---|
| frequency digits | drag up/down | tune by step (the further the drag, the bigger the step) |
| frequency digits | long press 600 ms | numeric keypad (exact Hz) |
| spectrum | drag left/right | continuous tuning (1 px ≈ span/240 Hz) |
| spectrum | tap | move the VFO to the tapped frequency |
| waterfall | swipe up/down | change span tier (±25K…±500K) |
| info row | tap FIL / AF | the matching overlay |
| footer | tap FREQ | the step overlay |

### 7.4 PTT and touch lock

- BOOT held ≥600 ms → `ptt:true`; **the whole screen ignores touch while
  transmitting**
- release → stop frames → `"s:"` → `ptt:false` → back to IDLE after 200 ms
- held 180 s → force unkey and show `TX TIMEOUT`
- a short press never transmits (keying happens only after the long-press
  threshold)

---

## 8. Performance budget

| Item | Budget / approach |
|---|---|
| LVGL buffer | 2 × 240×80 lines in PSRAM (about 76 KB ×2); DMA descriptors in internal RAM; `full_refresh=0` |
| spectrum | 15 Hz, `lv_canvas` in PSRAM, 850 → 240 columns taking the maximum |
| waterfall | 15 rows/s, 76 rows = a 5 s window |
| fonts | 24/32 px anti-aliased digits + 12 px body; icons as a glyph font, no images |
| core 0 | Opus encode/decode (**single task, serialised**, priority 7) + txhb (priority 10) |
| core 1 | LVGL + WebSocket + spectrum parsing |
| target | UI 30 fps, spectrum/waterfall 15 fps, no dropped frames and no false unkey while transmitting |

---

## 9. Risks and unknowns

| Risk | Handling |
|---|---|
| **the flashed image header must be DIO/40 MHz** | writing the bootloader with `qio80` makes the ROM print `ets_loader.c 78` and then reset through TG0WDT (measured). QIO needs the flash QE bit, which ROM cannot read. **Always write flash with `--flash_mode dio --flash_freq 40m`** and let the bootloader enable QIO from sdkconfig itself |
| a wrong ILI9341 vendor gamma/power sequence → garbled or tinted picture | copy the 20-entry `vendor_specific_init` from the xiaozhi board verbatim, together with `DISPLAY_INVERT_COLOR` and `RGB_ORDER(BGR)` |
| FT6336 and ES8311 share I2C, so init order matters | `bsp_i2c_init` → touch reset → codec configuration; call `lvgl_port_add_touch` after the touch reset |
| LVGL buffers in PSRAM causing tearing | use `double_buffer` with `full_refresh=0`; if measurement says otherwise, fall back to 40 lines in internal RAM |
| Opus encoding competing with LVGL for PSRAM bandwidth | the encoder uses internal RAM only (stack and frame buffer); PSRAM holds only display buffers and waterfall history |
| antenna/PA sharing IO1 with SD pins | confirm the PA enable level before encoding; SD stays disabled in V1 to avoid clashing with WS2812 (42) |

---

## 10. Milestones

| | Content | Acceptance |
|---|---|---|
| **M1** | board bring-up: ILI9341 + FT6336 + ES8311 | picture correct, touch reports coordinates, speaker plays a test tone |
| **M2** | MRRC protocol integration (login / WSradio / WSspectrum / Opus send and receive) | the server lists the connection and the other station is audible |
| **M3** | main-screen UI (frequency / spectrum / waterfall / status) | spectrum at 15 fps without tearing |
| **M4** | three overlays (volume / 10 settings / 6 channels) | every item can be changed on the radio and read back |
| **M5** | PTT and stability | 5 minutes of continuous transmission without an unkey, the 180 s timeout fires |

---

## 11. Test strategy

- **Unit-testable parts**: frequency formatting, step conversion, spectrum
  downsampling, gesture thresholds — run on the host (following the passport's
  `tools/validate.sh` approach).
- **Board-testable parts**: the acceptance item of each milestone M1–M5, run on the
  board and written back into a "measured" section of this document.
- **Performance regression**: `ui_fps` / `audio_frames` / `dropped` kept on a
  screen debug page (long-press the header to open it).
