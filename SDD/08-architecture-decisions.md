<p align="right">
  <strong>English</strong> · <a href="08-architecture-decisions.zh_CN.md">简体中文</a>
</p>

# 08. Architecture Decisions (ART 0513)

Each decision is written in the same five parts, because the useful part of a
decision is rarely the decision:

- **Problem** — the fault or constraint that forced a choice;
- **Alternatives** — what was rejected, and by what measurement;
- **Decision** — one sentence, so it can be quoted;
- **Consequences** — what this now costs, including what must never be done;
- **Amendments** — later facts, appended and dated (chapter `00` §V4).

An AD is never renumbered and never deleted. When a decision is overturned, the
overturning is recorded here as an amendment that names the measurement, and the
`Status` moves down the chapter `00` §V5 ladder honestly.

---

## AD-001: One session, one socket — `/WSterminal`

| Attribute | Value |
| --- | --- |
| Type | Architectural / Protocol |
| Status | Implemented, field-exercised (2026-11-21) |
| Decision | A single TLS/WebSocket session carries control text and three binary channels: `0x00` RX audio, `0x01` TX audio, `0x02` spectrum. The first payload byte is the channel; the rest is the legacy endpoint's payload, unchanged. |

**Problem.** The reference server exposes four separate endpoints. On this board
each one is a TLS session, and a TLS session is a 16 KB contiguous allocation
(`docs/design-v1.md` 4.3). Four of them do not fit. Worse than not fitting: the
transmit path had to tear down the downlink socket and rebuild the uplink socket
at the moment of keying — which costs a settle gap precisely when the operator
starts speaking, and loses the first syllable every time.

**Alternatives.** (a) Keep four sockets and shrink the TLS buffers — measured
`largest` falls to 18 KB, below the handshake floor, seen as reconnect loops.
(b) Open the TX socket lazily at key-up and close it at release — that is the
settle gap itself, and it makes key-up depend on a 400 ms handshake. (c) Invent a
leaner terminal protocol — then role checking, PTT owner arbitration and the state
machine drift between two implementations, which is how the server's own
four-endpoint PTT races appeared.

**Decision.** One session, one socket, channel-prefixed binary frames.

**Consequences.**
- The server's role/token checks and PTT owner arbitration apply unchanged; the
  firmware inherits them instead of re-implementing them.
- Media congestion must **not** be treated as a control failure: dropping an
  audio or spectrum frame is normal, killing the socket for it is not
  (`05` NFR-024, guardian rule `single-terminal-endpoint`).
- Spectrum is a luxury on this socket: while transmitting, the tier is dropped
  rather than queued, because a 40 KB spectrum backlog would delay the uplink.
- Every legacy client (browser, iOS, Android) keeps its endpoints — this is a
  terminal-only decision, not a protocol migration.

---

## AD-002: PCM16 only — no Opus decoder in this image

| Attribute | Value |
| --- | --- |
| Type | Design / Resource |
| Status | Implemented, field-exercised (2026-11-19) |
| Decision | The terminal declares the PCM16 profile and streams raw 16-bit mono; the server converts its own 48 kHz bus down to it. |

**Problem.** Opus looks free — the bitrate drops from 256 kbps to 12 kbps — but
the decoder is not free on a single 160 MHz core with no FPU. Measured on this
unit: **≈24 KB of heap and ≈60% of the core** while decoding. `free` at the time
was 49–55 KB and `largest` 44 KB; 24 KB is half the margin the TLS handshake
depends on.

**Alternatives.** (a) Keep Opus and drop the draw buffer to 8 lines — the UI
falls to 58% CPU and the waterfall becomes interpolated. (b) Decode Opus only
while not displaying — the display is always on. (c) Negotiate per direction —
the uplink would gain nothing, the bottleneck was never the link: the measured
downlink ceiling is set by this device's queues at ~31 KB/s audio either way.

**Decision.** PCM16, 16 kHz mono, 641 B per 20 ms uplink frame.

**Consequences.**
- Bandwidth becomes 256 kbps per direction and that **is not** the bottleneck
  (measured `31.3 KB/s` audio + `17.8 KB/s` spectrum = `50.2 KB/s` total, the same
  as a reference client on the same access point).
- The server must do the resampling; a server that only speaks Opus cannot serve
  this terminal, which is a deliberate, documented incompatibility.
- `opus` must not reappear as a symbol anywhere in `main/` — guardian rule
  `no-opus-in-firmware` exists because "we could compress this" is a recurring
  and reasonable-sounding idea.

---

## AD-003: The codec is fixed at 16 kHz, forever

| Attribute | Value |
| --- | --- |
| Type | Design / Hardware fact |
| Status | Implemented, hardware-verified (2026-11-18) |
| Decision | The ES8311 and both I2S directions run at 16 kHz for the life of the firmware; `bsp_audio_set_format` is never called at runtime. |

**Problem.** 48 kHz capture on this board is not slow, it is **deaf**. Same
capture code, same microphone, only the codec rate changed:

| Codec rate | PCM peak (full scale 32767) |
| --- | ---: |
| 48 000 Hz | **468** |
| 16 000 Hz | **12 708** |

A factor of 27. The upstream demo (`ref/ai-passport`) only ever ran at 16 kHz, so
48 kHz capture had never been exercised.

**Alternatives.** (a) Capture at 48 kHz and apply +30 dB of software gain — the
signal is there but the ADC noise is too, and it is quantised above the noise
floor; this is why the "quiet microphone" hunt wasted three days. (b) Two codec
instances — the ES8311 shares MCLK/BCLK/WS between capture and playback on this
board, so "48 kHz in, 16 kHz out" does not physically exist. (c) Reconfigure per
session — `bsp_audio_set_format` deletes and rebuilds the codec: an audible pop
plus tens of milliseconds of dropout, at the moment of keying.

**Decision.** 16 kHz, fixed, with software bridging on both paths: downlink 48 →
16 kHz by 3:1 decimation (the three-point average doubles as the anti-alias
filter); uplink 16 → 48 kHz by 3:1 zero-order hold, which the server then
resamples onward.

**Consequences.**
- The clock has no APLL: MCLK comes from PLL_F160M through a divider of 39, giving
  **16.03 kHz — 0.16% fast**. Over 30 minutes that is about a second of drained
  buffer, and it is why a short listening test proves nothing (`05` NFR-027).
- Uplink aliasing above 8 kHz is unavoidable and accepted; the radio's own TX
  bandwidth filter is far below it.
- Any future change of codec rate is a hardware-level claim and must arrive with
  a peak measurement, not a datasheet citation.

---

## AD-004: A congested uplink loses a frame, not the session

| Attribute | Value |
| --- | --- |
| Type | Design / Reliability |
| Status | Implemented, field-exercised (2026-11-22) |
| Decision | Device: the per-frame uplink write timeout is 100 ms. Server: a socket that holds the PTT receives **no** downlink audio. Both halves are required; either one alone does not fix the fault. |

**Problem.** "The PTT releases itself after a few seconds" was diagnosed as a key
fault three times. The button log says otherwise: one press held 16.5 s with no
release event. The uplink was dying:

```
tx: frames=78 sendfail=71
esp_transport_write() returned 0, timeout_ms=1000
net: /WSterminal error -> disconnected -> Failed to send the buffer
reconnect every ~2 s
```

Root cause: the server fanned the radio's RX audio out to the terminal socket
whether or not it was keyed. During transmission that one TCP therefore carried
**32 KB/s up** (PCM16 microphone) and **30 KB/s down** (audio the device had
muted and discarded anyway). On a mobile link the downlink saturates the window,
the uplink write blocks for a second, and the websocket client treats a failed
write as fatal and tears the session down — the TX indicator goes out, which the
operator reads as "the key released".

**Alternatives.** (a) Client-side mute only — the device already muted; the
server kept sending, and the frames still consumed window. (b) A new "audio off"
profile — the server's two capability parsers only know `pcm16`/`pcm48`, so it is
a protocol change on both sides to save what a `continue` saves. (c) Raise the
write timeout — that converts a 1 s stall into a longer stall and reconnects 2 s
later. (d) Drop the downlink socket while keyed — that is AD-001's settle gap
returning.

**Decision.** Server skips downlink audio for the keyed socket (half duplex: the
radio is not receiving, and those frames are not a loss — nothing was produced);
device drops a frame it cannot write in 100 ms.

**Consequences.**
- A frame that cannot be written in 100 ms cannot be followed by the next four
  either; failing fast costs one frame instead of turning the transmit loop into
  2 frames/s while the microphone keeps producing 50.
- The keyed socket's downlink silence must **not** be counted as a drop, and the
  realtime ratio for a heavily transmitting session reads low as a *correct*
  result (server-side accounting; recorded in `mrrc_modern/SDD/` AD-040).
- Measured after, one 30.5 s transmission: `frames=1500 sendfail=0 readfail=0`,
  50.0 frames/s, worst send 27.9 ms, `slow=0`, and **zero** W/E log lines inside
  the transmit window.
- Either half can be "temporarily" removed by a future change and both will look
  innocent in isolation. Guardian rule `half-duplex-context` exists to make the
  coupling legible at the call site.

---

## AD-005: Waterfall as 8-bit palette indices

| Attribute | Value |
| --- | --- |
| Type | Design / Resource |
| Status | Implemented, field-exercised (2026-11-22) |
| Decision | The 240×40 waterfall stores one byte per pixel, an index into a 256-entry palette; `heat_init()` fills the ramp once, before the first consumer runs. |

**Problem.** `uint16_t s_wf_buf[240 × 40]` was 19 200 B of `.bss` — the largest
single block in the application — and `largest` was 27–42 KB against a 16 KB
handshake floor.

**Alternatives.** (a) Shrink history to 24 rows — loses the 1–2 minute history
that makes the waterfall useful. (b) Store the trace only, drop the waterfall —
the waterfall is the reason the device exists. (c) Keep RGB565 and expand the
region on demand — that is a 4.8 KB temp buffer plus per-refresh copying, which
measured no better than the index path.

**Decision.** One byte per pixel as a palette index, converted by LVGL during
blending.

| | Before | After |
| --- | ---: | ---: |
| waterfall buffer | 19 200 B | **10 624 B** (1 024 palette + 9 600 indices) |
| steady-state `free` | 48 KB | **57–59 KB** |
| steady-state `largest` | 27–42 KB | **38–48 KB** |
| cost | — | 1–2 CPU points |

**Consequences.**
- `largest` gained 11 KB, which is what the TLS handshake actually needs; `free`
  gaining 9 KB is the less important half of the result.
- Ordering is load-bearing: reading the palette before `heat_init()` runs paints
  every row black, and the symptom looks like a canvas bug — hence guardian rule
  `palette-before-heat-init` (`order`, not `pattern`: no single line is wrong).
- The trace path still uses RGB565 points; only the waterfall moved. Anyone
  "unifying" the two formats reintroduces the 19 200 B.

---

## AD-006: Provisioning goes to NVS, not to Kconfig

| Attribute | Value |
| --- | --- |
| Type | Design / Operability |
| Status | Implemented, field-exercised (2026-11-21) |
| Decision | Kconfig supplies compile-time defaults; NVS holds the last provisioned values and always wins. |

**Problem.** Credentials live in the gitignored `sdkconfig.defaults.local`, so a
network change means a rebuild. In the field there is no rebuild: the device is
in someone's hand, with no serial console and no known network — the first thing
it must do after a Wi-Fi change is be fixable *without* a computer.

**Alternatives.** (a) Hardcode credentials — rejected: publicly released source
and a password in a fork. (b) A config file on flash — that is NVS with extra
steps and no wear levelling. (c) Portal-only provisioning with no fallback —
rejected: a device whose stored network still works should never need the portal.

**Decision.** SoftAP provisioning writes NVS; Kconfig is the default and is
consulted only when NVS has no value.

**Consequences.**
- Provisioning never requires a rebuild, and a reflash does not lose credentials
  (NVS survives unless the partition is erased).
- Precedence must be one-directional and inspectable in one module: a second NVS
  writer is how a provisioned network silently reverts after a reflash (guardian
  rules `nvs-single-namespace`; the key-ownership test asserts no key has two
  writers).
- A failed save must be observable to the operator, not silent — this is why
  `docs/provisioning.md` has a dedicated "if the save fails" section.
- Trigger conditions matter as much as the mechanism: the portal must appear when
  the network is unreachable, and must not hijack a working session.

---

## AD-007: Several networks, chosen by audio health rather than by signal

| Attribute | Value |
| --- | --- |
| Type | Design / Availability |
| Status | Implemented, field-exercised (2026-11-22) |
| Decision | Up to three (SSID, password) pairs; boot prefers the last known-good network; a mid-session switch is triggered by audio health, never by RSSI alone. |

**Problem.** The terminal is used in places without a single reliable access
point — a train tethered to a phone measured **6.5–12% packet loss with RTT peaks
of 6–10 s**. The intuitive proxy, RSSI, was measured to be a bad one: an access
point at **−25 dBm** was dropping about 10% of packets while showing a perfect
signal.

**Alternatives.** (a) Always join the strongest network — that is the −25 dBm
counter-example. (b) Let the user pick, every boot — the device usually boots in
a pocket. (c) Retry the last network forever — one bad network then bricks the
terminal until the battery dies.

**Decision.** Keep a short list, remember what worked, and switch on observed
audio health (sustained frame loss / underruns), with RSSI only used to rank
candidates at boot.

**Consequences.**
- "Connected" is not "usable": the health metric, not the association event, is
  the success criterion (`05` NFR-010).
- A switch costs a re-association and a `/WSterminal` re-establishment, so it must
  be rare and hysteresis-guarded; a flapping switch is worse than a bad network.
- Buffering cannot solve the mobile case (ten seconds of 32 KB/s is 320 KB against
  400 KB of SRAM), so the goal is documented as *graceful degradation*, not
  continuity (`05` NFR-008).

---

## AD-008: The knob has six positions and reports them as OK double-clicks

| Attribute | Value |
| --- | --- |
| Type | Design / UI |
| Status | Implemented, field-exercised (2026-11-22) |
| Decision | Each knob position change is reported upward as an OK double-click; rotate now cycles six settings (STEP, FIL, MIC, VOL, …) and UP/DOWN return to pure frequency tuning. |

**Problem.** With four settings the knob could map positions to distinct key
events, but the events that remained distinguishable were exhausted: the only
gesture the server and the display both treat as "cycle the selected row" is the
OK double-click.

**Alternatives.** (a) Keep two settings per knob position pair — the panel has six
positions, using four is a lie the operator cannot see. (b) Send a dedicated
"cycle" event — that is a server protocol change for a UI detail. (c) Use
press→press without OK semantics — measured 250–420 ms, exactly overlapping two
tuning taps, so it is indistinguishable from tuning by design.

**Decision.** Rotation costs a double-click event; UP/DOWN stay pure tuning.

**Consequences.**
- The mapping is lossy on purpose: the device cannot express "rotate" to the
  server, so any future knob gesture must fit the existing key vocabulary or be a
  protocol change.
- 250–420 ms is the number that kills alternative (c); it is recorded here so the
  idea is not retried every few months.
- Cycling is a *local* UI action, so a slow or busy link never delays the
  indicator — the visible state changes before the request is sent.

---

## Decision index

| AD | Decision | Primary requirement | Primary evidence |
| --- | --- | --- | --- |
| AD-001 | One `/WSterminal` session, channel-prefixed frames | NFR-024 | `../docs/transport-tuning.md` §6 |
| AD-002 | PCM16 only, no Opus in this image | NFR-025 | `../docs/optimization-notes.md` §2.1 |
| AD-003 | Codec fixed at 16 kHz, no runtime reconfiguration | NFR-027 | `../docs/design-v1.md` §3.1–3.2 |
| AD-004 | 100 ms uplink timeout plus server-side half duplex | NFR-004, NFR-005, NFR-015 | `../docs/transport-tuning.md` §4, §6 |
| AD-005 | Waterfall stored as 8-bit palette indices | NFR-022 | `../docs/optimization-notes.md` §20 |
| AD-006 | NVS over Kconfig for provisioning | NFR-030 | `../docs/provisioning.md` |
| AD-007 | Three networks, switch on audio health | NFR-008, NFR-010 | `../docs/optimization-notes.md` §22–§23 |
| AD-008 | Knob reports six positions as OK double-clicks | — (UI) | `../docs/UI.md`, measured 250–420 ms |

**Amendment (2026-11-24): a seventh position, screen brightness.** The cycle is
now `FREQ → CH → VOL → MIC → STEP → FIL → BL`.  The panel's backlight is a LEDC
PWM channel (`bsp_display.c: backlight_init`, GPIO21), and `bsp_display_backlight(0..100)`
already existed — it was simply hard-coded to 70 % at boot, so the one thing an
operator asked for after "make it louder" was the only knob they could not reach.
Brightness is stored in NVS like volume (`mrrc/bright`), because a display that
must be re-dimmed after every battery change is a display nobody dims.  Default 70 %
keeps the measured current draw; the floor is 5 % so a mistaken notch cannot
leave a black screen with no way back.
