# MRRC 终端 · FoloToy AI Passport（ESP32-C3）

把一块可穿戴的 AI Passport 改造成 MRRC Modern 手持电台终端：收听、发射、调频、切频道、看频谱与瀑布。

不是移植：本仓从 `ref/ai-passport`（FoloToy 官方开源基线）取 BSP 驱动，其余
（音频通路、协议、界面、PTT 状态机、按键映射）按这块芯片的实际约束重做。

## 现状（V2-1）

| 功能 | 状态 |
|---|---|
| **单连接** `/WSterminal`：控制 + 下行音频 + 上行音频共用一条 TLS | ✅ |
| 收听：Opus 48 kHz 解码 → 3:1 抽取 → 16 kHz → ES8311 | ✅ `err=0 drop=0` |
| 发射：OK 长按，16 kHz 麦克风 → 3:1 升采样 → 48 kHz PCM | ✅ 已实测通话清晰 |
| 三档旋钮：OK 双击循环 `FREQ → CHANNEL → VOL` | ✅ |
| 频道：CHANNEL 档下 UP/DOWN 走服务端 `memChannels`，频率+模式一起切 | ✅ |
| 模式切换：OK 三击（LSB→USB→CW-U→AM→FM→RTTY-L→DATA-L） | ✅ |
| 调频：UP/DOWN 单击 ±1 kHz，长按 450 ms 后每 120 ms 连发 | ✅ |
| 频谱：240×84 蓝色折线（`lv_line`，只存顶点，不占像素缓冲） | ✅ |
| 瀑布：240×30，64 色 Jet，`lv_canvas`，隔行插值滚动 | ✅ |
| 网络：`cnham-MM` + `w103d.vlsc.net`（IPv6-only 域名） | ✅ |
| 电量 / 状态 / toast | ✅ |

## 硬件事实（决定架构）

| 项 | 事实 | 后果 |
|---|---|---|
| MCU | ESP32-C3 单核 160 MHz，**无 FPU** | Opus 必须定点；无核可逃 |
| RAM | 400 KB，**无 PSRAM** | 两块像素缓冲就要 40 KB，见下 |
| 采样率 | **ES8311 在 48 kHz 采集近乎失聪**（峰值 468 vs 12708） | codec 固定 **16 kHz**，两侧靠软件重采样 |
| 按键 | 三键共用 GPIO0 一个 ADC 通道，**电源键不受 MCU 控制** | 无组合键；电源键做不了快捷键 |
| IPv6 | 需 `esp_netif_create_ip6_linklocal()`（且在 GOT_IP 之后） | 只开 `CONFIG_LWIP_IPV6` 拿不到地址 |

## 内存与性能（实测）

| 指标 | 值 |
|---|---|
| 稳态 `free / largest` | **51 KB / 26 KB** |
| PTT 期间 `largest` | ~17 KB |
| Opus 解码 | 5.4 ms/帧，约占实时 18.5%（34 帧/秒） |
| 静态缓冲 | 频谱 960 B（`lv_line`）+ 瀑布 14.4 KB（RGB565）+ 解码 5.7 KB |

**为什么频谱不占内存**：`lv_line` 只存 240 个顶点坐标，LVGL 自己栅格化描线；
`lv_image` 才需要「宽×高×2 字节」的像素缓冲。

## 构建

```bash
./tools/build_on_mac.sh        # mac-mini 构建并取回产物
./tools/patch_microopus.sh     # managed_components 重建后必须跑一次
```

依赖 ESP-IDF 5.4.2。`patch_microopus.sh` 做两件事，缺一不可：

1. 把 micro-opus 的 `Kconfig` pseudostack 下限从 60000 放宽到 8000（这块板用 16 KB）；
2. 切到 `NONTHREADSAFE_PSEUDOSTACK` + `INTERNAL_ONLY`——RISC-V GCC 不生成 `.tbss`，
   "每线程 pseudostack"会退化成普通全局，指针为 NULL 时崩在
   `celt_decoder.c:tf_decode`（`Store access fault, MTVAL=0`）。

## 烧录

```bash
./tools/flash_probe.sh              # 首次备份 8 MB 原厂固件，再烧、读日志
./tools/flash_probe.sh --no-backup
./tools/flash_probe.sh --log-only
```

Wi-Fi 与服务端凭据走 `idf.py menuconfig` → **MRRC Terminal**，只写本地 `sdkconfig`。

## 目录

```
main/
  app_main.c     启动顺序、按键手势（三档/三击/连发/PTT）、内存水位
  mrrc_net.c     Wi-Fi、IPv6、登录、/WSterminal、JSON、memChannels、token 缓存
  mrrc_audio.c   下行：Opus 解码、48k→16k、I2S
  mrrc_tx.c      上行：16k→48k、PTT 心跳、180 s 兜底
  mrrc_ui.c      LVGL：频率/S 表/频谱(lv_line)/瀑布(canvas)/三档徽标
components/bsp/  ai-passport BSP（i2c/display/audio/button/battery）
docs/design-v1.md  设计与全部实测数据、踩坑记录
```

## 已知问题

1. **瀑布只有 30 行**：受 RGB565 像素缓冲限制。要更长的瀑布需要改成 8 位索引绘制
   （理论省 ~13 KB），尚未做。
2. **双击有 0.6 秒延迟**：与三击共用 OK 键，必须等第三个 tap 的窗口过去。
3. **`min free` 曾触到 5 KB**：所有大块已在 `.bss` 预分配，运行期不再申请，
   但 cJSON 每条 `fullState`（5.7 KB）仍会切割内部堆。
4. **长会话时钟漂移未验证**：C3 无 APLL，MCLK 从 PLL_F160M 分频，约 0.16% 偏差。
