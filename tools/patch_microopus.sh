#!/usr/bin/env bash
# patch_microopus.sh — keep micro-opus usable on this board.
#
# Two things must hold before libopus can decode on the ESP32-C3:
#
# 1. The pseudostack must be smaller than 60 KB.  Upstream's Kconfig floor is
#    60000, which does not fit: two TLS sockets plus an Opus decoder leave this
#    board with ~52 KB free and a ~29 KB largest block.  The host measurement
#    (see docs/design-v1.md) is that decoding needs 8 KB, so 16 KB is a safe
#    value — but the Kconfig `range` has to be widened first.
#
# 2. Only one task may enter libopus.  The thread-safe mode relies on
#    `_Thread_local`, which Xtensa/RISC-V GCC does not emit a .tbss for, so the
#    "per-thread" pointers become plain globals and a NULL scratch pointer ends
#    up as a store fault (MTVAL=0) inside celt_decoder.c:tf_decode.
#
# Idempotent; safe to run before every build.  Requires managed_components to
# already exist (run a build once first).
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
KC="$HERE/managed_components/esphome__micro-opus/Kconfig"
SDK="$HERE/sdkconfig"

[ -f "$KC" ] || { echo "micro-opus not fetched yet; run a build first"; exit 0; }

python3 - "$KC" "$SDK" <<'PY'
import sys

kc_path, sdk_path = sys.argv[1], sys.argv[2]

kc = open(kc_path, encoding="utf-8").read()
if "range 8000 240000" not in kc:
    kc = kc.replace("range 60000 240000", "range 8000 240000")
    open(kc_path, "w", encoding="utf-8").write(kc)
    print("patched Kconfig range -> 8000..240000")

try:
    s = open(sdk_path, encoding="utf-8").read()
except FileNotFoundError:
    print("no sdkconfig yet; skipping")
    raise SystemExit(0)

changed = False
def sub(old, new):
    global s, changed
    if old in s:
        s = s.replace(old, new)
        changed = True

sub("CONFIG_OPUS_THREADSAFE_PSEUDOSTACK=y", "# CONFIG_OPUS_THREADSAFE_PSEUDOSTACK is not set")
if "CONFIG_OPUS_NONTHREADSAFE_PSEUDOSTACK=y" not in s:
    sub("# CONFIG_OPUS_NONTHREADSAFE_PSEUDOSTACK is not set",
        "CONFIG_OPUS_NONTHREADSAFE_PSEUDOSTACK=y")
sub("CONFIG_OPUS_PSEUDOSTACK_PREFER_PSRAM=y", "# CONFIG_OPUS_PSEUDOSTACK_PREFER_PSRAM is not set")
if "CONFIG_OPUS_PSEUDOSTACK_INTERNAL_ONLY=y" not in s:
    sub("# CONFIG_OPUS_PSEUDOSTACK_INTERNAL_ONLY is not set",
        "CONFIG_OPUS_PSEUDOSTACK_INTERNAL_ONLY=y")

import re
s = re.sub(r"(CONFIG_OPUS_PSEUDOSTACK_SIZE=)\d+", r"\g<1>16000", s)

if changed:
    open(sdk_path, "w", encoding="utf-8").write(s)
    print("patched sdkconfig: nonthreadsafe / internal-only / 16000")
PY
