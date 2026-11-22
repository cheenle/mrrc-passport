<p align="right">
  <strong>English</strong> · <a href="sdd-guardian.zh_CN.md">简体中文</a>
</p>

# SDD guardian: the repository's rules, executable

This repository has a set of constraints that are not negotiable — one socket,
PCM16 only, credentials out of the tree, `heat_init()` before the palette, a
100 ms uplink timeout. Until now they lived in `AGENTS.md` §1 and in the
post-mortems of `docs/optimization-notes.md`: prose that a reviewer may remember
and a tool cannot check.

`.agents/skills/sdd-guardian/` makes them executable. Every rule is a JSON entry
with a scope, a detector, a severity, and the incident that produced it. The same
registry drives three things: an agent brief before editing, a pre-tool hook that
can reject an edit, and a CI gate that fails a pull request.

Nothing here is a copy of the prose. `index.json` stores references
(`doc:docs/transport-tuning.md#6. Transmit`), and the harness slices the current
file at call time, so editing a doc cannot leave the harness quoting a paragraph
that no longer exists. If a heading is renamed, the guardian's own tests fail.

## 1. What a rule is

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

Three severities, and the difference is deliberate:

| Severity | Meaning | Where it shows up |
| --- | --- | --- |
| `block` | an incident, already paid for once | `check` exits 2, the hook rejects the edit, CI fails |
| `warn` | a real cost that a human may still choose | printed, never fails a build |
| `info` | orientation: read this before you touch that | only in `brief` |

Three kinds of detector cover every rule that matters here:

- `pattern` — regexes; C comments are stripped first, because "we removed opus"
  in a comment is not an opus decoder. The credentials rule opts out: a
  commented-out password in a public repo is still a password.
- `order` — pattern A must appear before pattern B in the same file. This is how
  the palette rule is expressible at all: `heat_init()` is correct only
  *before* the first consumer, and no single line is wrong.
- `numeric` — capture a number from a call and bound it. Timeouts regress by one
  digit, not by a redesign.

Every enforcing rule ships two fixtures, `selftest_sample` (must fire) and
`selftest_clean` (must not). A pattern that catches the trap and a pattern that
catches ordinary code look identical in review; only one of them is usable.

## 2. Running it

```bash
H=.agents/skills/sdd-guardian/harness/sdd_context.py

python3 $H prime                      # one screen: the block rules
python3 $H brief main/mrrc_net.c      # constraints + the doc sections that matter
python3 $H doc uplink-send-timeout    # one rule, with the incident behind it
python3 $H check                      # whole tree; exit 2 on any block
python3 $H check --staged             # only what you are about to commit
python3 $H selftest                   # the guardian's own tests
```

`check` is wired into `tools/validate.sh --static`, so it runs with the other
static gates, and into `.github/workflows/firmware-checks.yml`, so a pull
request that trips a block rule cannot merge green.

The pre-tool hook (`check` via `hook`) is optional and client-specific: the
command reads a `PreToolUse` event on stdin, scans the pending Write/Edit
content, and exits 2 with the rule's message. Nothing in CI depends on it — it
exists so an agent gets the reason while it is still writing the line.

## 3. Adding a rule

A rule is worth adding when all four of these are true:

1. **It already happened.** If nobody has paid for it, it is a preference, and
   preferences belong in review — not in a gate that blocks other people.
2. **A machine can judge it.** "Don't over-optimise" is not a rule. "No heap
   allocation in `mrrc_audio.c`" is.
3. **The scope is narrow.** `main/**` for a rule about one module is how a
   registry becomes noise, and noise is how people learn to pass `--quiet`.
4. **Both fixtures exist.** Write the clean counter-example first: if you cannot
   think of ordinary code the pattern would also flag, the pattern is wrong.

Then:

- add the entry to `constraints.json` with `sdd_ref` pointing at the document
  that explains the cost (an incident log, not a style guide);
- run `python3 $H selftest` — it checks id uniqueness, regex compilation, that
  the scope matches at least one tracked file, and both fixtures;
- if the rule is about a new area, add a topic to `index.json` so `brief` routes
  to it. The selftest fails if a ref's heading has been renamed.

## 4. The knowledge index

`index.json` maps file globs and keywords to doc sections. `brief` uses it to
answer "what should I read before touching `main/mrrc_audio.c`" without a human
deciding, and because the refs are resolved live, the answer cannot be stale.

The `always` list is what every task gets: `AGENTS.md` §1 and §6, and the four
hard constraints from `docs/design-v1.md` §3. Those are the rules that apply
even to a one-line change.

## 5. What it does not do

It does not check that the firmware builds, that the audio is not clipped, or
that a transmission survives thirty seconds — those are measurements, and
`tools/validate.sh --firmware` plus an actual radio are the only things that can
answer them. It does not replace reading the post-mortem. It does one thing:
it makes the mistakes this project already made once impossible to make again
silently.
