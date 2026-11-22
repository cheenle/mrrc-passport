<p align="right">
  <strong>English</strong> · <a href="README.zh_CN.md">简体中文</a>
</p>

# sdd-guardian — the firmware's rules, executable

The repository rules in `AGENTS.md` §1 and the post-mortems in `docs/` are prose.
This directory turns them into gates that a machine can apply: before an agent
edits a file, before a commit is made, and on every pull request.

```
.agents/skills/sdd-guardian/
├── SKILL.md                       # the lifecycle an agent follows
├── harness/
│   ├── constraints.json           # 13 rules, each with the incident behind it
│   ├── index.json                 # file/keyword -> live doc refs (no copies)
│   ├── sdd_context.py             # prime | brief | doc | check | hook | selftest
│   └── selftest.py                # 31 tests of the registry and the detectors
└── references/
    └── constraint-catalog.md      # every rule, its severities and its fixtures
```

## Use

```bash
H=.agents/skills/sdd-guardian/harness/sdd_context.py

python3 $H prime                      # the block rules, one screen
python3 $H brief main/mrrc_net.c      # constraints + the docs that matter
python3 $H check --staged             # gate the commit; exit 2 on a block
python3 $H selftest                   # test the rules themselves
```

Wired into `tools/validate.sh --static` and the `governance` CI job, so neither
a local run nor a pull request can pass while a block rule fires.

## Why rules carry fixtures

A pattern that catches the trap and a pattern that catches ordinary code look
identical in review. Each rule therefore ships `selftest_sample` (must fire) and
`selftest_clean` (must not), and `selftest` also proves every rule's scope
matches at least one tracked file — a rule whose scope matches nothing silently
protects nothing.

Design notes, the rule schema, and how to add a rule live in
`docs/sdd-guardian.md`; the Chinese edition is `docs/sdd-guardian.zh_CN.md`.
