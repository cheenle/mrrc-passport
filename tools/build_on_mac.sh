#!/usr/bin/env bash
# Build on the mac-mini (radio.vlsc.net, 8 cores) and pull the artefacts back.
#
#   ./tools/build_on_mac.sh          # sync + build + fetch
#   ./tools/build_on_mac.sh flash    # ... and flash the device from this host
#
# The mac-mini needs ESP-IDF v5.5.3 in ~/esp/esp-idf-v5.5.3 with esp32c3 tools
# installed.  The device stays plugged into *this* host, so flashing always runs
# here; the Mac only compiles.
set -euo pipefail

MAC=${MAC:-cheenle@radio.vlsc.net}
# Keep this a *bare* path: macOS refuses to create /home/... (autofs owns it).
REMOTE=${MRRC_REMOTE:-HAM/mrrc-passport}
IDF_DIR=${MRRC_IDF:-esp/esp-idf-v5.5.3}
HERE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PORT=${MRRC_PORT:-$( (ls /dev/ttyACM* /dev/ttyUSB* 2>/dev/null || true) | head -1 )}

echo "== sync source =="
# sdkconfig stays behind (generated), everything else including the gitignored
# sdkconfig.defaults.local goes over so the remote build carries this device's
# credentials and the same Kconfig defaults.
rsync -a --delete \
      --exclude build --exclude build-mac --exclude managed_components \
      --exclude sdkconfig --exclude sdkconfig.old --exclude .git \
      --exclude ref \
      "$HERE_ROOT/" "$MAC:$REMOTE/"

# sdkconfig is generated, so it is not synced, which also means a changed
# Kconfig default never reaches the remote build.  Drop the stale file whenever
# the defaults are newer than it.
ssh "$MAC" "cd $REMOTE && [ sdkconfig -ot sdkconfig.defaults ] && rm -f sdkconfig sdkconfig.old || true" >/dev/null 2>&1

echo "== build on the mac-mini =="
ssh "$MAC" "cd $REMOTE && zsh -c '. ~/$IDF_DIR/export.sh >/dev/null 2>&1 && idf.py build'" 2>&1 | tail -25 || {
    echo "BUILD FAILED"; exit 1; }

echo "== fetch artefacts =="
mkdir -p "$HERE_ROOT/build-mac"
rsync -a "$MAC:$REMOTE/build/mrrc-terminal.bin" \
         "$MAC:$REMOTE/build/bootloader/bootloader.bin" \
         "$MAC:$REMOTE/build/partition_table/partition-table.bin" \
         "$MAC:$REMOTE/build/flash_args" "$HERE_ROOT/build-mac/"
ls -l "$HERE_ROOT/build-mac/"

if [ "${1:-}" = "flash" ]; then
    [ -n "$PORT" ] || { echo "ERROR: no serial port found; set MRRC_PORT" >&2; exit 1; }
    echo "== flash $PORT (8 MB, USB-Serial-JTAG) =="
    cd "$HERE_ROOT/build-mac"
    python3 -m esptool --chip esp32c3 -p "$PORT" -b 460800 \
        --before default_reset --after hard_reset write_flash \
        --flash_mode dio --flash_size 8MB --flash_freq 80m \
        0x0 bootloader.bin 0x8000 partition-table.bin 0x10000 mrrc-terminal.bin
fi
