<p align="right">
  <strong>简体中文</strong> · <a href="README.md">English</a>
</p>

# MRRC 终端 · FoloToy AI Passport（ESP32-C3）

把一块可穿戴的 AI Passport 改造成 MRRC Modern 手持电台终端：收听、发射、调频、切频道、看频谱与瀑布。

不是移植：本仓从 `ref/ai-passport`（FoloToy 官方开源基线）取 BSP 驱动，其余
（音频通路、协议、界面、PTT 状态机、按键映射）按这块芯片的实际约束重做。

## 现状（v1.2.1）

| 功能 | 状态 |
|---|---|
| **单连接** `/WSterminal`：控制 + 下行音频 + 上行音频共用一条 TLS | ✅ |
| **音频：PCM16 16 kHz 双向**，无 Opus | ✅ `err=0` |
| 发射：OK 长按，16 kHz 麦克风 → PCM16 直发（服务器重采样到 48k） | ✅ |
| 四档旋钮：OK 双击循环 `FREQ → CHANNEL → VOL → MIC` | ✅ |
| 麦克风增益：0..42 dB，6 dB 步进，持久化到 NVS | ✅ |
| 发射表：发射时 S 表行换成功率与驻波 | ✅ |
| 频道：CHANNEL 档下 UP/DOWN 走服务端 `memChannels`，频率+模式一起切 | ✅ |
| 模式切换：OK 三击（LSB→USB→CW-U→AM→FM→RTTY-L→DATA-L） | ✅ |
| 调频：UP/DOWN 单击 ±1 kHz，长按 350 ms 后每 90 ms 连发 | ✅ |
| 频谱：240×45 蓝色折线（850 bin 峰值降采样到 240 列） | ✅ |
| 瀑布：240×40，64 色 Jet，每帧一行真实扫描 | ✅ |
| 网络：`cnham-MM` + `w103d.vlsc.net:8888`（IPv6-only 域名） | ✅ |
| 电量 / 状态 / S 表 / 频道名 | ✅ |

收发两个方向都已不再卡顿。1.2.1 里逐项测出并修掉了五个独立原因——TCP 窗口背后的
接收队列、播放从空环开始、上行跑在 Nagle 下、发送缓冲只有 4 个 MSS、以及一级只制造
削波的软件发射增益。修完实测：`rx 31 KB/s`（与同 Wi-Fi 的对照客户端一致）、每次发射
`queue_drops=0`、慢发送从 31% 降到 0。数据见
[传输链路调优](docs/transport-tuning.zh_CN.md)。

界面规格见 [`docs/UI.md`](docs/UI.md)。

## 音频：为什么没有 Opus

本版**移除了 libopus**，双向都是 16 kHz Int16 PCM。

- 去掉 codec 后 `bin` 小 92 KB，堆 `free 44K → 80K`、`largest 17K → 58K`
- 线上用 `/WSterminal` 的 PCM16 档位（`{"type":"audioCaps","profile":"pcm16"}`），
  服务器负责 16k ↔ 48k 的转换
- 帧格式是 `[0x01 频道][0x02 codec tag][int16 采样]`。
  **缺那个频道字节会让服务器把 PCM 当成频谱帧丢掉** —— 这是"发射没功率"的根因

采样率固定 16 kHz 是因为 **ES8311 在 48 kHz 采集近乎失聪**（峰值 468 vs 12708），
不是软件偏好。对 SSB 的 3 kHz 通带足够。

## 硬件事实（决定架构）

| 项 | 事实 | 后果 |
|---|---|---|
| MCU | ESP32-C3 单核 160 MHz，**无 FPU** | 热循环避免浮点（EMA 已改 Q8 定点）|
| Opus | 已移除 | 省 92 KB flash、40 KB 堆；PCM16 直通代价是 256 kbps 上行 |
| RAM | 400 KB，**无 PSRAM** | 两块像素缓冲就要 40 KB，见下 |
| 采样率 | **ES8311 在 48 kHz 采集近乎失聪**（峰值 468 vs 12708） | codec 固定 **16 kHz**，两侧靠软件重采样 |
| 按键 | 三键共用 GPIO0 一个 ADC 通道，**电源键不受 MCU 控制** | 无组合键；电源键做不了快捷键 |
| IPv6 | 需 `esp_netif_create_ip6_linklocal()`（且在 GOT_IP 之后） | 只开 `CONFIG_LWIP_IPV6` 拿不到地址 |

## 内存与性能（实测，v1.0.0）

| 指标 | 值 |
|---|---|
| 稳态 `free / largest` | **46–49 KB / 28–32 KB** |
| `taskLVGL` | **33–42%**（优化起点为 58%） |
| `IDLE` | 32–45%（起点为 14%） |
| 绘制缓冲 | 24 行 × 240 × 2 = 11.3 KB |
| 瀑布 | 40 行 × 240 × 2 = 18.8 KB |
| 音频环形缓冲 | 16 KB（约 0.51 s） |
| WS 接收缓冲 | 8 KB |

`largest` 是 PTT 的关键指标：TLS 握手需要约 16 KB **连续**内存，当前余量约 1.7 倍。

刷新周期 190 ms（5.26 Hz），与电台扫描率对齐，所以**每一行瀑布都是实测数据**，
不做插值。代价是 CPU 从 23%（300 ms 档）升到 33–42%。

`CONFIG_LV_ATTRIBUTE_FAST_MEM_USE_IRAM` 必须保持关闭：`lvgl/lvgl` 约束是
`^9.5.0`，实际解析到 9.6.x，9.6 给大量绘制函数加了 `LV_ATTRIBUTE_FAST_MEM`，
打开这一项会把约 117 KB 代码搬进 SRAM（C3 的 IRAM/DRAM 是同一块 400 KB），
启动堆从 201 KB 掉到 88 KB，`esp_wifi_init()` 直接 `ESP_ERR_NO_MEM` 并无限重启。

优化过程、被推翻的假设、每项的实测数据见
[`docs/optimization-notes.md`](docs/optimization-notes.md)。

## 构建

```bash
./tools/build_on_mac.sh        # mac-mini 构建并取回产物
```

依赖 ESP-IDF 5.5.3。

`tools/patch_microopus.sh` 仍在仓库里，但**本版不再需要**：libopus 已从依赖中
移除（见「音频：为什么没有 Opus」）。它保留是因为脚本文档记录了这个问题 —
micro-opus 的 pseudostack 下限 60000 放不进这块板，且 RISC-V GCC 不生成 `.tbss`，
"每线程 pseudostack"会退化成普通全局、指针 NULL 时崩在
`celt_decoder.c:tf_decode`（`Store access fault, MTVAL=0`）。将来若要重新引入
Opus，这两个坑必须先处理。

## 烧录

```bash
./tools/flash_probe.sh              # 首次备份 8 MB 原厂固件，再烧、读日志
./tools/flash_probe.sh --no-backup
./tools/flash_probe.sh --log-only
```

Wi-Fi 与服务端凭据放在不入库的 `sdkconfig.defaults.local`（根 `CMakeLists.txt`
在文件存在时自动追加）；也可以用 `idf.py menuconfig` → **MRRC Terminal** 写进本地
`sdkconfig`。文件不存在时构建照样成功，只是保留 Kconfig 默认值（SSID 空、端口 443）。

发布只发源码：tag 流水线编译并验证作为门禁，但不附带固件产物——CI 镜像无法携带
本终端的凭证。需要固件时从 tag 自行构建。

## 目录

```
main/
  app_main.c     启动顺序、按键手势（三档/三击/连发/PTT）、内存水位
  mrrc_net.c     Wi-Fi、IPv6、登录、/WSterminal、JSON、memChannels、token 缓存
  mrrc_audio.c   下行：PCM16 直送 I2S、抖动缓冲
  mrrc_tx.c      上行：16k PCM16 直发、PTT 心跳、180 s 兜底
  mrrc_ui.c      LVGL：频率/S 表/频谱(lv_line)/瀑布(canvas)/三档徽标
components/bsp/  ai-passport BSP（i2c/display/audio/button/battery）
docs/UI.md                  界面规格：布局、配色、字体、改动检查清单
docs/optimization-notes.md  性能实测、被推翻的假设、最终配置
docs/CHANGELOG-optimization.md  优化阶段全部改动的来龙去脉
docs/design-v1.md           早期设计（Opus 时代，部分已过时）
```

## 已知问题

1. **服务端的 `RuntimeError: Set changed size during iteration`** —— 不在本仓库。
   `mrrc_modern/server.py` 的音频循环遍历 `audio_rx_clients` 时，`audioCaps`
   处理会修改同一个 set，导致 ASGI 崩溃、WebSocket 断开、连接泄漏（观察到
   堆积到 31 个）。表现是 16K PCM 接入即断、设备侧 `audio(frames=0)`。
   修法：遍历前做快照 `for ws in list(audio_rx_clients)`。
2. **双击约有 0.45 秒延迟** —— 与三击共用 OK 键，必须等第三击窗口过去。
   硬件电源键不受 MCU 控制，无法用作独立按键。
3. **RX 双重采样** —— 服务器路径是 44.1k → 48k → 16k，两次线性插值。
   合并成 44.1k → 16k 能少一次失真，收益不大，未做。
4. **长会话时钟漂移未验证** —— C3 无 APLL，MCLK 从 PLL_F160M 分频，约 0.16% 偏差。
