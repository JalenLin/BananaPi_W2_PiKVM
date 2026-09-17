#!/bin/bash
# Run a command on an already-logged-in serial shell on the board and collect output.
#   usage: board-cmd.sh "<shell command>" [seconds to wait]
# Filters out the rtk_sdmmc retry noise a faulty SD slot produces.
set -euo pipefail

CMD="${1:-}"
WAIT="${2:-8}"
DEV="${SERIAL_DEV:-/dev/ttyUSB0}"
OUTDIR="$(mktemp -d)"
trap 'rm -rf "$OUTDIR"' EXIT

printf '%s' "$CMD" > "$OUTDIR/cmd"

docker run --rm --device "$DEV" -v "$OUTDIR:/io" debian:bookworm bash -c "
stty -F $DEV 115200 cs8 -cstopb -parenb -crtscts raw -echo min 1 time 0
timeout $((WAIT + 3)) cat $DEV > /io/resp 2>&1 &
RD=\$!
sleep 1
printf '%s\n' \"\$(cat /io/cmd)\" > $DEV
sleep $WAIT
kill \$RD 2>/dev/null || true
wait \$RD 2>/dev/null || true
" >/dev/null 2>&1

cat -v "$OUTDIR/resp" 2>/dev/null \
  | grep -av "trans: 0x\|rtk_sdmmc\|Reject SD\|SD card is being\|SD card power\|mmc0: error\|card claims\|_AO_\|HDMI not enabled"
