#!/bin/bash
# Build a Debian 13 (trixie) arm64 rootfs containing the PiKVM userspace.
# Everything happens inside an arm64 container (via binfmt emulation), so the
# host does not need debootstrap.
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="$PROJECT_ROOT/build"
KVER="4.9.119-BPI-W2-Kernel"
CID="bpiw2-rootfs-build"

mkdir -p "$BUILD"

for d in kvmd ustreamer; do
    [ -d "$PROJECT_ROOT/vendor/$d" ] || {
        echo "vendor/$d not found -- run make sources first"; exit 1; }
done

echo ">>> packing kernel modules and headers"
KMOD_SRC="$PROJECT_ROOT/vendor/bpi-w2-bsp/linux-rtk/output/lib/modules/$KVER"
[ -d "$KMOD_SRC" ] || { echo "kernel modules not found -- run make kernel first"; exit 1; }
rm -f "$BUILD/modules.tar"
# --owner/--group=0 matters: without it, unpacking chowns /usr/lib (the target
# of the /lib symlink) and the whole modules tree beneath it to the builder's
# uid, and systemd-tmpfiles then fails en masse with
# "Detected unsafe path transition".
tar cf "$BUILD/modules.tar" --owner=0 --group=0 --numeric-owner \
    -C "$PROJECT_ROOT/vendor/bpi-w2-bsp/linux-rtk/output" lib/modules

echo ">>> creating the rootfs container"
docker rm -f "$CID" >/dev/null 2>&1 || true
docker run --name "$CID" --platform linux/arm64 \
    -v "$BUILD/modules.tar:/tmp/modules.tar:ro" \
    -v "$PROJECT_ROOT/vendor/bpi-w2-bsp/linux-rtk/.config:/tmp/kernel.config:ro" \
    -v "$PROJECT_ROOT/vendor/kvmd:/tmp/src/kvmd:ro" \
    -v "$PROJECT_ROOT/vendor/ustreamer:/tmp/src/ustreamer:ro" \
    -v "$PROJECT_ROOT/scripts/rootfs-pikvm.sh:/tmp/rootfs-pikvm.sh:ro" \
    -v "$PROJECT_ROOT/overlay:/tmp/overlay:ro" \
    debian:trixie bash -eux -c '
export DEBIAN_FRONTEND=noninteractive

# The docker debian image excludes docs and man pages; restore normal behaviour
rm -f /etc/dpkg/dpkg.cfg.d/docker* /etc/apt/apt.conf.d/docker*

apt-get update -qq

# arm64 runs under binfmt/qemu emulation, and the emulated python3
# occasionally SIGSEGVs inside the dpkg post-installation scripts (py3compile
# reporting status code -11), which fails the whole apt-get install.
# Re-running usually succeeds, so wrap it in a retry.
apt_install() {
    local i
    for i in 1 2 3; do
        apt-get install -y --no-install-recommends "$@" && return 0
        echo "!!! apt-get install failed (possibly an intermittent SIGSEGV under qemu), retry $i"
        dpkg --configure -a || true
    done
    return 1
}

# -- base system -----------------------------------------------------
apt_install \
    systemd systemd-sysv systemd-resolved systemd-timesyncd dbus udev kmod \
    initramfs-tools u-boot-tools busybox \
    openssh-server iproute2 iputils-ping ifupdown \
    ca-certificates locales tzdata sudo procps \
    e2fsprogs dosfstools parted fdisk cloud-guest-utils \
    v4l-utils usbutils pciutils i2c-tools \
    less nano file htop

# -- PiKVM runtime dependencies ---------------------------------------
# kvmd upstream is an Arch package; these are the equivalent Debian ones.
# Everything comes from apt rather than pip, to avoid fighting the system
# python.
apt_install \
    nginx openssl \
    libevent-2.1-7t64 libevent-pthreads-2.1-7t64 \
    libjpeg62-turbo libbsd0 libxkbcommon0 \
    python3 python3-aiofiles python3-aiohttp python3-async-lru \
    python3-bcrypt python3-dbus python3-dbus-next python3-evdev \
    python3-hid python3-legacycrypt python3-libgpiod python3-mako \
    python3-netifaces python3-pam python3-passlib python3-pil \
    python3-psutil python3-pygments python3-pyotp python3-pyudev \
    python3-qrcode python3-ruamel.yaml python3-serial \
    python3-serial-asyncio python3-setproctitle python3-six \
    python3-spidev python3-systemd python3-usb python3-xlib \
    python3-yaml python3-zstandard \
    python3-pyghmi python3-pyrad python3-ldap python3-smbc python3-paramiko \
    python3-setuptools

# -- build and install ustreamer / kvmd -------------------------------
# The build packages are removed afterwards and never reach the image.
#
# The build-essential dependencies (gcc/g++/cpp/make/dpkg-dev/libc6-dev/
# binutils) must be named individually when purging: they form a dependency
# cycle (gcc -> cpp -> cpp-<arch> -> gcc), so purging only build-essential
# makes apt autoremove conservatively keep the whole chain -- 100 MB of
# dead weight.
BUILD_DEPS="build-essential gcc g++ cpp make dpkg-dev libc6-dev binutils
            pkg-config python3-dev
            python3-build python3-installer python3-wheel
            libevent-dev libjpeg62-turbo-dev libbsd-dev"
# python3-setuptools is deliberately not listed here: python3-pyghmi ->
# python3-pbr -> python3-setuptools, so purging it would take pyghmi too.
apt_install $BUILD_DEPS

/tmp/rootfs-pikvm.sh

apt-get purge -y $BUILD_DEPS
apt-get autoremove -y --purge

# -- kernel modules ---------------------------------------------------
tar xf /tmp/modules.tar -C /

# initramfs-tools reads the kernel config to check which compression is supported
cp /tmp/kernel.config /boot/config-4.9.119-BPI-W2-Kernel

# -- initramfs: the 4.9 kernel has no zstd, so gzip is required --------
echo "COMPRESS=gzip" > /etc/initramfs-tools/conf.d/compress.conf
# The initrd loads at 0x02200000 and the kernel at 0x03000000, only 14 MB
# apart. MODULES=most produces 27 MB and lands on the kernel; MODULES=dep
# cannot determine the root device from inside a container. Use list with an
# empty list for a minimal initrd -- SD/MMC and ext4 are built into this
# kernel, so root mounts without any modules.
echo "MODULES=list"  > /etc/initramfs-tools/conf.d/modules.conf

# -- system configuration ---------------------------------------------
# /etc/hostname, /etc/hosts and /etc/resolv.conf are NOT written here: docker
# bind-mounts all three into the container, so writes inside are not part of
# the image layer and docker export emits the empty files underneath. They are
# added after the export instead (see below).

# Use LABEL rather than device nodes: the same image may boot from SD
# (mmcblk0) or USB (sda), and a hardcoded node would fail to mount in the
# other case and drop to an emergency shell.
# /boot gets nofail so boot does not stall 90 seconds waiting for it.
cat > /etc/fstab <<EOF
LABEL=BPI-ROOT  /      ext4  defaults,noatime         0 1
LABEL=BPI-BOOT  /boot  vfat  defaults,noatime,nofail  0 2
EOF
mkdir -p /boot

# Serial console: the BSP bootargs specify ttyS0,115200
systemctl enable serial-getty@ttyS0.service

# Networking: DHCP, with systemd-networkd matching the interface name
mkdir -p /etc/systemd/network
cat > /etc/systemd/network/10-wired.network <<EOF
[Match]
Type=ether

[Network]
DHCP=yes
EOF
systemctl enable systemd-networkd systemd-resolved systemd-timesyncd ssh

# Convenient for debugging: allow root password login
echo "root:pikvm" | chpasswd
sed -i "s/^#\?PermitRootLogin.*/PermitRootLogin yes/" /etc/ssh/sshd_config

sed -i "s/^# en_US.UTF-8/en_US.UTF-8/" /etc/locale.gen
locale-gen >/dev/null
ln -sf /usr/share/zoneinfo/Asia/Taipei /etc/localtime

# systemd-firstboot.service (ConditionFirstBoot=yes, so it runs when
# machine-id is empty) passes --prompt-locale --prompt-keymap
# --prompt-timezone --prompt-root-password with StandardInput=tty. It skips
# anything already configured, but with no keymap set it stops on the serial
# console waiting for input and the boot hangs. Locale, timezone and the root
# password are all set above, so add the keymap.
#
# Note: /etc/vconsole.conf here is a symlink to /etc/default/keyboard, and
# keyboard-configuration is not installed, so it is a dangling symlink.
# Writing with echo > would follow it and create /etc/default/keyboard, which
# systemd can read but is semantically wrong. Remove the symlink first and
# write a real file.
rm -f /etc/vconsole.conf
echo "KEYMAP=us" > /etc/vconsole.conf

# -- remove what docker leaves behind ---------------------------------
# /.dockerenv: the systemd detect_container() call checks for this file.
# PID 1 decides the whole system is a container and skips every unit carrying
# ConditionVirtualization=!container. (On real hardware that is exactly how
# systemd-timesyncd and fstrim.timer got skipped, leaving the clock in 2014.)
# policy-rc.d: the docker debian image uses it to stop services starting during
# package installation (exit 101). Left in the image, no service from any
# package the user installs later would ever start.
rm -f /.dockerenv /usr/sbin/policy-rc.d

# machine-id must be empty so systemd generates a unique one per machine on
# the first boot. Keeping the build-time value would make every flashed card
# share one machine-id -- and systemd-networkd derives its DHCP identifier
# from it.
: > /etc/machine-id
ln -sf /etc/machine-id /var/lib/dbus/machine-id

# -- overlay: files this project ships --------------------------------
# Must come after kvmd is installed so it can replace the upstream
# /etc/kvmd/override.yaml and add the platform /usr/lib/kvmd/main.yaml.
#
# cp -a must not be used: the overlay is copied in from the host, and cp -a
# would carry the builder uid/gid and umask onto existing directories like
# /etc, /usr, /usr/lib and /etc/systemd/system (leaving them 1000:1000 775),
# after which systemd-tmpfiles fails en masse with "Detected unsafe path
# transition". Instead: only ensure the directories exist, and install each
# file individually as root.
(cd /tmp/overlay && find . -mindepth 1 -type d -printf "%P\n") | while read -r d; do
    mkdir -p "/$d"
done
(cd /tmp/overlay && find . ! -type d -printf "%P\n") | while read -r f; do
    if [ -x "/tmp/overlay/$f" ]; then m=755; else m=644; fi
    install -o root -g root -m "$m" "/tmp/overlay/$f" "/$f"
done
systemctl enable bpikvm-firstboot.service

# -- generate an initramfs for the BSP kernel -------------------------
update-initramfs -c -k 4.9.119-BPI-W2-Kernel
ls -l /boot/initrd.img-4.9.119-BPI-W2-Kernel

apt-get clean
rm -rf /var/lib/apt/lists/*
rm -rf /tmp/pikvm-build

# Backstop: the kvmd system accounts sit around uid 980 and nobody is 65534, so
# anything else >= 1000 must have come in from the host -- which is exactly
# how the ownership of / and /usr got broken before.
# Skip /tmp (the read-only bind mounts of the sources and overlay, which
# legitimately carry host uids and are not exported by docker export) and
# /sys /proc /dev (pseudo-filesystem mount points, overwritten at boot).
bad=$(find / -xdev \( -path /tmp -o -path /sys -o -path /proc -o -path /dev \) -prune -o \
        \( \( -uid +999 -o -gid +999 \) ! -uid 65534 ! -gid 65534 \) \
        -printf "%u:%g %p\n" 2>/dev/null | head -5)
if [ -n "$bad" ]; then
    echo "!!! rootfs contains files owned by the builder uid/gid:"
    echo "$bad"
    exit 1
fi

# The image must never carry any SSH credential.
#
# authorized_keys: every card flashed from the image would accept the same
#                  private key -- a backdoor.
# ssh_host_*_key : every card would share one host identity and could be
#                  impersonated; the correct place to create them is
#                  bpikvm-firstboot at boot.
#
# This is an assertion rather than "remember not to add one": if anybody later
# drops a key into the overlay or a build script, the build fails outright
# instead of quietly producing a backdoored card.
keys=$(find / -xdev \( -path /tmp -o -path /sys -o -path /proc -o -path /dev \) -prune -o \
        \( -name "authorized_keys" -o -name "authorized_keys2" \
           -o -name "ssh_host_*_key" -o -name "id_rsa" -o -name "id_ecdsa" \
           -o -name "id_ed25519" \) -type f -print 2>/dev/null | head -5)
if [ -n "$keys" ]; then
    echo "!!! rootfs contains SSH credentials, which must not go into the image:"
    echo "$keys"
    exit 1
fi
'

echo ">>> exporting the rootfs"
docker export "$CID" -o "$BUILD/rootfs.tar"
docker rm -f "$CID" >/dev/null

# docker bind-mounts /etc/{hostname,hosts,resolv.conf} into the container, so
# writes to those three never reach the image layer and docker export emits
# the empty files underneath.
# Symptoms: hostname becomes localhost, /etc/hosts is empty, DNS is dead.
#
# So the correct contents are appended to the tar after the export. Only
# --append, never --delete: when tar extracts, later entries overwrite earlier
# ones (a symlink replacing a regular file works fine), whereas --delete
# rewrites the whole archive and was measured to corrupt it
# (Skipping to next header).
echo ">>> fixing the /etc files docker bind-mounted away"
STAGE="$BUILD/etc-fix"
rm -rf "$STAGE"; mkdir -p "$STAGE/etc"
echo "bpi-w2-pikvm" > "$STAGE/etc/hostname"
cat > "$STAGE/etc/hosts" <<EOF
127.0.0.1   localhost
127.0.1.1   bpi-w2-pikvm
::1         localhost ip6-localhost ip6-loopback
EOF
# systemd-resolved creates stub-resolv.conf at runtime
ln -sf ../run/systemd/resolve/stub-resolv.conf "$STAGE/etc/resolv.conf"
tar --append -f "$BUILD/rootfs.tar" -C "$STAGE" \
    --owner=0 --group=0 --numeric-owner --mode=644 \
    etc/hostname etc/hosts etc/resolv.conf
rm -rf "$STAGE"
echo ">>> done: $BUILD/rootfs.tar ($(du -h "$BUILD/rootfs.tar" | cut -f1))"
