#!/bin/bash
# Fetch the upstream sources and apply this project's patches. Idempotent.
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VENDOR="$PROJECT_ROOT/vendor"

# Upstream sources and the refs they are pinned to.
#
# The kernel BSP has no tags, so master is the only option. kvmd and ustreamer
# are pinned to versions verified on hardware, so repeated builds produce the
# same thing. To move up a version, change it here and re-verify.
BSP_URL="https://github.com/BPI-SINOVOIP/BPI-W2-bsp.git"
BSP_REF="master"
# The mainline/LTS kernel line. The BSP is still needed alongside it -- u-boot,
# the audio firmware blob and the vendor initramfs all come from there.
LINUX_URL="https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git"
LINUX_REF="v6.18.52"
KVMD_URL="https://github.com/pikvm/kvmd.git"
KVMD_REF="v4.213"
USTREAMER_URL="https://github.com/pikvm/ustreamer.git"
USTREAMER_REF="v6.66"
# The WebRTC gateway kvmd-janus runs; Debian 13 does not package it. PiKVM
# builds a later commit; v1.4.2 already has its pkg-config file, which leaves
# PiKVM's janus.js patch (patches/janus).
JANUS_URL="https://github.com/meetecho/janus-gateway.git"
JANUS_REF="v1.4.2"

mkdir -p "$VENDOR"

# fetch_source <dirname> <url> <ref>
#
# An existing checkout is kept (no re-clone -- saves bandwidth and preserves
# local experiments); only the working tree is restored to the pinned ref.
# Cloning uses --depth 1 --branch, which works for both tags and branches.
fetch_source() {
    local dir="$VENDOR/$1" url="$2" ref="$3"

    if [ ! -d "$dir/.git" ]; then
        echo ">>> clone $1 ($url @ $ref)"
        git clone --depth 1 --branch "$ref" "$url" "$dir"
        return
    fi

    # Restore tracked files only. Deliberately no git clean: the BSP is an
    # in-tree build, so removing untracked files throws away the entire
    # kernel build.
    echo ">>> restoring $1 to an unmodified state"
    git -C "$dir" checkout -- .

    # Already on the target ref: no network needed
    if [ "$(git -C "$dir" rev-parse --verify --quiet "$ref^{commit}" || true)" \
         != "$(git -C "$dir" rev-parse HEAD)" ]; then
        echo ">>> switching $1 to $ref"
        git -C "$dir" fetch --depth 1 origin "$ref"
        git -C "$dir" checkout -q FETCH_HEAD
    fi
}

# apply_patches <dirname>
apply_patches() {
    local dir="$VENDOR/$1" pdir="$PROJECT_ROOT/patches/$2"
    local p
    [ -d "$pdir" ] || return 0
    for p in "$pdir"/*.patch; do
        [ -e "$p" ] || continue
        echo "    $(basename "$p")"
        git -C "$dir" apply --verbose "$p"
    done
}

fetch_source bpi-w2-bsp "$BSP_URL" "$BSP_REF"
echo ">>> applying kernel patches"
apply_patches bpi-w2-bsp kernel

# The mainline kernel is a second, independent tree: ~2 GB to clone, and only
# the kernel-6.18 branch needs it, so it is opt-in via WITH_MAINLINE=1
# (`make sources-mainline`).
SOURCES="bpi-w2-bsp ustreamer kvmd janus-gateway"
if [ "${WITH_MAINLINE:-0}" = "1" ]; then
    fetch_source linux-mainline "$LINUX_URL" "$LINUX_REF"
    echo ">>> applying mainline kernel patches"
    apply_patches linux-mainline linux-mainline
    SOURCES="$SOURCES linux-mainline"
fi

fetch_source ustreamer "$USTREAMER_URL" "$USTREAMER_REF"
echo ">>> applying ustreamer patches"
apply_patches ustreamer ustreamer

fetch_source kvmd "$KVMD_URL" "$KVMD_REF"
echo ">>> applying kvmd patches"
apply_patches kvmd kvmd

fetch_source janus-gateway "$JANUS_URL" "$JANUS_REF"
echo ">>> applying janus patches"
apply_patches janus-gateway janus

# janus.js imports adapter.js (WebRTC shims), which kvmd's nginx serves from
# /usr/share/janus/javascript. PiKVM takes adapter-latest.js; pin a release.
ADAPTER_URL="https://cdn.jsdelivr.net/npm/webrtc-adapter@9.0.3/out/adapter.js"
ADAPTER_SHA256="cf03c48eba575b2c2864276ef3372e4a784c84ac2db71df17de84bc96da00e6e"
ADAPTER="$VENDOR/janus-gateway/adapter.js"
if ! echo "$ADAPTER_SHA256  $ADAPTER" | sha256sum -c --status 2>/dev/null; then
    echo ">>> downloading webrtc-adapter"
    curl -fsSL -o "$ADAPTER" "$ADAPTER_URL"
    echo "$ADAPTER_SHA256  $ADAPTER" | sha256sum -c --status || {
        echo "adapter.js does not match the expected hash" >&2
        exit 1
    }
fi

echo ">>> done"
for d in $SOURCES; do
    echo "--- $d ---"
    git -C "$VENDOR/$d" status --short | head -20
done
