<p align="right">
  <a href="CHANGELOG-optimization.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Change log (this optimisation phase)

Range: `1a99a2f` (PCM16 working) through everything after commit `802bbc8`. Each
entry records the **measured result** and **whether it was kept**. Rollback
point: `802bbc8`.

---

## 1. Changes that were kept

### 1. Add the channel byte to the TX uplink (critical fix)

**File**: `main/mrrc_tx.c`

```c
// before: [0x02][PCM]          — the server discards it as a spectrum frame
// after:  [0x01][0x02][PCM]    — 0x01 = TX audio channel
```

**Symptom**: transmitting produced no power and no audio.
**Cause**: the first byte of a `/WSterminal` binary frame is the channel
(`0x00` downlink audio / `0x01` uplink audio / `0x02` spectrum). The firmware put
the codec tag `0x02` first, so the server treated it as the spectrum channel and
dropped the PCM.
**Result**: transmit works, `peak=12540`.

---

### 2. Delete the dead 48 kHz code path

**File**: `main/mrrc_tx.c`

Every connection confirms `pcm16 accepted (16000 Hz)`, so the 48 kHz branch
never ran — and it was the source of an out-of-bounds write (`TX_FRAME_16K`
changed 320 → 640 so `i*3` wrote to index 1917 while the buffer held 960). The
whole branch is gone; only 16 kHz remains.

---

### 3. Spectrum tier `mid` → `high` (Full)

**File**: `main/mrrc_net.c`

| | Tier | Frame size | Frame rate |
|---|---|---|---|
| Before | `mid` | 851 B | ~2.7 frames/s |
| After | `high` | 1701 B | ~5.3 frames/s |

**Reason**: every frame is one waterfall row, so half the rate is half the row
rate and looks like crawling.
**Cost**: ~73 kbit/s (was ~18). The firmware's 850-bin buffer was already sized
for Full.

---

### 4. Drop the spectrum tier to `low` while transmitting

**File**: `main/mrrc_net.c`

On PTT it sends `spectrumCaps: low` (426 B / divider 4) and returns to `high` on
release. Nobody watches the screen while transmitting, and the saved bandwidth
goes to the 256 kbit/s uplink.

---

### 5. Waterfall 30 → 40 rows, trace 56 → 45 px

**File**: `main/mrrc_ui.c`

- `WF_H`: 30 → 40 (`.bss` 14.4 KB → 18.8 KB)
- `PLOT_H`: 56 → 45 (0.8×; the height goes to the waterfall)

**Tried 60 rows and abandoned it**: it pushed `largest` down to 3 KB, the PTT TLS
handshake could not get contiguous memory, and the transmission dropped itself.

---

### 6. Leave a 5-row black gap between trace and waterfall

**File**: `main/mrrc_ui.c`, `Y_WF`: 192 → 196

LVGL merges adjacent dirty areas, so with the waterfall flush against the trace
both were repainted as one block (about 40 KB). With the gap they repaint
independently.

**Measured**: `taskLVGL` 44% → 35% (8 points — helpful, not decisive).

---

### 7. Orange text on the top and bottom rows, plus a bottom rule

**File**: `main/mrrc_ui.c`

- Top bar (mode / VFO / battery) and bottom chips (FREQ / RX / LINK) all use
  `C_ORANGE`
- New `rule_bot`: a 1 px orange line at y=270, mirroring `rule` at y=26

---

### 8. One font size for text (12), frequency at native 44

**File**: `main/mrrc_ui.c` + `sdkconfig(.defaults)`

| Use | Size |
|---|---|
| Everything except the frequency | 12 |
| Frequency | 44 (native) |

**Why not `transform_scale`**: it does not enlarge the object itself, so a 42 px
glyph drew into a 28 px box and only the top half survived; making the box taller
then gave a **completely black screen** (it conflicts with the rounded-corner
mask callback).

**Font cleanup**: 12 / 20 / 44 → **12 / 44** (20 was never used).
`sdkconfig.defaults` still said 12/14/28, which no longer matched the code, and
was brought in line.

---

### 9. UI refresh 200 ms → 300 ms

**File**: `main/mrrc_ui.c`, `REFRESH_MS`

At 200 ms `taskLVGL` took 47–55%, the audio task got <1%, and the ring
overflowed. At 300 ms, dropped frames went 10.4% → 2.3%.

---

### 10. Skip spectrum/waterfall drawing while transmitting

**File**: `main/mrrc_ui.c`

```c
if (s_started && !tx) { update_plot(); wf_push(); }
```

While transmitting the radio is not receiving and the tier is already `low`, so
drawing buys nothing; the CPU goes to the network instead.

---

### 11. LVGL draw buffer 8 → 12 lines

**File**: `components/bsp/src/bsp_display_lvgl.c`

With `double_buffer = false` a flush means "render, then block on SPI": at 8
lines a 40-row waterfall needs 5 round trips, at 12 lines only 4. It costs
5.6 KB of contiguous internal RAM (was 3.8 KB).

**Measured**: no CPU change (34–37%), but dropped frames 0.46% → 0%.
**Watch**: the first sample of `largest` reached 29K, close to the 2× margin over
the 16K TLS needs.

---

## 2. Changes that were reverted

### A. Audio ring buffer 8 KB → 16 KB

**Why tried**: every drop counted as `full`, so capacity looked too small.
**Result**: drop rate unchanged (3.0% → 10.36%) and `largest` fell 42K → 25K.
**Conclusion**: not a capacity problem. Reverted.

---

### B. Batched frame reads in `rb_get`

**Why tried**: hypothesised that "one lock acquisition and one blocking I2S
write per frame cannot keep up with a 20 ms arrival cadence".
**Result**: tried alone, dropped frames went 0.28% → 2.43%, no improvement.
Reverted.

---

### C. spin + yield lock around `rb_put`

**Why tried**: hypothesised lock contention was dropping frames.
**Result**: drops went up (11% → 13%). The counters showed `lock=0` and all drops
were `full`, so the hypothesis was simply wrong. Reverted.

---

### D. Uplink queue (16-slot static ring, single writer)

**Why tried**: `esp_websocket_client`'s lock is contended across tasks and
`send_bin` returned `-1`.
**Result**:
- One perfect transmission right after it was added (`frames=491 sendfail=0`)
- Then it degraded to `err=-1` (actually a full queue); raising it to 16 slots
  was still not enough
- Fixing that meant changing the net task polling and the TX state machine, and
  it got messier as it went
- User feedback: "it gets worse the more you fix it"

**Conclusion**: layering a mechanism onto an architecture that already works is
the wrong move. All reverted (still in `stash@{0}`).

---

### E. LVGL task sleep patch (`tools/patch_lvgl_port.sh`)

**Why tried**: read `vTaskDelay(1)` inside `esp_lvgl_port` and assumed it spun
1000 times a second.
**Result**: **wrong reading**. The loop already sleeps through
`xEventGroupWaitBits(..., task_delay_ms)` as LVGL requires; that `vTaskDelay(1)`
just adds one more millisecond. The real rate is about 20 per second. The patch
did nothing; it was deleted and the copy on the server reverted.

---

### F. I2S DMA buffer 90 ms → 150 ms

**File**: `components/bsp/src/bsp_audio.c` (`dma_desc_num` 6→8,
`dma_frame_num` 240→300)

**Why tried**: hypothesised that stutter came from `auto_clear_after_cb`
inserting silence when a DMA block ran dry.
**Result**: the user measured it and reported "still broken". Reverted.

---

## 3. Documents

| File | Content |
|---|---|
| `docs/optimization-notes.md` | Measured performance data, refuted hypotheses, method |
| `docs/UI.md` | Current UI layout, palette, fonts, per-region details, change checklist |

Both documents label **measured / hypothesised / refuted** so the next pass does
not repeat the same detours.

---

## 4. Rollback point

```
802bbc8  <- a stable commit (UI changes + TX fix, receive works)
```

To return to the state around 17:00 these uncommitted changes must be undone:

| File | Revert |
|---|---|
| `components/bsp/src/bsp_audio.c` | DMA buffer 8×300 → 6×240 |
| `components/bsp/src/bsp_display_lvgl.c` | draw buffer 12 → 8 lines |
| `main/mrrc_audio.c` | ring 16K→8K, batched reads→single frame (already reverted once; confirm) |
| `sdkconfig.defaults` | align the font configuration |

`docs/UI.md` and `docs/optimization-notes.md` are new documents; keep them.

---

## 5. Open problems

**Receive still stutters.** These directions are ruled out:

- Application-level ring buffer capacity
- Lock contention
- Batched frame reads
- I2S DMA depth (both 90 ms and 150 ms tried)

**Not yet tried**:

1. `auto_clear_after_cb = false` (stop the DMA from filling silence itself)
2. ES8311 window/mute/power configuration
3. Whether the server's downlink cadence merges bursts
4. Looking at the I2S waveform directly with a scope or logic analyser
