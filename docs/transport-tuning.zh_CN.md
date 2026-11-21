<p align="right">
  <strong>简体中文</strong> · <a href="transport-tuning.md">English</a>
</p>

# 传输链路调优：终端为什么会卡（2026-11-21）

下面每一条都是在设备和服务器上实测的，不是推断。症状是两个方向都持续卡顿，查下来
是五个互相独立的原因，必须逐个修。

复测时的观测点：

- **设备**：`CONFIG_MRRC_PROFILE=y` 每分钟打印一次
  `link: rx=… underruns=… wr_max=… rb_used_max=… rebuf=… rssi=…`、
  `ws: total=…`、`txlink: send_max=… slow=… loop_max=… stalls=…`，
  每次发射结束还有一条 `tx done:`。
- **服务端**（`MRRC-BG1SB`，`/opt/mrrc_modern/logs/server-stdout.log`）：
  每次发射一条 `TX session: frames=… fed=… written=… queue_drops=… peak=…`，
  每条连接一条 `Terminal media losses: … max pump stall …`。
- **对照客户端**：同 Wi-Fi 上再开一个 `/WSterminal` 客户端，这样"服务端发了多少"
  和"设备收到多少"可以在同一时间窗内对比。

## 1. 下行：天花板在设备，不在链路

| 同一时间窗 | 音频 | 频谱 | 合计 |
|---|---|---|---|
| 对照客户端 | 31.3 KB/s | 17.8 KB/s | 50.2 KB/s |
| 设备（修复前） | 25.6–28.6 KB/s | — | — |
| 设备（修复后） | 31.3 KB/s | — | 49.2 KB/s |

先排除的：Wi-Fi 链路（对照客户端走同一个 AP 能收满 50 KB/s，设备 RSSI −22 dBm）、
CPU（IDLE 仍有 38–46%）、以及单独调大 TCP 窗口（5760 和 16384 测出来一样）。

真正被填满的是窗口背后的队列：

| 配置 | 原值 | 现值 |
|---|---|---|
| `LWIP_TCP_WND_DEFAULT` | 5760 | 11520 |
| `LWIP_TCP_RECVMBOX_SIZE` | 6 | 8 |
| `LWIP_TCP_OOSEQ_MAX_PBUFS` | 4 | 8 |
| `ESP_WIFI_RX_BA_WIN` | 6 | 16 |

## 2. 下行：播放从空环开始

服务端并不是匀速 50 帧/秒。用对照客户端量到的到达间隔：p50 0 ms、p95 67–74 ms、
p99 230–299 ms，每秒约 0.6 次超过 100 ms 的空隙。而播放以前是收到第一帧就开始，
所以环形缓冲的占用取决于上一个突发留下多少——高水位只有 5.8 KB / 16 KB——每个空隙
都会让 I2S DMA 断粮，`auto_clear_after_cb` 就往里填静音。实测每秒 3–5 次环空唤醒。

修法：攒够 160 ms 再开始播放（`AUD_PREBUF_BYTES` 5120），播放中若环连续干涸
200 ms（`AUD_REBUFFER_US`）就重新攒。欠载降到 0–2 次/秒，重新缓冲次数不再增长。

坑：第一版在优先级 8 上忙等，直接触发 task watchdog。那个 `vTaskDelay` 是必需的。

## 3. 服务端：反压消失了

控制设备的接收队列之后，服务端每条连接统计里的 `max pump stall` 从 **5791 ms**
降到 **3 ms**。这个数字是"设备跟不跟得上"最直接的指标——它量的是服务端向终端
写入被阻塞了多久。

## 4. 上行：Nagle 加四 MSS 的发送缓冲

第一次实测，6.5 秒发射：`frames 323`、`sendfail 0`、`readfail 0`、
`loop_max 260 µs`、`stalls 0`——采集一帧没丢、循环从不阻塞——但
**323 次发送里有 100 次（31%）阻塞超过 30 ms，最坏 89 ms**。

原因：`tcp_transport` 和 `esp_websocket_client` 都不调用 `setsockopt`，所以这条
socket 一直开着 Nagle。每 20 ms 一个 641 字节的小包会等上一个未被确认的段，而服务端
的延迟 ACK 又叠加几十毫秒。客户端不暴露 transport 句柄，因此
`tools/patch_ws_nodelay.sh` 在状态机同时知道 socket 与连接状态的那个位置设置
`TCP_NODELAY`；`tools/build_on_mac.sh` 会连同其它 managed component 补丁一起应用。

| 配置 | >30 ms 的发送 | 最坏一次 |
|---|---|---|
| Nagle 开，SND_BUF 5760 | 100 / 323 = 31% | 89 ms |
| + `TCP_NODELAY` | 28 / 412 = 6.8% | 84 ms |
| + SND_BUF 11520 | 0–3 / 389–497 | 9–17 ms |

服务端侧印证：每次都是 `frames == fed == written`、`queue_drops=0`、`write_err=0`。

## 5. 上行：削波，以及它到底发生在哪

服务端报告 `peak=100%`、5% 的发射样本被削波：

```
audio levels: TX gain +12 dB is clipping the signal (57/320 samples ...)
```

两级增益叠加：设备 ES8311 模拟 PGA **+30 dB**，服务端软件 `tx_mic_db` **+12 dB**。
把服务端降到 +3 dB **并没有**解决问题（削波率 5.0% → 5.8%）——这个"无效"很有价值：
说明峰值在服务端加增益之前就已接近满量程。设备采集峰值 27522/32768 = 84%，
而 84% × 1.41（+3 dB）= 118%。

把 `tx_mic_db` 设成 **0**（`audio_levels.py` 里写明的恒等值）后：`peak=40%`，
整次发射**新增削波 0 个样本**。

以后的准则：要音量应该加**模拟 PGA**（在 ADC 之前，真正改善信噪比），服务端的软件
增益只是把已经量化好的样本重新缩放。如果换了更灵敏的麦克风或贴得更近，让设备峰值
超过约 90%，就把 MIC 档位降到 24 dB，而不是用软件补。
