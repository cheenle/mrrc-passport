# MRRC 终端 V1 设计（ESP32-C3 / FoloToy AI Passport 硬件）

> 状态：设计定稿，S0 已实现并编译通过（`main/app_main.c`），待上机量内存。
> 本文只写确定性内容。凡是"待实测"的，都标出来，不拿估计当结论。

## 1. 目标

在 AI Passport 这块硬件上做一台**能收听、能发射、能调频率**的 MRRC 终端。

**V1 明确不做**：频谱图、瀑布图。理由见 §4：这两块要吃掉 C3 承受不起的内存和算力，而它们对"通联"本身不是必需品。

## 2. 硬件事实

数据来源优先级：`components/bsp/include/bsp_pins.h` → BSP 实现 → 硬件指南 → 乐鑫 `soc_caps.h`。

| 项 | 事实 | 来源 |
|---|---|---|
| MCU | ESP32-C3，单核 RISC-V 160 MHz | `SOC_CPU_CORES_NUM (1U)` |
| FPU | **无**，浮点走软件 | `esp32c3/soc_caps.h` 无 `SOC_CPU_HAS_FPU`（S3 有） |
| RAM | 400 KB SRAM，**无 PSRAM** | 无 `SOC_SPIRAM_SUPPORTED` |
| Flash | 8 MB XMC | `flash_id` 实测 |
| I2S | **只有 1 个**，无 APLL | `SOC_I2S_NUM (1U)`、仅有 `SOC_I2S_SUPPORTS_PLL_F160M` |
| 显示 | ST7789P3 240×320 RGB565，SPI2 80 MHz mode 0，圆形屏 | `bsp_pins.h` |
| 音频 | ES8311 codec，I2C 0x18 + I2S0 全双工 | `bsp_pins.h` §音频 |
| 按键 | UP/DOWN/OK 三键，**共用 GPIO0 一个 ADC 通道**（电阻梯） | `bsp_pins.h` §按键 |
| 电池 | 500 mAh + CW2017 电量计（I2C 0x63） | `bsp_pins.h` §I2C |
| 尺寸 | 60 × 95 × 8.5 mm，50 g | `docs/hardware-design/specifications.zh_CN.md` |

## 3. 四条硬约束

这四条决定了架构，不能绕。

### 3.1 全双工共享时钟 → 收发必须同采样率

ES8311 的 MCLK/BCLK/WS 由 I2S0 的 TX 和 RX 共用。物理上不存在"收 48 kHz、发 16 kHz"。

**决定**：codec 固定 **48 kHz**，永不切换。麦克风数据在软件里降采样到 16 kHz 再编码。

理由：
- 下行是服务端 48 kHz Opus，必须原样解码播出；
- 上行 Opus 16 kHz 已经够（SSB 音频带宽 3 kHz），且 C3 上省算力；
- `bsp_audio_set_format()` 会 `delete codec → create codec → 重配 I2S`，运行期调它就是一次可听的爆音加几十毫秒断流。**PTT 按下时切换格式是不可接受的**。

### 3.2 无 APLL → 时钟有约 0.16% 误差

MCLK = 256×fs，源只能取 PLL_F160M（160 MHz）：

| 名义 fs | 需要 MCLK | 实际分频 | 实际 fs |
|---|---|---|---|
| 16 kHz | 4.096 MHz | 39 | ≈16.03 kHz |
| 48 kHz | 12.288 MHz | 13 | ≈48.08 kHz |

**后果**：设备时钟比服务端快约 0.16%，长时间收听会让 jitter buffer 缓慢排空，约每 10 分钟偏 1 秒。Opus 的 PLC 与对端缓冲能吸收，但**必须实测 30 分钟以上**，不能只看短连接正常就收工。

### 3.3 无 PSRAM → 所有缓冲进内部 RAM

- 全屏 framebuffer 240×320×2 = 153.6 KB，**不可能**。
- 现成的 LVGL 方案：40 行单缓冲 19.2 KB + 圆角蒙版运行时。
- BSP 文档给出的实测基线：**系统堆约 130 KB 可用**（LVGL 在跑、无网络时）。
- Wi-Fi 起来之后再扣 50–60 KB。

### 3.4 单核无 FPU

- Opus 必须定点（`CONFIG_OPUS_FLOATING_POINT=n`）。
- 任何 float 运算（归一化、抗锯齿、AGC）都是软浮点，逐帧调用要当成本算。
- Wi-Fi 协议栈和音频编解码共享同一个核：**音频任务必须能被抢占但不能被饿死**，PTT 心跳任务的优先级设计（V1 里是 `txhb`）要照抄 xz-02 的教训。

## 4. 内存预算

### 4.1 已实测的固定开销（host 上跑 micro-opus 0.4.1，64 位指针，对 C3 是保守上界）

| 项 | 大小 |
|---|---|
| `opus_decoder_get_size(1)` | 18 404 B（18 KB） |
| `opus_encoder_get_size(1)` | 15 268 B（15 KB） |
| pseudostack 下限（实测溢出点） | 解码 8 KB / 16k 编码 24 KB / 48k 编码 32 KB |

pseudostack 在 0.4.1 里是**懒分配的堆块**（`heap_caps_malloc_prefer`），C3 无 PSRAM 时进内部 RAM，每线程一份。

### 4.2 实测预算（真机，S0 固件，2026-11-16）

在 FoloTech AI Passport 实机上按阶段打点得到的数字（单位 KB，`MALLOC_CAP_INTERNAL`）：

| 阶段 | free | largest | 本阶段消耗 |
|---|---:|---:|---:|
| boot | **248** | 116 | — |
| display+LVGL | 211 | 112 | −37 |
| buttons | 210 | 112 | −1 |
| audio init | 196 | 112 | −14 |
| codec open | 202 | 112 | +6（重建 codec 时释放旧对象） |
| battery | 202 | 112 | 0 |
| **wifi up** | **154** | **112** | −48 |
| opus decoder | **136** | **92** | −18（与 `opus_decoder_get_size(1)`=18 404 B 完全吻合） |
| idle | 136 | 92 | — |

**结论与设计前的估算相反：这颗 C3 比"400 KB SRAM 减掉 Wi-Fi 就见底"的直觉宽裕得多。**

- Wi-Fi 连上后仍有 **154 KB 空闲、112 KB 最大连续块**；
- LVGL（含 40 行 19.2 KB 绘制缓冲）只占 37 KB，**不需要改手绘**；
- 一条 TLS 会话（预估 14–20 KB）放得下；两条也放得下；
- Opus 解码器 18 KB 是不可压缩的硬支出，但不能算瓶颈。

因此 §8.1 的"不用 LVGL"决定**作废**，V1 保留 LVGL；手绘方案降级为 V2 的可选优化（若后续要把频谱/瀑布加进来时需要它省内存）。

未测项：无。TLS 已实测（见下）。

**TLS 会话实测**（`CMCC-cnham` / 192.168.1.239，`POST /api/auth/login` 返回 200）：

- `free` 只掉 1.1 KB —— 因为 `esp_http_client_cleanup()` 立即释放会话，这个数**不代表峰值**；
- 真正的开销看 `min free`：**155 188 → 143 332 = 12 KB**（SSL_IN 2048 / OUT 1024）。

按 12 KB/连接算：两条常驻 TLS = 24 KB，三条 = 36 KB，在 136 KB 稳态里都能容下。
**原设计里"为省 TLS 而让发射通道按需连接"的优化可以保留，但不再是为了活命，而是为了峰值余地。**

### 4.3 待实测（S0 的产出）

`main/app_main.c` 按顺序打点，每一步打印 `free / largest / min`：

```
boot → display+lvgl → buttons → audio init → codec open → battery → wifi up → after tls → opus decoder
```

`largest` 是关键指标：free heap 够但最大连续块不够，16 KB 的 pseudostack 照样分配失败。

## 5. 软件架构

```
main/
  app_main.c     启动顺序、heap 打点、任务创建
  mrrc_net.c     Wi-Fi、登录、WS 连接、Opus 编解码、jitter buffer
  mrrc_ui.c      手绘 UI（条带刷新）
  mrrc_keys.c    按键 → 手势（PTT/调频/切档）
components/bsp/  原样复用：i2c / display / audio / button / battery（不用 lvgl 那一层）
```

任务划分（单核，靠优先级而不靠核间隔离）：

| 任务 | 优先级 | 栈 | 职责 |
|---|---|---|---|
| `txhb` | 10 | 2 KB | PTT 心跳，1.5 s 一次，**必须能抢占编码任务** |
| `audio` | 8 | 8 KB | Opus 编解码、I2S 读写（串行，同一任务） |
| `net` | 5 | 6 KB | WS 收发、JSON 解析 |
| `ui` | 4 | 4 KB | 条带刷新、按键反馈 |
| `btn` | 6 | 3 KB | 按键事件队列消费 |

**Opus 编解码必须在同一个任务里串行**——这是 xz-02 踩过的坑（两个任务同时进 libopus 会互相踩栈）。0.4.1 的 per-thread pseudostack 声称解决了，但没必要冒险。

**心跳必须独立且高优先级**——xz-02 的实测：心跳写在主循环里时，TX 期间 Opus 编码把核打满，心跳断，服务端 5 s 活性闸门收键，发射 5.6 s 必掉。C3 是单核，这个风险比 S3 更大，优先级分层是唯一手段。

## 6. 音频设计

### 6.1 数据流

```
收：WS(Opus 48k) → opus_decode → I2S TX → ES8311 DAC → 喇叭
发：ES8311 ADC → I2S RX(48k) → 3:1 抽取(48k→16k) → opus_encode(16k) → WS
```

抽取用最简单的 3 取 1 加一阶低通即可；48k 到 16k 只需要滤掉 8 kHz 以上，C3 上不构成负担。

### 6.2 Opus 参数（沿用 xz-02 的实测值）

```c
opus_encoder_create(16000, 1, OPUS_APPLICATION_AUDIO, &err);
OPUS_SET_BITRATE(16000);
OPUS_SET_COMPLEXITY(0);
OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE);
OPUS_SET_INBAND_FEC(1);
OPUS_SET_PACKET_LOSS_PERC(10);
```

帧长 60 ms（960 样本 @16k），与 xz-02 一致——服务端已验证接受这个节奏。

### 6.3 麦克风增益

ES8311 模拟 PGA 当前 30 dB（`bsp_audio_set_format` 里写死）。这台是**正常器件**（不是 xz-02 那颗被双面胶堵住的麦），30 dB 很可能过高，表现为近距离说话削顶。

**实测方法**：PTT 按住说话，看编码前峰值落在 3000–20000（满量程 32767）。削顶就把增益降到 18–24 dB。

## 7. 按键与 PTT

### 7.1 映射

| 手势 | 动作 |
|---|---|
| OK 长按 ≥600 ms | **PTT 发射**（松手即停） |
| OK 双击 | 切换 FREQ / VOL 档 |
| UP / DOWN 单击 | FREQ 档：±1 kHz；VOL 档：喇叭音量 ±5% |
| UP / DOWN 长按 | 连发（450 ms 后每 120 ms 一步） |

长按阈值设为 600 ms（`BSP_BTN_LONG_PRESS_MS` 当前 500，改 600）。理由同 xz-02：**短按绝不能漏出载波**。

### 7.2 三个必须写进代码的事实

1. **松手事件晚 120 ms**（按键锁的 `BSP_BTN_HOLD_MS`）。PTT 尾部会多发射 120 ms，这是硬件+驱动的固有延迟，不可消除，只能记账。
2. **触点抖断超过 120 ms 会被判成松手**。长按 PTT 期间如果按键接触不稳，会中途停发。`bsp_button_glitch_count()` 是探针：按住 10 秒，看计数是否持续增长。
3. **回调在 `esp_timer` 任务里**，只能入队，不能做任何阻塞操作（不能录音、不能碰 UI）。

### 7.3 PTT 状态机（照抄 xz-02 的安全链）

```
按下并越过 600 ms → ptt:true → txhb → 等 200 ms → 开始推 Opus 帧
松手 → 停帧 → "s:" → ptt:false → 200 ms 后回 IDLE
按住 180 s → 强制 ptt:false + 屏上 TX TIMEOUT
发射中每 1.5 s → txhb
```

服务端回 `PTT released` / `FT8 正在发射` / `CQ 进行中` 时同步收键——这三条是服务端的强制收键通知，不能忽略。

## 8. UI 设计

### 8.1 用 LVGL（设计修正：原计划手绘，实测后保留 LVGL）

实测显示 LVGL + 40 行绘制缓冲只占 37 KB，而 Wi-Fi 之后仍有 112 KB 连续块可用，因此 V1 直接用 ai-passport 现成的 LVGL 通路（`bsp_lvgl_init`），不自找手绘的麻烦。

保留 ai-passport 已有的两个处理（都是真机踩出来的，不要重写）：

- 圆形屏用 `LV_EVENT_FLUSH_START` 逐行遮罩，不用 `clip_corner`（后者生成 ARGB layer，会在 24 KB 的 LVGL 池里失败）；
- `CONFIG_LV_USE_CLIB_MALLOC=y`（走系统堆，避开静态池耗尽后断言）。

手绘条带方案留在文档里当备用：如果 V2 要加频谱+瀑布，内存不够时再拆 LVGL。

### 8.2 版式（240×320 竖屏）

```
y   0.. 28   顶栏：LSB/USB 徽标 | VFO A | 电池 %
y  40..100   频率："7.215.00"，32 px 抗锯齿
y 108..130   信息行：S9+10 | FIL 2.4K | AF 128
y 140..250   S 表：20 段条形 + 峰值保持
y 260..300   状态片：[FREQ] [RX/TX] [LINK]
```

不做频谱、不做瀑布——这是 V1 的范围决定，不是"暂时没做"。

## 9. 网络设计

### 9.1 V1：用现有的两条通道

服务端当前是 4 条独立 WS（`/WSradio` `/WSspectrum` `/WSaudioRX` `/WSaudioTX`，见 `mrrc_ft710/server.py:2065-2208`）。

V1 只开 **2 条**：

| 通道 | 用途 |
|---|---|
| `/WSradio` | `fullState` / `stateUpdate` / `s_meter` / 发 `set` `ptt` `txhb` |
| `/WSaudioRX` | 下行 Opus |
| `/WSaudioTX` | 上行 Opus（**只在 PTT 期间连**，省一条常驻 TLS） |

发射通道按需连接，松手后关闭——这是 V1 在**不改服务端**前提下把峰值内存压下来的关键手段。

### 9.2 V2：单连接多路复用（需改服务端）

如果 §4.3 的实测显示两条 TLS 也放不下，就必须在服务端加一条 `/WSterminal`，把控制、下行音频、上行音频复用到一条连接上。这是自己的服务端，改动可控，但要动 `server.py` 的三个 handler，属于独立任务。

**V1 的设计不依赖它，V2 才需要。**

## 9.3 实现踩到的两个坑（S1）

**① `MBEDTLS_SSL_IN_CONTENT_LEN` 压得太小，WebSocket 一收数据就断**

第一版为了省内存设成 2048 / 1024，结果 WS 握手成功后立刻报：

```
E esp-tls-mbedtls: read error :-0x7100
E transport_base: esp_tls_conn_read error, errno=Connection already in progress
E websocket_client: esp_transport_read() failed with -28928
```

原因：服务端第一帧 `fullState` 的 JSON 超过了 TLS 记录缓冲，握手能成（小包），一读业务数据就碎。改成 **6144 / 2048**（与 xz-02 一致）后正常。
内存代价：TLS 会话从 12 KB 升到约 16 KB，仍无压力。

**② `/api/auth/login` 限流 5 次 / 5 分钟 / IP**

`server.py:218` 写死 `_LOGIN_MAX_ATTEMPTS = 5`、`_LOGIN_WINDOW_SECONDS = 300`。开发期间反复烧录重启，每次开机都登录，很容易触发 429；固件若用固定短间隔（3 s）重试，会把自己永远锁在窗口里。

固件侧已改为 **指数退避**：30 s → 60 → 120 → 240 → 600（上限）。成功则重置。

开发建议：调试期间把服务端限流放宽（如 20 次/5 分钟），或把 token 存 NVS 复用。两条都不做的话，每次改固件验证要等 5 分钟。

**③ ES8311 在 48 kHz 下麦克风近乎失聪（真 bug）**

同一段采集代码、同一颗麦克风，只改 codec 采样率：

| codec 采样率 | PCM 峰值（满量程 32767） |
|---|---|
| 48 000 Hz | **468** |
| 16 000 Hz | **12 708** |

差 27 倍。`ref/ai-passport` 的音频 demo 全程只用 16 kHz，48 kHz 采集从未被验证过。
最终方案：codec 固定 **16 kHz**，接收解码 48 kHz 后 3:1 抽取（三点平均兼作抗混叠），
发送前 3:1 零阶保持升采样到 48 kHz 交给服务端（服务端自己再重采样到 44.1 kHz）。

**④ 一条被推翻的结论（记录以免重犯）**

S3 期间我曾从 `frames=34 / 5 s` 推出「TLS 上行只有 ~104 kbit/s，48 kHz PCM 发不出去，
必须改 Opus 编码」。**这个推论是错的**：同一份日志里 `linkdowns=2`，连接断开期间
`tx_task` 根本不发帧，因此不能用它计算稳定吞吐。实测（用户听感）48 kHz 裸 PCM 正常，
带宽够用，Opus 上行方案作废。

教训：用计数器估算吞吐之前，先确认计数器只覆盖了"链路正常"的时段。

**⑤ PTT 与 /WSaudioTX 的先后顺序（否则电台 key 了但没调制）**

`server.py` 在收到 `ptt:true` 时才认领 TX 上行所有权：

```python
for tx_ws in list(audio_tx_clients):
    if _ws_tokens.get(tx_ws) == token:
        _tx_owner_ws = tx_ws
```

它扫描的是**已经连上的** `audio_tx_clients`。如果先发 `ptt:true` 再连 `/WSaudioTX`，
扫描时我们的 socket 还不存在，owner 落在别人（网页端）手里，此后我们发的每一帧都被
当 non-owner 丢弃 —— 表现正是「FT-710 有 TX、完全没有声音」。

正确顺序：**先连 `/WSaudioTX` 并等它 accepted，再发 `ptt:true`**。

## 9.4 通道与延迟

| 用途 | 通道 | 连接策略 |
|---|---|---|
| 控制 + 状态 + S 表 | `/WSradio` | 常驻 |
| 下行音频 | `/WSaudioRX` | 常驻（S2） |
| 上行音频 | `/WSaudioTX` | PTT 期间按需连接（S3） |


| 阶段 | 内容 | 退出标准 |
|---|---|---|
| **S0** ✅已编译 | 探针固件：分阶段打点 | 拿到 9 个阶段的 free/largest/min |
| **S1** | 手绘 UI + 按键手势 | 屏幕能显示频率/S 表，按键响应无卡顿 |
| **S2** | 收发音频（不含 PTT 语义） | 能听到电台声音；录一段回放能听清自己 |
| **S3** | PTT 状态机 + 完整协议 | 完整通联一次，服务端无 `Link lost` |
| **S4** | 优化（jitter buffer、栈、增益、必要时拆 LVGL） | 连续收听 30 分钟无丢帧、无重启 |

## 11. 验收标准

1. PTT 松手后 300 ms 内服务端确认收到 `ptt:false`；
2. 连续发射 180 s 不掉线、不触发服务端活性闸门；
3. 连续收听 30 分钟，jitter buffer 水位不单调下降（验证 0.16% 时钟误差可接受）；
4. 空闲时 `largest free block ≥ 20 KB`（留给下次 PTT 的编码器分配）；
5. 长按 PTT 10 秒，`glitch_count` 不增长（按键接触良好）。

## 12. 风险清单

| 风险 | 影响 | 缓解 |
|---|---|---|
| heap 实测远低于估算 | V1 缩水 | **已实测：154 KB free / 112 KB largest，宽裕** |
| 单核跑不动 48k 解码 | 音频断续 | 降 Opus complexity、跳过 CELT 音乐模式 |
| 按键触点抖断 | PTT 中途停发 | 实测 glitch_count，必要时把 HOLD_MS 调大 |
| 时钟 0.16% 误差累积 | 长会话缓冲耗尽 | 30 分钟验收；必要时做缓冲水位自适应 |
| ES8311 30 dB 增益过高 | 发射削顶 | 按 §6.3 实测下调 |
| 发射通道按需连接引入延迟 | PTT 前 200 ms 静音 | 与 xz-02 的既有行为一致，可接受 |

## 13. 实机性能画像（-O2 构建，静默收听 60 s 窗口）

用 `CONFIG_MRRC_PROFILE=y` 采到的数据（该开关默认关闭，运行时间统计每 tick 都要记账）。

### CPU

| 项 | 数值 |
|---|---|
| `opus_decode()` 单帧 | **5.4 ms** |
| 实际下行帧率 | 2057 帧 / 60 s ≈ **34 帧/秒** |
| 解码占用实时比例 | **≈ 18.5%**（5.4 ms × 34 / 1000 ms） |
| IDLE 任务占比 | **37%** ⇒ 系统负载 ≈ 63% |

结论：解码是最大单项，但仍余约三分之一 CPU。频谱这类附加功能理论上有位置，
但要先把连接数压下来（多一条 TLS 就多一份加解密开销）。

### 内存

| 指标 | 稳态 | 备注 |
|---|---|---|
| free | 31–33 KB | |
| **largest 连续块** | **9–10 KB** | TLS 握手要 16 KB，靠多块拼接 |
| **历史最低 min** | **5 KB** | 峰值时刻接近耗尽，是最该盯的数字 |
| DMA 可用 | 23–24 KB | 健康 |
| 碎片率 | free/largest ≈ **3.4×** | 偏高 |

碎片来源明确：每条 `fullState` 都是 5.5 KB 的 JSON，由 cJSON 整块 malloc 再整块 free，
反复切割内部堆。`largest` 只有 9 KB 却仍能完成 TLS 握手，说明 mbedtls 是按多块申请的——
这也解释了发射时偶发的 `linkdowns`。

**下一步的内存工作应该针对 `largest` 而不是 `free`**：要么把 fullState 解析改成复用缓冲，
要么给 TLS 预留一段连续堆。
