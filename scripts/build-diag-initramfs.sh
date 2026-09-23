#!/bin/bash
# Build the diagnostic initramfs used by kernel/mainline/diag/initramfs.config
# (docs/09 §7): a static arm64 busybox, the aliastest tool and the diag /init,
# packed as build/diag/initramfs.cpio.
#
#   scripts/build-diag-initramfs.sh
#   EXTRA_CONFIG=kernel/mainline/diag/initramfs.config make kernel-mainline
#
# Runs as root in the builder container because the cpio needs a real
# /dev/console node; the output is handed back to the calling user.
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMAGE="${BUILDER_IMAGE:-bpiw2-pikvm/builder-mainline:trixie}"

docker run --rm --entrypoint bash -v "$PROJECT_ROOT:/work" "$IMAGE" -c '
set -euo pipefail
dpkg --add-architecture arm64
apt-get update -qq >/dev/null
apt-get install -qq -y libc6-dev-arm64-cross cpio >/dev/null

mkdir -p /work/build/diag
aarch64-linux-gnu-gcc -O2 -static -Wall \
    -o /work/build/diag/aliastest /work/kernel/mainline/diag/aliastest.c

cd /tmp
apt-get download -qq busybox-static:arm64 >/dev/null
dpkg-deb -x busybox-static_*_arm64.deb bb

R=/tmp/root
mkdir -p $R/bin $R/dev $R/proc $R/sys $R/tmp
cp bb/usr/bin/busybox $R/bin/busybox
cp /work/build/diag/aliastest $R/bin/aliastest
cp /work/kernel/mainline/diag/init $R/init
chmod +x $R/init
mknod -m 600 $R/dev/console c 5 1

cd $R && find . | cpio -o -H newc --quiet > /work/build/diag/initramfs.cpio
chown -R '"$(id -u):$(id -g)"' /work/build/diag
ls -l /work/build/diag/initramfs.cpio
'
