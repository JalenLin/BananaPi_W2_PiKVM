#!/bin/bash
# Run commands on the board or transfer files, over SSH -- faster than serial
# and unaffected by SD noise.
#   board-ssh.sh "<command>"              run on the board
#   board-ssh.sh --put <local> <remote>   upload
#   board-ssh.sh --get <remote> <local>   download
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
HOST="${BOARD_HOST:-bpi-w2-pikvm.local}"
# The image answers mDNS for its hostname. The ssh container has no mDNS
# resolver, so resolve it here: through nss-mdns if the host has it, else by
# asking on the wire.
case "$HOST" in
  *.local)
    addr="$(getent ahostsv4 "$HOST" | awk 'NR == 1 { print $1 }')" || true
    [ -n "$addr" ] || addr="$(python3 "$PROJECT_ROOT/scripts/mdns-resolve.py" "$HOST")" || true
    [ -n "$addr" ] || { echo "cannot resolve $HOST -- set BOARD_HOST to its IP" >&2; exit 1; }
    HOST="$addr"
    ;;
esac
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
