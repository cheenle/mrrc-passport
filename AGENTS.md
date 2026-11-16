# MRRC 终端 — 仓库规范

本仓是**我们自己**的固件，`components/bsp/` 与 `docs/` 的大部分内容来自
`ref/ai-passport`（FoloToy 开源基线）。上游那份 `AGENTS.md`（要求安装 5 个
passport skill、规定 LVGL 二次开发流程）**不适用于本项目**，已替换为本文。

## 先读什么

| 任务 | 必读 |
|---|---|
| 任何改动 | `docs/design-v1.md`（硬件事实、实测数据、全部踩过的坑） |
| 音频相关 | 本文 §1，以及 `docs/design-v1.md` §3.1 / §6 |
| 内存相关 | `docs/design-v1.md` §4、§13 |
| 引脚与器件 | `components/bsp/include/bsp_pins.h`（单一事实来源） |

## §1 不可改动的约束

这四条都是实测撞出来的，改了就会退回到已经踩过的坑：

1. **codec 固定 16 kHz**。ES8311 在 48 kHz 采集近乎失聪（峰值 468 vs 12708）。
   服务端要 48 kHz，靠软件重采样对接，不要在运行期切 `bsp_audio_set_format`。
2. **micro-opus 必须打补丁**：`tools/patch_microopus.sh`。pseudostack 要 16 KB
   （Kconfig 默认下限 60 KB 装不下），并且必须用 `NONTHREADSAFE`——RISC-V GCC
   不生成 `.tbss`，线程安全模式会把 NULL 写进 `celt_decoder`。
3. **LVGL 绘制缓冲保持 8 行**（`bsp_display_lvgl.c`）。40 行会吃掉 19 KB，
   让 Opus 没有内存可用。
4. **PTT 顺序**：先连 `/WSaudioTX` 并等它 accepted，再发 `ptt:true`。
   反过来服务端会把上行音频判成 non-owner 全部丢弃（电台 key 了但没调制）。

## §2 事实优先级

实测 > `bsp_pins.h` > BSP 实现 > 上游文档 > 猜测。

**不允许把推测写成结论**。S3 期间我曾用一段含两次断连的日志推算吞吐，得出
「TLS 只有 104 kbit/s，必须上 Opus」的错误结论，白做了一整套方案。凡是用来
支撑架构决定的数字，必须说明它是在什么条件下测的——`docs/design-v1.md` §13
和 §9.3 里记着这条教训。

## §3 构建与烧录

```bash
./tools/build_on_mac.sh           # mac-mini 构建并取回产物
./tools/flash_probe.sh            # 备份原厂固件（首次）→ 验芯片 → 烧 → 读日志
./tools/flash_probe.sh --log-only # 只读日志
```

- 构建依赖 ESP-IDF 5.4.2（`idf_component.yml` 已放宽到 `>=5.4.2,<6.0.0`）。
- **烧录前必须确认芯片是 ESP32-C3**。`flash_probe.sh` 会验；手工烧录时也必须验，
  把 S3 的镜像写进 C3 会直接变砖。
- 原厂 8 MB 固件备份在 `~/aipassport-factory-8MB.bin`，回退用
  `esptool write_flash 0x0 <backup>`。

## §4 凭据

Wi-Fi、服务端地址与密码只写在本地 `sdkconfig`（`idf.py menuconfig` → MRRC Terminal）。
`build_on_mac.sh` 不同步 `sdkconfig`，`.gitignore` 也已排除。**不要写进
`sdkconfig.defaults` 或任何入库文件**。

## §5 改动验收

| 改动类型 | 必须验证 |
|---|---|
| 音频路径 | 接收 `err=0 drop=0`；PTT 一次，对方能听清 |
| 网络/协议 | `/WSradio` + `/WSaudioRX` 都 connected，`freq` 非零 |
| 内存相关 | idle 打印的 `free / largest` 不比改动前差，`largest` 不低于 8 KB |
| 任何改动 | 串口无 `Guru Meditation`、无 `task_wdt` |

诊断数据一律从串口 60 秒一条的 idle 行读，不要靠猜：

```
mem: free=..K largest=..K min=..K dma_free=..K
stack: audio min free=.. B
tx: keys=.. fails=.. frames=.. sendfail=.. readfail=.. linkdowns=.. last=..
```

## §6 已知未完成

- `largest` 只有约 10 KB（`fullState` 的 5.5 KB JSON 反复 malloc/free 导致碎片）。
- VOL 档切换、UP/DOWN 长按连发未实现。
- 频谱/瀑布未做（内存与连接数都不够）。
- 30 分钟连续收听 + 10 次 PTT 的稳定性未验证。
