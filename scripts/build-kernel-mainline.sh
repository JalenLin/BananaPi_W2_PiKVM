#!/bin/bash
# Build the mainline/LTS kernel (Image + dtbs + modules) inside the trixie
# build container. See docs/09-mainline-bringup.md for what this line is for.
#
# The board DTS and the kconfig fragment live in kernel/mainline/ and are
# copied into the tree here; anything that modifies an existing upstream file
# is a patch under patches/linux-mainline/ applied by prepare-sources.sh.
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

TARGETS="${TARGETS:-Image dtbs modules}"

# EXTRA_CONFIG=kernel/mainline/<something>.config layers a second fragment on
# top of bpiw2.config -- for diagnostic builds that must not leak into the
# normal one. The combination last used is recorded in the tree, so going back
# to a plain build regenerates .config instead of silently keeping the extras.
EXTRA_CONFIG="${EXTRA_CONFIG:-}"

# CHECK_DTBS=1 also runs the dtb through dt-validate against the bindings in
# Documentation/devicetree/bindings. Off by default because it roughly doubles
# the dtbs step; worth running whenever the board DTS changes.
#
# The kernel tests this with ifneq($(CHECK_DTBS),), so CHECK_DTBS=0 would turn
# checking ON. It has to be unset, not falsy.
MAKE_ARGS=""
if [ "${CHECK_DTBS:-0}" != "0" ]; then
    MAKE_ARGS="CHECK_DTBS=1"
fi

BUILDER_IMAGE="${BUILDER_IMAGE:-bpiw2-pikvm/builder-mainline:trixie}" \
exec "$PROJECT_ROOT/scripts/in-docker.sh" bash -c '
set -euo pipefail
cd /work/vendor/linux-mainline

export ARCH=arm64
export CROSS_COMPILE=aarch64-linux-gnu-
export KBUILD_BUILD_USER=bpiw2-pikvm
export KBUILD_BUILD_HOST=builder
# Setting LOCALVERSION at all (even empty) is what stops scripts/setlocalversion
# appending "-dirty". The tree is always dirty here: the board DTS is copied in
# and the dts Makefile is patched. The version we want comes from
# CONFIG_LOCALVERSION in the fragment.
export LOCALVERSION=""

# Our board DTS. Kept as a plain file rather than a patch because it changes
# on nearly every bring-up step; the Makefile line that builds it is the patch.
cp /work/kernel/mainline/rtd1296-bananapi-w2.dts \
   arch/arm64/boot/dts/realtek/rtd1296-bananapi-w2.dts
cp /work/kernel/mainline/irq-rtd129x.c drivers/irqchip/irq-rtd129x.c
cp /work/kernel/mainline/sdmmc-rtd129x.c drivers/mmc/host/sdmmc-rtd129x.c
cp "/work/kernel/mainline/realtek,rtd1295-irq-mux.yaml" \
   "Documentation/devicetree/bindings/interrupt-controller/realtek,rtd1295-irq-mux.yaml"

# arm64 defconfig is the baseline everyone else ports against, so start there
# and layer only what this board needs on top.
FRAGMENTS="/work/kernel/mainline/bpiw2.config"
EXTRA='"$EXTRA_CONFIG"'
[ -n "$EXTRA" ] && FRAGMENTS="$FRAGMENTS /work/$EXTRA"
STAMP=.bpiw2-fragments
regen=0
[ -f .config ] || regen=1
[ "$(cat $STAMP 2>/dev/null)" = "$FRAGMENTS" ] || regen=1
for f in $FRAGMENTS; do [ "$f" -nt .config ] && regen=1; done
if [ "$regen" = 1 ]; then
    make defconfig
    ./scripts/kconfig/merge_config.sh -m -O . .config $FRAGMENTS
    make olddefconfig
    echo "$FRAGMENTS" > $STAMP
fi

make -j"$(nproc)" '"$MAKE_ARGS"' '"$TARGETS"'

echo
echo "--- built ---"
ls -l arch/arm64/boot/Image
ls -l arch/arm64/boot/dts/realtek/rtd1296-bananapi-w2.dtb
'
