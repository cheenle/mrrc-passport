<p align="right">
  <a href="11-component-model.md">English</a> · <strong>简体中文</strong>
</p>

# 11. 组件模型（ART 0515）

## 11.1 模块

七个应用模块加一个板级包。行数取自当前源码（`wc -l`），写出来是为了让"这只是个小改动"
可以被核对。

| 模块 | 行数 | 职责 | 必须守住的不变量 |
| --- | ---: | --- | --- |
| `main/app_main.c` | 673 | 启动顺序、按键任务、设置读写、模式/音量接线 | 启动顺序是"先载入再连接"：设置未知之前不发任何东西 |
| `main/mrrc_net.c` | 1 067 | 选网、会话、WS 客户端、控制 JSON、通道分发、上行写入 | 单 socket；上行写失败绝不拆会话；频谱先于上行被丢 |
| `main/mrrc_ui.c` | 869 | 全部 LVGL：头部、频率、S 表、轨迹、瀑布、chips、电量 | 唯一允许调 `lv_*` 的文件；`heat_init()` 在任何调色板消费者之前 |
| `main/mrrc_audio.c` | 397 | I2S 初始化、播放环、20 ms 成帧、预缓冲闸门、计数器 | 永远 16 kHz；不堆分配；宁丢帧不等待 |
| `main/mrrc_tx.c` | 217 | 上行成帧与 PTT 时序 | 释放是一次写且永不阻塞 |
| `main/mrrc_prov.c` | 174 | SoftAP、配网页面、保存路径 | 保存失败必须上报，绝不静默 |
| `main/mrrc_cfg.c` | 131 | 设置优先级（NVS 覆盖 Kconfig）、`"mrrc"` namespace | 每个键恰好一个主人模块 |
| `components/bsp/` | （上游） | ES8311、ST7789/LVGL port、I2C、电池、按键 | 编解码器只在 16 kHz 配置一次；运行期不调 `bsp_audio_set_format` |

## 11.2 接口

| 方向 | 接口 | 契约 |
| --- | --- | --- |
| 面向服务器 | `/WSterminal` 文本帧 | JSON 控制、`stateUpdate` 合并语义；终端绝不编造没被告知的状态 |
| 面向服务器 | `/WSterminal` 二进制 `0x00`/`0x01` | `[…][tag][int16 LE]`，tag `0x02` = PCM16 16 kHz 单声道 |
| 面向服务器 | `/WSterminal` 二进制 `0x02` | 频谱帧，仅在该档位开启时消费 |
| 面向手机 | SoftAP HTTP | 一个页面、两个字段、一次 POST；不依赖强制门户 |
| 面向编解码器 | I2S0 TX/RX + I2C 控制 | 共享 MCLK/BCLK/WS，两个方向都 16 kHz |
| 面向面板 | SPI + LVGL port | 240×320，单绘制缓冲，190 ms 刷新 |

## 11.3 依赖方向

```
app_main ──▶ mrrc_cfg   （设置优先级）
    │
    ├──▶ mrrc_net ──▶ mrrc_audio ──▶ bsp（I2S、编解码器）
    │        └──────▶ mrrc_tx ────▶ bsp
    ├──▶ mrrc_ui ───────────────────▶ bsp（LVGL port、面板、电池）
    └──▶ mrrc_prov ────────────────▶ mrrc_cfg
```

两条规则让它无环且可测：

- **网络从不绘图。** 来自网络的状态更新投递给 UI 任务；从 `mrrc_net.c` 到 `lv_*` 没有路径。
- **音频路径绝不在网络上阻塞。** 环由 net 任务在零超时互斥量下写入；audio 任务是唯一消费者，
  争用的帧被丢弃，不排队。

## 11.4 模块级"不许做"

| 模块 | 不许 |
| --- | --- |
| `app_main.c` | 用非命名宏的栈字面量创建任务（guardian `named-task-stacks`） |
| `mrrc_net.c` | 用超过 200 ms 的超时写上行帧（guardian `uplink-send-timeout`）；调 `lv_*`；保留第二个端点 |
| `mrrc_ui.c` | 在 `heat_init()` 之前读调色板；每次重绘分配内存 |
| `mrrc_audio.c` | 运行期重配编解码器；每帧分配；在环上等待 |
| `mrrc_tx.c` | 在释放路径里延宕（guardian `ptt-release-never-blocks`） |
| `mrrc_cfg.c` | 打开 `"mrrc"` 之外的 NVS namespace（guardian `nvs-single-namespace`） |
| `mrrc_prov.c` | NVS 写失败却报告成功 |
| `components/bsp/` | 没有峰值测量就改编解码器速率（AD-003） |

上表每一行要么是一条 guardian 规则，要么是某章点名的评审标准，没有一条是风格偏好。
guardian 机械执行其中六条，剩下两条由测试覆盖（`NvsOwnershipTests`，以及配网页面的契约）。

## 11.5 测试面

| 测什么 | 怎么验 | 需要硬件 |
| --- | --- | --- |
| 协议成帧 | 服务器侧处理函数 + 带 `CONFIG_MRRC_PROFILE=y` 的固件构建 | 否 |
| 环/underrun 行为 | 持续会话中的 idle 行计数器 | 是 |
| LVGL/重绘代价 | `bsp_display_flush_stats()` / `bsp_display_flush_bands()` | 是 |
| 发射路径 | `tx done:` 行加服务器 `TX session` 行 | 是 |
| 约束符合性 | guardian `selftest` + `check` | 否 |
| 文档形式 | 同一个 `selftest`（`SddStructureTests`） | 否 |
