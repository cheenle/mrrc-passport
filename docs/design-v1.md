<p align="right">
  <a href="design-v1.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# MRRC terminal V1 design (ESP32-C3 / FoloToy AI Passport hardware)

> Status: design settled; S0 is implemented and compiles (`main/app_main.c`).
> This document states only what is certain. Anything "still to be measured" is
> marked as such; estimates are never presented as conclusions.

> Later work changed parts of this document: the codec library was removed
> (§6.1), the transport was retuned, and the ring grew to 16 KB. Where a section
> describes the state on the day it was written, the current numbers are in
> [transport tuning](transport-tuning.md) and
> [optimisation notes](optimization-notes.md).

## 1. Goal

An MRRC terminal on the AI Passport hardware: **it listens, transmits, tunes, and
shows a spectrum**.

The first draft said "no spectrum, no waterfall" because the C3 could not afford
them. **That judgement was overturned**: after optimisation `taskLVGL` went from
58% to 33–42% and every one of the 40 waterfall rows is measured data. What
exists now is a full handheld interface (§8).

What is genuinely out of scope: BLE (the C3 has BLE 5.0 only, NimBLE needs 19 KB,
and another 30 KB of memory would have to be found first), recording, and
anything needing PSRAM.

## 2. Hardware facts

Order of authority: `components/bsp/include/bsp_pins.h` → the BSP implementation
→ the hardware guide → Espressif's `soc_caps.h`.

| Item | Fact | Source |
|---|---|---|
| MCU | ESP32-C3, single RISC-V core at 160 MHz | `SOC_CPU_CORES_NUM (1U)` |
| FPU | **none**, floating point is emulated | `esp32c3/soc_caps.h` has no `SOC_CPU_HAS_FPU` (the S3 does) |
| RAM | 400 KB SRAM, **no PSRAM** | no `SOC_SPIRAM_SUPPORTED` |
| Flash | 8 MB XMC | measured with `flash_id` |
| I2S | **one** peripheral, no APLL | `SOC_I2S_NUM (1U)`, only `SOC_I2S_SUPPORTS_PLL_F160M` |
| Display | ST7789P3 240×320 RGB565, SPI2 80 MHz mode 0, round panel | `bsp_pins.h` |
| Audio | ES8311 codec, I2C 0x18 + I2S0 full duplex | `bsp_pins.h` §audio |
| Keys | UP/DOWN/OK, **sharing one ADC channel on GPIO0** (resistor ladder) | `bsp_pins.h` §keys |
| Battery | 500 mAh with a CW2017 gauge (I2C 0x63) | `bsp_pins.h` §I2C |
| Size | 60 × 95 × 8.5 mm, 50 g | `docs/hardware-design/specifications.zh_CN.md` |

## 3. Four hard constraints

These decide the architecture and cannot be worked around.

### 3.1 Full duplex shares the clock, and 48 kHz capture is dead

The ES8311's MCLK/BCLK/WS are shared by I2S0's TX and RX; "receive at 48 kHz, send
at 16 kHz" does not physically exist on this board.

And **measurement killed 48 kHz capture on this chip**: same capture code, same
microphone, only the codec rate changed:

| Codec rate | PCM peak (full scale 32767) |
|---|---|
| 48 000 Hz | **468** |
| 16 000 Hz | **12 708** |

A factor of 27. The `ref/ai-passport` audio demo only ever runs at 16 kHz;
48 kHz capture was never verified.

**Decision**: the codec is fixed at **16 kHz** and never switches. The server
wants 48 kHz, so software resampling bridges the two:

- downlink: decode 48 kHz → 3:1 decimation (three-point average, doubling as the
  anti-alias filter) → play at 16 kHz
- uplink: microphone 16 kHz → 3:1 zero-order hold → 48 kHz PCM (the server
  resamples again to 44.1 kHz)

Second rule: **never call `bsp_audio_set_format` at runtime**. It deletes and
rebuilds the codec and reconfigures I2S, which is an audible pop plus tens of
milliseconds of dropout.

### 3.2 No APLL, so the clock is about 0.16% off

MCLK = 256×fs and the only source is PLL_F160M (160 MHz):

| Nominal fs | MCLK needed | Divider used | Actual fs |
|---|---|---|---|
| 16 kHz | 4.096 MHz | 39 | ≈16.03 kHz |
| 48 kHz | 12.288 MHz | 13 | ≈48.08 kHz |

**Consequence**: the device clock runs about 0.16% fast, so a long listening
session slowly drains the jitter buffer — roughly one second every ten minutes.
Opus PLC and the peer's buffer absorb it, but it **must be measured over 30+
minutes**; a short connection proving nothing is not the test.

### 3.3 No PSRAM, so every buffer lives in internal RAM

- A full 240×320×2 framebuffer is 153.6 KB — **impossible**.
- The LVGL arrangement used here: a 40-line single buffer (19.2 KB) plus the
  rounded-mask work at runtime.
- Measured baseline from the BSP documentation: **about 130 KB of system heap**
  available (LVGL running, no network).
- Wi-Fi then takes another 50–60 KB.

### 3.4 Single core, no FPU

- Opus must be fixed point (`CONFIG_OPUS_FLOATING_POINT=n`).
- Any float work (normalisation, anti-aliasing, AGC) is software floating point
  and must be budgeted per frame.
- The Wi-Fi stack and the audio codec share the one core: **the audio task must
  be preemptible but never starved**. The PTT heartbeat priority (V1 called it
  `txhb`) follows the xz-02 lesson.

## 4. Memory budget

### 4.1 Static buffers (`.bss`, fixed at compile time)

| Item | Size | Location |
|---|---:|---|
| audio ring `s_rb` | 16 384 B | `mrrc_audio.c` |
| 40-row RGB565 waterfall | ~18.8 KB | `mrrc_ui.c` |
| spectrum point array `s_pts` | 240 × 4 = 960 B | `mrrc_ui.c` |
| WS receive assembly buffer | 8 KB | `mrrc_net.c` |

### 4.2 Measured at runtime (2026-11-20, after several changes)

| Scenario | free | largest | Note |
|---|---:|---:|---|
| boot complete | 56 KB | 26 KB | baseline |
| 2 minutes of quiet listening | 43 KB | 18 KB | |
| 17 minutes of listening | 51 KB | 29 KB | a reconnect happened in between |

### 4.3 `largest` is the only number that matters

The TLS handshake needs a **16 KB contiguous block**. `free` showing 43 KB does
not mean it is usable — after fragmentation `largest` may be 16 KB and the
handshake fails, seen as repeated `linkdown` reconnects.

The margin is thin: `largest` is 18–29 KB, so 2–13 KB above the handshake.
**Before adding any buffer larger than 4 KB, look at this line.**

### 4.4 Two changes that tightened memory (know the cost of each)

| Change | Contiguous memory spent | Bought | Worth it |
|---|---:|---|---|
| audio ring 8 → 16 KB | 8 KB | the session-start burst stops dropping frames (253 → 0) | yes |
| WS buffer 4 → 8 KB | 4 KB | the server no longer stalls for 5.8 s | yes |

Both traded memory for a diagnosed fault, not for insurance. If `largest` falls
below 16 KB, **roll back the WS buffer first** (its 4 KB bought a latency
improvement; the ring's 8 KB bought real dropped frames).

## 5. Software architecture

```
main/
  app_main.c     boot order, heap markers, key task, one statistics line a minute
  mrrc_net.c     Wi-Fi, login, WS send/receive, state parsing, PTT and tier negotiation
  mrrc_audio.c   PCM16 ring → I2S (no codec)
  mrrc_tx.c      PTT uplink: I2S capture → framing → WS send, with a 1.5 s heartbeat
  mrrc_ui.c      LVGL interface (40-row waterfall, S-meter / power bar, four-position knob)
components/bsp/  i2c / display / audio / button / battery
```

Task split (one core, so priority is everything):

| Task | Priority | Stack | Job |
|---|---|---|---|
| `audio` | 8 | 8 KB | ring → I2S write; the RX path's real-time deadline |
| `txtask` | 7 | 4 KB | PTT uplink capture and send, plus the 1.5 s heartbeat |
| `net` | 5 | 6 KB | WS send/receive, JSON parsing, state updates |
| `keys` | — | 3 KB | consumes the key event queue |
| LVGL | 2 (port default) | — | rendering and flush |

**Heartbeat and uplink share one task** (`mrrc_tx.c`) at priority 7: above `net`,
below `audio`. The xz-02 lesson was that a heartbeat written into the main loop
gets starved by a codec-saturated core; there is no codec here, but a 20 ms I2S
read plus a WS write is still a hard deadline, so it must preempt `net`.

### 5.1 Real contention on one core (measured 2026-11-20)

The stutter was not mysterious; three numbers say it:

| Observation | Number | Meaning |
|---|---|---|
| server pump stall | **5791 ms** | `ws.send_bytes()` blocked for 5.8 s, i.e. the peer was not reading |
| frames actually received | **40.9 /s** (50 needed) | 82% of nominal, one frame in 5.5 missing |
| device memory | `free 59K largest 46K` | out-of-memory ruled out |

Bandwidth was never the constraint: audio 31 KB/s + spectrum 9 KB/s = **0.33
Mbps**. The cause was **four kinds of traffic on one TCP connection** with a
64-frame send queue (§9).

Three fixes, in the order they actually helped:

1. **WS receive buffer 4096 → 8192.** When `esp_websocket_client` runs out of
   buffer it does not drop packets, it **blocks the caller**, so the server's
   `send_bytes()` stalls — that was the 5.8 s.
2. **Drop the spectrum tier `high` → `low` on PTT.** This logic **did not exist
   at all**; while transmitting, 1701-byte spectrum frames queued ahead of
   642-byte audio frames.
3. **Discard spectrum frames on the device while keyed.** The server offers
   `high/mid/low` with no `off`, so "stop entirely" can only be done device-side.

Result: `err=0 drop=0`, audio back to 49.9 frames/s against a 50.0 nominal.

**Cost**: the WS buffer takes 4 KB more, so `largest` fell from 46 KB to 16–29 KB.
The PTT TLS handshake needs 16 KB contiguous, so the margin is thin — the number
to watch (§4.4).

## 6. Audio design

### 6.1 No codec (the turning point of 2026-11-19)

The first design used Opus (micro-opus 0.4.1). **Removed**, replaced by raw PCM16.

| | Opus | PCM16 |
|---|---|---|
| flash | +92 KB | — |
| heap `largest` | 17 KB | 58 KB |
| uplink bandwidth | 12–24 kbps | 256 kbps |
| codec CPU | significant | **zero** |

On one 160 MHz core, neither libopus's CPU nor its 16 KB contiguous heap was
affordable. The cost is 20× the bandwidth, but 256 kbps is nothing to Wi-Fi — and
measurement showed bandwidth was never the bottleneck (§5.1).

### 6.2 Uplink frame format

```
[0x01 channel][0x02 tag][320 × int16]  = 642 B / 20 ms
```

**The channel byte cannot be omitted.** Without it the server routes the PCM as
spectrum, which is heard as "transmits with no power" — that bug took a long time
because the device looked perfectly healthy while the other end heard nothing.

### 6.3 Sample rate

**16 kHz is a hardware fact, not a preference**: the ES8311 is nearly deaf when
capturing at 48 kHz (same speech, peak 468 versus 12708 at 16 kHz). That is
plenty for SSB's 3 kHz passband.

### 6.4 Receive buffer depth

`AUD_RB_BYTES = 16384` (about 0.51 s).

It came from 8 KB because of a measurement: at session start TCP delivers the
backlog in one burst (91.9 frames/s observed at the head, against a 50/s
ceiling), a 0.26 s ring cannot absorb it, and 253 frames were lost in six
minutes. At 16 KB, `drop=0`.

Note what this does *not* fix: **a bigger buffer absorbs bursts, not sustained
contention**. The latter is §5.1's TCP sharing, solved by the tier change and by
discarding spectrum. Two problems, one symptom, different causes.

### 6.5 Microphone gain

The ES8311 input PGA is **stepped**, not continuous:

```
0 / 6 / 12 / 18 / 24 / 30 / 36 / 42 dB
```

The driver maps any request into those buckets. **With a 1 dB step, 18 through
23 dB write the same register value** and sound identical — that trap cost a long
time. The knob now steps 6 dB, and the number on screen is the number in the
codec.

The ceiling is 42 dB, the chip's maximum; **there is no 48 dB setting**. Default
is 18 dB.

## 7. Keys and PTT

### 7.1 Mapping

| Gesture | Action |
|---|---|
| OK held ≥600 ms | **PTT transmit** (stops on release) |
| OK double-tap | cycle FREQ / VOL |
| UP / DOWN tap | FREQ: ±1 kHz; VOL: speaker volume ±5% |
| UP / DOWN held | auto-repeat (after 450 ms, one step every 120 ms) |

The long-press threshold is 600 ms (`BSP_BTN_LONG_PRESS_MS` was 500; raise it to
600). Same reason as xz-02: **a short tap must never emit carrier**.

### 7.2 Three facts that must be in the code

1. **The release event is 120 ms late** (`BSP_BTN_HOLD_MS` in the key lock). The
   tail of every transmission is 120 ms long; it is a hardware-plus-driver delay
   that cannot be removed, only accounted for.
2. **A contact break longer than 120 ms is read as a release.** If the key
   contact is unstable during a long press, the transmission stops mid-way.
   `bsp_button_glitch_count()` is the probe: hold for 10 s and watch whether the
   count grows.
3. **The callback runs in the `esp_timer` task**, so it may only enqueue. It must
   not block, record, or touch the UI.

### 7.3 PTT state machine (xz-02's safety chain, copied)

```
press past 600 ms → ptt:true → txhb → wait 200 ms → start pushing frames
release → stop frames → "s:" → ptt:false → back to IDLE after 200 ms
held 180 s → force ptt:false plus "TX TIMEOUT" on screen
every 1.5 s while transmitting → txhb
```

When the server answers `PTT released` / `FT8 transmitting` / `CQ in progress`,
unkey to match: those three are the server forcing a release and cannot be
ignored.

## 8. UI design

### 8.1 LVGL (design correction: hand-drawn was the plan, LVGL was kept)

Measurement showed LVGL plus a 40-line draw buffer costs only 37 KB, and after
Wi-Fi there are still 112 KB of contiguous block available, so V1 uses
ai-passport's existing LVGL path (`bsp_lvgl_init`) rather than inventing a
hand-drawn renderer.

Two ai-passport behaviours are kept (both found on real hardware; do not rewrite
them):

- the round panel is masked row by row on `LV_EVENT_FLUSH_START` rather than with
  `clip_corner` (the latter builds an ARGB layer and fails inside a 24 KB LVGL
  pool);
- `CONFIG_LV_USE_CLIB_MALLOC=y` (use the system heap, avoiding an assert when the
  static pool runs dry).

The hand-drawn strip plan stays in the document as a fallback: if V2 needs both
spectrum and waterfall and memory runs out, drop LVGL then.

### 8.2 Layout (240×320 portrait)

```
y   0.. 28   header: LSB/USB badge | VFO A | battery %
y  40..100   frequency: "7.215.00", 32 px anti-aliased
y 108..130   info row: S9+10 | FIL 2.4K | AF 128
y 140..250   S-meter: 20 segments plus peak hold
y 260..300   status chips: [FREQ] [RX/TX] [LINK]
```

No spectrum, no waterfall — that was V1's scope decision, not "not done yet".

## 9. Network design

### 9.1 Channels (final form: one connection)

| Channel | Carries | Note |
|---|---|---|
| `/WSterminal` | control JSON + downlink PCM + uplink PCM + spectrum | **the only TLS session** |

**Why only one**: each TLS handshake on the C3 needs 16 KB of contiguous heap and
four of them simply do not fit (`largest` was around 20 KB). The cost is four
kinds of traffic sharing one TCP connection and one send queue — the root of
§5.1's stutter, and the reason the device now drops the tier and discards
spectrum while transmitting.

Frame formats (all three differ; do not copy one to another):

| Type | WebSocket opcode | Frame |
|---|---|---|
| control | `0x01` text | JSON; anything over 4 KB arrives in fragments and must be assembled by `payload_offset` |
| downlink audio | `0x02` binary | `[0x00][0x02][int16 samples]`, 20 ms = 642 B, 50 frames/s |
| uplink audio | `0x02` binary | `[0x01][0x02][int16 samples]`, **two layers!** |
| spectrum | `0x02` binary | `[0x02][0x01] + 850 bins`, 1701 B (Full tier), about 5.3 Hz |

**The two directions are not symmetric**, and copying one from the other breaks:
downlink carries `[0x00]` followed directly by the codec tag, while uplink needs
`[0x01 channel][0x02 tag]`. With the channel byte missing the server routes the
PCM as spectrum: the device looks healthy and the far end hears nothing.
(The old text saying "uplink is single-layer" was wrong and was corrected with
the 2026-11-19 fix.)

Authentication is always `?token=`: `server.py` checks `ws.query_params` and does
**not** accept an Authorization header.

### 9.2 V2: multiplexing on one connection (needs a server change)

If §4.3's measurements showed even two TLS sessions do not fit, the server would
need a `/WSterminal` channel carrying control, downlink, and uplink. It is our own
server, so the change is controllable but touches three handlers in `server.py` and is a
separate task.

**V1 does not depend on it; V2 does.**

### 9.3 Traps hit during implementation (S1–S3)

**① `MBEDTLS_SSL_IN_CONTENT_LEN` too small, so the WebSocket died on the first read**

The first version set 2048 / 1024 to save memory and the WS session died right
after the handshake:

```
E esp-tls-mbedtls: read error :-0x7100
E transport_base: esp_tls_conn_read error, errno=Connection already in progress
E websocket_client: esp_transport_read() failed with -28928
```

The server's first `fullState` frame is larger than the TLS record buffer, so the
handshake (small packets) succeeded and the first real read broke. Setting
**6144 / 2048** (matching xz-02) fixed it. Cost: the TLS session went from 12 KB
to about 16 KB, which was affordable.

**② `/api/auth/login` is rate limited to 5 attempts per 5 minutes per IP**

`server.py:218` hard-codes `_LOGIN_MAX_ATTEMPTS = 5` and
`_LOGIN_WINDOW_SECONDS = 300`. During development the device is reflashed and
rebooted repeatedly, logging in every boot, which trips 429 quickly; a firmware
retry at a fixed 3 s interval would lock itself out forever.

The firmware now backs off exponentially: 30 s → 60 → 120 → 240 → 600 (cap),
resetting on success.

Development advice: relax the server limit while debugging (say 20 per 5 minutes)
or cache the token in NVS. Without either, every firmware check costs five
minutes.

**③ The ES8311 microphone is nearly deaf at 48 kHz (a real bug)**

Same capture code, same microphone, only the codec rate changed:

| Codec rate | PCM peak (full scale 32767) |
|---|---|
| 48 000 Hz | **468** |
| 16 000 Hz | **12 708** |

A factor of 27. The `ref/ai-passport` audio demo only ever uses 16 kHz. Final
arrangement: codec fixed at **16 kHz**; received audio decoded at 48 kHz and
decimated 3:1 (three-point average also anti-aliasing); transmitted audio
zero-order-hold upsampled 3:1 to 48 kHz for the server (which resamples to
44.1 kHz itself).

**④ One refuted conclusion (recorded so it is not repeated)**

During S3, `frames=34 / 5 s` was read as "TLS uplink gives only ~104 kbit/s, so
48 kHz PCM cannot work and Opus is mandatory". **That inference was wrong**: the
same log had `linkdowns=2`, and `tx_task` sends nothing while the link is down, so
the counter cannot be used for steady-state throughput. Measurement (by ear) said
48 kHz raw PCM worked and bandwidth was sufficient; the Opus uplink plan was
scrapped.

Lesson: before using a counter to estimate throughput, confirm the counter only
covers time when the link was up.

**⑤ Ordering between PTT and `/WSaudioTX` (otherwise the radio keys with no modulation)**

`server.py` claims TX uplink ownership when it receives `ptt:true`:

```python
for tx_ws in list(audio_tx_clients):
    if _ws_tokens.get(tx_ws) == token:
        _tx_owner_ws = tx_ws
```

It scans the `audio_tx_clients` that are **already connected**. If `ptt:true` goes
out first and `/WSaudioTX` connects afterwards, our socket does not exist at scan
time, ownership lands on someone else (the browser), and every frame we send is
discarded as non-owner — which is exactly "the FT-710 transmits with no audio".

Correct order: **connect `/WSaudioTX`, wait for it to be accepted, then send
`ptt:true`.**

## 10. Milestones

| Stage | Content | State |
|---|---|---|
| **S0** | probe firmware: Wi-Fi / TLS / codec / Opus staged markers | ✅ 154 KB free / 112 KB largest |
| **S1** | `/WSradio` control channel + LVGL UI + tuning keys | ✅ |
| **S2** | downlink audio (Opus 48k → 3:1 → 16k → speaker) | ✅ `err=0 drop=0` |
| **S3** | PTT uplink (16k microphone → 48k PCM) | ✅ voice confirmed on air |
| **V2-1** | single `/WSterminal` connection (control + downlink + uplink) | ✅ largest 18 → 26 KB |
| **V2-1b** | four-position knob + channel list + triple-tap mode | ✅ |
| **V2-1c** | IPv6 + public hostname `w103d.vlsc.net` | ✅ |
| **V2-2** | spectrum + waterfall (`lv_line` / `lv_canvas`) | ✅ |
| **V2-3** | power management and sleep | ⬜ |

## 11. Acceptance criteria and measured results

| Criterion | Result |
|---|---|
| `ptt:false` reached the server after release | ✅ |
| continuous transmission without disconnecting | ✅ `frames=212 sendfail=0` |
| speech intelligible | ✅ confirmed on air |
| receive without dropped frames | ✅ `err=0 drop=0` |
| idle `largest ≥ 20 KB` | ✅ 26 KB |
| long PTT with no false contact break | ✅ `glitch=0` |
| knob / triple tap / channels | ✅ log shows `mode -> VOL`, `chan 5 -> 7210000 LSB` |
| 30 minutes of listening without drift | ⬜ not measured |

## 12. Risk list

| Risk | State | Mitigation |
|---|---|---|
| **`largest` only 18–29 KB** | **most important** | TLS needs 16 KB contiguous; check §4.3 before adding buffers |
| latency cost of the 16 KB ring | present | normally no backlog; at worst 0.5 s more |
| several clients receiving audio | present | with 2–3 operators the server's fan-out doubles and the local frame rate drops |
| 0.16% clock drift (no APLL) | unverified | needs a 30-minute session watching buffer level |
| ES8311 capture at 48 kHz | avoided | codec fixed at 16 kHz |
| contact break during a long press | not seen | `glitch=0`; keep watching |

## 12.1 How to debug "stutter" (the lesson of 2026-11-20)

This round took one wrong turn worth recording, because the symptom lies.

**Symptom**: choppy audio. **First reaction**: not enough buffer. **Result**:
8 KB → 16 KB looked good for two minutes (`drop=0`), then after 17 minutes the
frame rate fell to 34.5/s, worse than before the change.

**Why the diagnosis was wrong**: only the device's `drop` was examined, never the
server's `pump stall`. They are two views of one illness:

| Number | Where | Meaning |
|---|---|---|
| `pump stall` | server log | how long the server's socket write blocked → **link or peer is slow** |
| frame rate | device log | frames actually received per second → 50/s needed |
| `drop` | device log | frames lost to a full ring → **buffer too shallow** |
| `largest` | device log | contiguous heap → whether handshakes fail and reconnect |

**Order to check in** (use this next time):

1. Compute the bandwidth first. **At a measured 0.33 Mbps, bandwidth is innocent**;
   stop suspecting it.
2. Look at the server's `pump stall`. Hundreds of milliseconds means the peer is
   not reading, not that the network is short.
3. Check whether the device's frame rate matches 50/s. If it does not while `drop`
   stays small, it is **reconnecting**, not dropping.
4. Only a large `drop` with a low frame rate means buffer depth.

**The real root cause here**: on one TCP connection, 1701-byte spectrum frames
queued ahead of 642-byte audio frames, and transmission added a 16 KB/s uplink on
top. Three things in one queue, heard as "the network stutters".

**Lesson**: do not substitute "enlarge the buffer" for locating the cause. A
buffer absorbs bursts; it cannot absorb sustained contention.

## 13. On-device profile (‑O2 build, 60 s quiet-listening window)

Collected with `CONFIG_MRRC_PROFILE=y` (off by default; run-time statistics charge
every tick).

### CPU

| Item | Value |
|---|---|
| `opus_decode()` per frame | **5.4 ms** |
| actual downlink rate | 2057 frames / 60 s ≈ **34 frames/s** |
| share of real time spent decoding | **≈ 18.5%** (5.4 ms × 34 / 1000 ms) |
| IDLE share | **37%** ⇒ system load ≈ 63% |

Conclusion: decoding was the largest single item but a third of the CPU remained.
Extra features like the spectrum are theoretically affordable, but the number of
connections must come down first (each extra TLS session adds encrypt/decrypt
work).

### Memory

| Metric | Steady state | Note |
|---|---|---|
| free | 31–33 KB | |
| **largest contiguous block** | **9–10 KB** | TLS needs 16 KB, assembled from several blocks |
| **historical minimum** | **5 KB** | at the peak it nearly ran out — the number to watch |
| DMA-capable | 23–24 KB | healthy |
| fragmentation | free/largest ≈ **3.4×** | high |

The fragmentation has a known source: every `fullState` is a 5.5 KB JSON document
allocated and freed whole by cJSON, repeatedly cutting the internal heap. That
`largest` was only 9 KB yet the handshake still completed means mbedTLS allocates
in several blocks — which also explains the occasional `linkdowns` while
transmitting.

**Further memory work should target `largest`, not `free`**: either parse
`fullState` into a reused buffer, or reserve a contiguous heap region for TLS.

### 9.3.6 IPv6: all three switches are required

When switching to the public hostname the interface kept reporting
`0 IPv6 address(es)`; it took layer-by-layer checking to find:

```c
CONFIG_LWIP_IPV6=y                    // only compiles IPv6 into lwIP
CONFIG_LWIP_IPV6_AUTOCONFIG=y         // only allows accepting RA (SLAAC)
esp_netif_create_ip6_linklocal(netif) // must be called explicitly, after GOT_IP
```

`esp_netif_create_default_wifi_sta()` does **not** attach a link-local address, so
the first two had no effect until the third was added. After that `fe80::` appears
immediately, a global `2409:...` address follows within about 6 seconds (first
RA), and `/WSterminal` connects to `w103d.vlsc.net` over IPv6. Note that
`esp_ip6_addr_t.addr` prints in little-endian word order, so naive formatting
shows it reversed.

### 9.3.7 Keys: double tap and triple tap share one button

OK carries PTT (hold), the knob cycle (double tap), and mode switching (triple
tap). Double and triple tap inherently conflict on one key: if the double tap
acted immediately, the action would already have happened when the third tap
arrives. So the double-tap action waits out a **600 ms** window (the key task
polls at 50 ms), which is the 0.6 s delay the operator feels.

Also, `iot_button`'s `BUTTON_DOUBLE_CLICK` **never fires** on this board: it
requires `repeat == 2`, and the BSP key lock delays the release by 120 ms so the
second press lands inside the first press's hold window. Click counting is
therefore done in the application (§9.3.5).
