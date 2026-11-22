<p align="right">
  <strong>English</strong> · <a href="13-feasibility-assessment.zh_CN.md">简体中文</a>
</p>

# 13. Feasibility Assessment (ART 0530)

## 13.1 Feasibility Summary

The design is feasible on this hardware **because every remaining limit has been
measured and accepted in writing**, not because the hardware is comfortable. Three
limits are hard: there is no PSRAM (so `largest` decides), the codec only behaves at
16 kHz (so the rate is fixed), and one core means the UI and the audio path compete
for the same 160 MHz (so the UI has a CPU ceiling, not a CPU budget).

What is *not* feasible and is therefore out of scope: Opus (AD-002), 48 kHz capture
(AD-003), multi-endpoint sessions (AD-001), and continuity across a mobile link
(AD-007 — the goal is graceful degradation).

## 13.2 Risks

| ID | Risk | Probability | Impact | Mitigation |
| --- | --- | --- | --- | --- |
| R1 | Mobile link delivers 6.5–12% loss with RTT peaks of 6–10 s | certain (measured) | High | Half duplex + 100 ms frame timeout (AD-004); drop frames, never the session; acceptance requires an adverse-link run |
| R2 | `largest` falls below the 16 KB TLS handshake floor after a feature lands | Medium | Critical | AD-005 freed 11 KB; the `mem:` line is part of acceptance; rollback order documented (`docs/design-v1.md` 4.4) |
| R3 | Codec pushed to another rate "for quality" | Low | Critical | 48 kHz peaks at 468/32767 — guardian has no rule for this, so the measurement lives in AD-003 and chapter `15` |
| R4 | Server changes its terminal contract (profile, half-duplex suppression) | Medium | High | The dependency is named in AD-001/AD-004; a contract change is a server AD, and the firmware's guardian rule `half-duplex-context` points at it |
| R5 | Single server, no failover: `w103d.vlsc.net` down means no radio | Medium | High | Out of scope by boundary; the terminal must remain diagnosable and retry, never spin |
| R6 | Clock is 0.16% fast, draining the cushion over 30+ minutes | certain (measured) | Medium | Accepted and documented (AD-003); a 30-minute listening run is the verification, a short run proves nothing |

## 13.3 Assumptions

| ID | Assumption | If it fails |
| --- | --- | --- |
| A1 | The server owns keyer arbitration and will refuse a second keyer | A takeover would let two terminals key; only the server can prevent it |
| A2 | The radio obeys the first release write | Stuck carrier; the radio's TOT becomes the only stop (chapter `15` §15.1) |
| A3 | SoftAP works with no router and no internet | Provisioning becomes impossible in the field, which is the fault AD-006 exists to avoid |
| A4 | Credentials stay in the gitignored local file | Published source would ship a password (guardian `secrets-in-tracked-config`) |
| A5 | A reference client can be placed on the same access point when diagnosing | Without it, "link or device" is undecidable (chapter `00` §V2) |

## 13.4 Current Issues

| ID | Issue | Status |
| --- | --- | --- |
| I1 | No uplink retransmission; a dropped frame is a 20 ms hole | accepted by design (chapter `15` §15.6) |
| I2 | Half duplex depends on server behaviour; the device can only drop what it cannot use | open, cross-repository (AD-004) |
| I3 | No TOT enforcement in the firmware | accepted: server cap plus the radio's TOT |
| I4 | Spectrum tier is a luxury during transmit; frames are dropped | accepted (AD-001) |
| I5 | Profiling instruments must be off in shipped builds, so field diagnostics are coarse | accepted; NFR-028 keeps the switch, the cost is known |
| I6 | The knob can only express six positions as one gesture (OK double-click) | accepted (AD-008); new gestures need a protocol change |

## 13.5 Dependencies

| Dependency | Kind | Owner | Effect if it changes |
| --- | --- | --- | --- |
| `mrrc_modern` terminal protocol (`/WSterminal`, `pcm16`, half duplex) | contract | server repository | firmware and server must land in the same breath; AD-004 is the precedent |
| ESP-IDF v5.5.3 and two managed-component patches (`TCP_NODELAY`, send buffer) | toolchain | this repository (`tools/`) | a version bump may silently re-enable Nagle — the patch script must be re-run and the send histogram re-measured |
| ES8311 + ST7789 driver behaviour | hardware | `components/bsp/` (upstream fork) | codec rate and LVGL port behaviour are measured facts (AD-003) |
| Radio TOT | safety | the radio | the last guaranteed stop; nothing in firmware can replace it |

## 13.6 Feasibility Conclusion

Feasible, with a narrow margin in exactly one dimension: contiguous memory. Every
other budget (CPU, bandwidth, latency) has been measured with at least a 1.5×
margin under its ceiling, and the acceptance lines in chapter `15` §15.5 exist so
that the margin is re-measured rather than assumed. The design is not "comfortable";
it is *documented*, which is the difference between a device that works on someone's
desk and one that can be fixed by whoever picks it up next.
