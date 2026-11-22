<p align="right">
  <a href="sdd-guardian.md">English</a> · <strong>简体中文</strong>
</p>

# SDD guardian：把仓库的规矩变成能跑的检查

这个仓库有一组不可谈判的约束——单 socket、只走 PCM16、凭证不进版本库、调色板之前必须
`heat_init()`、上行超时 100 ms。到目前为止它们住在 `AGENTS.md` §1 和
`docs/optimization-notes.md` 的复盘里：评审者可能记得、工具无法检查的散文。

`.agents/skills/sdd-guardian/` 把它们变成可执行的。每条规则是一段 JSON：作用范围、
检测器、严重级别，以及**产生它的那次事故**。同一份注册表驱动三处：动手前的 agent
简报、能拒绝改动的 pre-tool 钩子、以及会红掉 PR 的 CI 门禁。

这里没有一处是散文的副本。`index.json` 只存引用
（`doc:docs/transport-tuning.md#6. Transmit`），调用时才从**当前文件**切出那一段，
所以改文档不会让 guardian 引用一段已经不存在的文字；标题被改名时，guardian 自己的
测试会先红。

## 1. 规则是什么

```json
{
  "id": "uplink-send-timeout",
  "title": "The uplink frame timeout stays at or below 200 ms",
  "severity": "block",
  "kind": "numeric",
  "sdd_ref": "docs/transport-tuning.md §6",
  "scope": ["main/mrrc_net.c", "main/mrrc_net.h"],
  "call_pattern": "send_bin\\s*\\([^;]{0,120}?,\\s*([0-9]+)\\s*\\)",
  "max": 200,
  "message": "Frames are 20 ms apart. A 500 ms wait ...",
  "selftest_sample": {"path": "main/mrrc_net.c", "content": "send_bin(h, frame, len, 500);"},
  "selftest_clean":  {"path": "main/mrrc_net.c", "content": "send_bin(h, frame, len, 100);"}
}
```

三种严重级别，区别是刻意的：

| 级别 | 含义 | 出现在哪 |
| --- | --- | --- |
| `block` | 已经付过一次代价的事故 | `check` 退出码 2、钩子拒绝、CI 红 |
| `warn` | 有真实代价，但仍可人工权衡 | 只打印，不让构建失败 |
| `info` | 导航：动这块之前先读那篇 | 只出现在 `brief` |

三种检测器覆盖了这里所有值得管的规则：

- `pattern` —— 正则；先剥掉 C 注释，因为注释里写「我们移除了 opus」并不等于有 opus
  解码器。凭证规则反过来显式关闭剥离：公开仓库里被注释掉的密码仍然是密码。
- `order` —— 同一文件里模式 A 必须出现在模式 B 之前。调色板规则只有这样才表达得出：
  `heat_init()` 只有放在第一个消费者**之前**才对，没有任何单独一行是错的。
- `numeric` —— 从调用里抓一个数字并给它上限。超时是用一位数字回归的，不是重新设计。

每条生效的规则都带两个夹具：`selftest_sample`（必须命中）与 `selftest_clean`
（必须不命中）。评审时「抓住坑的规则」和「连正常代码一起抓的规则」长得一模一样，
只有一个是能用的。

## 2. 怎么跑

```bash
H=.agents/skills/sdd-guardian/harness/sdd_context.py

python3 $H prime                      # 一屏：所有 block 规则
python3 $H brief main/mrrc_net.c      # 约束 + 真正相关的那几段文档
python3 $H doc uplink-send-timeout    # 一条规则，连同它背后的事故
python3 $H check                      # 全树；任何 block 命中即退出 2
python3 $H check --staged             # 只看即将提交的内容
python3 $H selftest                   # guardian 自己的测试
```

`check` 接进了 `tools/validate.sh --static`（和其它静态门禁一起跑），也接进了
`.github/workflows/firmware-checks.yml`（踩了 block 规则的 PR 不能变绿合并）。

pre-tool 钩子（`hook` 子命令）是可选的、依客户端而定：从 stdin 读一条
`PreToolUse` 事件，扫描待写入/待编辑的内容，命中 block 就以退出码 2 连同规则说明返回。
CI 不依赖它——它存在的意义是让 agent 在**还在写这一行的时候**就看到原因。

## 3. 怎么加一条规则

四条都成立才值得加：

1. **真发生过。** 没有人为它付过代价的，叫偏好；偏好属于评审意见，不属于拦住别人的门禁。
2. **机器能判定。** 「别过度优化」不是规则；「`mrrc_audio.c` 里不许堆分配」是。
3. **范围要窄。** 一条关于某个模块的规则写成 `main/**`，就是注册表变成噪音的开始，
   而噪音正是人们学会加 `--quiet` 的原因。
4. **两个夹具都得有。** 先写那个干净的反例：如果你想不出这条正则会误伤什么正常代码，
   那就是正则写错了。

然后：

- 往 `constraints.json` 加条目，`sdd_ref` 指向解释代价的那篇文档（事故记录，不是风格指南）；
- 跑 `python3 $H selftest`——它检查 id 唯一、正则可编译、作用范围能匹配到至少一个受版本
  控制的文件、以及两个夹具；
- 如果这是新领域，在 `index.json` 加一个 topic 让 `brief` 能路由过去。标题被改名会让
  自测失败。

## 4. 知识索引

`index.json` 把文件 glob 与关键词映射到文档段落。`brief` 用它回答「改
`main/mrrc_audio.c` 之前该读什么」，不需要有人临场决定；而引用是实时解析的，答案不会过期。

`always` 列表是每个任务都会拿到的：`AGENTS.md` §1、§6，以及 `docs/design-v1.md` §3 的
四条硬约束。哪怕只改一行，这几条也成立。

## 5. 它不做什么

它不检查固件能不能编译、音频有没有削波、一次发射能不能撑过三十秒——那些是测量，
只有 `tools/validate.sh --firmware` 和一台真电台能回答。它也不替代读复盘。
它只做一件事：让这个项目**已经犯过一次**的错误，没法再悄悄犯第二次。
