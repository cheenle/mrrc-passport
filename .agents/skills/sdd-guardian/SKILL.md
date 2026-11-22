---
name: sdd-guardian
description: Executable engineering constraints for the MRRC terminal firmware — the incidents this ESP32-C3 image already survived, expressed as block/warn/info rules with fixtures, plus a live-routed index into docs/ and a pre-tool hook
type: prompt
whenToUse: When creating, modifying, reviewing or debugging code in this repository; when a change touches the audio path, the single websocket, PTT/microphone, LVGL/waterfall, memory budget, provisioning or credentials, the build and release tooling, or the documentation
arguments: [task]
---
<p align="right"><strong>English</strong> · <a href="SKILL.zh_CN.md">简体中文</a></p>

# SDD guardian — firmware constraints, enforced

`AGENTS.md` §1 lists what must not change. `docs/optimization-notes.md` and
`docs/transport-tuning.md` record what each violation cost. This skill is the
loop that keeps a change inside those lines and keeps the record in sync, using
`.agents/skills/sdd-guardian/harness/`:

- `constraints.json` — the rules (scope, detector, severity, incident reference,
  two fixtures each);
- `index.json` — routing from files/keywords to live doc sections (refs only,
  never copies);
- `sdd_context.py` — the CLI (`prime`, `brief`, `doc`, `check`, `hook`,
  `selftest`).

`${ARGUMENTS:+Task focus: $ARGUMENTS}`

## Phase 0 — Brief, always

```bash
H=.agents/skills/sdd-guardian/harness/sdd_context.py
python3 $H brief <the files you will touch>
python3 $H brief --task "<one-line description>"
```

Read what comes back. It is short on purpose: the constraints that apply, and
the sections of the post-mortems that describe what already went wrong in this
exact area. `brief` for `main/mrrc_net.c` is not the same document as `brief` for
`main/mrrc_ui.c`, and neither is the same as `brief` for `main/Kconfig.projbuild`.

Need one item? `python3 $H doc uplink-send-timeout` ·
`python3 $H doc doc:docs/transport-tuning.md#6.\ Transmit` · `python3 $H doc ptt`

## Phase 1 — Design check

Before implementing, answer three questions in the change description:

1. **Which constraint does this touch?** If it touches a `block` rule, the
   change is not a change — it is a redesign, and the rule's `sdd_ref` explains
   the measurement that produced it. Contradicting it requires amending the doc
   *and* the rule in the same pull request, with a measurement.
2. **What does it cost?** This chip has one core, no PSRAM, and `free=49–58K,
   largest=36–48K`. A feature that adds a task, a buffer, or a per-frame
   allocation is a budget decision, not an implementation detail. The number to
   protect is `largest`, not `free`.
3. **What is the failure symptom?** Every entry in the docs started as a symptom
   that pointed at the wrong subsystem: "the PTT releases itself" was a congested
   uplink; "the waterfall is black" was an init order. If you cannot say what
   the operator will see when this is wrong, you cannot debug it later.

## Phase 2 — Implement inside the rules

The `block` rules, in one line each (the registry carries the full reason):

- credentials (`CONFIG_MRRC_*PASSWORD`) live only in the gitignored
  `sdkconfig.defaults.local` — never in a tracked file;
- `heat_init()` runs before the first palette consumer in `mrrc_ui.c`;
- the uplink frame timeout in `mrrc_net.c` stays at or below 200 ms (it is 100);
- one socket: `/WSterminal`, never `/WSaudioRX`-style endpoints;
- PCM16 only — no `opus` symbol anywhere in `main/`;
- one NVS namespace (`"mrrc"`), one owner module per key;
- `CONFIG_MRRC_PROFILE` stays `n` in shipped defaults.

`warn` rules you may deliberately cross, but say so out loud in the commit:
LVGL only from `taskLVGL` (`mrrc_ui.c`), no heap allocation in the audio/uplink
path, task stacks from named macros, release never blocks.

Style that the rules cannot check but review will: measure before you claim
(`docs/optimization-notes.md` is a table of measurements, not of intentions),
one change per commit, and the same commit updates the doc that would have told
you not to do it.

## Phase 3 — Close the loop

```bash
python3 $H check --staged        # exit 2 means a block rule fired
python3 $H selftest              # the guardian's own tests
./tools/validate.sh --static     # includes the guardian, plus repo/glyph/actionlint gates
```

Then the documentation half, which is part of the same change:

- a performance or transport number → `docs/optimization-notes.md`
  (and `docs/transport-tuning.md` if it is on the audio path);
- a setting, a key, or provisioning behaviour → `docs/provisioning.md`;
- layout, palette, fonts → `docs/UI.md`;
- user-visible behaviour → `CHANGELOG.md` **and** `CHANGELOG.zh_CN.md`;
- a new hard constraint → `AGENTS.md` §1 **and** a new rule with fixtures.

If the change makes a rule obsolete, delete the rule in the same pull request.
A registry nobody prunes teaches people to bypass it.

## When a rule fires and you disagree

That is the interesting case, so handle it as evidence, not as an obstacle:

1. read the rule's `sdd_ref` — it names the incident and the measurement;
2. if the measurement no longer holds (different hardware, different protocol),
   say what changed and produce the new number;
3. update the doc, the rule (or the `max`/pattern), and its fixtures together;
4. never silence a rule with `--quiet`, a scope exclusion for your own file, or
   a widened pattern. Those are the three ways a gate becomes decoration.
