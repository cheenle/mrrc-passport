<p align="right">
  <a href="README.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# MRRC Terminal · FoloToy AI Passport (ESP32-C3)

A wearable AI Passport turned into a handheld terminal for the MRRC Modern
stack: listen, transmit, tune, change channels, and watch the spectrum and
waterfall.

This is not a port. The BSP drivers come from `ref/ai-passport` (FoloToy's open
baseline); everything else — the audio path, the protocol, the UI, the PTT state
machine, the key mapping — was rebuilt around what this chip can actually do.

## Status (v1.2.0)

| Feature | State |
|---|---|
| **Single** `/WSterminal` connection: control + downlink audio + uplink audio over one TLS session | ✅ |
| **Audio: PCM16 16 kHz both ways**, no Opus | ✅ `err=0` |
| Transmit: long-press OK, 16 kHz microphone → PCM16 straight out (the server resamples to 48 k) | ✅ |
| Four-position knob: OK double-tap cycles `FREQ → CHANNEL → VOL → MIC` | ✅ |
| Microphone gain: 0..42 dB in 6 dB steps, persisted in NVS | ✅ |
| TX meters: power and SWR replace the S-meter row while transmitting | ✅ |
| Channels: UP/DOWN in CHANNEL mode walks the server's `memChannels`, switching frequency and mode together | ✅ |
| Mode: OK triple-tap (LSB→USB→CW-U→AM→FM→RTTY-L→DATA-L) | ✅ |
| Tuning: UP/DOWN tap ±1 kHz, hold 350 ms then repeat every 90 ms | ✅ |
| Spectrum: 240×45 blue trace (850 bins peak-downsampled to 240 columns) | ✅ |
| Waterfall: 240×40, 64-colour Jet, one row per real sweep | ✅ |
| Network: `cnham-MM` + `w103d.vlsc.net` (IPv6-only host, port 8888) | ✅ |
| Battery / status / S-meter / channel name | ✅ |

Audio no longer loses frames on the wire: the PCM ring is 16 KB (0.51 s) and the
WebSocket receive buffer is 8 KB, and the link is half duplex, so the spectrum
stops while keyed. On this bench unit a residual ~2% still drops; see
[Known issues](#known-issues).

The interface spec is in [`docs/UI.md`](docs/UI.md).

## Audio: why there is no Opus

This build **removed libopus**; both directions are 16 kHz Int16 PCM.

- Dropping the codec made the image 92 KB smaller and moved the heap from
  `free 44K / largest 17K` to `free 80K / largest 58K`
- On the wire it uses the PCM16 profile of `/WSterminal`
  (`{"type":"audioCaps","profile":"pcm16"}`); the server converts 16k ↔ 48k
- The frame is `[0x01 channel][0x02 codec tag][int16 samples]`. **Omitting the
  channel byte makes the server discard the PCM as a spectrum frame** — that was
  the root cause of "transmits but no power"

The sample rate is fixed at 16 kHz because **the ES8311 is nearly deaf when
capturing at 48 kHz** (peak 468 versus 12708), not out of preference. 3 kHz of
SSB passband is comfortably inside that.

## Hardware facts that shaped the design

| Item | Fact | Consequence |
|---|---|---|
| MCU | ESP32-C3, single core at 160 MHz, **no FPU** | hot loops avoid floating point (the EMA is Q8 fixed point) |
| Opus | removed | saves 92 KB of flash and 40 KB of heap; PCM16 costs a 256 kbps uplink |
| RAM | 400 KB, **no PSRAM** | two pixel buffers alone are 40 KB, see below |
| Sample rate | **ES8311 capture at 48 kHz is nearly deaf** (peak 468 vs 12708) | codec pinned to **16 kHz**, software resampling on both sides |
| Buttons | three keys share one ADC channel on GPIO0; **the power key is not under MCU control** | no chording; the power key cannot be a shortcut |
| IPv6 | needs `esp_netif_create_ip6_linklocal()` (after GOT_IP) | `CONFIG_LWIP_IPV6` alone never yields an address |

## Memory and performance (measured, v1.2.0)

| Metric | Value |
|---|---|
| Steady-state `free` / `largest` | **46–49 KB / 28–32 KB** |
| Boot heap `free` / `largest` | 201 KB / 112 KB |
| `taskLVGL` | 33–42% |
| Draw buffer | 24 lines × 240 × 2 = 11.3 KB |
| Waterfall | 40 rows × 240 × 2 = 18.8 KB |
| Audio ring | 16 KB (~0.51 s) |
| DRAM used / total | 179,022 / 321,296 B (55.7%) |

`largest` is the number that matters for PTT: the TLS handshake needs about
16 KB of **contiguous** memory, and we keep roughly 2× that.

The refresh period is 190 ms (5.26 Hz), matched to the radio's sweep rate, so
**every waterfall row is measured data** with no interpolation. That costs CPU:
33–42% versus 23% at 300 ms.

`CONFIG_LV_ATTRIBUTE_FAST_MEM_USE_IRAM` must stay **off**. `lvgl/lvgl` is
constrained to `^9.5.0`, which resolves to 9.6.x, and 9.6 marks far more draw
code with `LV_ATTRIBUTE_FAST_MEM`; with that option on it moved ~117 KB into
SRAM. On the C3, IRAM and DRAM are one 400 KB pool, so the boot heap fell from
201 KB to 88 KB and `esp_wifi_init()` failed with `ESP_ERR_NO_MEM`, reboot-looping
the device.

The optimisation history, the hypotheses that were refuted, and the measurement
behind each change are in [`docs/optimization-notes.md`](docs/optimization-notes.md).

## Build

```bash
idf.py set-target esp32c3
idf.py build
idf.py -p /dev/ttyACM0 flash
```

Requires ESP-IDF 5.5.3. `tools/build_on_mac.sh` builds on the mac-mini and
fetches the artifacts back.

Device-specific settings (Wi-Fi SSID and password, server host, port, and
password) live in the gitignored `sdkconfig.defaults.local`, which the root
`CMakeLists.txt` appends when present. Without that file the build still
succeeds and simply leaves the Kconfig defaults (empty SSID, port 443). The file
is not in the repository — `sdkconfig.defaults` lists the keys, and
`idf.py menuconfig` → **MRRC Terminal** edits the same values in `sdkconfig`.

`tools/patch_microopus.sh` is still in the tree but **is no longer needed**:
libopus is not a dependency any more (see above). It stays because the script
documents the trap — micro-opus's pseudostack floor of 60000 does not fit this
board, and RISC-V GCC emits no `.tbss`, so the "per-thread pseudostack" degrades
to a plain global and a NULL pointer crashes in `celt_decoder.c:tf_decode`
(`Store access fault, MTVAL=0`). Both traps must be handled before Opus can come
back.

## Releases

Releases are source-only. The tag workflow builds and verifies the image as a
gate but publishes no firmware artifact, because a CI image cannot carry this
terminal's credentials. Build from the tag with your own
`sdkconfig.defaults.local`.

## Flash

```bash
./tools/flash_probe.sh              # back up the 8 MB factory image first, then flash and read the log
./tools/flash_probe.sh --no-backup
./tools/flash_probe.sh --log-only
```

## Layout

```
main/
  app_main.c     boot order, key gestures (four positions / triple tap / repeat / PTT), heap watermark
  mrrc_net.c     Wi-Fi, IPv6, login, /WSterminal, JSON, memChannels, token cache
  mrrc_audio.c   downlink: PCM16 straight to I2S, jitter ring
  mrrc_tx.c      uplink: 16k PCM16, PTT heartbeat, 180 s failsafe
  mrrc_ui.c      LVGL: frequency / S-meter / spectrum (lv_line) / waterfall (canvas) / knob badges
components/bsp/  ai-passport BSP (i2c / display / audio / button / battery)
docs/UI.md                      interface spec: layout, palette, fonts, change checklist
docs/optimization-notes.md      measured performance, refuted hypotheses, final config
docs/CHANGELOG-optimization.md  every change in the optimisation phase and why
docs/design-v1.md               early design (Opus era; partly obsolete)
```

## Known issues

1. **Residual audio drops (~2%)** — `main/mrrc_audio.c` counts a drop both when
   the ring mutex cannot be taken and when the ring is full, so the two causes
   are not yet separated. The upstream fix reports `drop=0`; this unit does not
   reproduce that.
2. **The server's `RuntimeError: Set changed size during iteration`** — not in
   this repository. `mrrc_modern/server.py` iterates `audio_rx_clients` while
   handling `audioCaps`, which mutates the same set: the ASGI task crashes, the
   WebSocket drops, and connections leak (31 were observed stacked up). The
   symptom is that 16K PCM disconnects immediately with `audio(frames=0)` on the
   device. Fix: snapshot before iterating, `for ws in list(audio_rx_clients)`.
3. **Double-click has about a 0.45 s delay** — it shares the OK key with the
   triple click, so the third-tap window must expire first. The hardware power
   key is not under MCU control and cannot be a separate button.
4. **RX is sampled twice** — the server path is 44.1k → 48k → 16k, two linear
   interpolations. Collapsing it to 44.1k → 16k would remove one source of
   distortion; the gain is small and it is not done.
5. **Long-session clock drift is unverified** — the C3 has no APLL, so MCLK is
   divided from PLL_F160M, a deviation of about 0.16%.
