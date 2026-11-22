<p align="right">
  <a href="SKILL.md">English</a> · <strong>简体中文</strong>
</p>

---
name: sdd-guardian
description: MRRC 终端固件的可执行工程约束 —— 这颗 ESP32-C3 已经挺过来的事故被写成带夹具的 block/warn/info 规则，外加一个实时路由到 docs/ 的索引和一个 pre-tool 钩子
type: prompt
whenToUse: 在本仓库新建、修改、评审或调试代码时；改动涉及音频路径、单条 websocket、PTT/麦克风、LVGL/瀑布、内存预算、配网或凭证、构建发布工具链、或文档时
arguments: [task]
---

# SDD guardian —— 固件约束，可执行

`AGENTS.md` §1 列出不可改动的东西，`docs/optimization-notes.md` 与
`docs/transport-tuning.md` 记录每次违反它的代价。这个 skill 就是让改动留在这些线内、
并让记录同步的循环，用 `.agents/skills/sdd-guardian/harness/` 里的三样东西：

- `constraints.json` —— 规则（范围、检测器、级别、事故依据，每条两个夹具）；
- `index.json` —— 从文件/关键词路由到**实时**文档段落（只有引用，没有副本）；
- `sdd_context.py` —— 命令行（`prime`、`brief`、`doc`、`check`、`hook`、`selftest`）。

`${ARGUMENTS:+本次任务重点：$ARGUMENTS}`

## 阶段 0 —— 永远先取简报

```bash
H=.agents/skills/sdd-guardian/harness/sdd_context.py
python3 $H brief <你将要修改的文件>
python3 $H brief --task "<一句话描述>"
```

把返回的东西读完。它刻意很短：适用于这些文件的约束，以及复盘里描述「这块地方曾经
出过什么事」的段落。`main/mrrc_net.c` 的简报、`main/mrrc_ui.c` 的简报、
`main/Kconfig.projbuild` 的简报，三者不是同一份文档。

只想要某一条？`python3 $H doc uplink-send-timeout` ·
`python3 $H doc doc:docs/transport-tuning.md#6.\ Transmit` · `python3 $H doc ptt`

## 阶段 1 —— 设计核对

动手之前，在改动说明里回答三个问题：

1. **碰到哪条约束？** 碰到 `block` 规则时，这不是一次改动，而是一次重新设计，规则的
   `sdd_ref` 会告诉你当初的测量。要违反它，必须**同一个 PR** 里同时修改文档、规则
   （或 `max`/正则）与夹具，并给出新的测量。
2. **代价是什么？** 这颗芯片单核、无 PSRAM，`free=49–58K, largest=36–48K`。
   多一个任务、一个缓冲、一次每帧分配，都是预算决策，不是实现细节。要保护的数字是
   `largest`，不是 `free`。
3. **出错时操作者会看到什么？** 文档里每一条都始于一个指向错误子系统的现象：
   「PTT 自己松开」其实是上行被挤死；「瀑布全黑」其实是初始化顺序。说不出症状，
   以后就查不出来。

## 阶段 2 —— 在规则之内实现

`block` 规则，一条一句（完整理由在注册表里）：

- 凭证（`CONFIG_MRRC_*PASSWORD`）只存在于被 gitignore 的 `sdkconfig.defaults.local`，
  绝不进受版本控制的文件；
- `mrrc_ui.c` 里 `heat_init()` 必须在第一个调色板消费者之前；
- `mrrc_net.c` 的上行单帧超时 ≤ 200 ms（现在是 100）；
- 只有一条 socket：`/WSterminal`，不再有 `/WSaudioRX` 那类端点；
- 只走 PCM16 —— `main/` 下不得出现 `opus` 符号；
- 一个 NVS namespace（`"mrrc"`），一个键只有一个主人模块；
- 出货默认里 `CONFIG_MRRC_PROFILE` 保持 `n`。

`warn` 规则可以主动跨越，但要在提交信息里说出来：LVGL 只在 `taskLVGL`
（`mrrc_ui.c`）里碰；音频/上行路径不做堆分配；任务栈用命名宏；释放路径不阻塞。

规则查不到、但评委会看的风格：先说测量再下结论（`docs/optimization-notes.md` 是测量表，
不是意愿表）、一次提交一件事、以及同一个提交里更新那篇「本该拦住你」的文档。

## 阶段 3 —— 闭环

```bash
python3 $H check --staged        # 退出 2 = 命中 block
python3 $H selftest              # guardian 自己的测试
./tools/validate.sh --static     # 含 guardian，以及 repo/glyph/actionlint 门禁
```

然后是文档那一半，属于同一个改动：

- 性能或传输数字 → `docs/optimization-notes.md`（音频路径上的还要写
  `docs/transport-tuning.md`）；
- 设置、键、配网行为 → `docs/provisioning.md`；
- 布局、调色板、字体 → `docs/UI.md`；
- 用户可见行为 → `CHANGELOG.md` **与** `CHANGELOG.zh_CN.md`；
- 新的硬约束 → `AGENTS.md` §1 **与** 一条带夹具的新规则。

如果这次改动让某条规则过时，就在同一个 PR 里删掉它。没人修剪的注册表，只会教人绕过它。

## 规则响了而你认为它不对

这才是有意思的情况，把它当证据处理，不要当障碍：

1. 读规则的 `sdd_ref` —— 它点名了那次事故和那次测量；
2. 如果测量不再成立（换了硬件、换了协议），说清变了什么，并给出新的数字；
3. 文档、规则（或 `max`/正则）、夹具，三者一起改；
4. 永远不要用 `--quiet`、为自己文件加排除、或放宽正则去消音。那是让门禁变成装饰的
   三种方式。
