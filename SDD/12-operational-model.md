<p align="right">
  <strong>English</strong> · <a href="12-operational-model.zh_CN.md">简体中文</a>
</p>

# 12. Operational Model (ART 0522)

The terminal has no shell, no filesystem and no visible log in the field. Every
operational behaviour below is therefore written as an ordered list with explicit
timings, because "it should reconnect" is not an operational model — the numbers
are what the operator experiences.

All timings in this chapter were checked against the source and the source is the
authority; if they ever disagree, the chapter is wrong (chapter `00` §V7).

## 12.1 Boot sequence

| # | Step | Instrument | Failure behaviour |
| --- | --- | --- | --- |
| 1 | `app_main` starts, `mem_report("boot")` | `mem:` line | reset loop — log shows how far it got |
| 2 | settings load from NVS (`"mrrc"` namespace), Kconfig fallback | — | absent keys fall back silently to the compiled defaults (AD-006) |
| 3 | display + LVGL (`mem_report("display")`) | panel lights, chips populate | blank panel; boot continues (a dead panel must not cost the audio path) |
| 4 | audio + codec at 16 kHz (`mem_report("audio")`) | `i2s` counter appears | transmission is refused later (chapter `15` §15.2) |
| 5 | buttons + key task (`mem_report("buttons")`) | keys logged on press | knob dead, everything else works |
| 6 | network task (`mem_report("net started")`) | `link:` line | UC-005 provisioning after the retry policy below |
| 7 | Wi-Fi join, IPv6 link-local, login, `/WSterminal` | `ws: total=` | see §12.3 |
| 8 | audio capability declaration, first frames, playback | first audible frame | if the profile is refused, stay connected and report |

The `mem_report` stages exist so that a boot that dies at a known point names the
subsystem that ate the block; they are the reason a memory regression can be
attributed rather than guessed.

## 12.2 Network selection

1. Load up to **three** (SSID, password) pairs: NVS first, then Kconfig, in order.
2. Prefer the last known-good network; if it is in range, join it and stop.
3. Otherwise scan and rank candidates; the ranking uses signal only to order, never
   to judge quality (AD-007 — the −25 dBm access point dropping 10% of packets).
4. One retry, **30 s** later, before declaring the network unusable: a router that is
   merely rebooting must not push the device into setup mode.
5. If the join fails twice, raise SoftAP (§12.4).
6. If a joined network stops delivering audio health, switch to the next candidate;
   a switch costs a re-association and a session rebuild, so it must be a decision,
   not a reaction (§12.6).

## 12.3 Session establishment and reconnection

| Situation | Behaviour | Timing |
| --- | --- | --- |
| login fails | retry with exponential backoff, capped | start **30 s**, double, cap **600 s**, reset on success |
| login succeeds, cached token rejected | "no control link with cached token" → discard token, re-login | wait **12 s** for the socket before concluding |
| socket drops on its own | let the client reconnect | client reconnect **500 ms** plus the TLS handshake (~1.3 s); this is the delay an operator hears after a failed transmission |
| socket still down | destroy and rebuild the session, re-login | after **20 s** down |
| net task loop | poll state, service the queue | every **3 000 ms** |

A fixed short retry would park the terminal inside the server's login rate limit
forever (five logins per five minutes per IP), which is why the backoff starts at
30 s and grows.

## 12.4 Provisioning

Trigger (either condition):

| Condition | Why |
| --- | --- |
| no SSID configured at all | factory state |
| two failed joins, 30 s apart | the operator moved somewhere none of the stored networks exist |

Sequence: raise SoftAP `MRRC-Passport-XXXX` → DHCP serves `192.168.4.1` → the panel
shows SSID, password and URL → the phone joins and opens the page → form submit →
NVS write → reboot into station mode.

Cost and its budget: **+80 KB flash** for `esp_http_server`, about **15 KB heap** while
setup runs — affordable precisely because setup happens before the TLS stack exists
(measured `free=73K largest=64K` in setup mode).

If the save fails, `cfg_save()` logs the NVS statistics, erases the partition and
retries once: setup is the one moment where wiping is safe, because the credentials
on the page are about to replace what is there anyway. A silent failure here would
be indistinguishable from a wrong password, which is the one outcome the design
refuses.

## 12.5 Settings persistence

| Setting | Written when | Cost |
| --- | --- | --- |
| speaker volume, microphone gain, step, filter width | on each change (knob) | an NVS commit per notch change; measured `wr_max` up to **32 ms** |
| server address / password, SSIDs | provisioning only | — |
| session token | on login and on invalidation | one write per session |

The 32 ms write is the largest single stall in the UI path and it is accepted: a
knob change is a deliberate act at human speed. What is *not* accepted is writing
during audio playback on a timer — the persisted set is exactly what the operator
touched, nothing else.

## 12.6 Switching networks mid-session

1. A switch requires **sustained** degradation of the audio health metric, not one
   lost frame and not a low RSSI.
2. On a switch: tear down the session, re-join, re-login, force RX.
3. Hysteresis is mandatory: a flapping switch is worse than a bad network, because
   each switch costs a re-association plus a login.
4. If every candidate is bad, stay on the least-bad and keep the display honest —
   the documented goal is graceful degradation, not continuity (AD-007).

## 12.7 Field failure matrix

| Symptom | First instrument | Likely layer | Action |
| --- | --- | --- | --- |
| audio stutters, `drop` rising | `mem:` then `link:` | device queues | chapter `08` AD-001/AD-004 territory; do not enlarge buffers first |
| TX indicator goes out while holding the key | `tx done:` then W/E lines | uplink or session | if `sendfail>0` with W/E lines, the socket is dying (AD-004) |
| no audio but the display updates | `i2s …/s` | audio task or ring | ring starvation vs. downlink silence |
| nothing at all, panel dark | `mem:` boot stage | power or boot | which `mem_report` stage was last |
| cannot reach the server | `ws: total=`, IPv6 address | network (§4.3) | all three IPv6 switches |
| mic too quiet on air, `peak` low | transmit line `peak` | level (chapter `15` §15.4) | raise the PGA step, never the server's software gain |

## 12.8 Diagnostics cadence

- once a minute: the idle line (`mem:`, `tx:`, `i2s`, `drop`), compiled in only when
  `CONFIG_MRRC_PROFILE=y` (guardian `diagnostics-not-shipped`);
- once per transmission: the `tx done:` acceptance line (chapter `15` §15.5);
- on demand: `link:`, `ws:`, `txlink:`, `ring()`, `batt=` lines from the same build.

There is no persistent log on the device. The evidence a field failure leaves is
whatever was on the USB serial line while someone was watching, which is why the
counters are designed to be sufficient without history.

## 12.9 Power and battery

The battery indicator shows percent and turns green while charging. There is no
graceful-shutdown sequence and no low-battery transmission prohibition: the device
either has enough power to transmit or it browns out, and a brown-out during
transmission is recovered by the radio's own TOT (chapter `15` §15.1). This is
recorded as an accepted limitation, not an oversight.
