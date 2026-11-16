#!/usr/bin/env bash
# flash_probe.sh — back up the factory image (once), flash the S0 probe, dump the log.
#
#   ./tools/flash_probe.sh              # backup if needed, flash, read 25 s
#   ./tools/flash_probe.sh --no-backup  # skip the 8 MB read
#   ./tools/flash_probe.sh --log-only   # don't flash, just listen
#
# The board is an ESP32-C3 with a native USB-Serial-JTAG console, so the port
# disappears and reappears across a hard reset; every step waits for it.
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PORT="${MRRC_PORT:-$( (ls /dev/cu.usbmodem* /dev/ttyACM* 2>/dev/null || true) | head -1 )}"
BACKUP="${MRRC_BACKUP:-$HOME/aipassport-factory-8MB.bin}"
BACKUP_ADDR=0x800000

esptool() {
    if [ -n "${ESTOOL:-}" ] && [ -x "$ESTOOL" ]; then "$ESTOOL" "$@"
    elif command -v uvx >/dev/null 2>&1; then
        http_proxy="${MRRC_HTTP_PROXY:-${http_proxy:-}}" \
        https_proxy="${MRRC_HTTP_PROXY:-${https_proxy:-}}" \
        uvx --from 'esptool==4.8.1' esptool.py "$@"
    else
        echo "ERROR: esptool not found (set ESTOOL=...)" >&2; return 1
    fi
}

wait_port() {
    for _ in $(seq 1 40); do
        PORT="${MRRC_PORT:-$( (ls /dev/cu.usbmodem* /dev/ttyACM* 2>/dev/null || true) | head -1 )}"
        [ -n "$PORT" ] && [ -e "$PORT" ] && return 0
        sleep 0.5
    done
    echo "ERROR: no serial port found — is the board plugged in with a data cable?" >&2
    return 1
}

# Read for N seconds and print everything that arrives.
listen() {
    local secs="${1:-25}"
    python3 - "$PORT" "$secs" <<'PY'
import os, select, sys, time
port, secs = sys.argv[1], float(sys.argv[2])
fd = os.open(port, os.O_RDONLY | os.O_NONBLOCK)
buf = b""
end = time.time() + secs
while time.time() < end:
    r, _, _ = select.select([fd], [], [], 0.3)
    if not r:
        continue
    try:
        d = os.read(fd, 8192)
    except BlockingIOError:
        continue
    if not d:
        break
    buf += d
os.close(fd)
sys.stdout.write(buf.decode("utf-8", "replace"))
sys.stdout.write("\n--- %d bytes in %.0f s ---\n" % (len(buf), secs))
PY
}

wait_port || exit 1
echo "== port $PORT =="

if [ "${1:-}" = "--log-only" ]; then
    listen 25
    exit 0
fi

if [ "${1:-}" != "--no-backup" ] && [ ! -s "$BACKUP" ]; then
    echo "== backup factory flash -> $BACKUP =="
    esptool -p "$PORT" -b 921600 read_flash 0 "$BACKUP_ADDR" "$BACKUP" || exit 1
    ls -lh "$BACKUP"
fi

echo "== chip check =="
CHIP=$(esptool -p "$PORT" chip_id 2>&1 | grep -o 'ESP32-[A-Z0-9]*' | head -1)
echo "chip: ${CHIP:-unknown}"
if [ "$CHIP" != "ESP32-C3" ]; then
    echo "ERROR: refusing to flash, this image is ESP32-C3 only (found ${CHIP:-none})" >&2
    exit 1
fi

echo "== flash =="
wait_port || exit 1
esptool -p "$PORT" -b 921600 --after hard_reset write_flash \
    --flash_mode dio --flash_size 8MB --flash_freq 80m \
    0x0 "$HERE/build-mac/bootloader.bin" \
    0x8000 "$HERE/build-mac/partition-table.bin" \
    0x10000 "$HERE/build-mac/mrrc-terminal.bin" || exit 1

echo "== log =="
sleep 1
wait_port || exit 1
listen 25
