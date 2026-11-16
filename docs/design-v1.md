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

### 3.1 全双工共享时钟 → 收发只能同一个采样率，而 48 kHz 采集是死的

ES8311 的 MCLK/BCLK/WS 由 I2S0 的 TX 和 RX 共用，物理上不存在"收 48 kHz、发 16 kHz"。

但**实测把这颗芯片的 48 kHz 采集判死了**：同一段采集代码、同一颗麦克风，只改 codec 采样率：

| codec 采样率 | PCM 峰值（满量程 32767） |
|---|---|
| 48 000 Hz | **468** |
| 16 000 Hz | **12 708** |

差 27 倍。`ref/ai-passport` 的音频 demo 全程只用 16 kHz，48 kHz 采集从未被验证过。

**决定**：codec 固定 **16 kHz**，永不切换。服务端要求 48 kHz，两侧靠软件重采样对接：

- 下行：Opus 解码出 48 kHz → 3:1 抽取（三点平均，兼作抗混叠）→ 16 kHz 播放
- 上行：麦克风 16 kHz → 3:1 零阶保持 → 48 kHz PCM（服务端自己再重采样到 44.1 kHz）

另一条纪律：**运行期不要调 `bsp_audio_set_format`**。它会 delete codec → 重建 → 重配 I2S，
中间是一次可听的爆音加几十毫秒断流。

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

### 4.3 最终稳态（S3 完成后）

三通道同时在线（`/WSradio` + `/WSaudioRX`，PTT 时 `/WSaudioTX` 替换下行）：

| 场景 | free | largest | 备注 |
|---|---:|---:|---|
| 静默收听 | ~33 KB | ~10 KB | `min` 曾触到 5 KB |
| PTT 期间 | ~15 KB | ~7 KB | 零余量，靠"发射时暂停下行"腾出空间 |
| 松手后 | ~47 KB | — | 下行重连前 |

`largest` 才是瓶颈：TLS 握手要 16 KB 连续块，现在靠多块拼接完成，这正是偶发
`linkdowns` 的来源。详细分析见 §13。

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

### 6.1 数据流（最终形态）

```
收：/WSaudioRX (Opus 48k) → opus_decode → 48k PCM → 3:1 抽取 → 16k → I2S TX → ES8311 DAC → 喇叭
发：ES8311 ADC → I2S RX 16k → 3:1 零阶保持 → 48k PCM → tag 0x00 → /WSaudioTX
```

上行为什么是裸 PCM 而不是 Opus：`server.py` 的 `AudioHandler.feed_tx_audio()` 明确写着
"Input is 48 kHz Int16 PCM (from Opus decoder or browser)"，服务端自己重采样到 44.1 kHz。
codec 本来就在 16 kHz，升采样比编码便宜，还省掉编码器 15 KB 与编码用的 pseudostack
（16 kHz 编码实测需要 24 KB，这块板没有）。

### 6.2 Opus 参数

只解码，上行是 PCM：

```c
opus_decoder_create(48000, 1, &err);
```

pseudostack 16 000（host 实测：解码 8 KB 溢出、16 KB 有 2× 余量），模式必须是
`NONTHREADSAFE_PSEUDOSTACK` + `INTERNAL_ONLY`，见 `tools/patch_microopus.sh`。

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

### 9.1 通道（最终形态：一条连接）

| 通道 | 承载 | 备注 |
|---|---|---|
| `/WSterminal` | 控制 JSON + 下行 Opus + 上行 PCM + 频谱 | **唯一一条 TLS** |

帧格式（三者不同，别照抄）：

| 类型 | WebSocket opcode | 帧结构 |
|---|---|---|
| 控制 | `0x01` text | JSON，超过 4 KB 会被分片，需按 `payload_offset` 累积 |
| 下行音频 | `0x02` binary | `[0x00][codec][payload]`，codec `0x01` = Opus |
| 上行音频 | `0x02` binary | `[0x01][PCM]`（单层！） |
| 频谱 | `0x02` binary | `[0x02][0x01] + 850 bins`，852 B，约 5.3 Hz |

鉴权一律走 `?token=`：`server.py` 用 `ws.query_params` 校验，**不认 Authorization header**。

### 9.2 V2：单连接多路复用（需改服务端）

如果 §4.3 的实测显示两条 TLS 也放不下，就必须在服务端加一条 `/WSterminal`，把控制、下行音频、上行音频复用到一条连接上。这是自己的服务端，改动可控，但要动 `server.py` 的三个 handler，属于独立任务。

**V1 的设计不依赖它，V2 才需要。**

### 9.3 实现踩到的坑（S1–S3）

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

## 10. 里程碑

| 阶段 | 内容 | 状态 |
|---|---|---|
| **S0** | 探针固件：Wi-Fi / TLS / codec / Opus 分阶段打点 | ✅ 154 KB free / 112 KB largest |
| **S1** | `/WSradio` 控制通道 + LVGL 界面 + 调频键 | ✅ |
| **S2** | 下行音频（Opus 48k → 3:1 抽取 → 16k → 喇叭） | ✅ `err=0 drop=0` |
| **S3** | PTT 上行（16k 麦克风 → 48k PCM） | ✅ 已实测通话清晰 |
| **V2-1** | 单连接 `/WSterminal`（控制+下行+上行） | ✅ largest 18 → 26 KB |
| **V2-1b** | 三档旋钮 + 频道表 + 三击切模式 | ✅ |
| **V2-1c** | IPv6 + 公网域名 `w103d.vlsc.net` | ✅ |
| **V2-2** | 频谱 + 瀑布（`lv_line` / `lv_canvas`） | ✅ |
| **V2-3** | 省电与休眠 | ⬜ |

## 11. 验收标准与实测结果

| 标准 | 结果 |
|---|---|
| PTT 松手后服务端收到 `ptt:false` | ✅ |
| 连续发射不掉线 | ✅ `frames=212 sendfail=0` |
| 通话可懂 | ✅ 已实测「可以听清」 |
| 接收连续无丢帧 | ✅ `err=0 drop=0` |
| 空闲 `largest ≥ 20 KB` | ✅ 26 KB |
| 长按 PTT 触点不误判 | ✅ `glitch=0` |
| 三档 / 三击 / 频道 | ✅ 日志验证 `mode -> VOL`、`chan 5 -> 7210000 LSB` |
| 连续收听 30 分钟无漂移 | ⬜ 未测 |

## 12. 风险清单

| 风险 | 现状 | 缓解 |
|---|---|---|
| `largest` 只有 ~10 KB | **存在** | 复用 `fullState` 解析缓冲，这是根因；见 §13 |
| PTT 期间 `largest` ~7 KB | 存在 | 已用「发射时暂停下行」腾出约 13 KB |
| 时钟 0.16% 漂移（C3 无 APLL） | 未验证 | 需 30 分钟会话实测缓冲水位 |
| ES8311 在 48 kHz 采集失效 | 已规避 | codec 固定 16 kHz |
| micro-opus 不检查分配失败 | 已规避 | pseudostack 预分配 + `prewarm` 触发 |
| 长按期间触点抖断 | 未发现 | `glitch=0`，持续观察 |

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

### 9.3.6 IPv6：三个开关缺一不可

换公网域名时接口一直报 `0 IPv6 address(es)`，逐层排查才定位：

```c
CONFIG_LWIP_IPV6=y                    // 只是把 IPv6 编进 lwIP
CONFIG_LWIP_IPV6_AUTOCONFIG=y         // 只是允许接受 RA（SLAAC）
esp_netif_create_ip6_linklocal(netif) // 必须显式调用，且在 GOT_IP 之后
```

`esp_netif_create_default_wifi_sta()` **不会**自动给接口挂 link-local，所以在补上第三个
之前，前两个开了也没用。补上后：`fe80::` 立即出现，约 6 秒后（第一个 RA）拿到全局
`2409:...` 地址，`/WSterminal` 随即通过 IPv6 连上 `w103d.vlsc.net`。
注意打印时 `esp_ip6_addr_t.addr` 是小端字序，直接格式化会是反的。

### 9.3.7 按键：双击与三击共用一个键的取舍

OK 键同时承担 PTT（长按）、三档循环（双击）、模式切换（三击）。双击和三击在同一个
键上天然冲突：若双击立即执行，检测到第三下时动作已经发生。因此双击动作要等
**600 ms** 窗口过去（期间键任务以 50 ms 轮询），代价是双击有 0.6 秒延迟感。

另外 `iot_button` 的 `BUTTON_DOUBLE_CLICK` 在这块板上**永远不会触发**：它的判定依赖
`repeat == 2`，而 BSP 按键锁让抬起事件晚 120 ms，第二次按下落进第一次的按住窗口。
所以轻点计数是应用层自己做的（见 §9.3.5）。
