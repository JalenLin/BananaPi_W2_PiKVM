#!/bin/bash
# Boot from a USB storage device via the LK console.
#
# When to use this: the BPI-W2's SD slot is faulty but LK (the Android
# bootloader) still works. The board must be at SW4=0, sitting at the
# 'Realtek>' prompt, with our image on a USB device -- a USB card reader
# holding the SD card counts, and needs no reflashing.
#
#   usage: lk-boot-usb.sh ["extra bootargs"] [seconds to capture]
#
# The command sequence lives in scripts/lk-console.py, which waits for the
# prompt between commands instead of sleeping for a fixed time. LK shares its
# UART with the audio core, so output interleaves and fixed sleeps desync.
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

EXTRA="${1:-}"
CAPTURE="${2:-150}"
DEV="${SERIAL_DEV:-/dev/ttyUSB0}"
OUT="${LK_BOOT_LOG:-/tmp/lk-boot.log}"
OUTDIR="$(dirname "$OUT")"

# No root= here. Mainline has no mmc or USB host driver for this SoC, so
# nothing this bootloader can reach will become a root device; the kernel is
# expected to panic in mount_root, which is what M0 accepts. The board DTS
# carries stdout-path, so the console works even though LK overwrites
# /chosen/bootargs with its own idea of the command line.
BOOTARGS="earlycon=uart8250,mmio32,0x98007800 console=ttyS0,115200 keep_bootcon loglevel=8 $EXTRA"

docker run --rm --device "$DEV" \
    -v "$PROJECT_ROOT/scripts:/scripts:ro" \
    -v "$OUTDIR:/out" \
    python:3-slim sh -c "
stty -F $DEV 115200 cs8 -cstopb -parenb -crtscts raw -echo min 1 time 0
DEV=$DEV LOG=/out/$(basename "$OUT") CAPTURE=$CAPTURE \
BOOTARGS='$BOOTARGS' \
exec python3 -u /scripts/lk-console.py
"

echo ">>> log: $OUT ($(stat -c%s "$OUT" 2>/dev/null) bytes)"
