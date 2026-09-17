#!/bin/bash
# Reach the Raspberry Pi acting as the HDMI source (for diagnostics).
#
# Credentials are not stored in this file -- it is version-controlled, and
# hardcoding a password means committing it. Pass them as environment
# variables:
#
#   PI_HOST=192.168.100.99 PI_PASS='...' scripts/pi-ssh.sh "tvservice -s"
#
# Better still, install an SSH key and drop the sshpass part entirely.
set -euo pipefail

HOST="${PI_HOST:-}"
PASS="${PI_PASS:-}"

if [ -z "$HOST" ] || [ -z "$PASS" ]; then
    echo "usage: PI_HOST=<ip> PI_PASS=<password> $0 \"<command>\"" >&2
    exit 2
fi

docker run --rm --network host -i -e SSHPASS="$PASS" bpiw2-pikvm/ssh:latest \
    sshpass -e ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
    -o LogLevel=ERROR -o ConnectTimeout=10 "root@$HOST" "$@"
