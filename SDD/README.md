<p align="right">
  <strong>English</strong> · <a href="README.zh_CN.md">简体中文</a>
</p>

# MRRC Terminal SDD — Software Design Description

> ESP32-C3 handheld terminal: receive, transmit, display and provisioning
> IBM Team Solution Design (TeamSD) aligned, with one local amendment (chapter 00)

## Purpose

This SDD is the design record for the firmware in this repository. It exists
because the same fault was found twice, months apart, by two different people,
from the same wrong hypothesis. The record's job is not to describe the code —
the code does that — but to keep three things attached to each other:

- **what the device must do**, as numbers that can be measured (`05`);
- **why it is built this way**, as decisions with their alternatives and costs (`08`);
- **what went wrong before**, as evidence that cannot be quietly forgotten (`15`, and
  the incident tables in `../docs/transport-tuning.md` and `../docs/optimization-notes.md`
  until chapter 14 exists).

Every claim here was measured on this board or on the server that feeds it. Where
a claim is not measured, the text says so and marks the chapter or row as
`proposed`. Chapter 00 makes that rule load-bearing: it is the highest-priority
chapter in this document and it governs how all the others may be written.

## Document Index

| # | Chapter | ART Code | File | Status |
| --- | --- | --- | --- | --- |
| 00 | Verification Discipline (local amendment) | — | [00-verification-discipline.md](00-verification-discipline.md) | written |
| 01 | Executive Summary | — | — | pending |
| 03 | Project Definition (success criteria) | ENG 343 | — | pending |
| 04 | System Context | APP 011 | — | pending |
| 05 | Non-Functional Requirements | ART 0507 | [05-non-functional-requirements.md](05-non-functional-requirements.md) | written |
| 06 | Use Case Model | ART 0508 | — | pending |
| 08 | Architecture Decisions | ART 0513 | [08-architecture-decisions.md](08-architecture-decisions.md) | written |
| 09 | Architecture Overview | ART 0512 | — | pending |
| 11 | Component Model | ART 0515 | — | pending |
| 12 | Operational Model | ART 0522 | — | pending |
| 13 | Feasibility Assessment | ART 0530 | — | pending |
| 14 | Version History | — | — | pending |
| 15 | Transmit Safety Architecture | ART 0535 | [15-transmit-safety-architecture.md](15-transmit-safety-architecture.md) | written |

A `pending` chapter is not a stub and must not be cited as if it existed: the
knowledge that belongs there still lives in the engineering notes listed below,
and those notes remain authoritative until the chapter is written. Writing an
empty chapter would claim coverage this document does not have — the one thing
chapter 00 §V5 forbids.

## Related Records

| Record | Authority |
| --- | --- |
| `../AGENTS.md` §1, §6 | The short form of the hard constraints and the per-change acceptance table |
| `../docs/design-v1.md` | Hardware facts and their measurements; the source for chapters 04, 09, 13 |
| `../docs/optimization-notes.md` | The measured cost ledger (CPU, memory, repaint, network) behind chapter 05 |
| `../docs/transport-tuning.md` | The transport incident log behind AD-004 and chapter 15 |
| `../docs/provisioning.md` | Provisioning behaviour behind AD-006 |
| `../docs/UI.md` | Layout, palette and font budget behind AD-005 |
| `../.agents/skills/sdd-guardian/` | Executable form of the chapter 00 rules and of `08`'s consequences |
| `mrrc_modern/SDD/` | The **server's** record. Transmit liveness, keyer arbitration and downlink suppression are that side's decisions; this document only records what the firmware relies on |

**Boundary.** This document owns the terminal: its codec, buffers, tasks,
display, provisioning, and the device half of the transmit path. It does not own
the server's session policy, the radio's CAT behaviour, or the network between
them. Where a firmware decision depends on a server behaviour, the dependency is
named in `08` and the server is cited as the owner.

## Quick Facts

| Attribute | Value |
| --- | --- |
| Document ID | SDD-MRRC-PASSPORT-2026-001 |
| SDD Version | V0.1 (first batch: chapters 00, 05, 08, 15) |
| Baseline Date | 2026-11-23 |
| Hardware | FoloToy AI Passport: ESP32-C3 rev v1.1, 160 MHz single core, 400 KB SRAM, no PSRAM, 8 MB flash, 240×320 ST7789, ES8311 codec |
| Verified unit | MAC `98:c3:77:f5:4b:2c`, `/dev/ttyACM0` |
| Toolchain | ESP-IDF v5.5.3 |
| Server | `w103d.vlsc.net:8888` (IPv6 only), single `/WSterminal` session |
| Audio | PCM16 mono 16 kHz, 20 ms frames, 641 B per uplink frame including tag |
| Transports | `/WSterminal` only (control text, `0x00` RX, `0x01` TX, `0x02` spectrum) |
| Steady state (measured) | `free` 57–59 KB, `largest` 38–48 KB, `taskLVGL` 28–30%, `IDLE` 42–45%, `i2s` 50.0 frames/s, `drop=0` |
| Chapters written | 00, 05, 08, 15 |
| Chapters pending | 01, 03, 04, 06, 09, 11, 12, 13, 14 |

## Reading order

For a change to the audio or transmit path: `00` → `15` → `08` AD-002/AD-003/AD-004
→ `05` §5.1/§5.3. For a change to display or memory: `00` → `05` §5.4 → `08`
AD-001/AD-005 → `docs/UI.md`. For anything else: `00` first, always; it is
shorter than the mistake it prevents.
