#!/usr/bin/env bash
# Fetch the firmware for VE1, the RTD1296's CODA980 video codec, and turn it
# into what the mainline coda driver loads (v4l-coda980-rtd1295.bin).
#
# Realtek ships it in BPI's Android 7 tree as ve1.bin: the BIT processor's
# 16-bit code words as text, one hex word per line ("e40e\n0020\n..."). The
# coda driver wants the words as little-endian binary -- it recognises the
# native order by the first opcode, 0xe40e, and reorders while copying -- and
# copies (size - 16) / 4 words, so 16 bytes of padding keep the tail intact.
#
# The blob is not redistributed with this project; it is downloaded from a
# pinned commit and checked against a known hash.
#
#   scripts/fetch-vpu-firmware.sh [outdir]     (default: build/firmware-mainline)
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${1:-$PROJECT_ROOT/build/firmware-mainline}"

REPO="BPI-SINOVOIP/BPI-1296-Android7"
COMMIT="d377aa6e73ed42f125603961da0f009604c0754e"
FILE="android/device/realtek/kylin/common/prebuilt/vendor/modules/ve1.bin"
URL="https://raw.githubusercontent.com/$REPO/$COMMIT/$FILE"
SRC_SHA256="aa467528e3d3bfb9527d3bdd48974a54057ce9459403afdcfd0ef39b86bca143"
BIN_SHA256="4a236f48d3e2eee9eb15520c656e55b76c16585dc308d69dc48d2ec154b4d09c"
NAME="v4l-coda980-rtd1295.bin"

if [ -f "$OUT/$NAME" ] && echo "$BIN_SHA256  $OUT/$NAME" | sha256sum -c --status; then
    echo ">>> $NAME already in $OUT"
    exit 0
fi

mkdir -p "$OUT"
tmp="$(mktemp)"
trap 'rm -f "$tmp"' EXIT

echo ">>> downloading ve1.bin from $REPO@${COMMIT:0:7}"
curl -fsSL -o "$tmp" "$URL"
echo "$SRC_SHA256  $tmp" | sha256sum -c --status || {
    echo "ve1.bin does not match the expected hash" >&2
    exit 1
}

python3 - "$tmp" "$OUT/$NAME" <<'EOF'
import struct, sys
words = [int(w, 16) for w in open(sys.argv[1]).read().split()]
assert words[0] == 0xe40e and all(0 <= w <= 0xffff for w in words)
data = b"".join(struct.pack("<H", w) for w in words)
data += b"\0" * ((-len(data)) % 8 + 16)
open(sys.argv[2], "wb").write(data)
EOF

echo "$BIN_SHA256  $OUT/$NAME" | sha256sum -c --status || {
    echo "the converted firmware does not match the expected hash" >&2
    exit 1
}
echo ">>> $OUT/$NAME"
