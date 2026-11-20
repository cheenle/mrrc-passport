<p align="right">
  <a href="README.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Moved → `~/HAM/mrrc-touch`

This directory holds the original board bring-up project (M1–M3 were verified
here). On 2026-11-19 the code moved to its own repository, **`mrrc-touch`**
(`git@…:mrrc-touch`, first commit `3b56b5d`).

It is kept for two reasons:

1. The verification order and commit messages (`b37ba10` → `6343950` → `5adc398`
   → `c1d52e4` → `d5a8828`) are themselves the evidence trail: each commit is one
   real-board failure and the fix for it.
2. As a reference: the S3 board facts were turned into constraint entries in
   `mrrc-touch/AGENTS.md`.

**Do not add features here.** New development happens in `mrrc-touch`.
