<p align="right">
  <strong>English</strong> · <a href="06-use-case-model.zh_CN.md">简体中文</a>
</p>

# 06. Use Case Model (ART 0508)

## 6.1 Actors

| Actor | Description | Constraint |
| --- | --- | --- |
| Operator | Holds the terminal, turns the knob, presses PTT | Sees only the panel; cannot read logs in the field |
| Server (`mrrc_modern`) | Owns the session, the radio, the keyer arbitration and the audio levels it sends | May replace this terminal mid-session; may refuse to key |
| Phone (provisioning) | Joins the terminal's SoftAP and submits credentials | Has no app; only a browser |
| Radio (via server) | The actual receiver/transmitter | Its TOT is the last guaranteed stop |

The firmware cannot observe the radio directly, and cannot observe the operator's
intent except through keys. Every use case below is written with that blindness in
mind: what the device can prove, and what it must assume.

## 6.2 Core Use Cases

### UC-001: Power on and reach a session

| Field | Description |
| --- | --- |
| Goal | Audible audio within one pre-buffer of power-on, with no keys pressed |
| Preconditions | At least one provisioned network in range (or the SoftAP fallback, see UC-005) |
| Basic Flow | Boot → NVS settings load → Wi-Fi select → associate → login → `/WSterminal` → audio capability declaration → first frames → playback |
| Exceptions | No network: SoftAP (UC-005). Login rejected: stay connected, show the state, retry on a backoff. Session dropped later: reconnect and force RX (UC-003 exception) |
| Verification | SC1; serial timeline; `ws: total=` counter |

### UC-002: Listen

| Field | Description |
| --- | --- |
| Goal | Continuous receive audio with a truthful display for as long as the operator listens |
| Preconditions | Session up (UC-001) |
| Basic Flow | Downlink frames → ring → playback gated by the 160 ms pre-buffer → display updates at 5.26 Hz from real spectrum frames |
| Exceptions | Network gap: the pre-buffered cushion covers it; a dry ring for 200 ms rebuilds the cushion. Long session: 0.16% clock drift drains the cushion slowly — accepted, documented (AD-003) |
| Verification | SC2, SC4; `underruns`, `rebuf`, `i2s …/s`, `drop` |

### UC-003: Transmit

| Field | Description |
| --- | --- |
| Goal | The other station hears the operator; the terminal never leaves the radio keyed |
| Preconditions | Session up, codec running, server accepts the key (chapter `15` §15.2) |
| Basic Flow | Key down → preconditions → TX frames at 50/s → key up → single release write → indicator off |
| Exceptions | Congested uplink: a frame that cannot be written in 100 ms is dropped, the transmission continues (AD-004). Session dies mid-transmission: reconnect, force RX, report. Server refuses: indicator stays off and the reason is logged |
| Verification | SC3; `tx done:` line with `frames`, `sendfail`, `readfail`, `peak`; zero W/E lines in the window |

### UC-004: Tune and change settings with the knob

| Field | Description |
| --- | --- |
| Goal | Change frequency, mode, step, filter width, microphone gain and volume without a menu system |
| Preconditions | Session up for anything that reaches the radio; STEP/FIL/MIC/VOL are local or index-based and need no round trip to *display* |
| Basic Flow | Rotate → position change reported as an OK double-click (AD-008) → chip updates locally → the value is sent; UP/DOWN are pure tuning on the VFO |
| Exceptions | Filter width is sent as an **index**, never as a frequency: the server derives the Hz and the device only mirrors it (NFR-026). A slow link delays the echo, never the indicator |
| Verification | Chip values against the server's state; the three-point filter check (13→2400, 17→2700, 20→3000) |

### UC-005: Provision a terminal that has no network

| Field | Description |
| --- | --- |
| Goal | A device with nothing reachable becomes usable from a phone, in under two minutes |
| Preconditions | The operator can join the terminal's SoftAP; the terminal decided its stored networks are not usable |
| Basic Flow | Trigger → SoftAP + minimal page → phone submits SSID/password → NVS write (AD-006) → reboot or re-associate → UC-001 |
| Exceptions | Save fails: the page says so and the previous credentials stay in force — a silent failure here looks exactly like a wrong password |
| Verification | SC6; NFR-012; `docs/provisioning.md` |

### UC-006: Survive a network change

| Field | Description |
| --- | --- |
| Goal | The terminal keeps working when the operator moves between networks, and degrades instead of dying when the link is bad |
| Preconditions | Up to 3 networks provisioned (AD-007) |
| Basic Flow | Boot ranks candidates → join the last known-good → if the audio health metric degrades sustainedly, switch → re-establish the session → force RX |
| Exceptions | All networks bad: stay on the least-bad, keep retrying, keep the display honest. Flapping: hysteresis must prevent an oscillation, because a switch costs a re-association |
| Verification | SC7; NFR-008, NFR-010; the tethered-on-a-train run (6.5–12% loss, RTT peaks 6–10 s) |

### UC-007: Diagnose a fault from the bench

| Field | Description |
| --- | --- |
| Goal | Tell link, device and server faults apart with one USB serial line |
| Preconditions | A profile-enabled build (`CONFIG_MRRC_PROFILE=y`, deliberately not shipped) |
| Basic Flow | Read the idle line (`mem`, `tx`, `i2s`, `drop`) → read `link:`/`ws:`/`txlink:` → correlate with the server's `TX session` line → compare with a reference client |
| Exceptions | Only one instrument available: then the conclusion is an observation, not evidence (chapter `00` §V2) |
| Verification | SC8; chapter `00` §0.3 |

## 6.3 Use case to requirement map

| Use case | Requirements | Decisions |
| --- | --- | --- |
| UC-001 | NFR-011, NFR-024, NFR-027 | AD-001, AD-003 |
| UC-002 | NFR-001, NFR-002, NFR-003, NFR-007 | AD-001, AD-003, AD-005 |
| UC-003 | NFR-004, NFR-005, NFR-006, NFR-013…017 | AD-002, AD-004 |
| UC-004 | NFR-026 | AD-008 |
| UC-005 | NFR-012, NFR-030 | AD-006 |
| UC-006 | NFR-008, NFR-009, NFR-010 | AD-007 |
| UC-007 | NFR-028, NFR-031 | — (chapter `00` §0.3) |
