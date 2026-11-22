<p align="right">
  <strong>English</strong> · <a href="15-transmit-safety-architecture.zh_CN.md">简体中文</a>
</p>

# 15. Transmit Safety Architecture (ART 0535)

Transmit safety is a chapter of its own for the same reason it is a chapter of its
own on the server: the property is spread across the audio path, the socket, the
key handling and the operator's expectations, and no single file owns it. A
change that looks local — a timeout, a gain, a filter table — can make the radio
key with nothing to send, or stop keying while the light says otherwise.

The firmware's own contribution is small and this chapter is mostly about the
**boundaries of that contribution**: what this device can guarantee alone, and
what it delegates to the server and to the radio.

## 15.1 The layered model

Transmission is only safe if every layer below can also *stop*:

| # | Layer | Owner | What it guarantees | What it cannot |
| --- | --- | --- | --- | --- |
| L0 | Operator gesture | firmware (`keys_task`) | a key-down is reported once, a key-up is reported once, and a lost release is reported as a release | nothing, if the socket is already gone |
| L1 | Session state | firmware (`mrrc_net.c`) | TX frames are only produced after the session is established **and** the server has acknowledged the key | nothing, if the server accepts frames it cannot use |
| L2 | Frame pacing | firmware (`mrrc_audio.c`, `mrrc_tx.c`) | 20 ms cadence, one frame in flight per write, a frame that cannot be written in 100 ms is dropped | retransmission — there is none, and adding one would be worse |
| L3 | Socket liveness | server + firmware | a dead session is detected and forced to RX rather than left keyed | physical carrier, if the radio ignores the release |
| L4 | Level and modulation | firmware (codec) + server (gain) | samples reach the radio without clipping, at the level the operator set | what the radio does with them |
| L5 | Radio TOT | the radio | the carrier is dropped after its own timeout regardless of anything above | anything above it |

Two properties of this table are decisions, not observations:

- **stops are never gated.** Every layer may refuse to *start* transmitting for a
  reason (no session, no codec, wrong profile), but no layer may refuse to stop.
  That asymmetry is what makes the stack safe to change.
- **the last resort is not in the firmware.** The firmware cannot unkey a radio it
  is not talking to; `05` NFR-017 (no silent carrier) is about refusing to key,
  not about surviving a broken link. The radio's TOT is the only guaranteed stop,
  which is why the transmit path is designed to fail *towards* silence.

## 15.2 Preconditions: the device may refuse to transmit

Keying requires all of the following, checked at key-down, in this order:

1. the `/WSterminal` session is up and has completed its audio capability
   declaration (AD-001, AD-002);
2. the codec is running at 16 kHz and the microphone path has produced at least
   one frame since boot (AD-003 — a keyed radio with a dead codec is a silent
   carrier);
3. the uplink is not already carrying another owner's transmission: the server's
   keyer arbitration is authoritative and the device must not key optimistically;
4. the operator's key state and the reported state agree — a release that arrives
   while the device is still in "keying" is treated as a release, never queued.

Failure of any of these is **visible**: the TX indicator does not light and the
serial log carries one line naming the missing condition. A silent refusal is
indistinguishable from a dead key, and this device has already had one fault
misdiagnosed for that reason (chapter `00` §0.1, third row).

## 15.3 Release is fire-and-forget

Release is a single write, and nothing waits for it:

- no verification loop, no "did the radio really unkey" retry, no delay in the
  release path — guardian rule `ptt-release-never-blocks` (`05` NFR-013);
- the reason is not politeness: a verify loop costs hundreds of milliseconds of
  **stuck carrier** when it goes wrong, which is the worst failure the device can
  produce;
- stuck-keyup detection belongs to observability (L3), not to the release path.

The firmware reports the release and forgets it. If the write fails, the failure
is logged and the next session establishment forces RX; the device never holds a
"maybe still keyed" state, because a state it cannot verify is a state that will
eventually be wrong.

## 15.4 Level: clip at the ADC, not in software

Two gains used to be stacked: the ES8311 PGA at +30 dB and the server's software
`tx_mic_db` at +12 dB. The server reported 5% of TX samples clipped while the
device reported `peak=32768` — full scale at the ADC, before any software.

The useful negative result: lowering the server gain to +3 dB moved clipping from
5.0% to **5.8%** — the peaks were already at the ADC's ceiling. Setting
`tx_mic_db` to 0 (the identity) gave `peak=40%` and zero clipped samples, because
the level was never the server's to fix.

| Level target | Value | Why |
| --- | --- | --- |
| ADC peak for speech | **20 000–29 000** (60–88% of 32767) | headroom for the loudest syllable, without quantisation loss |
| measured at MIC 30 dB | 27 522 (84%) | the reference point for this unit |
| measured at MIC 36 dB | 32 768 (clipping) | what a "louder is better" step looks like |
| software gain | 0 dB on the server | scaling already-quantised samples cannot recover headroom |

The analog PGA is the only correct place to buy level: it is before the ADC, so
it improves SNR. A software gain only rescales what is already quantised, and a
step that *looks* like +6 dB can therefore do nothing but clip.

## 15.5 What the counters must show

A transmission is accepted on evidence, not on impression (chapter `00` §V1). The
acceptance line, once per transmission:

```
tx done: frames=<n> sendfail=<n> readfail=<n> peak=<n>
```

with these required values, all measured in the same run:

| Field | Requirement | 2026-11-22 reference run |
| --- | --- | --- |
| `frames` | ≈50 × seconds, so the cadence held for the whole transmission | 1500 in 30.5 s = 50.0/s |
| `sendfail` | 0 | 0 |
| `readfail` | 0 | 0 |
| `peak` | 20 000–29 000 for speech | 27 522 at MIC 30 dB (32 768 at 36 dB = clipping) |
| `send_max` (profile line) | ≤50 ms mobile, ≤17 ms LAN | 27.9 ms mobile, `slow=0` |
| W/E log lines during the window | **0** | 0 |

A non-zero `sendfail` with a clean `readfail` means the uplink is being throttled
from below (AD-004 territory). A non-zero `sendfail` **with** W/E lines means the
session is dying, and the PTT indicator is a symptom, not the fault.

## 15.6 Known boundaries

Recorded so that the next investigation does not rediscover them as surprises:

- **No retransmission.** A dropped uplink frame is gone. The radio's audio simply
  has a 20 ms hole; retransmitting would delay every later frame.
- **No TOT enforcement in the firmware.** A 120 s server-side cap and the radio's
  own TOT are the limits; an acceptance test that holds PTT for 2 minutes will be
  cut off at the server's cap, and that is correct behaviour, not a fault.
- **Half duplex depends on the server.** The device does not control whether the
  server sends downlink audio while keyed (AD-004); it can only drop what it
  cannot use. A server that resumes the downlink during transmit will bring the
  fault back even with the 100 ms timeout in place.
- **Clipping can return through the PGA.** The MIC step is a user setting that
  survives reboot (AD-006); raising it for a weak signal can push `peak` past
  29 000, and the only correct response is to lower the step, not to compensate
  downstream.
- **Liveness is the server's judgement.** The firmware cannot know whether the
  server still considers it the keyer; it can only notice its own socket dying.
