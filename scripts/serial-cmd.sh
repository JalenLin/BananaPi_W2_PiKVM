#!/bin/bash
# Send a command to the BPI-W2's LK / u-boot serial console and collect the reply.
#   usage: serial-cmd.sh "<command>" [seconds to wait]
# Reaches /dev/ttyUSB0 through docker (the device belongs to the dialout group).
set -euo pipefail

CMD="${1:-}"
WAIT="${2:-5}"
DEV="${SERIAL_DEV:-/dev/ttyUSB0}"
OUTDIR="$(mktemp -d)"
trap 'rm -rf "$OUTDIR"' EXIT

printf '%s' "$CMD" > "$OUTDIR/cmd"

docker run --rm --device "$DEV" -v "$OUTDIR:/io" debian:bookworm bash -c "
stty -F $DEV 115200 cs8 -cstopb -parenb -crtscts raw -echo min 1 time 0
timeout $WAIT cat $DEV > /io/resp 2>&1 &
RD=\$!
sleep 1
printf '%s\r' \"\$(cat /io/cmd)\" > $DEV
sleep $((WAIT - 2))
kill \$RD 2>/dev/null || true
wait \$RD 2>/dev/null || true
" >/dev/null 2>&1

cat -v "$OUTDIR/resp" 2>/dev/null
