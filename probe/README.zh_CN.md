<p align="right">
  <strong>简体中文</strong> · <a href="README.md">English</a>
</p>

# 已迁移 → `~/HAM/mrrc-touch`

这个目录是最初的板级探针工程（M1–M3 都在这里验证），代码已于 2026-11-19
整体迁到独立仓库 **`mrrc-touch`**（`git@…:mrrc-touch`，首次提交 `3b56b5d`）。

保留它是为了两件事：

1. 当时的验证顺序与提交信息（`b37ba10` → `6343950` → `5adc398` → `c1d52e4` → `d5a8828`）
   本身就是证据链——每条 commit 对应一次真机故障与它的修法；
2. 对照用：S3 板级事实在 `mrrc-touch/AGENTS.md` 里被整理成了约束条目。

**不要再往这里加功能。** 新的开发都在 `mrrc-touch`。
