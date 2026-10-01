#!/usr/bin/env bash
# Build the rootfs for the mainline kernel line: Arch Linux ARM (aarch64)
# with PiKVM's packages, as PiKVM OS itself is. Output: build/rootfs-arch.tar
#
# The BSP 4.9 kernel line cannot use it -- systemd 258+ needs kernel 5.4
# (docs/02-decisions.md) -- and keeps the Debian rootfs of build-rootfs.sh.
#
# Needs: docker with arm64 binfmt (qemu-user-static), and `make sources`.
# The work inside the container is scripts/rootfs-arch.sh.
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="$PROJECT_ROOT/build"
CACHE="$BUILD/cache"
CID="bpiw2-rootfs-arch"
BASE_IMAGE="bpiw2-pikvm/alarm-base:aarch64"
OUT="$BUILD/rootfs-arch.tar"

TARBALL="ArchLinuxARM-aarch64-latest.tar.gz"
URL="http://os.archlinuxarm.org/os/$TARBALL"

for d in kvmd ustreamer; do
    [ -d "$PROJECT_ROOT/vendor/$d" ] || {
        echo "vendor/$d not found -- run make sources first" >&2
        exit 1
    }
done
mkdir -p "$CACHE"

# -- the Arch Linux ARM base, as a docker image -----------------------
# "latest" is what Arch Linux ARM publishes; the system is upgraded to the
# current repositories inside anyway. Re-downloaded only when it changed.
echo ">>> checking $TARBALL"
curl -fsSL -o "$CACHE/$TARBALL.md5.new" "$URL.md5"
if ! cmp -s "$CACHE/$TARBALL.md5.new" "$CACHE/$TARBALL.md5" \
   || ! (cd "$CACHE" && md5sum -c --status "$TARBALL.md5"); then
    echo ">>> downloading $TARBALL"
    curl -fSL -o "$CACHE/$TARBALL" "$URL"
    mv "$CACHE/$TARBALL.md5.new" "$CACHE/$TARBALL.md5"
    (cd "$CACHE" && md5sum -c "$TARBALL.md5")
    docker rmi -f "$BASE_IMAGE" >/dev/null 2>&1 || true
else
    rm -f "$CACHE/$TARBALL.md5.new"
fi
if ! docker image inspect "$BASE_IMAGE" >/dev/null 2>&1; then
    echo ">>> importing it as $BASE_IMAGE"
    docker import --platform linux/arm64 "$CACHE/$TARBALL" "$BASE_IMAGE"
fi

# -- build ------------------------------------------------------------
echo ">>> building the rootfs in $CID"
docker rm -f "$CID" >/dev/null 2>&1 || true
docker run --name "$CID" --platform linux/arm64 \
    -v "$PROJECT_ROOT/vendor/kvmd:/tmp/src/kvmd:ro" \
    -v "$PROJECT_ROOT/vendor/ustreamer:/tmp/src/ustreamer:ro" \
    -v "$PROJECT_ROOT/scripts/rootfs-arch.sh:/tmp/rootfs-arch.sh:ro" \
    -v "$PROJECT_ROOT/overlay:/tmp/overlay:ro" \
    "$BASE_IMAGE" /bin/bash /tmp/rootfs-arch.sh

echo ">>> exporting the rootfs"
docker export "$CID" -o "$OUT"
docker rm -f "$CID" >/dev/null

# docker bind-mounts /etc/{hostname,hosts,resolv.conf} into the container,
# so the image layer only has empty files there. Append the real ones; on
# extraction the later tar entries win (see build-rootfs.sh for why this is
# --append and never --delete).
echo ">>> adding the /etc files docker bind-mounted away"
STAGE="$BUILD/etc-fix-arch"
rm -rf "$STAGE"; mkdir -p "$STAGE/etc"
echo "bpi-w2-pikvm" > "$STAGE/etc/hostname"
cat > "$STAGE/etc/hosts" <<EOF
127.0.0.1   localhost
127.0.1.1   bpi-w2-pikvm
::1         localhost ip6-localhost ip6-loopback
EOF
ln -sf ../run/systemd/resolve/stub-resolv.conf "$STAGE/etc/resolv.conf"
tar --append -f "$OUT" -C "$STAGE" \
    --owner=0 --group=0 --numeric-owner --mode=644 \
    etc/hostname etc/hosts etc/resolv.conf
rm -rf "$STAGE"
echo ">>> done: $OUT ($(du -h "$OUT" | cut -f1))"
