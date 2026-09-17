#!/bin/bash
# Apply the ownership and permissions that PiKVM upstream's kvmd.install sets.
#
# We unpack files onto the system directly rather than letting pacman run the
# .install script, so this has to be done by hand. Without it, kvmd running as
# the kvmd user cannot read htpasswd and every login returns HTTP 500.
set -eu

chown kvmd:kvmd            /etc/kvmd/htpasswd     2>/dev/null || true
chown kvmd:kvmd            /etc/kvmd/totp.secret  2>/dev/null || true
chown kvmd-ipmi:kvmd-ipmi  /etc/kvmd/ipmipasswd   2>/dev/null || true
chown kvmd-vnc:kvmd-vnc    /etc/kvmd/vncpasswd    2>/dev/null || true
chmod 600 /etc/kvmd/*passwd 2>/dev/null || true
chmod 600 /etc/kvmd/totp.secret 2>/dev/null || true

mkdir -p /var/lib/kvmd/msd /var/lib/kvmd/pst
chown kvmd          /var/lib/kvmd/msd 2>/dev/null || true
chown kvmd-pst:kvmd-pst /var/lib/kvmd/pst 2>/dev/null || true
chmod 1775          /var/lib/kvmd/pst 2>/dev/null || true

chown root:root /etc/kvmd/nginx/ssl /etc/kvmd/vnc/ssl 2>/dev/null || true
chmod 644 /etc/kvmd/nginx/*.conf* 2>/dev/null || true
