<p align="right">
  <strong>English</strong> · <a href="01-executive-summary.zh_CN.md">简体中文</a>
</p>

# 01. Executive Summary

The MRRC Terminal is a handheld operating position for a remote amateur radio: an
ESP32-C3 in a FoloToy AI Passport case that carries one TLS session to an
`mrrc_modern` server, plays the receiver's audio, shows a truthful waterfall, and
transmits when the operator holds PTT.

It exists because a browser on a phone is not an operating position. A phone locks
its screen, cannot hold a key comfortably, and cannot be operated with gloves. This
device does one thing: it behaves like the front panel of a radio that happens to be
somewhere else.

## What makes it non-obvious

Four decisions shaped every other one, and each came from a measurement that
contradicted the intuition:

- **one socket, not four** (AD-001) — four TLS sessions do not fit in this chip's
  contiguous memory, and rebuilding a TX socket at key-up loses the first syllable;
- **PCM16, not Opus** (AD-002) — the decoder cost ~24 KB and ~60% of the core;
- **16 kHz forever** (AD-003) — at 48 kHz the codec captures peaks of 468/32767, so
  the "quiet microphone" was the sample rate;
- **the key was never the fault** (AD-004) — "PTT releases itself" was a congested
  uplink: the server was sending 30 KB/s down while the device sent 32 KB/s up.

## Who it is for

An operator who is away from the shack, holding one hand-held object, wanting to
listen, occasionally transmit, and see where the band is. Not for contesting, not
for FT8, not for remote desktop.

## What "done" looks like

Ten success criteria in chapter `03` §3.4, each with the instrument that would show
it broken: continuous audio for a sustained session, a transmission that survives a
mobile link, `largest` never below the TLS handshake floor, a device that can be
re-provisioned without a computer, and a field failure diagnosable from one serial
line.

## Boundaries

The server owns the radio, the session policy, the keyer arbitration and the audio
levels it sends. This firmware owns the codec, buffers, tasks, display, provisioning
and the device half of the transmit path. The radio's TOT is the last guaranteed
stop; nothing in the firmware can replace it.

## Record status

Chapters 00–15 are written; everything here is measured on the unit named in
`README.md`, and every claim carries its instrument, date and scenario (chapter
`00` §V1). Where a number is bench-only, the row says `implemented` rather than
`field-exercised`. Nothing is stubbed: a chapter that is not written is marked
`pending` and cannot be cited.
