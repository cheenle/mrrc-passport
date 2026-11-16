# MRRC 终端 — 仓库规范

本仓是**我们自己**的固件，`components/bsp/` 与 `docs/` 的大部分内容来自
`ref/ai-passport`（FoloToy 开源基线）。上游那份 `AGENTS.md`（要求安装 5 个
passport skill、规定 LVGL 二次开发流程）**不适用于本项目**，已替换为本文。

## 先读什么

| 任务 | 必读 |
|---|---|
| 任何改动 | `docs/design-v1.md`（硬件事实、实测数据、全部踩过的坑） |
| 音频相关 | 本文 §1，`docs/design-v1.md` §3.1 / §6 |
| 网络相关 | 本文 §2，`docs/design-v1.md` §9 |
| 内存/性能 | `docs/design-v1.md` §4 / §13 |
| 引脚与器件 | `components/bsp/include/bsp_pins.h`（单一事实来源） |

## §1 不可改动的约束

每一条都对应一次真机崩溃或无声：

1. **codec 固定 16 kHz**。ES8311 在 48 kHz 采集近乎失聪（峰值 468 vs 12708）。
   服务端要 48 kHz，靠软件重采样对接；运行期不要调 `bsp_audio_set_format`。
2. **micro-opus 必须打补丁**：`tools/patch_microopus.sh`。pseudostack 16 KB
   （Kconfig 默认下限 60 KB 装不下），且必须 `NONTHREADSAFE`——RISC-V GCC
   不生成 `.tbss`，线程安全模式会把 NULL 写进 `celt_decoder`。
3. **LVGL 绘制缓冲保持 8 行**。2 行会让 240×30 的瀑布分 15 次刷屏，
   直接把 `taskLVGL` 顶到看门狗。
4. **上行帧是单层 `[0x01][PCM]`**，下行是双层 `[0x00][codec][payload]`。
   两者不同，别照抄。
5. **频谱用 `lv_line`，不要用 `lv_image`**。后者要 240×H×2 的像素缓冲，
   前者只存顶点坐标。
6. **LAN 路由已不走**：服务端是 `w103d.vlsc.net`（IPv6-only）。

## §2 IPv6 的三个开关（缺一不可）

```c
CONFIG_LWIP_IPV6=y                    // 编译进 lwIP
CONFIG_LWIP_IPV6_AUTOCONFIG=y         // 接受 Router Advertisement (SLAAC)
esp_netif_create_ip6_linklocal(netif) // 且在 GOT_IP 之后调用
```

只有前两个时，接口永远报 `0 IPv6 address(es)`——`esp_netif_create_default_wifi_sta()`
不会自动给接口挂 link-local。这是换域名两次失败的根因。

## §3 事实优先级

实测 > `bsp_pins.h` > BSP 实现 > 上游文档 > 猜测。

**不允许把推测写成结论**。历史教训：曾用一段含两次断连的日志推算吞吐，得出
"TLS 只有 104 kbit/s，必须上 Opus"的错误结论，白做了一整套方案。凡用来支撑
架构决定的数字，必须说明它是在什么条件下测的。

## §4 构建与烧录

```bash
./tools/build_on_mac.sh           # mac-mini 构建并取回产物
./tools/flash_probe.sh            # 备份（首次）→ 验芯片 → 烧 → 读日志
./tools/flash_probe.sh --log-only
```

- **烧录前必须确认芯片是 ESP32-C3**。`flash_probe.sh` 会验；手工烧录也必须验，
  把 S3 镜像写进 C3 会直接变砖。
- 原厂 8 MB 固件备份在 `~/aipassport-factory-8MB.bin`，回退：
  `esptool write_flash 0x0 <backup>`。

## §5 凭据

Wi-Fi、服务端地址与密码只写本地 `sdkconfig`（`idf.py menuconfig` → MRRC Terminal）。
构建脚本不同步 `sdkconfig`，`.gitignore` 已排除。**不要写进入库文件**。

## §6 改动验收

| 改动类型 | 必须验证 |
|---|---|
| 音频路径 | 接收 `err=0 drop=0`；PTT 一次，对方能听清 |
| 网络/协议 | `/WSterminal` connected，`freq` 非零 |
| UI/绘制 | 无 `task_wdt`（LVGL 渲染超时会触发） |
| 内存相关 | idle 的 `free / largest` 不比改动前差 |
| 任何改动 | 串口无 `Guru Meditation`、无 `task_wdt` |

诊断只看串口 60 秒一条的 idle 行：

```
mem: free=..K largest=..K min=..K dma_free=..K
tx: keys=.. fails=.. frames=.. sendfail=.. readfail=.. last=..
```

需要 CPU 明细时临时开 `CONFIG_MRRC_PROFILE=y`（有开销，验完关掉）。

## §7 已知未完成

- 瀑布 30 行受 RGB565 缓冲限制；8 位索引绘制（省 ~13 KB）未做。
- 双击有 0.6 秒延迟（与三击共键）。
- 长会话 0.16% 时钟漂移未验证。
