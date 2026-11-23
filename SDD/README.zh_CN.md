<p align="right">
  <a href="README.md">English</a> · <strong>简体中文</strong>
</p>

# MRRC 终端 SDD —— 软件设计说明

> ESP32-C3 手持终端：接收、发射、显示与配网
> 对齐 IBM Team Solution Design (TeamSD)，含一处本地增补（第 00 章）

## 目的

这份 SDD 是本仓固件的设计记录。它存在的理由很具体：同一个故障，隔了几个月，被两个
不同的人，从同一个错误假设出发，各自查了一遍。记录的职责不是描述代码——代码会自己
描述自己——而是把三样东西绑在一起：

- **设备必须做到什么**，写成可测量的数字（`05`）；
- **为什么这样做**，写成带备选方案与代价的决策（`08`）；
- **以前出过什么事**，写成不会被悄悄忘掉的证据（`15`，以及在第十四章写出来之前，
  `../docs/transport-tuning.md` 与 `../docs/optimization-notes.md` 里的事故表）。

这里每一条结论都在这块板子或喂它的那台服务器上量过。没量过的，正文会写出来，并把
该章或该行标成 `proposed`。第 00 章让这条规矩承重：它是本文档里优先级最高的一章，
规定其余各章**怎么写法**。

## 文档索引

| # | 章节 | ART 码 | 文件 | 状态 |
| --- | --- | --- | --- | --- |
| 00 | 校验纪律（本地增补） | — | [00-verification-discipline.md](00-verification-discipline.md) | 已写 |
| 01 | 执行摘要 | — | [01-executive-summary.md](01-executive-summary.md) | 已写 |
| 03 | 项目定义（成功判据） | ENG 343 | [03-project-definition.md](03-project-definition.md) | 已写 |
| 04 | 系统上下文 | APP 011 | [04-system-context.md](04-system-context.md) | 已写 |
| 05 | Non-Functional Requirements | ART 0507 | [05-non-functional-requirements.md](05-non-functional-requirements.md) | 已写 |
| 06 | 用例模型 | ART 0508 | [06-use-case-model.md](06-use-case-model.md) | 已写 |
| 08 | Architecture Decisions | ART 0513 | [08-architecture-decisions.md](08-architecture-decisions.md) | 已写 |
| 09 | 架构总览 | ART 0512 | [09-architecture-overview.md](09-architecture-overview.md) | 已写 |
| 11 | 组件模型 | ART 0515 | [11-component-model.md](11-component-model.md) | 已写 |
| 12 | 运行模型 | ART 0522 | [12-operational-model.md](12-operational-model.md) | 已写 |
| 13 | 可行性评估 | ART 0530 | [13-feasibility-assessment.md](13-feasibility-assessment.md) | 已写 |
| 14 | 版本历史 | — | [14-version-history.md](14-version-history.md) | 已写 |
| 15 | Transmit Safety Architecture | ART 0535 | [15-transmit-safety-architecture.md](15-transmit-safety-architecture.md) | 已写 |

V1.0 已无待写章节。这条规则对将来任何一章仍然成立：`待写` 的章节既不是占位符，也不得被
当成"已有"来引用——那里的知识仍住在下面列出的工程笔记里，在成章之前那些笔记就是权威。
写一个空章等于宣称本文档覆盖了它其实没有的内容，而这正是第 00 章 §V5 唯一禁止的事。

## 相关记录

| 记录 | 权威范围 |
| --- | --- |
| `../AGENTS.md` §1、§6 | 硬约束的简版，以及按改动类型的验收表 |
| `../docs/design-v1.md` | 硬件事实与其测量；第 04、09、13 章的素材来源 |
| `../docs/optimization-notes.md` | 实测代价账本（CPU、内存、重绘、网络），第 05 章的依据 |
| `../docs/transport-tuning.md` | 传输事故记录，AD-004 与第 15 章的依据 |
| `../docs/provisioning.md` | AD-006 背后的配网行为 |
| `../docs/UI.md` | 布局、调色板与字体预算，AD-005 的依据 |
| `../.agents/skills/sdd-guardian/` | 第 00 章规则与 `08` 后果的可执行形式 |
| `mrrc_modern/SDD/` | **服务器**的记录。发射活性、按键仲裁、下行抑制属于那一侧；本文只记录固件依赖了什么 |

**边界。** 本文档拥有终端：它的编解码器、缓冲、任务、显示、配网，以及发射路径的设备端。
它不拥有服务器的会话策略、电台的 CAT 行为、以及两者之间的网络。凡是固件决策依赖服务器
行为的，依赖在 `08` 里点名，并把服务器写成所有者。

## 速览事实

| 属性 | 值 |
| --- | --- |
| 文档 ID | SDD-MRRC-PASSPORT-2026-001 |
| SDD 版本 | V1.1（13 章；2026-11-24 那一轮测量已并入） |
| 基线日期 | 2026-11-23 |
| 硬件 | FoloToy AI Passport：ESP32-C3 rev v1.1，160 MHz 单核，400 KB SRAM，无 PSRAM，8 MB flash，240×320 ST7789，ES8311 编解码器 |
| 验证单元 | MAC `98:c3:77:f5:4b:2c`，`/dev/ttyACM0` |
| 工具链 | ESP-IDF v5.5.3 |
| 服务器 | `w103d.vlsc.net:8888`（仅 IPv6），单条 `/WSterminal` 会话 |
| 音频 | PCM16 单声道 16 kHz，20 ms 一帧，上行单帧含 tag 共 641 B |
| 传输 | 仅 `/WSterminal`（文本控制、`0x00` RX、`0x01` TX、`0x02` 频谱） |
| 稳态实测 | `free` 57–59 KB，`largest` 38–48 KB，`taskLVGL` 28–30%，`IDLE` 42–45%，`i2s` 50.0 帧/秒，`drop=0` |
| 已成章 | 00、01、03、04、05、06、08、09、11、12、13、14、15 |
| 待写 | 无 |

## 阅读顺序

改音频或发射路径：`00` → `15` → `08` 的 AD-002/AD-003/AD-004 → `05` §5.1/§5.3。
改显示或内存：`00` → `05` §5.4 → `08` AD-001/AD-005 → `docs/UI.md`。
其他任何改动：先读 `00`——它比它防的那个错要短得多。
