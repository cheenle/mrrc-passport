<p align="right">
  <strong>English</strong> · <a href="05-non-functional-requirements.zh_CN.md">简体中文</a>
</p>

# 05. Non-Functional Requirements (ART 0507)

Requirements here are numbers that can be measured on this board, and every row
carries the method that would show the requirement has been broken. Per chapter
`00` §V1, each target also names its scenario: a throughput figure without its
window is a rumour, not a requirement.

**Status vocabulary** (chapter `00` §V5): `field-exercised` means measured on the
real unit over a sustained run; `implemented` means measured on the bench only;
`proposed` means no measurement exists yet and the row must say so.

## 5.1 Real-time and Throughput Requirements

| ID | Requirement | Target | Priority | Verification | Status |
| --- | --- | --- | --- | --- | --- |
| NFR-001 | Receive playback continuity | Ring-underrun wakeups ≤2/s over a 13-minute soak; no re-buffer growth | Critical | `link: … underruns=` per minute plus the `gov(ins/trim)` counters. 2026-11-23: 0–18/s with the ring pinned at 98%; after the clock fix and the banded governor, **0 on every sample** and 0.84% frames trimmed over a 101-minute session | field-exercised |
| NFR-002 | Receive frame rate | 50.0 frames/s sustained (20 ms frames), `drop=0`, `i2s` cadence never gaps longer than one frame | Critical | `tx: keys=… frames=…` idle line and the `i2s …/s` counter | field-exercised |
| NFR-003 | Downlink throughput | ≥31.3 KB/s audio and ≥49 KB/s total on a healthy access point — equal to a reference client on the same AP | High | Reference client in the same window; `ws: total=…` | field-exercised |
| NFR-004 | Uplink cadence during transmit | 50 frames/s for the whole transmission; `sendfail=0`; `readfail=0` | Critical | `tx done: frames=… sendfail=… readfail=…` — 30.5 s run: `frames=1500 sendfail=0` | field-exercised |
| NFR-005 | Uplink blocking | No send blocking >30 ms under a healthy LAN (`slow=0`); under a mobile link, worst single send ≤50 ms and never a transport timeout | Critical | `txlink: send_max=… slow=…`; LAN 9–17 ms after AD-004, mobile-link 30.5 s run 27.9 ms | field-exercised |
| NFR-006 | Capture fidelity | No sample above 90% of full scale at the ADC: `peak` 20 000–29 000 for speech, and zero server-reported clipped samples | High | `tx done: … peak=…` plus the server's `audio levels` line; 27522 measured at MIC 30 dB | implemented |
| NFR-007 | Playback start latency | Audio starts after ≤160 ms of buffered frames and rebuilds the cushion within 200 ms of a dry ring | Medium | `AUD_PREBUF_BYTES` 5120 / `AUD_REBUFFER_US`; underrun counters | implemented |

## 5.2 Availability and Degradation Requirements

| ID | Requirement | Target | Priority | Verification | Status |
| --- | --- | --- | --- | --- | --- |
| NFR-008 | Mobile-link survivability | Under 6.5–12% packet loss and RTT peaks of 6–10 s, the control and PTT socket survives; audio frames may be dropped, the session may not | Critical | The 2026-11-22 tethered-on-a-train run; W/E log lines inside the transmit window must be 0 | field-exercised |
| NFR-009 | Reconnect behaviour | Exactly one `/WSterminal` session per device; 0 sockets left registered in two roles after a reconnect; at most 1 forced RX per disconnect | Critical | Server side (`clients` counts) and the device `ws: total=` counter | field-exercised |
| NFR-010 | Network selection | Up to 3 (SSID, password) pairs; boot selects the last known-good network; a switch requires sustained audio degradation, never RSSI alone | High | `link: rssi=…` plus frame counters; the −25 dBm access point that dropped ~10% of packets is the counter-example | implemented |
| NFR-011 | Boot to audio | Wi-Fi association + login + first audible frame within 1 pre-buffer (160 ms of staged audio, ≤30 s wall clock), with 0 manual steps after power-on | Medium | Serial timeline from reset; `Initial state` line on the server | field-exercised |
| NFR-012 | Provisioning without infrastructure | A device with 0 reachable networks can be provisioned from 1 phone: SoftAP, no router, no captive-portal dependency, ≤2 minutes end to end | High | `../docs/provisioning.md` trigger conditions; the save-failure path must be observable | field-exercised |

## 5.3 Transmit Safety Requirements

The full model is chapter `15`; these are the measurable requirements it rests on.

| ID | Requirement | Target | Priority | Verification | Status |
| --- | --- | --- | --- | --- | --- |
| NFR-013 | Release is immediate | 0 waits for confirmation at any layer; 1 write per release, and the radio obeys the first | Critical | Chapter `15` §15.3; no `vTaskDelay` in a release path (guardian rule `ptt-release-never-blocks`) | implemented |
| NFR-014 | Uplink frame timeout | 400 ms per frame (400 ms in the code); a frame that cannot be written is dropped, never queued behind the next | Critical | Guardian rule `uplink-send-timeout`; `send_max` in the transmit window. 400 ms is sized to *not kill the socket*: the websocket client treats a failed write as fatal, and a 100 ms timeout turned a 105 ms stall into a lost session (2026-11-24) | field-exercised |
| NFR-015 | Half-duplex discipline | A keyed socket carries 1 direction (uplink) only; 0 downlink frames delivered to it; the device must not depend on that alone | Critical | Server-side suppression plus `mmrc`-side drop accounting; 30.5 s run with zero downlink audio delivered to the keyed socket | field-exercised |
| NFR-016 | Transmit observability | Every transmission ends with exactly 1 line carrying 4 counters (`frames`, `sendfail`, `readfail`, `peak`); that line is the acceptance test, not an impression | High | `tx done:` line; guardian rule set is written around the same counters | field-exercised |
| NFR-017 | No silent carrier | 0 silent key-ups: transmit is refused visibly whenever 1 of the 3 preconditions (session, codec, profile) is absent | Critical | Chapter `15` §15.2 precondition list | implemented |

## 5.4 Resource Requirements

`largest` (the biggest contiguous free block) is the binding constraint, because
the TLS handshake needs 16 KB contiguous; `free` is advisory (chapter `00` §V2 —
a counter without a reference lies).

| ID | Requirement | Target | Priority | Verification | Status |
| --- | --- | --- | --- | --- | --- |
| NFR-018 | Contiguous memory floor | `largest` ≥24 KB in steady state, never below 16 KB (the handshake floor) | Critical | `mem: free=… largest=… min=…` once a minute; steady state measured 38–48 KB | field-exercised |
| NFR-019 | Free-heap floor | `free` ≥49 KB steady, with no downward trend over a 30-minute session | High | Same line; 57–59 KB measured after AD-005 | field-exercised |
| NFR-020 | UI CPU ceiling | `taskLVGL` ≤30% steady at the shipped tick rate (190 ms) | High | `CONFIG_MRRC_PROFILE=y` CPU table; 27% measured 2026-11-23 (36% before the playback-clock fix), 56–61% before the 2026-11-22 repaint work | field-exercised |
| NFR-021 | Idle CPU floor | `IDLE` ≥40% steady, so that a new feature has somewhere to live | Medium | Same table; 42–45% measured | field-exercised |
| NFR-022 | Waterfall storage | Waterfall buffer ≤11 KB of `.bss` for a 40-row history (palette + indices) | Medium | `size` output; 10 624 B (1 024 palette + 9 600 indices) versus 19 200 B as RGB565 | field-exercised |
| NFR-023 | Static allocation discipline | 0 heap allocations per frame in the audio, transmit and network-receive paths; 100% of task stacks come from named macros | High | Guardian rules `no-heap-in-audio-path`, `named-task-stacks`; `.bss` table in `../docs/design-v1.md` 4.1 | field-exercised |

## 5.5 Compatibility Requirements

| ID | Requirement | Target | Priority | Verification | Status |
| --- | --- | --- | --- | --- | --- |
| NFR-024 | Protocol conformance | One `/WSterminal` session carrying text control and three binary channels: `0x00` RX, `0x01` TX, `0x02` spectrum; the first byte is the channel, the rest is the legacy payload unchanged | Critical | Guardian rule `single-terminal-endpoint`; server-side `_terminal_*` handlers | field-exercised |
| NFR-025 | Audio profile conformance | PCM16 mono 16 kHz declared once per session; 641 B per 20 ms uplink frame including the tag; no Opus path in this image | Critical | Guardian rule `no-opus-in-firmware`; server `audioCaps` reply | field-exercised |
| NFR-026 | Filter width mapping | The device sends the filter **index** (`filter_width`) and never a derived frequency; the voice and narrow tables agree with the server's derived values (13→2400 Hz, 17→2700 Hz, 20→3000 Hz) | High | Server log of `filter_width` plus derived `filter_hz`; three-point check recorded | field-exercised |
| NFR-027 | Codec rate stability | The codec runs at 16 kHz and is never reconfigured at runtime; a 30-minute session may drift ≤0.2% and must not lose audio to it | Critical | `../docs/design-v1.md` 3.2 (MCLK divider 39: 16.03 kHz, 0.16% fast); a 30+ minute listening run | field-exercised |

| NFR-032 | Display brightness is adjustable and survives a battery change | 0–100 % in 5 % steps, floor 5 %, applied within one knob notch, restored at boot | Medium | Knob position `BL`; NVS key `mrrc/bright`; boot line `knob: … bright=N%` | implemented |
| NFR-033 | An empty transmission is reported, not silent | A press that delivers fewer than 10 frames (200 ms) logs `TX session carried almost no audio` | High | Server log; measured on the 2026-11-24 `frames=0 peak=0%` press | field-exercised |

## 5.6 Operability Requirements

| ID | Requirement | Target | Priority | Verification | Status |
| --- | --- | --- | --- | --- | --- |
| NFR-028 | Diagnostics are opt-in | 0 diagnostic lines per hour from shipped defaults (`CONFIG_MRRC_PROFILE=n`); enabling it requires 1 deliberate rebuild | High | Guardian rule `diagnostics-not-shipped`; `sdkconfig.defaults` | field-exercised |
| NFR-029 | Credential containment | 0 Wi-Fi/server credentials in tracked files; exactly 1 home (`sdkconfig.defaults.local`, gitignored); the release build carries 0 of them | Critical | Guardian rule `secrets-in-tracked-config`; the release workflow builds without it | field-exercised |
| NFR-030 | Settings survive a reflash | After 1 reflash, 100% of provisioned networks and server settings are still in effect and 0 rebuilds are needed | High | `../docs/provisioning.md`; reboot with a different `sdkconfig` | field-exercised |
| NFR-031 | Diagnosability of a field failure | 1 USB serial line + the server log are enough to separate the 3 fault classes (link, device, server), with 0 debugger sessions | High | Chapter `00` §0.3 instrument table; the transport round was solved this way | field-exercised |

## 5.7 Traceability

| Requirement | Decided in | Enforced by | Evidence |
| --- | --- | --- | --- |
| NFR-001, 002, 003, 007 | AD-001, AD-005 | `link:`/`i2s` counters | `../docs/transport-tuning.md` §1–§2 |
| NFR-004, 005, 015 | AD-004 | guardian `uplink-send-timeout` | `../docs/transport-tuning.md` §4, §6 |
| NFR-006 | AD-002, chapter `15` §15.4 | `peak` target in the transmit line | `../docs/transport-tuning.md` §5 |
| NFR-008, 009, 010 | AD-007 | boot and switch policy | `../docs/optimization-notes.md` §22–§23 |
| NFR-011, 012 | AD-006 | boot and provisioning path | `../docs/provisioning.md` |
| NFR-018–023 | AD-005 | `mem:` line, `size` output | `../docs/design-v1.md` §4, `../docs/optimization-notes.md` §15, §20 |
| NFR-013–017 | AD-004, chapter `15` | guardian `ptt-release-never-blocks`, `uplink-send-timeout` | `../docs/transport-tuning.md` §6, §26 |
| NFR-032 | AD-008 (2026-11-24 amendment) | — | `../docs/UI.md`, boot line `knob: … bright=` |
| NFR-033 | — | server `TX session carried almost no audio` | `mrrc_modern/patches/README.md` item 9 |
| NFR-024–026 | AD-001, AD-003 | guardian `single-terminal-endpoint`, `no-opus-in-firmware` | `../docs/optimization-notes.md` §2 |
| NFR-027 | AD-003 | codec fixed at build time | `../docs/design-v1.md` §3.1–3.2 |
| NFR-028–031 | AD-006, chapter `00` §0.3 | guardian `diagnostics-not-shipped`, `secrets-in-tracked-config`, `nvs-single-namespace` | `../docs/provisioning.md` |

Any requirement added here must also add a row to §5.7. A requirement with no
decision, no enforcement and no evidence is a wish, and chapter `00` §V1 keeps it
out of this table until all three exist.
