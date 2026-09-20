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

# arm64 defconfig is the baseline everyone else ports against, so start there
# and layer only what this board needs on top.
if [ ! -f .config ] || [ /work/kernel/mainline/bpiw2.config -nt .config ]; then
    make defconfig
    ./scripts/kconfig/merge_config.sh -m -O . .config \
        /work/kernel/mainline/bpiw2.config
    make olddefconfig
fi

make -j"$(nproc)" '"$MAKE_ARGS"' '"$TARGETS"'

echo
echo "--- built ---"
ls -l arch/arm64/boot/Image
ls -l arch/arm64/boot/dts/realtek/rtd1296-bananapi-w2.dtb
'
