#!/bin/bash
# Build the BSP kernel (plus dtb and modules) inside the build container.
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

exec "$PROJECT_ROOT/scripts/in-docker.sh" bash -c '
set -euo pipefail
cd /work/vendor/bpi-w2-bsp
[ -f chosen_board.mk ] || ./configure BPI-W2-720P
make kernel
'
