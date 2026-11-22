<p align="right">
  <strong>English</strong> · <a href="00-verification-discipline.zh_CN.md">简体中文</a>
</p>

# 00. Verification Discipline (local amendment)

> **Status: normative.** This chapter has no ART code because TeamSD does not
> define it: it is this project's amendment. It outranks every other chapter.
> A statement elsewhere in this document that cannot satisfy §V1 is not a design
> statement — it is a wish, and it gets marked `proposed`.

## 0.1 Why this chapter exists

Every expensive fault in this repository had the same shape: **a number that was
true under conditions nobody wrote down.** Three examples, all real:

| What was believed | What was measured | What the difference cost |
| --- | --- | --- |
| "23 000 was the peak, the codec is fine" | 48 kHz capture peaks at **468/32767**; 16 kHz at 12 708 | three days of chasing a "quiet microphone" in software |
| "the device stops receiving at 28 KB/s, so the link is the ceiling" | a reference client on the same AP gets **50.2 KB/s** | two rounds of buffer enlargement that changed nothing |
| "the PTT is releasing itself, so it is a key problem" | the key was held **16.5 s** with no release event; `sendfail=71/78` | a full redesign of the key handling, before the socket was looked at |

In each case the fix was cheap and the *finding* was expensive. This chapter
exists so the finding survives the person who made it.

## 0.2 The rules

Each rule is stated as an obligation on whoever writes a claim into this SDD or
into a commit message.

| ID | Rule | Fails when |
| --- | --- | --- |
| **V1** | Every quantitative statement names its **measurement point** (the counter, log line or command that produced it), its **date**, and the **scenario** (link type, session length, what was running). | A number appears with none of the three. |
| **V2** | A measurement must discriminate between at least two explanations. If it cannot, it is an observation, not evidence. | "It works now" with nothing that would have differed had it not. |
| **V3** | **A negative result is a result** and is recorded with the same care. | A hypothesis is deleted rather than falsified in writing. |
| **V4** | Corrections are **in place and dated**. An AD is amended (`⑧ …`), a row is rewritten with "was → now", a claim is downgraded. Deleting a wrong claim requires writing why it was wrong. | A wrong statement silently disappears into a git diff. |
| **V5** | Status is a ladder, never a free-text adjective: `proposed` → `accepted` → `implemented` → `field-exercised` → `hardware-verified`. Promotion requires the artifact for that rung. | A design document is treated as implemented because it exists. |
| **V6** | A number without conditions is a rumour. Throughput, latency and CPU claims carry their window and their traffic. | `50 frames/s` written without saying for how long and with what else running. |
| **V7** | Changing the instrument invalidates its history. Re-measure before comparing across a change to the counters themselves. | Old and new numbers are put in one table with no note. |
| **V8** | When measurement and theory disagree, the measurement wins; when two measurements disagree, **find the differing condition first**. | One of the two is discarded as "wrong" without locating the difference. |
| **V9** | A claim that can no longer be re-measured (instrument removed, unit unavailable) is marked `historical`, never kept as current. | A 2026-11 number is cited as the state of the board today. |

## 0.3 The three instruments

No single instrument on this device can distinguish "the link is bad" from "the
device is slow" — that is exactly the ambiguity that produced the first two
examples in §0.1. Every transport claim therefore uses at least two of:

| # | Instrument | What it can prove | Where |
| --- | --- | --- | --- |
| 1 | **Device profile line** (`CONFIG_MRRC_PROFILE=y`) — `link: rx=… underruns=… wr_max=… rebuf=… rssi=…`, `ws: total=…`, `txlink: send_max=… slow=… stalls=…` | what the device actually did: ring occupancy, write blocking, drops | `main/mrrc_audio.c`, `main/mrrc_net.c`; off in shipped defaults (guardian rule `diagnostics-not-shipped`) |
| 2 | **Server counters** — `TX session: frames=… fed=… written=… queue_drops=… peak=…`, `Terminal media losses: … max pump stall …` | what the server sent and how long its write blocked | server-side; owner is `mrrc_modern/SDD/` |
| 3 | **Reference client** — a second `/WSterminal` client on the same access point | what the network could carry at that moment | manual, recorded in the transport log |

Rule of thumb that came out of the transport round: **if the reference client is
fine and the device is not, stop looking at the network.**

## 0.4 The repeated-verification loop

Long sessions are the only instrument that catches drift, and drift is the
dominant failure mode here: a clock 0.16% fast, a ring losing 2% of frames, a
buffer that grows by 40 bytes a minute. The loop below is required for anything
that touches audio, transmit, or memory:

1. **Short run** (≤1 min) — does it work at all? Cheap, catches nothing about
   stability, and must never be quoted as evidence of stability.
2. **Sustained run** (≥10 min, ideally 30+) — the tests that found the real
   faults: the 13-minute soak that showed `drops ≈ 57/min`; the 30.5-second
   transmission that showed `frames=1500 sendfail=0`. Note that the clock-drift
   fault (§`docs/design-v1.md` 3.2) is explicitly *not* provable in a short
   session.
3. **Adverse run** — same code on a mobile link (6.5–12% loss, RTT peaks 6–10 s).
   Passing here is the difference between "works in the lab" and "works".
4. **Regression of the finding** — the counter that caught the fault is now part
   of the acceptance line, so the fault cannot return silently. This is why the
   guardian's rules are mostly counters and bounds rather than prose.

## 0.5 What a correction looks like

Three forms, all of which keep the wrong thing visible:

- **AD amendment** — new facts appended as `⑧`, `⑨` under the existing AD, with
  the date and the measurement that changed the decision. The AD number never
  changes; the decision's *history* is what the amendment records.
- **Row rewrite with "was → now"** — as in `08` AD-005's memory table. The old
  value stays in the same cell so the cost of the decision remains legible.
- **Downgrade** — when evidence is withdrawn, the status moves down the §V5
  ladder and the text says which branch of the tree lost its support.

## 0.6 Enforcement

These rules are prose, so they are enforced where prose can be:

- the **guardian** (`../.agents/skills/sdd-guardian/`) carries the mechanically
  checkable consequences — a timeout bound, a palette ordering, a single socket —
  and its `selftest` fails when a rule, its fixtures, or a citation goes stale;
- the **SDD structural tests** in that same `selftest` refuse a chapter that
  breaks the form this document requires: an NFR row without a verification
  method, an AD without `Problem`/`Rationale`/`Consequences`, a version history
  that is not newest-first, a citation of a chapter marked `pending`;
- everything else is review. The reviewer's question is the one this chapter was
  written from: **"under what conditions was that number true, and what would
  have looked different if you were wrong?"**
