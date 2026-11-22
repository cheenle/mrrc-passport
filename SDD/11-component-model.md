<p align="right">
  <strong>English</strong> · <a href="11-component-model.zh_CN.md">简体中文</a>
</p>

# 11. Component Model (ART 0515)

## 11.1 Modules

Seven application modules and one board package. The sizes are current source line
counts (`wc -l`), given so that "this is a small change" can be checked against
what actually changes.

| Module | Lines | Responsibility | Invariant it must hold |
| --- | ---: | --- | --- |
| `main/app_main.c` | 673 | boot order, key task, settings load/save, mode/volume wiring | boot order is load-then-connect: nothing is sent before settings are known |
| `main/mrrc_net.c` | 1 067 | Wi-Fi selection, session, WS client, control JSON, channel dispatch, uplink writes | one socket; a failed uplink write never tears the session down; spectrum is dropped before uplink is delayed |
| `main/mrrc_ui.c` | 869 | all LVGL: header, frequency, S-meter, trace, waterfall, chips, battery | the only file that may call `lv_*`; `heat_init()` before any palette consumer |
| `main/mrrc_audio.c` | 397 | I2S init, playback ring, 20 ms framing, pre-buffer gate, counters | 16 kHz forever; no heap; a frame is dropped rather than waited for |
| `main/mrrc_tx.c` | 217 | uplink framing and PTT sequencing | release is one write and never blocks |
| `main/mrrc_prov.c` | 174 | SoftAP, provisioning page, save path | a failed save is reported, never silent |
| `main/mrrc_cfg.c` | 131 | settings precedence (NVS over Kconfig), the `"mrrc"` namespace | every key has exactly one owner module |
| `components/bsp/` | (upstream) | ES8311, ST7789/LVGL port, I2C, battery, buttons | the codec is configured once at 16 kHz; `bsp_audio_set_format` is not called at runtime |

## 11.2 Interfaces

| Direction | Interface | Contract |
| --- | --- | --- |
| to server | `/WSterminal` text frames | JSON control, `stateUpdate` merge semantics; the terminal never invents state it has not been told |
| to server | `/WSterminal` binary `0x00`/`0x01` | `[...][tag][int16 LE]`, tag `0x02` = PCM16 16 kHz mono |
| to server | `/WSterminal` binary `0x02` | spectrum frames, consumed only when the tier is enabled |
| to phone | SoftAP HTTP | one page, two fields, one POST; no captive-portal dependency |
| to codec | I2S0 TX/RX + I2C control | shared MCLK/BCLK/WS, 16 kHz both directions |
| to panel | SPI + LVGL port | 240×320, single draw buffer, 190 ms refresh |

## 11.3 Dependency direction

```
app_main ──▶ mrrc_cfg   (settings precedence)
    │
    ├──▶ mrrc_net ──▶ mrrc_audio ──▶ bsp (I2S, codec)
    │        └──────▶ mrrc_tx ────▶ bsp
    ├──▶ mrrc_ui ───────────────────▶ bsp (LVGL port, panel, battery)
    └──▶ mrrc_prov ────────────────▶ mrrc_cfg
```

Two rules that keep this acyclic and testable:

- **the network never draws.** UI updates from the network are posted to the UI
  task; there is no path from `mrrc_net.c` to `lv_*`.
- **the audio path never blocks on the network.** The ring is written by the net
  task under a zero-timeout mutex; the audio task is the only consumer, and a
  contended frame is dropped, not queued.

## 11.4 Module-level "must not"

| Module | Must not |
| --- | --- |
| `app_main.c` | create a task whose stack literal is not a named macro (guardian `named-task-stacks`) |
| `mrrc_net.c` | write uplink frames with a timeout above 200 ms (guardian `uplink-send-timeout`); call `lv_*`; keep a second endpoint |
| `mrrc_ui.c` | read the palette before `heat_init()`; allocate per repaint |
| `mrrc_audio.c` | reconfigure the codec at runtime; allocate per frame; wait on the ring |
| `mrrc_tx.c` | delay within a release (guardian `ptt-release-never-blocks`) |
| `mrrc_cfg.c` | open an NVS namespace other than `"mrrc"` (guardian `nvs-single-namespace`) |
| `mrrc_prov.c` | report success when the NVS write failed |
| `components/bsp/` | change the codec rate without a peak measurement (AD-003) |

Every row above is either a guardian rule or a review criterion named in a
chapter; none is a style preference. The guardian enforces six of them
mechanically, and the remaining two are checked by their tests
(`NvsOwnershipTests`, and the provisioning page contract).

## 11.5 Test surface

| What | How it is verified | Hardware needed |
| --- | --- | --- |
| Protocol framing | server-side handlers plus the firmware build with `CONFIG_MRRC_PROFILE=y` | no |
| Ring/underrun behaviour | idle line counters over a sustained session | yes |
| LVGL/repaint cost | `bsp_display_flush_stats()` / `bsp_display_flush_bands()` | yes |
| Transmit path | the `tx done:` line plus the server's `TX session` line | yes |
| Constraint conformance | guardian `selftest` + `check` | no |
| Document form | the same `selftest` (`SddStructureTests`) | no |
