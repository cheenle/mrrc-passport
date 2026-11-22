<p align="right">
  <a href="AGENTS.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# MRRC Terminal — repository rules

This repository is **our own** firmware. Most of `components/bsp/` and `docs/`
comes from `ref/ai-passport` (the FoloToy open baseline). The upstream
`AGENTS.md` — which asks for five passport skills and prescribes an LVGL
secondary-development flow — **does not apply here** and has been replaced by
this file.

## What to read first

| Task | Must read |
|---|---|
| Any change | **`SDD/` — read chapter 00 first** (verification discipline; then 05 requirements, 08 decisions, 15 transmit safety) |
| Any change | `docs/design-v1.md` (hardware facts, measured data, every trap we hit) |
| Audio | §1 here, `docs/design-v1.md` §3.1 / §6 |
| Network | §2 here, `docs/design-v1.md` §9 |
| **Stutter / performance / dropped frames** | **`docs/design-v1.md` §5.1 + §12.1** (the order to diagnose in; do not start by enlarging buffers) |
| Memory | `docs/design-v1.md` §4, especially §4.3 (`largest` is the only number to watch) |
| Pins and parts | `components/bsp/include/bsp_pins.h` (single source of truth) |
| **Before editing any file** | `python3 .agents/skills/sdd-guardian/harness/sdd_context.py brief <files>` — the applicable rules plus the post-mortem sections that explain them (`docs/sdd-guardian.md`) |

## §1 Constraints that must not change

Each one corresponds to a real-board crash or a silent transmitter. The
mechanically checkable ones are encoded in
`.agents/skills/sdd-guardian/harness/constraints.json` and enforced by
`tools/validate.sh --static` and the `governance` CI job; the rest are review
criteria. Rules are added only after the fact, never pre-emptively — see
`docs/sdd-guardian.md` §3.

1. **The codec is fixed at 16 kHz.** The ES8311 is nearly deaf when capturing at
   48 kHz (peak 468 versus 12708). The server wants 48 kHz, so software
   resampling bridges the gap; never call `bsp_audio_set_format` at runtime.
2. **No libopus — raw PCM16** (removed 2026-11-19). A single 160 MHz core cannot
   pay its CPU cost or its 16 KB contiguous heap. Bandwidth went from 12 kbps to
   256 kbps and measurement showed that was never the bottleneck.
3. **LVGL draw buffer is 24 lines** (`BSP_LVGL_DRAW_BUFFER_LINES`). Eight lines
   was a temporary value from before PCM16, when it was misread as "not enough
   memory"; Espressif's guidance is that below 10% of the screen, rendering
   slows down badly. 32 lines is better for CPU but leaves `largest` at 18 KB,
   so 24 is the compromise.
4. **The uplink frame has two layers: `[0x01 channel][0x02 tag][int16]`**; the
   downlink is `[0x00][tag][int16]`. **Drop the channel byte and the server
   routes PCM as spectrum**, which looks like "the device is fine but the other
   station hears nothing".
5. **Drop the spectrum tier and discard spectrum frames while transmitting**
   (half duplex). A 1701-byte spectrum frame queued ahead of a 642-byte audio
   frame is seconds of latency on one TCP connection. Do not treat this as an
   optional optimisation and delete it.
6. **The spectrum uses `lv_line`, not `lv_image`.** The latter needs a 240×H×2
   pixel buffer; the former stores vertex coordinates only (`s_pts` with 240
   entries, not 850).
7. **LAN routing is gone**: the server is `w103d.vlsc.net` (IPv6 only).
8. **Microphone gain steps by 6 dB.** The ES8311 PGA is stepped
   (0/6/12/…/42); with a 1 dB step, 18→23 dB writes the same register value and
   looks like "the knob does nothing". Maximum is 42 dB.

## §2 The three IPv6 switches (all are required)

```c
CONFIG_LWIP_IPV6=y                    // compile IPv6 into lwIP
CONFIG_LWIP_IPV6_AUTOCONFIG=y         // accept Router Advertisements (SLAAC)
esp_netif_create_ip6_linklocal(netif) // and call it after GOT_IP
```

With only the first two the interface reports `0 IPv6 address(es)` forever:
`esp_netif_create_default_wifi_sta()` does not attach a link-local address
itself. This was the root cause of two failed domain switches.

## §3 Order of authority

Measurement > `bsp_pins.h` > the BSP implementation > upstream documentation >
guessing.

**Do not write a guess as a conclusion.** Historical lesson: throughput was once
inferred from a log containing two disconnects, which produced the wrong
conclusion that "TLS gives only 104 kbit/s, so Opus is mandatory" — and a whole
design was built on it. Every number used to justify an architectural decision
must state the conditions it was measured under.

## §4 Build and flash

```bash
./tools/build_on_mac.sh           # build on the mac-mini and fetch the artifacts
./tools/flash_probe.sh            # back up (first time) -> verify chip -> flash -> read log
./tools/flash_probe.sh --log-only
```

- **Confirm the chip is an ESP32-C3 before flashing.** `flash_probe.sh` checks
  it; a manual flash must check too — writing an S3 image into a C3 bricks it.
- The factory 8 MB image is backed up at `~/aipassport-factory-8MB.bin`; roll
  back with `esptool write_flash 0x0 <backup>`.

## §5 Credentials

Wi-Fi, the server address, and passwords belong in the local configuration only:
`sdkconfig` (via `idf.py menuconfig` → MRRC Terminal) or the gitignored
`sdkconfig.defaults.local`. Build scripts do not sync `sdkconfig`, and
`.gitignore` excludes both. **Never put them in a tracked file.**

## §6 Acceptance per change type

| Change | Must verify |
|---|---|
| Audio path | receive `err=0 drop=0`; one PTT, and the other station hears it clearly |
| Network / protocol | `/WSterminal` connected, `freq` non-zero |
| UI / drawing | no `task_wdt` (an over-long LVGL render triggers it) |
| Memory | idle `free / largest` no worse than before the change |
| Any change | no `Guru Meditation` and no `task_wdt` on the serial log |
| Any change | `python3 .agents/skills/sdd-guardian/harness/sdd_context.py check --staged` exits 0 (or the hit is justified in the commit message) |

Diagnose from the once-a-minute idle line only:

```
mem: free=..K largest=..K min=..K dma_free=..K
tx: keys=.. fails=.. frames=.. sendfail=.. readfail=.. last=..
```

For a CPU breakdown, temporarily enable `CONFIG_MRRC_PROFILE=y` (it costs
overhead; turn it off after measuring).

## §7 Known gaps

- The waterfall is limited by an RGB565 buffer; 8-bit indexed drawing (~13 KB
  saved) is not implemented.
- Double-click has a 0.6 s delay (it shares a key with the triple click).
- Long-session clock drift of 0.16% is unverified.
