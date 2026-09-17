#!/bin/bash
# Run a command inside the build container. The project root is mounted at /work.
#   usage: scripts/in-docker.sh <command...>
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMAGE="${BUILDER_IMAGE:-bpiw2-pikvm/builder:bullseye}"

exec docker run --rm -i \
    -v "$PROJECT_ROOT:/work" \
    -w "/work" \
    -e "BUILD_UID=$(id -u)" \
    -e "BUILD_GID=$(id -g)" \
    "$IMAGE" "$@"
