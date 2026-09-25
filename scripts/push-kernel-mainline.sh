#!/bin/bash
# Install a freshly built mainline kernel on a running board over SSH, instead
# of pulling the SD card. Run after `make kernel-mainline`.
#
#   scripts/push-kernel-mainline.sh [--modules] [--reboot]
#
#   --modules   also replace /lib/modules/<version> (needed whenever a module
#               changed, or the version string did)
#   --reboot    reboot the board once everything is written
#
# The board is BOARD_HOST (see board-ssh.sh); bpi-w2-pikvm.local works where
# the host resolves mDNS.
#
# The previous kernel and dtb are kept on the boot partition as *.prev. The
# BSP u-boot only ever loads `uImage`, so falling back to them still means
# putting the card in a reader and renaming them back -- this saves the card
# swap on the way forward, not on the way back.
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LINUX="$PROJECT_ROOT/vendor/linux-mainline"
BUILD="$PROJECT_ROOT/build"
SSH="$PROJECT_ROOT/scripts/board-ssh.sh"
BOOTDIR=/boot/bananapi/bpi-w2/linux

MODULES=0
REBOOT=0
for a in "$@"; do
    case "$a" in
        --modules) MODULES=1 ;;
        --reboot)  REBOOT=1 ;;
        *) echo "unknown argument: $a" >&2; exit 2 ;;
    esac
done

STAGE="$BUILD/push-kernel"
rm -rf "$STAGE"; mkdir -p "$STAGE"

cp "$LINUX/arch/arm64/boot/Image" "$STAGE/uImage"
cp "$LINUX/arch/arm64/boot/dts/realtek/rtd1296-bananapi-w2.dtb" "$STAGE/bpi-w2.dtb"

# The bootloaders take the load address from text_offset; see the long
# comment in build-image.sh for why this has to be 0x1c000000.
TEXT_OFFSET="${KERNEL_TEXT_OFFSET:-0x1c000000}"
esc=""
for i in 0 1 2 3 4 5 6 7; do
    esc="$esc\\x$(printf '%02x' $(( (TEXT_OFFSET >> (8 * i)) & 0xff )))"
done
printf "$esc" | dd of="$STAGE/uImage" bs=1 seek=8 conv=notrunc status=none

KREL="$(cat "$LINUX/include/config/kernel.release")"
echo ">>> kernel $KREL, text_offset $TEXT_OFFSET"

# The boot partition must be mounted, or the files land on the rootfs under
# the mount point and the next boot still runs the old kernel.
"$SSH" "mountpoint -q /boot && test -d $BOOTDIR" || {
    echo "!!! $BOOTDIR is not on a mounted /boot on the board" >&2; exit 1; }

# Room first: the boot partition holds uImage, uImage.prev and the incoming
# uImage.new at once only if the leftover .new files are gone.
"$SSH" "rm -f $BOOTDIR/*.new"

for f in uImage bpi-w2.dtb; do
    echo ">>> uploading $f"
    "$SSH" --put "$STAGE/$f" "$BOOTDIR/$f.new"
done

# Verify before swapping in: a truncated kernel on vfat is a card swap.
want="$(cd "$STAGE" && md5sum uImage bpi-w2.dtb | awk '{print $1}' | tr '\n' ' ')"
got="$("$SSH" "cd $BOOTDIR && md5sum uImage.new bpi-w2.dtb.new" | awk '{print $1}' | tr '\n' ' ')"
if [ "$want" != "$got" ]; then
    echo "!!! checksum mismatch after upload: want [$want] got [$got]" >&2
    exit 1
fi

# rtd-1296-bananapi-w2-2GB.dtb is the name u-boot's compiled-in environment
# asks for; bpi-w2.dtb is the uEnv.txt one. Keep both in step, as
# build-image.sh does.
"$SSH" "set -e; cd $BOOTDIR
    mv uImage uImage.prev; mv bpi-w2.dtb bpi-w2.dtb.prev
    mv uImage.new uImage; mv bpi-w2.dtb.new bpi-w2.dtb
    cp bpi-w2.dtb rtd-1296-bananapi-w2-2GB.dtb
    sync"
echo ">>> kernel and dtb installed (previous ones kept as *.prev)"

if [ "$MODULES" = 1 ]; then
    echo ">>> packing modules"
    BUILDER_IMAGE=bpiw2-pikvm/builder-mainline:trixie \
    "$PROJECT_ROOT/scripts/in-docker.sh" bash -c '
        cd /work/vendor/linux-mainline &&
        make -s ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- LOCALVERSION="" \
             INSTALL_MOD_PATH=/work/build/push-kernel/mods INSTALL_MOD_STRIP=1 \
             modules_install'
    tar czf "$STAGE/modules.tgz" --owner=0 --group=0 --numeric-owner \
        -C "$STAGE/mods/lib/modules" "$KREL"
    "$SSH" --put "$STAGE/modules.tgz" /tmp/modules.tgz
    "$SSH" "set -e; rm -rf /usr/lib/modules/$KREL
        tar xzf /tmp/modules.tgz -C /usr/lib/modules; rm /tmp/modules.tgz
        depmod $KREL; sync"
    echo ">>> modules $KREL installed"
fi

if [ "$REBOOT" = 1 ]; then
    echo ">>> rebooting"
    "$SSH" "systemctl reboot" || true
fi
