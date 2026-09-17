#!/bin/bash
# Break into the u-boot console during a boot loop and run commands.
#
# Use this when the board will not come up and SSH is unreachable. It keeps
# sending ESC to win u-boot's "Hit Esc or Tab key to enter console mode"
# countdown, then feeds the commands in one at a time.
#
#   uboot-cmd.sh "<cmd>" ["<cmd>" ...]
#   LISTEN=180 uboot-cmd.sh ...     # raise LISTEN to watch a whole boot
#
# Note: u-boot eats the first character of each command, so send an empty
# string first as bait.
#
# Common uses:
#   # see how u-boot builds bootargs
#   uboot-cmd.sh "" "printenv bootcmd" "printenv boot_normal"
#
#   # boot without mounting any rootfs -- separates a software problem from
#   # a hardware/power problem
#   LISTEN=180 uboot-cmd.sh "" "run checksd" "setenv partition 0:1" \
#       "run loadbootenv" 'env import -t ${scriptaddr} ${filesize}' \
#       "setenv root /dev/ram" "run uenvcmd"
set -euo pipefail

DEV="${SERIAL_DEV:-/dev/ttyUSB0}"
LISTEN="${LISTEN:-60}"
OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT

printf '%s\n' "$@" > "$OUT/cmds"

docker run --rm --device "$DEV" -v "$OUT:/io" debian:bookworm bash -c "
stty -F $DEV 115200 cs8 -cstopb -parenb -crtscts raw -echo min 1 time 0
timeout $LISTEN cat $DEV > /io/resp 2>&1 &
RD=\$!
# Win the countdown: during a boot loop there is no telling when the next one
# comes round, so just keep sending
for i in \$(seq 1 200); do printf '\033' > $DEV; sleep 0.15; done
sleep 1
while IFS= read -r l; do printf '%s\r' \"\$l\" > $DEV; sleep 3; done < /io/cmds
sleep $((LISTEN > 40 ? LISTEN - 40 : 3))
kill \$RD 2>/dev/null || true; wait \$RD 2>/dev/null || true
" >/dev/null 2>&1

cat "$OUT/resp"
