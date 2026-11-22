<p align="right">
  <a href="constraint-catalog.md">English</a> · <strong>简体中文</strong>
</p>


# 约束目录

由 `constraints.json` 生成 —— 不要手改。`selftest` 会在这个文件与注册表不一致时失败，
所以目录不会漂移。

重新生成：`python3 .agents/skills/sdd-guardian/harness/sdd_context.py catalog --write`


## Block —— 已经付过一次代价的事故

| id | 规则 | 作用范围 | 依据 |
| --- | --- | --- | --- |
| `secrets-in-tracked-config` | Wi-Fi and server credentials never enter a tracked file | `main/**`, `sdkconfig.defaults`, `sdkconfig.defaults.*`, `tools/**` | AGENTS §5; docs/development/engineering/environment-setup.md |
| `palette-before-heat-init` | heat_init() runs before anything reads the palette | `main/mrrc_ui.c` | docs/UI.md §2; docs/optimization-notes.md waterfall palette |
| `uplink-send-timeout` | The uplink frame timeout stays at or below 200 ms | `main/mrrc_net.c`, `main/mrrc_net.h` | docs/transport-tuning.md §6 |
| `single-terminal-endpoint` | One socket: /WSterminal, never the four-endpoint layout | `main/**` | docs/transport-tuning.md §6; docs/optimization-notes.md §2 |
| `no-opus-in-firmware` | PCM16 only — there is no Opus decoder in this image | `main/**` | docs/optimization-notes.md §2.1 (measured, 24 KB + 60% CPU) |
| `nvs-single-namespace` | Every NVS handle opens the "mrrc" namespace | `main/**` | main/mrrc_cfg.c (namespace mrrc); docs/provisioning.md 'Why NVS over Kconfig' |
| `diagnostics-not-shipped` | CONFIG_MRRC_PROFILE stays off in shipped defaults | `sdkconfig.defaults`, `sdkconfig.defaults.*` | AGENTS §1; docs/optimization-notes.md §0 |

## Warn —— 有真实代价，但仍可人工权衡

| id | 规则 | 作用范围 | 依据 |
| --- | --- | --- | --- |
| `lvgl-single-task` | LVGL is touched from taskLVGL (mrrc_ui.c) only | `main/**` | docs/optimization-notes.md §3 (taskLVGL 28-30% after the layout rewrite) |
| `no-heap-in-audio-path` | No per-frame heap allocation in the audio or uplink path | `main/mrrc_audio.c`, `main/mrrc_tx.c`, `main/mrrc_net.c` | docs/design-v1.md §4.3 ('largest' is the only number that matters) |
| `named-task-stacks` | Task stacks come from named macros, not literals | `main/**` | docs/design-v1.md §4; docs/optimization-notes.md §1 |
| `ptt-release-never-blocks` | Release is fire-and-forget | `main/mrrc_tx.c`, `main/app_main.c` | docs/transport-tuning.md §6; AGENTS §1 |

## Info —— 动那块区域之前先读

| id | 规则 | 作用范围 | 依据 |
| --- | --- | --- | --- |
| `half-duplex-context` | Before touching the uplink, read why transmit used to tear the socket down | `main/mrrc_net.c`, `main/mrrc_tx.c`, `main/mrrc_audio.c` | docs/transport-tuning.md §6 |
| `docs-sync` | A behaviour change also changes the record | `main/**`, `tools/**` | AGENTS §6 (acceptance per change type) |
