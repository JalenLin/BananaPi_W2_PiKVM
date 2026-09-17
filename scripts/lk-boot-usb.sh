#!/bin/bash
# Boot from a USB storage device via the LK console.
#
# When to use this: the BPI-W2's SD slot is faulty but LK (the Android
# bootloader) still works. The board must be at SW4=0, sitting at the
# `Realtek>` prompt, with our image on a USB device.
#
#   usage: lk-boot-usb.sh ["extra bootargs"] [seconds to capture]
set -euo pipefail

EXTRA="${1:-}"
CAPTURE="${2:-120}"
DEV="${SERIAL_DEV:-/dev/ttyUSB0}"
OUT="${LK_BOOT_LOG:-/tmp/lk-boot.log}"
OUTDIR="$(dirname "$OUT")"
BASE="bananapi/bpi-w2/linux"

BOOTARGS="earlycon=uart8250,mmio32,0x98007800 console=ttyS0,115200 root=/dev/sda2 rootwait rw loglevel=7 $EXTRA"

docker run --rm --device "$DEV" -v "$OUTDIR:/out" debian:bookworm bash -c "
stty -F $DEV 115200 cs8 -cstopb -parenb -crtscts raw -echo min 1 time 0
timeout $((CAPTURE + 90)) cat $DEV > /out/$(basename "$OUT") 2>&1 &
RD=\$!
send() { printf '%s\r' \"\$1\" > $DEV; sleep \"\$2\"; }

sleep 1
send ''                                                        2
# `usb start` often fails to enumerate the first time (Device not responding
# to set address), so issuing it three times is safer. If the storage device
# is not enumerated, fatload fails, `boot a` then executes garbage, and the
# ACPU goes down with it.
send 'usb start'                                              15
send 'usb start'                                              15
send 'usb storage'                                             5
send 'fatload usb 0:1 0x02100000 $BASE/bpi-w2.dtb'             5
send 'fatload usb 0:1 0x0f900000 $BASE/bluecore.audio'        15
send 'fatload usb 0:1 0x03000000 $BASE/uImage'                45
send 'fdt addr 0x02100000'                                     3
send 'fdt set /chosen bootargs \"$BOOTARGS\"'                  3
send 'boot a'                                                  8
send 'boot k'                                                  $CAPTURE

kill \$RD 2>/dev/null || true
wait \$RD 2>/dev/null || true
" >/dev/null 2>&1

echo ">>> log: $OUT ($(stat -c%s "$OUT" 2>/dev/null) bytes)"
