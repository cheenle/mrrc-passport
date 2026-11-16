# MRRC 终端 · FoloToy AI Passport（ESP32-C3）

把一块可穿戴的 AI Passport 改造成 MRRC Modern 手持电台终端：收听、发射、调频。

不是移植：本仓从 `ref/ai-passport`（FoloToy 官方开源基线）取 BSP 驱动，其余
（音频通路、协议、界面、PTT 状态机）按这块芯片的实际约束重做。

## 现状

| 功能 | 状态 | 说明 |
|---|---|---|
| 收听 | ✅ | `/WSaudioRX` → Opus 48 kHz 解码 → 3:1 抽取 → 16 kHz → ES8311 |
| 发射 | ✅ | 已实测通话清晰。`/WSaudioTX` 按需连接，16 kHz 麦克风 → 3:1 升采样 → 48 kHz PCM |
| 调频 | ✅ | UP/DOWN 单击 ±1 kHz，写入服务端 `freq` |
| 屏幕 | ✅ | 频率 / 模式 / 波段 / S 表 / 电池 / TX 变红（240×320 圆屏） |
| PTT 安全链 | ✅ | 500 ms 长按门槛、1.5 s 心跳、180 s 兜底、松手先 `s:` 再 `ptt:false` |
| 音量档 / 连发 | ⬜ | 未实现（OK 双击、UP/DOWN 长按连发） |
| 频谱 / 瀑布 | ⬜ | 未做，内存与连接数都不够，见设计文档 §4 |

## 硬件事实（决定架构）

| 项 | 事实 | 后果 |
|---|---|---|
| MCU | ESP32-C3 单核 160 MHz | 无核可逃，任务优先级就是唯一手段 |
| FPU | **无** | Opus 必须定点；所有浮点走软件 |
| RAM | 400 KB，**无 PSRAM** | 双 TLS 连上后只剩 ~31 KB |
| 采样率 | **ES8311 在 48 kHz 采集近乎失聪** | codec 固定在 **16 kHz**，两侧靠软件重采样 |
| 按键 | 三键共用 GPIO0 一个 ADC 通道 | 无组合键；松手事件晚 120 ms |

## 构建

```bash
# 在 mac-mini（192.168.1.228）上构建并取回产物
./tools/build_on_mac.sh

# 首次或在 managed_components 重建后，需要让 micro-opus 可用：
./tools/patch_microopus.sh
```

依赖 ESP-IDF 5.4.2。`tools/patch_microopus.sh` 做两件事，缺一不可：

1. 把 micro-opus 的 `Kconfig` pseudostack 下限从 60000 放宽到 8000（这块板只放得下 16 KB）；
2. 切到 `NONTHREADSAFE_PSEUDOSTACK` + `INTERNAL_ONLY`——RISC-V GCC 不生成 `.tbss`，
   所谓"每线程 pseudostack"会退化成普通全局，指针为 NULL 时崩在
   `celt_decoder.c:tf_decode`（`Store access fault, MTVAL=0`）。

## 烧录与观察

```bash
./tools/flash_probe.sh              # 首次会先备份 8 MB 原厂固件，再烧、再读 25 s 日志
./tools/flash_probe.sh --no-backup  # 已备份过
./tools/flash_probe.sh --log-only   # 只读日志
```

Wi-Fi 与服务端凭据走 `idf.py menuconfig` → **MRRC Terminal**，只写入本地
`sdkconfig`（不入库）。构建脚本不会同步 `sdkconfig`。

## 目录

```
main/
  app_main.c     启动顺序、按键分发、PTT 手势、内存/栈水位
  mrrc_net.c     Wi-Fi、登录、/WSradio、JSON 状态、token 缓存
  mrrc_audio.c   /WSaudioRX、Opus 解码、48k→16k、I2S 输出
  mrrc_tx.c      /WSaudioTX、16k→48k、PTT 上行
  mrrc_ui.c      LVGL 界面
components/bsp/  来自 ai-passport 的 BSP（i2c/display/audio/button/battery）
  └ bsp_display_lvgl.c  绘制缓冲 40 → 8 行（省 15 KB 给 Opus）
docs/design-v1.md  设计与全部实测数据、踩坑记录
tools/patch_microopus.sh, build_on_mac.sh, flash_probe.sh
```

## 实测数据

| 项 | 值 |
|---|---|
| 静态 RAM / Flash | app ≈ 1.57 MB / 分区 8 MB（81% 空闲） |
| 稳态可用堆 | free ~33 KB，largest ~10 KB，历史最低 5 KB |
| Opus 解码 | 5.4 ms/帧，约占实时 18.5%（34 帧/秒） |
| CPU 负载 | 约 63%（IDLE 37%） |

## 已知问题

1. **`largest` 只有 ~10 KB**：每条 `fullState` 是 5.5 KB JSON，cJSON 整块 malloc/free
   反复切碎内部堆。TLS 握手要 16 KB，靠多块拼接，偶发 `linkdowns`。
   修法是复用解析缓冲，见设计文档 §13。
2. **PTT 期间 `largest` 降到 ~7 KB**：功能可用但零余量。
3. **时钟漂移 0.16%**（C3 无 APLL，MCLK 从 PLL_F160M 分频）：长会话需实测。
4. **未做长时间稳定性验证**：连续收听 30 分钟 + 10 次 PTT 的分布还没摸过。
