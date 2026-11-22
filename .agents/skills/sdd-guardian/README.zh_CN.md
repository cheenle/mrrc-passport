<p align="right">
  <a href="README.md">English</a> · <strong>简体中文</strong>
</p>

# sdd-guardian —— 把固件的规矩变成能跑的检查

`AGENTS.md` §1 的仓库规矩，和 `docs/` 里的事故复盘，本来是散文。这个目录把它们变成
机器能执行的关卡：agent 改文件之前、提交之前、以及每一个 PR 上。

```
.agents/skills/sdd-guardian/
├── SKILL.md                       # agent 遵循的生命周期
├── harness/
│   ├── constraints.json           # 13 条规则，每条都带着它背后的事故
│   ├── index.json                 # 文件/关键词 -> 实时文档引用（不存副本）
│   ├── sdd_context.py             # prime | brief | doc | check | hook | selftest
│   └── selftest.py                # 33 项，测注册表与检测器本身
└── references/
    └── constraint-catalog.md      # 全部规则、级别与夹具（自动生成，中英双语）
```

## 用法

```bash
H=.agents/skills/sdd-guardian/harness/sdd_context.py

python3 $H prime                      # 一屏：所有 block 规则
python3 $H brief main/mrrc_net.c      # 约束 + 真正相关的文档
python3 $H check --staged             # 给提交上闸；命中 block 退出 2
python3 $H selftest                   # 测规则本身
```

已接进 `tools/validate.sh --static` 与 CI 的 `governance` 作业：本地跑不过、PR 也过不去。

## 为什么每条规则都带夹具

「抓住坑的正则」和「连正常代码一起抓的正则」在评审里长得一模一样。所以每条规则都带
`selftest_sample`（必须命中）与 `selftest_clean`（必须不命中），`selftest` 还会证明每条
规则的作用范围至少匹配到一个受版本控制的文件 —— 范围匹配不到任何文件的规则，等于什么都
没保护。

设计说明、规则 schema、以及怎么加一条规则：`docs/sdd-guardian.zh_CN.md`。
