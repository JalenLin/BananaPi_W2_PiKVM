#!/bin/bash
# Recover from a kernel pushed with push-kernel-mainline.sh that does not
# boot, without taking the card out: over the serial console, stop u-boot
# and boot uImage.prev / bpi-w2.dtb.prev instead.
#
#   scripts/boot-prev-kernel.sh            boot the previous kernel once
#   scripts/boot-prev-kernel.sh --restore  ...then, once ssh answers, make it
#                                          the default again (the failed one
#                                          is kept as uImage.bad)
#
# Works while the board is boot-looping (a panicking kernel reboots itself
# after 10 s, see bpiw2.config) or right after a power cycle. It needs the
# serial port to itself, so stop any listener first (docker stop bpiw2-serial).
#
# How it works: u-boot reads uEnv.txt and then boot_from_sd() loads whatever
# sd_vmlinux / sd_boot_dtb name (common/cmd_boot.c). This replays u-boot's
# own boot_normal by hand, overriding those two after uEnv.txt is imported.
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
D=bananapi/bpi-w2/linux

RESTORE=0
[ "${1:-}" = "--restore" ] && RESTORE=1

# u-boot eats the first character after the countdown, hence the empty bait.
LISTEN="${LISTEN:-90}" "$PROJECT_ROOT/scripts/uboot-cmd.sh" \
    "" \
    "run checksd" \
    "setenv partition 0:1" \
    "run loadbootenv" \
    'env import -t ${scriptaddr} ${filesize}' \
    "setenv sd_vmlinux $D/uImage.prev" \
    "setenv sd_boot_dtb $D/bpi-w2.dtb.prev" \
    "run uenvcmd" \
    | grep -aE "Loading \"|Starting Kernel|Unable|failed|Kernel panic" || true

[ "$RESTORE" = 1 ] || exit 0

echo ">>> waiting for ssh"
for _ in $(seq 1 60); do
    timeout 8 "$PROJECT_ROOT/scripts/board-ssh.sh" true 2>/dev/null && break
    sleep 5
done
"$PROJECT_ROOT/scripts/board-ssh.sh" "set -e; cd /boot/$D
    mv uImage uImage.bad; mv bpi-w2.dtb bpi-w2.dtb.bad
    cp uImage.prev uImage; cp bpi-w2.dtb.prev bpi-w2.dtb
    cp bpi-w2.dtb rtd-1296-bananapi-w2-2GB.dtb
    sync; uname -v"
echo ">>> previous kernel is the default again; the failed one is uImage.bad"
