#!/bin/bash
# Run the command as BUILD_UID/BUILD_GID so build outputs are not left owned by root.
set -euo pipefail

BUILD_UID="${BUILD_UID:-1000}"
BUILD_GID="${BUILD_GID:-1000}"

if [ "$(id -u)" = "0" ]; then
    groupadd -g "$BUILD_GID" -o builder 2>/dev/null || true
    useradd -u "$BUILD_UID" -g "$BUILD_GID" -o -m -s /bin/bash builder 2>/dev/null || true
    exec setpriv --reuid "$BUILD_UID" --regid "$BUILD_GID" --init-groups \
        env HOME=/home/builder "$@"
fi

exec "$@"
