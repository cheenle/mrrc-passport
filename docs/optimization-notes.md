# 性能优化与排障记录

本文记录 MRRC Passport（ESP32-C3 终端）在音频链路、UI 渲染、按键处理上的
实测数据与结论。**目的是回溯**：当某个症状再次出现时，先查这里哪条已经验过、
哪条是猜测、哪条被推翻过，避免重复走弯路。

标注约定：

- **实测** — 有硬件日志或计时数据支撑
- **推测** — 未被验证，不要当作结论使用
- **已推翻** — 曾经相信、后来被数据否定的判断

---

## 0. 基线版本

`1a99a2f` 是接收完全正常的最后一个提交：

```
memChannels: 6 slots, 6 usable
audio profile pcm16 accepted (16000 Hz)
idle free=78K largest=52K  audio(frames=2512 err=0 drop=0)
```

**任何大规模改动出问题时，先回到这里。**

---

## 1. 硬件与配置约束

| 项 | 值 | 影响 |
|---|---|---|
| SoC | ESP32-C3，单核 160 MHz | 没有第二个核给网络/UI 分担 |
| PSRAM | 无 | 所有缓冲都吃内部 RAM |
| 屏幕 | 240×320 ST7789，SPI 80 MHz | 大端，需 `swap_bytes` |
| Codec | ES8311，16 kHz | 48 kHz 下麦克风几乎无效 |
| `FREERTOS_HZ` | 1000 | 1 tick = 1 ms（影响所有延时的实际含义） |
| 堆 | 约 400 KB SRAM，实测 largest 40–60 KB | TLS 需要约 16 KB **连续**空间 |

### 字模与内存

每套 Montserrat 字模是几 KB 的点阵位图，**在 flash 里，不在堆上**。
当前只编 3 套（实测：12 / 20 / 44）。曾一度编 6 套（12/14/16/20/28/40），
`bin` 大 37 KB —— 省的是 flash，**不影响 `largest`**。
（已推翻：曾误以为减字模能提升堆余量。）

---

## 2. 音频链路（PCM16，无 Opus）

### 2.1 为什么去掉 libopus（实测）

去掉后 `bin` 从 `0x186ce0` 降到 `0x16f4b0`（约 92 KB），
堆 `free 44K → 80K`、`largest 17K → 58K`。
C3 没有余量跑 Opus + TLS + LVGL。

### 2.2 上行帧格式（关键，坑过一次）

`/WSterminal` 的每个二进制帧都带**频道字节**：

```
[0x01][0x02][int16 PCM ...]
 ^chan  ^codec tag
```

- `0x01` = TX 音频（上行）
- `0x00` = RX 音频（下行）
- `0x02` = 频谱

**曾经的 bug**：固件直接发 `[0x02][PCM]`，服务端把 `0x02` 当**频谱通道**，
PCM 被丢弃 —— 表现是「发射没功率、没声音」。补上频道字节后立刻正常。

### 2.3 PCM16 档位协商

连上后发：

```json
{"type":"audioCaps","profile":"pcm16"}
```

服务端回 `accepted:true` + `rate:16000`。字段名是 **`profile`**，不是 `rx`。

**注意**：`/WSterminal` 端点必须支持 `audioCaps`；早期版本只认
`spectrumCaps`，声明被静默忽略（表现：永远收不到回应）。

### 2.4 下行丢帧的三个位置

`mrrc_audio.c` 的 `rb_put()`：

| 位置 | 含义 | 排查 |
|---|---|---|
| `lock` | 拿不到互斥锁（网络回调与音频任务争用） | `audio drops: lock=N` |
| `full` | 环形缓冲满 | `audio drops: full=N` |

日志每分钟打印一次：

```
audio drops: lock=0 full=26
```

**实测结论**：绝大多数丢帧是 `full`，不是 `lock`。

### 2.5 已试过、不要重复的方案

| 方案 | 结果 |
|---|---|
| 缓冲 8 KB → 16 KB | **无用**。`full` 照样涨，丢帧率不变 → 不是容量问题 |
| `rb_put` 加 spin + yield | **有害**。丢帧反而上升（11% → 13%） |
| 批量取帧（`rb_get` 一次取多帧） | 出现过一次 10%→2.3% 的改善，但与其他改动混在一起，未单独验证 |

---

## 3. UI 与 LVGL（CPU 的主要消耗方）

### 3.1 实测 CPU 分布（典型）

```
taskLVGL       35-44%     ← 最大消耗方
IDLE           40-54%
esp_timer       2-3%
websocket       1-4%
wifi            1-2%
audio          <1%
```

`refresh()`（我们自己的 UI 代码）只占 **3-9%**，其余全在 LVGL 内部。

### 3.2 已排除的原因（实测）

做了三次对照实验，**分别禁用重绘**后测量 `taskLVGL`：

| 实验 | taskLVGL |
|---|---|
| 禁用瀑布重绘 | 49% |
| 禁用频谱重绘 | 43% |
| 两者都禁用 | 45% |

**结论：CPU 消耗与「画什么」基本无关**，是 LVGL 每次运行的固定开销。
因此「减少绘制内容」这条路走不通。

（诊断开关 `CONFIG_MRRC_UI_NO_WF` / `MRRC_UI_NO_PLOT` 曾临时加入，现已移除。）

### 3.3 已排除的原因（推测后被推翻）

- **「`lv_timer_handler()` 每秒 1000 次」— 已推翻。**
  读到 `esp_lvgl_port` 里有 `vTaskDelay(1)`，以为循环固定 1 ms 一轮。
  实际上循环开头的 `xEventGroupWaitBits(..., task_delay_ms)` **已经按 LVGL
  的要求睡过了**，那个 `vTaskDelay(1)` 只是额外多睡 1 ms。
  真实频率约 **20 次/秒**，不是 1000。

- **「频谱与瀑布相邻导致脏区合并」— 部分成立但非主因。**
  把 `Y_WF` 从 192 改到 196（留 5 行黑隙）后，`taskLVGL` 44% → 35%，
  但这是 8 个百分点的变化，不是决定性因素。

### 3.4 绘制缓冲行数 —— 收益最大的一处

**Espressif 官方性能指南给了明确阈值**（`esp_lvgl_port/docs/performance.md`）：

> - Frame buffer size >25% of the screen does not bring relevant performance boost
> - Frame buffer size <10% will have **severe negative effect** on performance

240×320 = 76800 px 换算：10% = 7680 px = **32 行**。

我们长期跑在 3.8%–5.6%，正好落在官方说的"严重影响"区间 ——
**这就是 taskLVGL 吃掉一半 CPU 的真正原因。**

| 缓冲行数 | 占屏比 | taskLVGL | largest |
|---|---|---|---|
| 8 行 | 3.8% | 58-60% | 46K |
| 12 行 | 5.6% | 43-49% | 44K |
| **32 行 + FAST_MEM_IRAM** | **10.0%** | **20-26%** | 24K |

配套启用 `CONFIG_LV_ATTRIBUTE_FAST_MEM_USE_IRAM=y`（官方单列的加速项：
把最热的 LVGL 函数放进 IRAM，免去 flash cache 等待）。

⚠️ **代价要盯紧**：`largest` 从 38K 降到 24K，启动期最低水位 `min=9K`。
PTT 的 TLS 握手需要约 16 KB 连续空间 —— 24K 够用但余量不大。
如果发射失败，先把缓冲退到 24 行（7.5%）。

### 3.4b 已排除的配置项

| 项 | 结论 |
|---|---|
| 12 种像素格式全开 | **已处理**：只留 RGB565，bin 小 114 KB。CPU 无变化（不在热路径） |
| `LV_DRAW_SW_ASM_NONE` | **无法改**：四选一，替代项需 NEON / Helium / RISC-V **Vector**。ESP32-C3 是 RV32IMC，没有 V 扩展 |
| `swap_bytes = true` | **已改为关闭**：面板侧用 RAMCTRL(0xB0) 配小端。实测 CPU 无变化 —— `lv_draw_sw_rgb565_swap` 本就不是瓶颈，省掉的只是每帧 2 万次遍历 |
| `LV_USE_FLEX` / `GRID` | **不能关**：`lv_list`、`lv_menu`、`lv_calendar` 甚至 LVGL 自带示例都依赖 |
| `double_buffer` | **开不了**：两倍缓冲 = 30 KB 连续内存，与 PTT 的 TLS 抢空间 |
| CPU 240 MHz | 不适用：C3 最高 160 MHz |

### 3.5 改动绘制缓冲行数要小心

`BSP_LVGL_DRAW_BUFFER_LINES` 从 8 提到 20（9.4 KB）能减少 SPI 事务次数，
但它占的是**连续内部 RAM**。曾与其他内存改动叠加，导致 `largest` 掉到 9 KB，
PTT 的 TLS 握手失败 —— 表现是「发射自己断」。

**一次只改一项，改完测内存。**

### 3.6 UI 刷新率

`REFRESH_MS` 从 200 改成 100（10 Hz）会让 CPU 翻倍，已经从那次回退。
当前 **300 ms（3.3 Hz）** —— 200 ms 时 `taskLVGL` 仍占 47-55%，
再降到 3.3 Hz 后丢帧从 10.4% 降到 2.3%（实测）。

频谱档位 `spectrumCaps`：

| 档位 | 帧大小 | divider | 帧率 |
|---|---|---|---|
| `high`（Full） | 1701 B | 1 | ~5.3 帧/秒 |
| `mid` | 851 B | 2 | ~2.7 帧/秒 |
| `low` | 426 B | 4 | ~1.3 帧/秒 |

接收用 `high`，发射时降到 `low`（发射时屏幕没人看，省带宽给音频上行）。

**坑**：早期固件写死 `mid`，瀑布看起来像爬 —— 每帧就是一行历史，
半速就是半行速。

---

## 4. 按键（PTT 与双击）

### 4.1 ADC 按键的真实时序（实测）

| 操作 | 驱动报告的按压时长 |
|---|---|
| 正常单击 | 169–246 ms |
| 真长按（PTT） | 569–580 ms |
| **快双击（被驱动合并）** | **619–627 ms** |

驱动把快双击的两下**合并成一次按压**，报成长按 —— 与真 PTT 只差 40 ms，
**无法按时长区分**。这是「双击误触发 PTT」的根源。

### 4.2 已尝试的判别方案

| 方案 | 结果 |
|---|---|
| 检查「上一击的窗口是否开着」 | 不可靠：窗口由抬手设置，第二下按下时可能还没设 |
| 按按压时长判断 | 不可靠：见上表，619 vs 569 |
| 要求按键静默 900 ms 才允许 PTT | 有效但体验差（双击后想立刻说话要等） |
| 上行队列 + 单写者 | **引入过新问题，已全部回滚** |

### 4.3 按键事件只能用 PRESS

`iot_button` 只在按住超过 `short_press_time`（180 ms）才发 `CLICK`，
更短的**静默丢弃**。这块板的 ADC 解码 + BSP 按键锁让快按落进这个洞，
表现是「按了不起作用」。

**UP/DOWN 必须用 `BSP_BTN_PRESS`**（按下即发，无条件、零延迟），
不要用 `CLICK`。

### 4.4 按键轮询周期

`CONFIG_BUTTON_PERIOD_TIME_MS = 5` —— `iot_button` 每 5 ms 轮询一次，
每秒 200 次。实测 `esp_timer` 任务占 2–3%，不是 CPU 大头，但它是持续唤醒源。
改为 10 ms 可减半，注意 `SHORT_TICKS`/`LONG_TICKS` 按这个值计算，改后要验按键判定。

---

## 5. 显示与字体

### 5.1 `transform_scale` 在这块屏上不可靠

用 `lv_obj_set_style_transform_scale(s_freq, 384, 0)` 把 28 px 字放大 1.5×，
结果是：

- 字形渲染 42 px，但对象只有 28 px → 被裁成**上半截**
- 把对象改高 → **完全黑屏**（与圆角遮罩回调冲突）

**改用原生 44 px 字模**，稳定清晰。缩放这条路不要再走。

### 5.2 圆角屏幕

面板有圆角，y=0 的字形会被裁。顶栏文字放在 `H_HDR` 内的 y=8 左右。

### 5.3 字号与底板宽度

一行四块底板必须算清总宽。`FIL 2.4K` 在 14 px 下要 ~62 px，
曾因底板给 56 px 而**折行并与下方 S 表重叠**。改 12 px 后 `FL 2.4K` 约 55 px，刚好。

---

## 6. 服务端交互的坑

### 6.1 断线的真实原因

服务器日志（`/opt/mrrc_modern/logs/server-stdout.log`）里：

```
TX audio ownership → PTT client
Terminal client disconnected during TX! Forcing RX.
Terminal client connected (2 total, ...)
```

**是设备端的 WebSocket 断了，服务器只是按规矩强制回 RX**，不是服务器踢的。
排查方向应该是固件侧（CPU 饿死网络任务），不是服务端的 PTT watchdog。

### 6.2 找对服务器

- **正确**：W103D 盒子 `w103d.vlsc.net`，服务在 `/opt/mrrc_modern`，日志同上
- **错误**：本机 `~/HAM/mrrc/`（旧的 ALS-7400l 那套，日志里会出现
  `rigctld 未就绪` —— 与被测设备毫无关系）

### 6.3 发送失败的错误码

`esp_websocket_client_send_bin()` 的返回值：

| 值 | 含义 |
|---|---|
| `>0` | 实际写入字节数 |
| `0` | 未写入（socket 忙） |
| `-1` | 拿不到客户端锁（**跨任务争用**） |
| `-2` | socket 未连接 |

`-1` 说明多个任务在同时写同一个 WebSocket。曾为此引入上行队列，
结果**更糟**（队列满、内存碎片、状态机错乱），**已全部回滚**。

---

## 7. 方法论（踩过的坑）

这一节比上面的数据更重要。

1. **一次只改一项。** 曾同时改「绘制缓冲 + 音频环形 + 锁策略」，结果三项互相
   影响，无法归因，最后全部回滚。

2. **先测量，再改。** 多次凭直觉加大缓冲、加队列、改锁，全部失败。
   而加了 `audio drops: lock=/full=` 计数后，一眼就看出是 `full` 不是 `lock`。

3. **警惕「省内存」的直觉。** 字模在 flash 不在堆；去掉背景色不省内存
   （样式字段而已）；而 `largest` 是**连续**空间，减大的 `.bss` 才有效。

4. **构建失败要拦住。** `&&` 链中如果 grep 到错误但返回 0，会把旧固件刷进去。
   刷机前确认 `Project build complete` 真的出现。

5. **设备掉线不一定是代码问题。** 那次「插上了」但 `/dev/cu.usbmodem*` 消失，
   是 USB 枚举问题，重插即可。

6. **不要改 `managed_components/`。** 它会被重新拉取。需要打补丁就写
   `tools/patch_*.sh`，构建前跑，幂等。

---

## 8. 当前已知良好配置

| 项 | 值 |
|---|---|
| `REFRESH_MS` | 300 (3.3 Hz) |
| `WF_H` | 40 行（18.8 KB） |
| `PLOT_H` | 45 px |
| `Y_WF` | 196（与频谱留 5 行黑隙） |
| `AUD_RB_BYTES` | 8192 (~250 ms) |
| `BSP_LVGL_DRAW_BUFFER_LINES` | 8 |
| 字模 | 12 / 20 / 44 |
| 频谱档位 | 接收 `high`，发射 `low` |
| TX 帧长 | 20 ms @ 16 kHz（320 样本） |

实测：

```
free=67K  largest=44K  min=43K
IDLE 53%  taskLVGL 36%
audio drops: lock=0 full=5    (0.46%)
```

---

## 9. 下一步候选（按风险从低到高）

1. `CONFIG_BUTTON_PERIOD_TIME_MS` 5 → 10（减半按键轮询）
2. 关掉 `LV_DRAW_SW_ASM_NONE`，让 LVGL 走优化路径
3. 只开 RGB565 一种像素格式
4. 绘制缓冲 8 → 12 或 16（**改完必须测 `largest`**）
5. 瀑布改手动 flush，绕开 LVGL 脏区机制（改动大）
