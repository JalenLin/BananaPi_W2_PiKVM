#!/bin/bash
# Turn an Arch Linux ARM (aarch64) rootfs into the PiKVM userspace for the
# mainline kernel line. scripts/build-rootfs-arch.sh runs this inside the
# arm64 container; do not run it on the host.
#
# What comes from where:
#   - Arch Linux ARM: the base system.
#   - PiKVM's own repository (rpi4-aarch64): Janus (janus-gateway-pikvm) and
#     the packages kvmd depends on that Arch does not have.
#   - Built here from vendor/ with our patches: ustreamer and kvmd, packaged
#     with their upstream PKGBUILDs and kept back from upgrades (IgnorePkg),
#     so that `pacman -Syu` cannot replace them with unpatched versions.
#   - Not installed: a kernel. u-boot boots our 6.18 kernel with the BSP's
#     initramfs; build-image.sh puts the modules and firmware in.
#   - Not installed either: a kvmd-platform-* package -- none of them is for
#     this board. overlay/ carries the equivalent (/usr/lib/kvmd/main.yaml,
#     udev rules, platform file).
set -euxo pipefail

SRC=/tmp/src
PIKVM_REPO="https://files.pikvm.org/repos/arch/rpi4-aarch64"
# From mdevaev/pi-builder, which PiKVM OS is built with
PIKVM_REPO_KEY=912C773ABBD1B584

# -- pacman -----------------------------------------------------------
# A container has no free-space information pacman trusts, and pacman 7's
# download sandbox needs Landlock, which qemu-user does not provide. PiKVM's
# own builder turns off the same two things.
sed -i -e "s/^CheckSpace/#CheckSpace/" -e "s/^#DisableSandbox/DisableSandbox/" /etc/pacman.conf
grep -q "^DisableSandbox" /etc/pacman.conf || sed -i "/^\[options\]/a DisableSandbox" /etc/pacman.conf
# Our patched builds; see the top of this file
sed -i "/^\[options\]/a IgnorePkg = kvmd ustreamer" /etc/pacman.conf

pacman-key --init
pacman-key --populate archlinuxarm
for ks in hkps://keyserver.ubuntu.com:443 hkps://keys.openpgp.org:443 hkps://pgp.mit.edu:443; do
    pacman-key --keyserver "$ks" -r "$PIKVM_REPO_KEY" && break
done
pacman-key --lsign-key "$PIKVM_REPO_KEY"
cat >> /etc/pacman.conf <<EOF

[pikvm]
Server = $PIKVM_REPO
SigLevel = Required DatabaseOptional
EOF

# qemu-user occasionally crashes a process; retry what is worth retrying
retry() {
    local i
    for i in 1 2 3; do
        "$@" && return 0
        echo "!!! failed (possibly a crash under qemu), retry $i: $*" >&2
    done
    return 1
}

# The kernel, its firmware and initramfs generator are not ours to use; out
# before the upgrade, which would otherwise fetch new ones first
pacman --noconfirm -Rns linux-aarch64
pacman -Qq | grep "^linux-firmware" | xargs -r pacman --noconfirm -Rns

retry pacman --noconfirm -Syu

# -- base system ------------------------------------------------------
retry pacman --noconfirm --needed -S \
    sudo openssh nano htop less busybox \
    v4l-utils usbutils pciutils i2c-tools alsa-utils \
    e2fsprogs dosfstools parted cloud-guest-utils \
    janus-gateway-pikvm

# -- ustreamer and kvmd from our patched sources ----------------------
# makepkg refuses to run as root
useradd -m builder
echo "builder ALL=(ALL) NOPASSWD: ALL" > /etc/sudoers.d/90-builder
retry pacman --noconfirm --needed -S base-devel git \
    python-build python-installer python-wheel python-setuptools \
    alsa-lib opus speexdsp
# The last three are for ustreamer's Janus plugin; upstream's PKGBUILD does
# not list speexdsp. Installed explicitly, so they stay after the build tools
# go.

# qemu-user crashes cc1 often enough that a whole package rarely builds in
# one go, and makepkg retries start from scratch. Retry single compiler runs
# that die of a signal or an internal compiler error (exit 4) instead. Only
# for the build; removed below.
for c in gcc cc; do
    cat > "/usr/local/bin/$c" <<EOF
#!/bin/sh
for i in 1 2 3 4 5 6; do
    /usr/bin/$c "\$@"
    rc=\$?
    if [ \$rc -lt 128 ] && [ \$rc -ne 4 ]; then exit \$rc; fi
    echo "$c: died (rc=\$rc) under qemu, retry \$i" >&2
done
exit \$rc
EOF
    chmod 755 "/usr/local/bin/$c"
done

PKG=/tmp/pkg
mkdir -p "$PKG/ustreamer" "$PKG/kvmd"
cp -a "$SRC/ustreamer" "$PKG/ustreamer/ustreamer"
cp -a "$SRC/kvmd" "$PKG/kvmd/kvmd-src"

# ustreamer: upstream's PKGBUILD, building the tree next to it instead of a
# git checkout. WITH_JANUS follows from the Janus headers being installed.
sed -e 's|^source=.*|source=()|' -e 's|^md5sums=.*|md5sums=()|' "$SRC/ustreamer/pkg/arch/PKGBUILD" > "$PKG/ustreamer/PKGBUILD"
grep -q 'cp -r $pkgname $pkgname-build' "$PKG/ustreamer/PKGBUILD"

# kvmd: upstream's PKGBUILD, the kvmd package only (no platform variants)
sed -e 's|^source=.*|source=()|' -e 's|^md5sums=.*|md5sums=()|' \
    -e '/^_variants=(/,/^)/c _variants=()' \
    "$SRC/kvmd/PKGBUILD" > "$PKG/kvmd/PKGBUILD"
cat >> "$PKG/kvmd/PKGBUILD" <<'EOF'

prepare() {
	rm -rf "$srcdir/kvmd-$pkgver"
	cp -a "$startdir/kvmd-src" "$srcdir/kvmd-$pkgver"
	# vendor/ may hold the outputs of earlier builds
	rm -rf "$srcdir/kvmd-$pkgver"/{build,dist} "$srcdir/kvmd-$pkgver"/*.egg-info
}
EOF
cp "$SRC/kvmd/kvmd.install" "$PKG/kvmd/"
# ustreamer's PKGBUILD copies $srcdir/ustreamer; point it at ours
cat >> "$PKG/ustreamer/PKGBUILD" <<'EOF'

prepare() {
	rm -rf "$srcdir/$pkgname"
	cp -a "$startdir/$pkgname" "$srcdir/$pkgname"
	# vendor/ may hold the objects of earlier builds, whose dependency files
	# name another distribution's headers
	make -C "$srcdir/$pkgname" clean
}
EOF
chown -R builder: "$PKG"

# makepkg -s would install the dependencies through sudo, and setuid does not
# work under qemu-user ("effective uid is not 0"). Install what the .SRCINFO
# lists as root instead; pacman -T tells which are not satisfied yet, and
# understands versions and provides (janus-gateway-pikvm provides
# janus-gateway).
install_deps() {
    local specs missing
    mapfile -t specs < <(cd "$1" && sudo -u builder makepkg --printsrcinfo \
                         | sed -n 's/^\t\(make\)\?depends = //p')
    missing=$({ pacman -T "${specs[@]}" || true; } | sed 's/[<>=].*//')
    if [ -n "$missing" ]; then
        # shellcheck disable=SC2086
        retry pacman --noconfirm --needed -S $missing
    fi
}

# ustreamer first: kvmd depends on it, and would otherwise get the unpatched
# one from the PiKVM repository.
install_deps "$PKG/ustreamer"
(cd "$PKG/ustreamer" && retry sudo -u builder makepkg --noconfirm)
pacman --noconfirm -U "$PKG"/ustreamer/ustreamer-*.pkg.tar.*
ls /usr/lib/ustreamer/janus/libjanus_ustreamer.so

install_deps "$PKG/kvmd"
(cd "$PKG/kvmd" && retry sudo -u builder makepkg --noconfirm)
pacman --noconfirm -U "$PKG"/kvmd/kvmd-[0-9]*.pkg.tar.*

# The Web UI's terminal (ttyd behind kvmd's nginx), as on PiKVM OS. After
# kvmd, so that its dependency on kvmd is already met by ours.
retry pacman --noconfirm --needed -S kvmd-webterm

# What a kvmd-platform-* package would add besides main.yaml (which, with
# the udev rules and the platform file, comes from overlay/)
CFG=/usr/share/kvmd/configs.default
install -DTm644 "$CFG/os/sysctl.conf" /usr/lib/sysctl.d/99-kvmd.conf
install -DTm644 "$CFG/os/udev/common.rules" /usr/lib/udev/rules.d/99-kvmd-common.rules
install -DTm440 "$CFG/os/sudoers/v2-hdmi" /etc/sudoers.d/99_kvmd

# The build tools go again
rm -f /usr/local/bin/gcc /usr/local/bin/cc
rm -f /etc/sudoers.d/90-builder
userdel -r builder
rm -rf "$PKG"
for p in base-devel git python-build python-installer python-wheel; do
    pacman --noconfirm -Rns "$p" || echo "keeping $p: something needs it"
done
{ pacman -Qdtq || true; } | xargs -r pacman --noconfirm -Rns

# -- system configuration ---------------------------------------------
# /etc/hostname, /etc/hosts and /etc/resolv.conf are bind-mounted by docker;
# build-rootfs-arch.sh adds them after the export.

# LABELs, not device nodes: the same image may boot from SD or USB. The
# third line is kvmd's MSD storage, a directory on the rootfs bind-mounted
# read-only (see /usr/lib/kvmd/main.yaml and patches/kvmd/0002).
cat > /etc/fstab <<EOF
LABEL=BPI-ROOT  /      ext4  defaults,noatime         0 1
LABEL=BPI-BOOT  /boot  vfat  defaults,noatime,nofail  0 2
/var/lib/kvmd/msd.data  /var/lib/kvmd/msd  none  bind,nodev,nosuid,noexec,ro,X-kvmd.otgmsd-user=kvmd  0 0
EOF
mkdir -p /boot /var/lib/kvmd/msd /var/lib/kvmd/msd.data

# DHCP on the wired port, and mDNS so a fresh board answers as
# bpi-w2-pikvm.local. Arch Linux ARM's own en*/eth* files go.
rm -f /etc/systemd/network/*.network
cat > /etc/systemd/network/10-wired.network <<EOF
[Match]
Type=ether

[Network]
DHCP=yes
MulticastDNS=yes
EOF
mkdir -p /etc/systemd/resolved.conf.d
cat > /etc/systemd/resolved.conf.d/10-mdns.conf <<EOF
[Resolve]
MulticastDNS=yes
EOF

# Root login with a password, as on PiKVM OS; no stock user
echo "root:pikvm" | chpasswd
sed -i "s/^#\?PermitRootLogin.*/PermitRootLogin yes/" /etc/ssh/sshd_config
userdel -r alarm || true

echo "en_US.UTF-8 UTF-8" > /etc/locale.gen
locale-gen
echo "LANG=en_US.UTF-8" > /etc/locale.conf
ln -sf /usr/share/zoneinfo/Asia/Taipei /etc/localtime
# systemd-firstboot would otherwise stop the boot asking for a keymap
echo "KEYMAP=us" > /etc/vconsole.conf

# -- overlay: files this project ships --------------------------------
# After kvmd, whose /etc/kvmd/override.yaml it replaces. Each file is
# installed on its own as root: copying the tree would carry the builder's
# uid and modes onto /etc, /usr and so on.
(cd /tmp/overlay && find . -mindepth 1 -type d -printf "%P\n") | while read -r d; do
    mkdir -p "/$d"
done
(cd /tmp/overlay && find . ! -type d -printf "%P\n") | while read -r f; do
    if [ -x "/tmp/overlay/$f" ]; then m=755; else m=644; fi
    install -o root -g root -m "$m" "/tmp/overlay/$f" "/$f"
done

systemctl enable systemd-networkd systemd-resolved systemd-timesyncd sshd \
    kvmd kvmd-nginx kvmd-otg kvmd-media kvmd-janus kvmd-webterm bpikvm-firstboot

# -- what must not be in an image -------------------------------------
# Keys: bpikvm-firstboot creates them on each board. kvmd.install has just
# generated TLS certificates, and Arch Linux ARM may carry SSH host keys.
rm -f /etc/ssh/ssh_host_* /etc/kvmd/nginx/ssl/* /etc/kvmd/vnc/ssl/*
# machine-id: each board must generate its own (networkd's DHCP identifier
# derives from it)
: > /etc/machine-id
# systemd treats the system as a container while this exists
rm -f /.dockerenv
# pacman's sandbox and space check work on the board; only qemu needed them off
sed -i -e "s/^#CheckSpace/CheckSpace/" -e "s/^\(DisableSandbox\)/#\1/" /etc/pacman.conf
pacman -Scc --noconfirm
rm -rf /tmp/pkg   # (/tmp/src holds the read-only source mounts)

bad=$(find / -xdev \( -path /tmp -o -path /sys -o -path /proc -o -path /dev \) -prune -o \
        \( \( -uid +999 -o -gid +999 \) ! -uid 65534 ! -gid 65534 \) \
        -printf "%u:%g %p\n" 2>/dev/null | head -5)
if [ -n "$bad" ]; then
    echo "!!! rootfs contains files owned by the builder uid/gid:"
    echo "$bad"
    exit 1
fi
keys=$(find / -xdev \( -path /tmp -o -path /sys -o -path /proc -o -path /dev \) -prune -o \
        \( -name "authorized_keys" -o -name "authorized_keys2" \
           -o -name "ssh_host_*_key" -o -name "id_rsa" -o -name "id_ecdsa" \
           -o -name "id_ed25519" \) -type f -print 2>/dev/null | head -5)
# kvmd's TLS keys, by path: "server.key" alone would also match test
# fixtures that packages ship (python-ldap's slapdtest, for one)
keys="$keys$(ls /etc/kvmd/*/ssl/*.key 2>/dev/null || true)"
if [ -n "$keys" ]; then
    echo "!!! rootfs contains private keys, which must not go into the image:"
    echo "$keys"
    exit 1
fi

pacman -Q kvmd ustreamer janus-gateway-pikvm kvmd-webterm
echo ">>> PiKVM userspace (Arch) installed"
