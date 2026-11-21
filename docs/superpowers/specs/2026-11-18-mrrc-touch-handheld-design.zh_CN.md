<p align="right">
  <strong>简体中文</strong> · <a href="2026-11-18-mrrc-touch-handheld-design.md">English</a>
</p>

# MRRC Touch 手持终端设计（ESP32-S3 · 2.8" ILI9341 · 电容触摸）

> 日期：2026-11-18　分支：`feat/s3-touch-2.8`　基线：`mrrc-passport` v1.1.0（cc924f4）
>
> 一句话：把 passport 已经踩平的机制（协议时序、音频约束、PTT 状态机、心跳）搬到一块
> 带触摸的竖屏 S3 板上，把"没有屏幕、只有三个 ADC 键"的交互换成"一屏常驻 + 三个互斥浮层"。

---

## 1. 目标与非目标

**目标**

1. 触摸调频（拖动 / 长按精确输入）、频道、设置（10 项）、音量，全部在屏上完成。
2. 240×320 竖屏，主屏常驻（频率 + 频谱 + 瀑布 + 状态栏），三个浮层互斥。
3. 上行 Opus 16 kbps（16 kHz 单声道 / 60 ms 帧 / FEC），下拉 PCM16/Opus 都能解。
4. 保留 passport 的全部硬约束与实测结论，不重新试错。

**非目标（V1 明确不做）**

- Wi-Fi 配网界面（沿用 NVS 里已存的凭据；换网走串口或重新烧写）。
- SD 卡录音、FT8 解码、双 VFO 同时显示、频谱缩放动画。
- 本机播放录音回放（`/WSaudioRX` 之外的回放）。
- 电池快充协议识别（只做 IO9 分压读数 + 顶栏百分比）。

---

## 2. 硬件事实

**板型**：LCD Wiki **ES3C28P**（带触摸）/ ES3N28P（不带）。同款别名 Hosyond 2.8" ESP32-S3 Display。
上游板级参考：`78/xiaozhi-esp32` 的 `main/boards/quandong/esp32-s3-spi-2.8/`。

| 项 | 事实 | 来源 |
|---|---|---|
| SoC | ESP32-S3 QFN56 rev v0.2，双核 240 MHz，FPU，Wi-Fi/BLE5 | esptool `chip_id` |
| Flash | 16 MB，Puya（`0x5E`），eFuse：quad / 3.3 V | esptool `flash_id` |
| PSRAM | 8 MB **octal**，占 GPIO33–37 | JTAG 实测 `MCU_SEL=4` |
| 显示 | **ILI9341V**，240×320，4 线 SPI，RGB565 | lcdwiki 文档 |
| 触摸 | **FT6336G**，I2C 0x38，INT=17，RST=18 | lcdwiki 文档 + 实测 GPIO18 为输出 |
| 音频 | **ES8311**（I2C 0x18 + I2S 全双工）+ **FM8002E** 功放（低电平使能） | 固件字符串 + lcdwiki |
| 麦克风 | 板载 MEMS（LMA2718B381），模拟，接 ES8311 | lcdwiki |
| LED | WS2812B ×1，**IO42** | lcdwiki |
| SD | SDIO 4-bit：CLK=38 / CMD=40 / D0=39 / D1=41 / D2=48 / D3=47 | lcdwiki |
| 电池 | TP4054 充电 + **ADC=IO9** 分压检测 | lcdwiki |
| 按键 | BOOT=IO0（唯一物理键）、RESET=EN | lcdwiki |

**引脚表（单一事实来源，写入 `bsp_pins.h`）**

```
LCD   : CS=10  DC=46  SCK=12  MOSI=11  MISO=13  RST=与 EN 共用（软复位）  BL=45(PWM)
TOUCH : SDA=16 SCL=15 (与 codec 共用)  INT=17  RST=18  地址 0x38
CODEC : SDA=16 SCL=15  地址 0x18
I2S   : MCLK=4  BCLK=5  WS=7  DOUT=8  DIN=6
PA    : IO1，低电平使能
BAT   : IO9 (ADC1_CH8)
LED   : IO42
BUS   : I2C0 / SPI3 / I2S0
```

> 交叉验证：I2C 15/16、SPI 11/12、GPIO10 与 GPIO18 为输出，全部与我们在这块板上用
> USB-JTAG 读到的 `IO_MUX` + GPIO matrix 一致。

---

## 3. 架构

```
┌──────────────────────────── core 1 ────────────────────────────┐
│  LVGL 9.5 (: 30 fps) ─ 主屏 / 三个浮层 / 触摸 indev             │
│      ↑ 状态快照                    ↑ 触摸事件                   │
│  ui_state.c (无锁快照)  ⇄  mrrc_ui.c (只读渲染)                 │
│      ↑                                                          │
│  mrrc_net.c  (WebSocket: WSradio / WSspectrum / WSaudioTX/RX)   │
└──────────────────────────────┬─────────────────────────────────┘
                                │ ringbuf (frame-aligned)
┌──────────────────────────── core 0 ────────────────────────────┐
│  audio_task (prio 7) ← 一个任务独占两个方向（Opus 非线程安全）  │
│      16 kHz → Opus 16 kbps → [0x01][opus]                       │
│      [0x00][codec][payload] → Opus dec 48 kHz → ES8311          │
│  tx_hb      (prio 10) 1.5 s 一次 txhb —— 独立任务，不进 LVGL 循环│
└─────────────────────────────────────────────────────────────────┘
```

**为什么心跳必须独立**（handheld 实测）：TX 时编码任务吃满 core 0，若心跳挂在主循环
（prio 1）会被饿死数秒，服务端 5 s 活性闸门直接收键 → "发射 5.6 s 必断"。

**为什么编解码必须是同一个任务**（handheld 实测）：ESP32-S3 的 GCC 把 `_Thread_local`
降级成普通全局（不生成 `.tbss`），micro-opus 的 "thread-safe pseudostack" 实际上不是
per-thread；两个任务同时进 libopus 会踩坏 `global_stack/scratch_ptr`，报
`Cache disabled but cached memory region accessed` 直接 panic。PTT 是半双工，编解码
本就不必并行 —— 串行化零成本。

---

## 4. BSP：接口不动，实现替换

沿用 passport 的 `components/bsp` 接口（`bsp_display_*` / `bsp_audio_*` / `bsp_button_*` /
`bsp_i2c_*` / `bsp_lvgl_lock`），上层 `main/` 不因换板而改。

| 模块 | passport（C3） | 本板（S3） |
|---|---|---|
| `bsp_display` | ST7789P3，SPI2（9/8/1/20/21） | **ILI9341**，SPI3（11/12/10/46/45），`esp_lcd_ili9341` + 厂家 20 条 init 序列；**去掉圆屏逐行遮罩** |
| `bsp_audio` | ES8311 16 kHz 采集 / 48 kHz 播放 | 同上，只改 I2S 引脚 + I2C(15/16) + PA(IO1，低有效) |
| `bsp_button` | 三键 ADC 电阻梯（GPIO0） | **单键数字输入**（GPIO0），长按 = PTT，单击 = 无动作 |
| `bsp_i2c` | 自有引脚 | 15=SCL / 16=SDA，400 kHz |
| `bsp_battery` | CW2017（I2C 0x63） | **ADC 分压 IO9**（删除 CW2017 路径） |
| `bsp_touch` | 无 | **新增**：`esp_lcd_touch_ft5x06` + `lvgl_port_add_touch` |
| `bsp_lvgl` | LVGL 9.5 + esp_lvgl_port 2.9 + 圆屏遮罩 | LVGL 9.5，方屏，缓冲放 PSRAM |

**依赖**（`idf_component.yml`，均为 Apache-2.0）：
`espressif/esp_lcd_ili9341 ^2.1.0`、`espressif/esp_lcd_touch_ft5x06 ^1.1.1`。

---

## 5. 音频链路

```
采集  ES8311 @16 kHz mono ──► Opus enc 16 kHz / 60 ms / complexity 0 / 16 kbps
                            ──► [0x01][opus]  ──► /WSaudioTX
播放  /WSaudioRX ──► [0x00][codec][payload] ──► Opus dec 48 kHz ──► ES8311 @48 kHz
```

硬约束（passport 实测，不可改）：

1. **codec 采样固定 16 kHz**。ES8311 在 48 kHz 采集近乎失聪（峰值 468 vs 12708）。
2. **不声明 `audioCaps`**。服务端只有 `legacy / pcm16 / pcm48` 三种 profile，`legacy` 即
   Opus 48 kHz 解码；handheld 走的正是这条，实测通。
3. **micro-opus 必须打补丁**：pseudostack 16 KB + `NONTHREADSAFE`（passport 的 C3 上
   RISC-V GCC 不生成 `.tbss`，线程安全模式会把 NULL 写进 `celt_decoder`）。S3 的 Xtensa GCC
   同样不生成 `.tbss`，所以**整个固件只允许一个任务调用 libopus**（见上）。依赖：
   `esphome/micro-opus ^0.4.1`。

下行 payload 的 codec tag 必须按值分派（`0x00` PCM48 / `0x01` Opus / `0x02` PCM16），
**不能只认一种**：网页端连上同一会话时可能推 PCM48。

---

## 6. 网络与协议

### 6.0 单连接 `/WSterminal`（2026-11-19 实测修正）

原设计照 handheld 开四条独立 WebSocket（`/WSradio` `/WSpectrum` `/WSaudioRX`
`/WSaudioTX`）+ Opus。**真机否掉了它**：

| 观察 | 数值 |
|---|---|
| 四条连接时的内部 RAM | `free=2451 B, largest=2048 B` |
| 第四条 socket 的任务 | `websocket_client: Error create websocket task` |
| mbedtls 记录缓冲压到 4 KB | `/WSradio` 报 `-0x7100`（`fullState` 是 5.5 KB，一条 TLS 记录装不下） |
| 任务栈放 PSRAM | 启动即崩（`i2s_channel_disable` 刷屏后在 945 ms 复位循环） |
| 改回单连接后 | `free=15799 B, largest=8704 B`，控制/频谱/音频全在一条上跑通 |

**结论**：passport 的单连接不是"因为 C3 穷"的妥协，而是这个量级（400–512 KB
内部 RAM）唯一装得下的形态。本项目因此**回到 `/WSterminal` + PCM16**，不开 Opus。

### 6.1 `/WSterminal` 帧格式

```
上行  [0x01][0x02][PCM16 16kHz mono]      channel=TX audio, tag=pcm16
下行  [0x00][tag][payload]                channel=RX audio, tag: 0x02=pcm16 / 0x00=pcm48
频谱  [0x02][850 bins]                    channel=spectrum
```

**channel 字节不能少**：少了它，服务端会把 codec tag `0x02` 当成频谱通道，PCM 被
直接丢弃 —— 电台键控了却没有任何信号，ATR-1000 报 `TX silence` 后自行退出，
表现就是"发射没功率"（2026-11-19 实测踩到）。

握手：连接后立刻发
`{"type":"audioCaps","profile":"pcm16"}`（服务端回 `accepted:true, rate:16000`）
与 `{"type":"spectrumCaps","profile":"mid"}`。

### 6.2 半双工

发射期间必须 `esp_codec_dev_set_out_mute(true)`，并把下行队列**排空丢弃**：
否则自己的上行经服务端回灌就是啸叫，且退出发射后会重放堆积的旧音频。


复用 passport 的 `mrrc_net.c` 结构（登录 → `/WSradio` / `/WSspectrum` / `/WSaudio*`），
从 handheld 移植 Opus 编解码与频谱降采样：

| 能力 | 来源 |
|---|---|
| 登录 / token / 重连 / `fullState` 解析 | passport（已跑通） |
| PTT 状态机（`ptt:true` → 帧 → `"s:"` → `ptt:false`）、`txhb` | passport + handheld |
| 频谱 850 bins → 240 列（声明 `mid` 档，约 15 Hz） | handheld |
| Opus 编解码 + 60 ms 帧对齐 | handheld |
| 频道：`{"mem": n}` 召回 + 订阅 `memChannels` 广播 | 服务端已有（6 槽 `freq/mode/label`） |
| 设置项：51 个字段里挑 10 个下发 | 服务端 `/WSradio` |

**频道不落本地**：服务端 `mem_channels.json` 为唯一事实来源；召回走服务端原子路径
（先频率、再模式、再补频率 + 跳 2 s 轮询），手持机只发序号。

---

## 7. UI

### 7.1 主屏（240×320，常驻）

```
y   0.. 26   顶栏   [LSB] ─── VFO A (点:切 A/B) ─── 电量
y  30.. 86   频率   "7.215.00"  24 px，上下拖=调频，长按=数字键盘
y  86..104   信息行 S9+10 │ FIL 2.4K(点) │ AF 128(点)
y 104..200   频谱   240×96，左右拖=连续调频，点按=移到该频点
y 200..276   瀑布   240×76，40 行历史 ≈ 5 s，上/下滑=切范围
y 278..300   底栏   `FREQ/1k` (tap: step) │ [RX/TX] │ [LINK]
```

### 7.2 三个互斥浮层

| 浮层 | 内容 | 打开方式 |
|---|---|---|
| 音量 | AF 滑块 + 本机喇叭滑块（两档独立） | 点信息行 `AF` |
| 设置 | 10 项：滤波宽度 / 射频功率 / 前置放大·衰减 / 降噪 NB·NR / 静噪 / 麦克风增益 / 天调 / AGC 速度 / 频谱范围 / 频道 | 底栏长按或顶栏图标 |
| 频道 | 6 槽列表（label + 频率 + 模式） | 底栏 `FREQ` 长按 |

规则：**同时只存在一个浮层**，点遮罩或 BOOT 单击关闭，无嵌套。

### 7.3 手势映射

| 区域 | 手势 | 动作 |
|---|---|---|
| 频率数字 | 上下拖 | 按步进调频（拖得越远步进越大） |
| 频率数字 | 长按 600 ms | 数字键盘（精确 Hz） |
| 频谱 | 左右拖 | 连续调频（1 px ≈ span/240 Hz） |
| 频谱 | 单击 | VFO 移到点中频点 |
| 瀑布 | 上下滑 | 切范围档位（±25K…±500K） |
| 信息行 | 单击 FIL / AF | 对应浮层 |
| 底栏 | 单击 FREQ | 步进浮层 |

### 7.4 PTT 与触摸锁

- BOOT 长按 ≥600 ms → `ptt:true`；**发射期间全屏触摸禁用**
- 松手 → 停帧 → `"s:"` → `ptt:false` → 200 ms 回 IDLE
- 按住 180 s → 强制收键 + 屏显 `TX TIMEOUT`
- 短按不会发射（键控只在越过长按阈值后发生）

---

## 8. 性能预算

| 项 | 预算 / 做法 |
|---|---|
| LVGL 缓冲 | 2 × 240×80 行，PSRAM（约 76 KB ×2）；DMA 描述符在内部 RAM；`full_refresh=0` |
| 频谱 | 15 Hz，`lv_canvas`（PSRAM），850 → 240 列取最大值 |
| 瀑布 | 15 行/秒，76 行 = 5 s 窗口 |
| 字体 | 数字 24/32 px 抗锯齿 + 12 px 正文；图标用字形字体，不引图片 |
| core 0 | Opus 编解码（**单任务串行**，prio 7）+ txhb(prio 10) |
| core 1 | LVGL + WebSocket + 频谱解析 |
| 目标 | UI 30 fps，频谱/瀑布 15 fps，TX 期间不掉帧、不误收键 |

---

## 9. 风险与未知

| 风险 | 处理 |
|---|---|
| **烧写镜像头必须 DIO/40 MHz** | 用 `qio80` 写 bootloader 会让 ROM 打 `ets_loader.c 78` 后 TG0WDT 循环复位（实测）。QIO 需要 flash 的 QE 位，ROM 阶段读不了。**写 flash 一律 `--flash_mode dio --flash_freq 40m`**，QIO 交给 bootloader 自己按 sdkconfig 打开 |
| ILI9341 厂家 gamma/power 序列不对 → 花屏/偏色 | 直接抄 xiaozhi 板级的 20 条 `vendor_specific_init`；`DISPLAY_INVERT_COLOR` 与 `RGB_ORDER(BGR)` 一并照抄 |
| FT6336 与 ES8311 共用 I2C，初始化顺序敏感 | 先 `bsp_i2c_init` → 触摸复位 → codec 配置；触摸复位后再 `lvgl_port_add_touch` |
| PSRAM 上跑 LVGL 缓冲导致撕裂 | 用 `double_buffer` + `full_refresh=0`；实测不过就退回内部 RAM 40 行 |
| Opus 编码与 LVGL 抢 PSRAM 带宽 | 编码只用内部 RAM（栈 + 帧缓冲），PSRAM 只放显示缓冲与瀑布历史 |
| 天线/功放共用 IO1 与 SD 引脚 | 编码前确认 PA 使能电平；SD 卡 V1 不启用，避免与 WS2812(42) 冲突 |

---

## 10. 里程碑

| | 内容 | 验收 |
|---|---|---|
| **M1** | 板级点亮：ILI9341 + FT6336 + ES8311 | 屏显正确、触摸有坐标、喇叭有测试音 |
| **M2** | MRRC 协议接入（登录 / WSradio / WSspectrum / Opus 收发） | 服务端 `listeners` 出现连接，能听到对方 |
| **M3** | 主屏 UI（频率 / 频谱 / 瀑布 / 状态栏） | 频谱 15 fps 不撕裂 |
| **M4** | 三浮层（音量 / 设置 10 项 / 频道 6 槽） | 每项都能改到电台并回读 |
| **M5** | PTT + 稳定性 | 连续 TX 5 分钟不掉键，180 s 超时生效 |

---

## 11. 测试策略

- **单元可测的部分**：频率格式化、步进换算、频谱降采样、手势判定阈值 —— 主机上跑（沿用
  passport 的 `tools/validate.sh` 思路）。
- **真机可测的部分**：M1–M5 各里程碑的验收项，逐条在板上跑，结果写回本文档的"实测"小节。
- **性能回归**：`ui_fps` / `audio_frames` / `dropped` 三个计数器常驻屏幕调试页（长按顶栏进入）。
