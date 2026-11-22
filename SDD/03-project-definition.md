<p align="right">
  <strong>English</strong> · <a href="03-project-definition.zh_CN.md">简体中文</a>
</p>

# 03. Project Definition (ENG 343)

## 3.1 Project Attributes

| Attribute | Value |
| --- | --- |
| Product | MRRC Terminal — handheld amateur-radio operating position |
| Hardware | FoloToy AI Passport (ESP32-C3 rev v1.1, 160 MHz single core, 400 KB SRAM, 8 MB flash, 240×320 ST7789, ES8311) |
| Baseline | Fork of `ref/ai-passport`; `components/bsp/` and most of `docs/` come from that upstream, the application layer is ours |
| Operating system | ESP-IDF v5.5.3 (FreeRTOS, single core) |
| Counterpart | `mrrc_modern` server at `w103d.vlsc.net:8888`, one `/WSterminal` session |
| Distribution | Source only. CI builds and verifies an image, but publishes no binary: the image cannot carry credentials and a flashing user must build with their own |
| Owner of this record | The firmware side: codec, buffers, tasks, display, provisioning, device half of the transmit path |

## 3.2 In Scope

- Receive: one `/WSterminal` session, PCM16 downlink, playback through the shared
  I2S clock, playback continuity across network gaps.
- Transmit: key handling, 20 ms PCM16 uplink, level control, release semantics.
- Display: frequency, mode, S-meter, spectrum trace, waterfall, chips bar, battery;
  5.26 Hz tick on a 240×320 panel with no framebuffer.
- Settings: knob (six positions), filter width index, step, microphone gain,
  speaker volume, persisted in NVS.
- Connectivity: three Wi-Fi networks, boot selection, health-based switching,
  SoftAP provisioning without any infrastructure.
- Observability: the once-a-minute idle line, the per-transmission line, and the
  `CONFIG_MRRC_PROFILE` diagnostic build.

## 3.3 Out of Scope

- The server. Session policy, keyer arbitration, downlink suppression and the
  RX/TX gain live in `mrrc_modern` and are that repository's decisions; this
  firmware only records what it relies on (chapter `08` AD-004).
- The radio. CAT commands, filter behaviour beyond the width index, and the
  radio's own TOT belong to the server and the hardware.
- Compression. There is no Opus path and adding one is a re-litigation of
  AD-002, not a feature.
- Multi-device arbitration. Two terminals on one server session is the server's
  problem; the firmware assumes it may be replaced mid-session and must survive
  being told to stop transmitting.
- Recording, FT8, SDR listening: server features with no terminal surface.

## 3.4 Success Criteria

Every criterion is a state the device is in, verified by a named instrument
(chapter `00` §0.3). A criterion that cannot be checked is not listed.

| ID | Criterion | Verification |
| --- | --- | --- |
| SC1 | Power-on reaches a working session with no manual step | Serial timeline: association → login → `/WSterminal` established → first audible frame, all within one pre-buffer |
| SC2 | Receive is continuous for a sustained session | `link:` line: `underruns` ≤2/s and `rebuf` not growing over ≥10 minutes |
| SC3 | Transmit survives a full transmission | `tx done:` line: 50 frames/s, `sendfail=0`, zero W/E log lines in the window |
| SC4 | The display tells the truth | Real spectrum frames only (no interpolated sweeps counted as data), 5.26 Hz tick, no `task_wdt` |
| SC5 | Memory never falls below the handshake floor | `mem:` line: `largest` ≥24 KB steady, never <16 KB |
| SC6 | A device with no network can still be provisioned | SoftAP + phone, no router, no rebuild |
| SC7 | A network change does not brick the terminal | Three configured networks; boot picks a working one; a failing link switches on audio health |
| SC8 | A field failure is diagnosable from one serial line | Chapter `00` §0.3: link/device/server faults separable without a debugger |
| SC9 | No credential is ever published | Guardian rule `secrets-in-tracked-config` on the whole tree; CI builds without `sdkconfig.defaults.local` |
| SC10 | The source builds reproducibly elsewhere | `./tools/validate.sh --all` on a clean checkout (ESP-IDF 5.5.3), guardian self-tests green |

## 3.5 Milestones

| Milestone | Date | Evidence |
| --- | --- | --- |
| Hardware facts established: 16 kHz codec, no APLL, no PSRAM | 2026-11-18 | AD-003; `docs/design-v1.md` §3 |
| Opus removed, PCM16 adopted | 2026-11-19 | AD-002; `docs/optimization-notes.md` §2.1 |
| CPU/memory optimisation round | 2026-11-20…21 | `docs/optimization-notes.md` §15 table |
| Transport round: queues, pre-buffer, Nagle, gain | 2026-11-21 | `docs/transport-tuning.md` §1–§5 |
| Repaint, trace, palette, network switching, uplink fix | 2026-11-22 | AD-004, AD-005, AD-007; `docs/optimization-notes.md` §17–§23 |
| Design record established (this document) | 2026-11-23 | `SDD/README.md` |

## 3.6 What would make this project a failure

Stated so that the criteria above are not read as a checklist with no direction:

- the waterfall showing synthesised sweeps while claiming to be a waterfall;
- a terminal that transmits into a congested link and dies instead of losing a frame;
- a device that can only be fixed with a serial cable;
- a "loudness" fix that clips, because the level was never the server's to fix;
- numbers in the record that nobody can reproduce.
