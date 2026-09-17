#!/bin/bash
# Usage: mk-debian-rootfs.sh <suite>
set -e
SUITE=$1
SP=/tmp/claude-1000/-home-jalen-bpiw2-pikvm/11517dc8-8cdd-4252-ad6d-18310b9d2280/scratchpad/qemutest
CID=deb-$SUITE-build

docker rm -f $CID >/dev/null 2>&1 || true
docker run --name $CID --platform linux/arm64 debian:$SUITE bash -c '
set -e
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq systemd systemd-sysv dbus python3 >/dev/null
echo "root:root" | chpasswd
cat > /etc/systemd/system/boottest.service <<UNIT
[Unit]
Description=Boot test marker
After=basic.target
[Service]
Type=oneshot
RemainAfterExit=yes
StandardOutput=journal+console
ExecStart=/bin/sh -c "echo =====BOOT_TEST_MARKER_OK=====; systemctl --version | head -1; python3 -VV | head -1; echo --- cgroup ---; cat /proc/self/cgroup; echo --- failed ---; systemctl --failed --no-legend --no-pager; echo =====BOOT_TEST_DONE=====; sleep 2; systemctl poweroff -f"
[Install]
WantedBy=multi-user.target
UNIT
mkdir -p /etc/systemd/system/multi-user.target.wants
ln -sf /etc/systemd/system/boottest.service /etc/systemd/system/multi-user.target.wants/boottest.service
echo "SUITE_SYSTEMD=$(dpkg-query -W -f=\${Version} systemd)"
echo "SUITE_PYTHON=$(dpkg-query -W -f=\${Version} python3)"
'
docker export $CID -o $SP/rootfs-$SUITE.tar
docker rm -f $CID >/dev/null

docker run --rm --entrypoint bash -v "$SP:/t" debian:bookworm -c "
apt-get update -qq && apt-get install -qq -y e2fsprogs >/dev/null
cd /t
rm -rf rootfs-$SUITE && mkdir rootfs-$SUITE
tar xf rootfs-$SUITE.tar -C rootfs-$SUITE
rm -f rootfs-$SUITE.img
truncate -s 3G rootfs-$SUITE.img
mkfs.ext4 -q -F -d rootfs-$SUITE rootfs-$SUITE.img
"
echo "built: $SP/rootfs-$SUITE.img"
