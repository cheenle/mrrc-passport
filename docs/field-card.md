<p align="right">
  <strong>English</strong> · <a href="field-card.zh_CN.md">简体中文</a>
</p>

# Field card

One page, meant to be printed and taped to the back of the unit. Every number here
also appears in `SDD/` — a test checks that, so the card cannot quietly become the
only place a wrong value lives.

## 1. Is the transmission healthy?

Read the `tx done:` line. All six must hold **in the same run**:

| Field | Required | Reference run (2026-11-22) |
| --- | --- | --- |
| `frames` | ≈50 × seconds | 1500 in 30.5 s = 50.0/s |
| `sendfail` | 0 | 0 |
| `readfail` | 0 | 0 |
| `peak` | 20 000–29 000 | 27 522 at MIC 30 dB |
| `send_max` | ≤50 ms mobile, ≤17 ms LAN | 27.9 ms mobile, `slow=0` |
| W/E log lines in the window | 0 | 0 |

`sendfail` high **without** W/E lines → the uplink is throttled from below.
`sendfail` high **with** W/E lines → the session is dying; the PTT light is a
symptom, not the fault.

## 2. Symptom → first instrument

| Symptom | Read first | Then |
| --- | --- | --- |
| audio stutters, `drop` rising | `mem:` then `link:` | do **not** enlarge buffers first |
| TX light goes out while keying | `tx done:` then W/E lines | if W/E lines appear, it is the socket |
| no audio, display alive | `i2s …/s` | ring starvation vs. downlink silence |
| nothing at all, panel dark | `mem:` boot stage | which `mem_report` stage was last |
| cannot reach server | `ws: total=`, IPv6 address | the three IPv6 switches (§4) |
| quiet on air, `peak` low | transmit `peak` | raise the PGA step, never server gain |

## 3. Level: at the ADC, not in software

- Target `peak` **20 000–29 000** (32767 full scale).
- MIC **30 dB** measured **27 522** (84%) on this unit; MIC **36 dB** measured
  **32 768** — clipping at the ADC, before any software.
- The server's `tx_mic_db` stays at **0**. At +3 dB, clipping went from 5.0% to
  5.8%: the ceiling was never the server's to fix.

## 4. Network

- **Three things** must all be true, or the interface shows `0 IPv6` forever:
  `CONFIG_LWIP_IPV6=y`, `CONFIG_LWIP_IPV6_AUTOCONFIG=y`, and
  `esp_netif_create_ip6_linklocal(netif)` after `GOT_IP`.
- Boot prefers the **last known-good** network; a switch needs sustained audio
  degradation, not a low RSSI — an AP at **−25 dBm** was measured dropping 10% of
  packets.
- One retry **30 s** after a failed join, then SoftAP.
- Provisioning: join `MRRC-Passport-XXXX`, open `http://192.168.4.1`, fill the
  form. Budget: +80 KB flash, ~15 KB heap while it runs.

## 5. Do not

| Do not | Because |
| --- | --- |
| enlarge a buffer before reading `mem:`/`link:` | two rounds did that and changed nothing |
| add software TX gain | it rescales quantised samples; the ceiling is the ADC |
| call `bsp_audio_set_format` at runtime | deletes and rebuilds the codec: pop + tens of ms dropout |
| send the filter *frequency* instead of the index | the server derives Hz; indexes are 13→2400, 17→2700, 20→3000 |
| drop the channel byte on a WS frame | `0x00`/`0x01`/`0x02` is the whole protocol |
| run `CONFIG_MRRC_PROFILE=y` in a shipped build | it changes what it measures |

## 6. Turning the instruments on

```
idf.py menuconfig → MRRC Terminal → Profile diagnostics (CONFIG_MRRC_PROFILE=y)
```

Then once a minute: `mem:`, `tx:`, `i2s`, `drop`; on demand: `link:`, `ws:`,
`txlink:`, `ring()`, `batt=`. Once per transmission: `tx done:`.

There is **no persistent log**. The evidence a field failure leaves is whatever was
on the serial line while someone watched — which is why these counters are chosen to
be sufficient without history.

## 7. Where the reasoning lives

`SDD/00` (how claims must be written) · `SDD/05` (numbers) · `SDD/08` (why) ·
`SDD/12` (timings) · `SDD/15` (transmit safety) · `docs/transport-tuning.md`
(the transport incidents) · `docs/optimization-notes.md` (the cost ledger).
