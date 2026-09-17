#!/bin/bash
# Build and install the PiKVM userspace (ustreamer + kvmd) inside the arm64
# rootfs container.
#
# scripts/build-rootfs.sh mounts this into the container and runs it; do not
# run it on the host. The sources are mounted read-only at /tmp/src and copied
# to /tmp/pikvm-build before building.
#
# The install layout deliberately matches kvmd's upstream (Arch) PKGBUILD:
# everything lives under /usr, because configs/os/services/*.service hardcode
# /usr/bin/kvmd*. Debian's pip/installer default to /usr/local, so
# DEB_PYTHON_INSTALL_LAYOUT=deb switches to the deb_system scheme
# (/usr/lib/python3/dist-packages + /usr/bin).
set -euo pipefail

SRC=/tmp/src
BUILD=/tmp/pikvm-build

[ -d "$SRC/kvmd" ] && [ -d "$SRC/ustreamer" ] || {
    echo "$SRC/{kvmd,ustreamer} not found -- run make sources first" >&2
    exit 1
}

export DEB_PYTHON_INSTALL_LAYOUT=deb

# arm64 runs under binfmt/qemu emulation, and the emulated python3
# occasionally SIGSEGVs for no reason
# (observed as py3compile reporting status code -11, and python -m build
# reporting "Backend subprocess exited when trying to invoke
# get_requires_for_build_wheel").
# Re-running usually succeeds, so the build steps are wrapped in a retry.
retry() {
    local i
    for i in 1 2 3; do
        "$@" && return 0
        echo "!!! failed (possibly an intermittent SIGSEGV under qemu), retry $i: $*" >&2
    done
    return 1
}

rm -rf "$BUILD"
mkdir -p "$BUILD"
cp -a "$SRC/ustreamer" "$SRC/kvmd" "$BUILD/"

# ─────────────────────────────────────────────────────────────────────
# ustreamer
# ─────────────────────────────────────────────────────────────────────
# WITH_PYTHON=1 also builds the memsink module kvmd imports.
# No WITH_JANUS: the kvmd-media / janus (WebRTC) path is not enabled here.
# No WITH_GPIO: there are no GPIO indicator LEDs wired.
echo ">>> building ustreamer"
retry make -C "$BUILD/ustreamer" -j"$(nproc)" WITH_PYTHON=1 PREFIX=/usr
retry make -C "$BUILD/ustreamer" WITH_PYTHON=1 PREFIX=/usr install-strip

ustreamer --version
python3 -c "import ustreamer; print('ustreamer python module:', ustreamer.__file__)"

# ─────────────────────────────────────────────────────────────────────
# kvmd -- what follows mirrors package_kvmd() from the upstream PKGBUILD
# ─────────────────────────────────────────────────────────────────────
echo ">>> building the kvmd wheel"
cd "$BUILD/kvmd"
rm -rf dist build
retry python3 -m build --wheel --no-isolation -o dist
retry python3 -m installer dist/*.whl

echo ">>> installing kvmd's helpers and configuration"
install -Dm755 -t /usr/bin scripts/kvmd-{bootconfig,gencert,certbot,update-switch}
install -Dm755 -t /usr/lib/kvmd scripts/kvmd-{udev-flash-pico,ucamera-prepare}

install -dm755 /usr/lib/systemd/system
cp -rd configs/os/services -T /usr/lib/systemd/system

install -DTm644 configs/os/sysusers.conf /usr/lib/sysusers.d/kvmd.conf
install -DTm644 configs/os/tmpfiles.conf /usr/lib/tmpfiles.d/kvmd.conf
install -DTm644 configs/os/sysctl.conf   /usr/lib/sysctl.d/99-kvmd.conf
install -DTm644 configs/os/udev/common.rules /usr/lib/udev/rules.d/99-kvmd-common.rules

# The MSD / PST remount helpers need sudo. msd is currently disabled, but
# installing this now does no harm and saves doing it later.
install -DTm440 configs/os/sudoers/v2-hdmi /etc/sudoers.d/99_kvmd

mkdir -p /usr/share/kvmd
cp -r firmware hid web extras contrib/keymaps /usr/share/kvmd
find /usr/share/kvmd/web -name '*.pug' -delete

CFG_DEFAULT=/usr/share/kvmd/configs.default
mkdir -p "$CFG_DEFAULT"
cp -r configs/* "$CFG_DEFAULT"

find /usr/share/kvmd -name ".gitignore" -delete
find "$CFG_DEFAULT" -type f -exec chmod 444 {} +
chmod 400 "$CFG_DEFAULT/kvmd"/*passwd
chmod 400 "$CFG_DEFAULT/kvmd"/*.secret
chmod 750 "$CFG_DEFAULT/os/sudoers"
chmod 400 "$CFG_DEFAULT/os/sudoers"/*

mkdir -p /etc/kvmd/nginx/ssl /etc/kvmd/vnc/ssl
chmod 755 /etc/kvmd/nginx /etc/kvmd/vnc /etc/kvmd/nginx/ssl /etc/kvmd/vnc/ssl
install -Dm444 -t /etc/kvmd/nginx "$CFG_DEFAULT/nginx"/*.conf*
chmod 644 /etc/kvmd/nginx/nginx.conf.mako /etc/kvmd/nginx/ssl.conf

mkdir -p /etc/kvmd/janus
chmod 755 /etc/kvmd/janus
install -Dm444 -t /etc/kvmd/janus "$CFG_DEFAULT/janus"/*.jcfg

install -Dm644 -t /etc/kvmd "$CFG_DEFAULT/kvmd"/*.yaml
install -Dm600 -t /etc/kvmd "$CFG_DEFAULT/kvmd"/*passwd
install -Dm600 -t /etc/kvmd "$CFG_DEFAULT/kvmd"/*.secret
install -Dm644 -t /etc/kvmd "$CFG_DEFAULT/kvmd"/web.css
mkdir -p /etc/kvmd/override.d

mkdir -p /var/lib/kvmd/msd /var/lib/kvmd/pst
chmod 1775 /var/lib/kvmd/pst

# ─────────────────────────────────────────────────────────────────────
# Mirrors post_install() from upstream's kvmd.install
# ─────────────────────────────────────────────────────────────────────
echo ">>> creating the kvmd users and groups"
systemd-sysusers /usr/lib/sysusers.d/kvmd.conf

# https://github.com/systemd/systemd/issues/13522
for user in $(awk '/^u /{print $2}' /usr/lib/sysusers.d/kvmd.conf); do
    usermod --expiredate= "$user" >/dev/null
done

chown kvmd:kvmd           /etc/kvmd/htpasswd /etc/kvmd/totp.secret
chown kvmd-ipmi:kvmd-ipmi /etc/kvmd/ipmipasswd
chown kvmd-vnc:kvmd-vnc   /etc/kvmd/vncpasswd
chmod 600 /etc/kvmd/*passwd
chown kvmd         /var/lib/kvmd/msd
chown kvmd-pst:kvmd-pst /var/lib/kvmd/pst

# TLS certificates and SSH host keys are not generated at build time -- that
# would make every flashed card share one private key.
# bpikvm-firstboot.service creates them on the first boot instead.
rm -f /etc/ssh/ssh_host_*

# ─────────────────────────────────────────────────────────────────────
# Services
# ─────────────────────────────────────────────────────────────────────
# Debian's nginx.service would occupy port 80 and clash with kvmd-nginx.
systemctl disable nginx.service
# kvmd-otg creates the USB gadget (keyboard + mouse) before kvmd starts.
systemctl enable kvmd.service kvmd-nginx.service kvmd-otg.service

echo ">>> PiKVM userspace installed"
