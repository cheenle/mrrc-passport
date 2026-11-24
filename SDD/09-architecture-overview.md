<p align="right">
  <strong>English</strong> · <a href="09-architecture-overview.zh_CN.md">简体中文</a>
</p>

# 09. Architecture Overview (ART 0512)

## 9.1 Runtime shape

One core, five concurrent contexts, no MMU and no PSRAM. Everything below is a
consequence of "one core": a task that blocks does not lose a timeslice, it loses
the machine.

| Context | Created by | Stack | Priority | Owns | Never does |
| --- | --- | ---: | ---: | --- | --- |
| `audio` | `mrrc_audio.c` | 8 192 B | 8 | I2S TX/RX, playback ring, capture from the codec | network I/O, LVGL |
| `txtask` | `mrrc_tx.c` | 4 096 B | 7 | framing of uplink PCM into 20 ms messages | socket writes outside its own send path |
| `keys` | `app_main.c` | 3 072 B | 6 | key scanning, long-press, knob decode | anything that can block |
| `net` | `mrrc_net.c` | 6 144 B | 5 | Wi-Fi, session state, JSON control, dispatch | LVGL, blocking the audio path |
| websocket client | `mrrc_net.c` (`WS_TASK_STACK` 6 144 B) | — | (library) | TLS, socket, frame assembly (8 KB buffer) | application policy |
| `taskLVGL` | `esp_lvgl_port` | (library) | (library) | all drawing | — (it is the only context allowed to touch LVGL) |

Priority ordering is the design: audio outranks the transmit task, which outranks
keys, which outranks the network. A congested socket must never delay a playback
frame — that inversion is what makes a jitter ring necessary in the first place.

## 9.2 Buffers

| Buffer | Size | Where | Why this size |
| --- | ---: | --- | --- |
| playback ring `s_rb` | 16 384 B (~0.51 s @16 kHz) | `mrrc_audio.c` | measured: the session-start burst dropped 253 frames with 8 KB, 0 with 16 KB (`docs/design-v1.md` 4.4) |
| pre-buffer band | floor 4 096 B (~128 ms), ceiling 12 288 B adapting up to 14 336 B | `mrrc_audio.c` | the gate used to be one-shot and the ring then drifted to 98% full (measured 2026-11-23); it is now a controller — trim the front to `ceiling - one tick` above the band, repeat the last frame when it runs dry, and widen the ceiling by 1 KB a minute while trims outnumber repeats (measured: that took trims from 1.3/s to ~0) |
| re-buffer trigger | dry for 200 ms | `mrrc_audio.c` | rebuilds the cushion after a real outage without looping |
| LVGL draw buffer | 240 × 24 lines, single | `bsp_display_lvgl.c` | 8 lines → 58% CPU, 12 → 43–49%, 32 → `largest` 18 KB (too little) |
| waterfall | 10 624 B (palette 1 024 + indices 9 600) | `mrrc_ui.c` | AD-005; was 19 200 B as RGB565 |
| spectrum points | 240 × 4 B | `mrrc_ui.c` | one point per pixel column, rebuilt per sweep |
| WS receive assembly | 8 192 B | `mrrc_net.c` | the largest inbound frame plus headroom; 4 KB made the server stall 5.8 s |

The rule that governs all of them: **a buffer is bought by a diagnosed fault, never
as insurance** (chapter `00` §V2). Two of the entries above were bought that way
and both are annotated with what they bought.

## 9.3 Signal paths

**Downlink.** TLS → WS assembly → channel byte `0x00` → ring (framed, length-prefixed)
→ playback gate (pre-buffer) → I2S TX at 16 kHz. The spectrum channel `0x02` takes
a different exit: into the UI's point array, which the UI consumes at its own tick.

**Uplink.** Codec I2S RX at 16 kHz → frame accumulator (20 ms = 320 samples) →
`txtask` → channel byte `0x01` + tag `0x02` → single write with a 100 ms timeout.
A frame that cannot be written is dropped and counted; it is not retried.

**Display.** Real spectrum frames and state updates wake the UI; the UI renders at
190 ms (5.26 Hz) from whatever is current, and interpolates only between two *real*
sweeps — a missing sweep is a gap, never a synthesised row.

## 9.4 Concurrency rules

These are the four that have bitten this codebase, stated as invariants:

1. **LVGL has exactly one owner** (`taskLVGL`). Any draw from another context goes
   through `mrrc_ui_*`, which posts work; a direct `lv_*` call elsewhere is a
   guardian warning (`lvgl-single-task`).
2. **The playback ring is guarded by a mutex with a zero timeout.** Contention is
   resolved by dropping the frame, not by waiting: a blocking wait on the producer
   side was measured as ~57 dropped frames per minute (≈2%) with no other symptom.
3. **The keyed socket carries one direction.** While this terminal holds the PTT,
   no downlink audio arrives on that socket (AD-004) and the firmware must not
   assume otherwise in either direction.
4. **The jitter cushion is controlled, not merely gated.** The pre-buffer decides
   when playback *starts*; a governor decides where it *stays*: above the ceiling
   the oldest frames are trimmed, below the floor the last frame is repeated for
   20 ms rather than handing the DMA an empty buffer (an empty write stops pacing
   the loop, which is how one gap became a burst of underruns). Counted as
   `gov(ins=… trim=…)`.
5. **Nothing in the audio or transmit path allocates.** Buffers are static; the RW
   path runs on a fixed budget and a heap call in a 50 Hz loop is how `largest`
   becomes 12 KB of fragments.

## 9.5 What is deliberately absent

- No RTOS queues between audio and network for RX data: the ring *is* the queue,
  and a second one would double the latency and the memory.
- No dynamic task creation, no dynamic LVGL objects for the waterfall (the canvas
  is allocated once), no filesystem.
- No retransmission, no TCP-level tuning beyond the two managed-component patches
  (`TCP_NODELAY`, send buffer) that `tools/build_on_mac.sh` applies.
- No second network stack: the WS client owns TLS and the boot/provisioning path
  uses the same `esp_netif`.
