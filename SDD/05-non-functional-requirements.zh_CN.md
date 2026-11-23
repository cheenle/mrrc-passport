<p align="right">
  <a href="05-non-functional-requirements.md">English</a> · <strong>简体中文</strong>
</p>

# 05. 非功能需求（ART 0507）

这里的需求都是能在这块板子上量出来的数字，每一行都带着"出现了什么现象就说明它被破坏"的
验证方法。按第 `00` 章 §V1，每个目标还要写明场景：没有窗口的吞吐数字是传闻，不是需求。

**状态词汇**（第 `00` 章 §V5）：`field-exercised` = 在真机上持续跑过并测过；
`implemented` = 只在台面上测过；`proposed` = 还没有任何测量，该行必须写明这一点。

## 5.1 实时与吞吐需求

| ID | 需求 | 目标 | 优先级 | 验证 | 状态 |
| --- | --- | --- | --- | --- | --- |
| NFR-001 | 接收播放连续性 | 13 分钟 soak 内环形空唤醒 ≤2 次/秒；re-buffer 计数不再增长 | 关键 | 每分钟的 `link: … underruns=` 加 `gov(ins/trim)` 计数。2026-11-23：环被钉在 98% 时是 0–18/s；时钟修正 + 带内控制器之后**每个采样都是 0**，101 分钟会话只修剪掉 0.84% 的帧 | field-exercised |
| NFR-002 | 接收帧率 | 持续 50.0 帧/秒（20 ms 一帧），`drop=0`，`i2s` 节拍不出现超过一帧的空档 | 关键 | idle 行的 `tx: keys=… frames=…` 与 `i2s …/s` 计数 | field-exercised |
| NFR-003 | 下行吞吐 | 健康 AP 上 ≥31.3 KB/s 音频、总流量 ≥49 KB/s——与同一 AP 的参考客户端相同 | 高 | 同一窗口的参考客户端；`ws: total=…` | field-exercised |
| NFR-004 | 发射期上行节拍 | 整个发射期间 50 帧/秒；`sendfail=0`；`readfail=0` | 关键 | `tx done: frames=… sendfail=… readfail=…`——30.5 秒实测 `frames=1500 sendfail=0` | field-exercised |
| NFR-005 | 上行写阻塞 | 健康局域网内无超过 30 ms 的写阻塞（`slow=0`）；移动链路下最慢单帧 ≤50 ms 且绝不触发 transport 超时 | 关键 | `txlink: send_max=… slow=…`；AD-004 后局域网 9–17 ms，移动链路 30.5 秒实测 27.9 ms | field-exercised |
| NFR-006 | 采集保真 | ADC 处没有样本超过满量程的 90%：语音 `peak` 20000–29000，且服务器报告的削波样本为零 | 高 | `tx done: … peak=…` 加服务器 `audio levels` 行；MIC 30 dB 时实测 27522 | implemented |
| NFR-007 | 播放起始延迟 | 缓冲到 ≤160 ms 即开始播放，环干涸后 200 ms 内重建缓冲垫 | 中 | `AUD_PREBUF_BYTES` 5120 / `AUD_REBUFFER_US`；underrun 计数 | implemented |

## 5.2 可用性与降级需求

| ID | 需求 | 目标 | 优先级 | 验证 | 状态 |
| --- | --- | --- | --- | --- | --- |
| NFR-008 | 移动链路可生存 | 在 6.5–12% 丢包、RTT 峰值 6–10 秒下，控制与 PTT 会话存活；音频帧可以丢，会话不可以 | 关键 | 2026-11-22 火车上手机热点那一轮；发射窗口内 W/E 日志行必须为 0 | field-exercised |
| NFR-009 | 重连行为 | 每台设备恰好一条 `/WSterminal` 会话；重连后处于两个角色的 socket 数为 0；每次断开最多 1 次强制 RX | 关键 | 服务器侧（`clients` 计数）与设备 `ws: total=` 计数 | field-exercised |
| NFR-010 | 网络选择 | 最多 3 组 (SSID, 密码)；开机优先上次可用网络；切换需要持续音频劣化才触发，绝不只看 RSSI | 高 | `link: rssi=…` 加帧计数；−25 dBm 却丢约 10% 包的那个 AP 就是反例 | implemented |
| NFR-011 | 开机到出声 | 上电后 0 次人工操作；1 次预缓冲（160 ms 已排队音频，墙钟 ≤30 秒）内完成关联、登录与首帧可听 | 中 | 复位后的串口时间线；服务器的 `Initial state` 行 | field-exercised |
| NFR-012 | 无基础设施也能配网 | 0 个可达网络的设备可用 1 部手机完成配网：SoftAP，不需要路由器，不依赖强制门户，全程 ≤2 分钟 | 高 | `../docs/provisioning.md` 的触发条件；保存失败路径必须可观察 | field-exercised |

## 5.3 发射安全需求

完整模型见第 `15` 章；这里是它赖以成立的可测需求。

| ID | 需求 | 目标 | 优先级 | 验证 | 状态 |
| --- | --- | --- | --- | --- | --- |
| NFR-013 | 释放即刻生效 | 任何一层的等待确认次数为 0；每次释放 1 次写入，电台服从第一次 | 关键 | 第 `15` 章 §15.3；释放路径里不得出现 `vTaskDelay`（guardian 规则 `ptt-release-never-blocks`） | implemented |
| NFR-014 | 上行单帧超时 | 每帧 400 ms（代码里是 400）；发不出去的帧被丢掉，绝不排在下一帧后面 | 关键 | guardian 规则 `uplink-send-timeout`；发射窗口内的 `send_max`。400 ms 的取值目标是**不把会话写死**：websocket 客户端把写失败当致命错误，100 ms 会把一次 105 ms 的停顿放大成一次断线（2026-11-24） | field-exercised |
| NFR-015 | 半双工纪律 | 持键 socket 只承载 1 个方向（上行）；送达它的下行帧为 0；设备不得单独依赖这一点 | 关键 | 服务器侧抑制加设备侧丢弃记账；30.5 秒实测中持键 socket 收到下行音频为零 | field-exercised |
| NFR-016 | 发射可观测 | 每次发射结束恰好 1 行、含 4 个计数器（`frames`、`sendfail`、`readfail`、`peak`）；验收看这一行，不看印象 | 高 | `tx done:` 行；guardian 规则集就是围绕同一批计数器写的 | field-exercised |
| NFR-017 | 不允许无声载波 | 无声键控次数为 0：会话、编解码器、档位这 3 个前置条件缺任何 1 个，发射都被**可见地**拒绝 | 关键 | 第 `15` 章 §15.2 前置条件表 | implemented |

## 5.4 资源需求

`largest`（最大连续空闲块）是绑定约束，因为 TLS 握手需要 16 KB 连续块；`free` 只是参考
（第 `00` 章 §V2——没有参照的计数器会说谎）。

| ID | 需求 | 目标 | 优先级 | 验证 | 状态 |
| --- | --- | --- | --- | --- | --- |
| NFR-018 | 连续内存下限 | 稳态 `largest` ≥24 KB，任何时候不低于 16 KB（握手底线） | 关键 | 每分钟 `mem: free=… largest=… min=…`；稳态实测 38–48 KB | field-exercised |
| NFR-019 | 空闲堆下限 | 稳态 `free` ≥49 KB，且 30 分钟会话内不呈下降趋势 | 高 | 同一行；AD-005 之后实测 57–59 KB | field-exercised |
| NFR-020 | UI CPU 上限 | 出货节拍（190 ms）下 `taskLVGL` 稳态 ≤30% | 高 | `CONFIG_MRRC_PROFILE=y` 的 CPU 表；实测 28–30%，2026-11-22 重绘工作之前是 56–61% | field-exercised |
| NFR-021 | 空闲 CPU 下限 | `IDLE` 稳态 ≥40%，让新功能有地方住 | 中 | 同一张表；实测 42–45% | field-exercised |
| NFR-022 | 瀑布存储 | 40 行历史的瀑布缓冲 ≤11 KB `.bss`（调色板 + 索引） | 中 | `size` 输出；10 624 B（1 024 调色板 + 9 600 索引）对比 RGB565 的 19 200 B | field-exercised |
| NFR-023 | 静态分配纪律 | 音频、发射、网络接收路径每帧堆分配次数 0；任务栈 100% 来自命名宏 | 高 | guardian 规则 `no-heap-in-audio-path`、`named-task-stacks`；`../docs/design-v1.md` 4.1 的 `.bss` 表 | field-exercised |

## 5.5 兼容性需求

| ID | 需求 | 目标 | 优先级 | 验证 | 状态 |
| --- | --- | --- | --- | --- | --- |
| NFR-024 | 协议一致性 | 单条 `/WSterminal` 会话同时承载文本控制与三条二进制通道：`0x00` RX、`0x01` TX、`0x02` 频谱；首字节是通道，其余是旧端点载荷原样 | 关键 | guardian 规则 `single-terminal-endpoint`；服务器侧 `_terminal_*` 处理函数 | field-exercised |
| NFR-025 | 音频档位一致性 | 每会话声明一次 PCM16 单声道 16 kHz；上行 20 ms 帧含 tag 共 641 B；本镜像里没有 Opus 路径 | 关键 | guardian 规则 `no-opus-in-firmware`；服务器 `audioCaps` 应答 | field-exercised |
| NFR-026 | 滤波宽度映射 | 设备发送滤波**索引**（`filter_width`），绝不发派生频率；voice 与 narrow 两张表与服务器派生值一致（13→2400 Hz、17→2700 Hz、20→3000 Hz） | 高 | 服务器日志里 `filter_width` 与派生 `filter_hz`；三点核对已记录 | field-exercised |
| NFR-027 | 编解码器速率稳定 | 编解码器跑在 16 kHz 且运行期从不重配；30 分钟会话允许 ≤0.2% 漂移且不得因此丢音频 | 关键 | `../docs/design-v1.md` 3.2（MCLK 分频 39：16.03 kHz，快 0.16%）；30 分钟以上收听 | field-exercised |

| NFR-032 | 屏幕亮度可调且能挺过换电池 | 0–100%，5% 步进，下限 5%，转一档即生效，开机恢复 | 中 | 旋钮 `BL` 档；NVS 键 `mrrc/bright`；开机行 `knob: … bright=N%` | implemented |
| NFR-033 | 空发射要被报出来，不许沉默 | 少于 10 帧（200 ms）的按键额外记录 `TX session carried almost no audio` | 高 | 服务器日志；2026-11-24 那次 `frames=0 peak=0%` 即触发 | field-exercised |

## 5.6 可运维需求

| ID | 需求 | 目标 | 优先级 | 验证 | 状态 |
| --- | --- | --- | --- | --- | --- |
| NFR-028 | 诊断按需开启 | 出货默认每小时诊断行 0 条（`CONFIG_MRRC_PROFILE=n`）；打开需要 1 次刻意的重新构建 | 高 | guardian 规则 `diagnostics-not-shipped`；`sdkconfig.defaults` | field-exercised |
| NFR-029 | 凭证受控 | 受版本控制文件里的 Wi-Fi/服务器凭证 0 个；存放处恰好 1 个（`sdkconfig.defaults.local`，已 gitignore）；发布构建里带 0 个 | 关键 | guardian 规则 `secrets-in-tracked-config`；发布工作流在没有它的情况下构建 | field-exercised |
| NFR-030 | 设置能挺过重刷 | 重刷 1 次之后，配网得到的网络与服务器设置 100% 仍然生效，且需要重新编译 0 次 | 高 | `../docs/provisioning.md`；用不同 `sdkconfig` 重启验证 | field-exercised |
| NFR-031 | 现场故障可诊断 | 1 条 USB 串口行 + 服务器日志即可区分 3 类故障（链路、设备、服务器），调试器使用次数 0 | 高 | 第 `00` 章 §0.3 仪器表；传输那一轮就是这样解出来的 | field-exercised |

## 5.7 可追溯性

| 需求 | 决策在 | 由谁执行 | 证据 |
| --- | --- | --- | --- |
| NFR-001、002、003、007 | AD-001、AD-005 | `link:`/`i2s` 计数器 | `../docs/transport-tuning.md` §1–§2 |
| NFR-004、005、015 | AD-004 | guardian `uplink-send-timeout` | `../docs/transport-tuning.md` §4、§6 |
| NFR-006 | AD-002、第 `15` 章 §15.4 | 发射行里的 `peak` 目标 | `../docs/transport-tuning.md` §5 |
| NFR-008、009、010 | AD-007 | 开机与切换策略 | `../docs/optimization-notes.md` §22–§23 |
| NFR-011、012 | AD-006 | 开机与配网路径 | `../docs/provisioning.md` |
| NFR-018–023 | AD-005 | `mem:` 行、`size` 输出 | `../docs/design-v1.md` §4、`../docs/optimization-notes.md` §15、§20 |
| NFR-013–017 | AD-004、第 `15` 章 | guardian `ptt-release-never-blocks`、`uplink-send-timeout` | `../docs/transport-tuning.md` §6、§26 |
| NFR-032 | AD-008（2026-11-24 修订） | — | `../docs/UI.md`、开机行 `knob: … bright=` |
| NFR-033 | — | 服务器 `TX session carried almost no audio` | `mrrc_modern/patches/README.md` 第 9 条 |
| NFR-024–026 | AD-001、AD-003 | guardian `single-terminal-endpoint`、`no-opus-in-firmware` | `../docs/optimization-notes.md` §2 |
| NFR-027 | AD-003 | 编解码器在构建期固定 | `../docs/design-v1.md` §3.1–3.2 |
| NFR-028–031 | AD-006、第 `00` 章 §0.3 | guardian `diagnostics-not-shipped`、`secrets-in-tracked-config`、`nvs-single-namespace` | `../docs/provisioning.md` |

在这里加一条需求，必须同时在 §5.7 加一行。没有决策、没有执行手段、没有证据的需求是愿望，
第 `00` 章 §V1 会让它进不了这张表。
