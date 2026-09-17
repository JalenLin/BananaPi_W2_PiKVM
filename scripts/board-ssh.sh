#!/bin/bash
# Run commands on the board or transfer files, over SSH -- faster than serial
# and unaffected by SD noise.
#   board-ssh.sh "<command>"              run on the board
#   board-ssh.sh --put <local> <remote>   upload
#   board-ssh.sh --get <remote> <local>   download
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
HOST="${BOARD_HOST:-192.168.99.93}"
PASS="${BOARD_PASS:-pikvm}"
IMG="bpiw2-pikvm/ssh:latest"
OPTS="-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -o ConnectTimeout=10"

case "${1:-}" in
  --put)
    SRC="$(realpath -m "$2")"; DST="$3"
    docker run --rm --network host -v "$SRC:/payload:ro" -e SSHPASS="$PASS" "$IMG" \
        sshpass -e scp $OPTS /payload "root@$HOST:$DST"
    ;;
  --get)
    docker run --rm --network host -v "$(dirname "$(realpath -m "$3")"):/out" -e SSHPASS="$PASS" "$IMG" \
        sshpass -e scp $OPTS "root@$HOST:$2" "/out/$(basename "$3")"
    ;;
  *)
    docker run --rm --network host -i -e SSHPASS="$PASS" "$IMG" \
        sshpass -e ssh $OPTS "root@$HOST" "$@"
    ;;
esac
