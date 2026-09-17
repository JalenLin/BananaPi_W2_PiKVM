# Userspace: ustreamer and kvmd

**This document covers how things were done and why** — the rootfs build
flow, the install layout, dependencies, and the mechanics of USB OTG and card
expansion. The item-by-item change list is in `06-changes.md`.

The rootfs produced by `make rootfs` already contains a complete, working
PiKVM userspace. Flash the card, boot, and it runs; nothing has to be
installed by hand.

## Pinned versions

`scripts/prepare-sources.sh` fetches three upstream trees into `vendor/`:

| Source | ref | Note |
|--------|-----|------|
| `BPI-SINOVOIP/BPI-W2-bsp` | `master` | No tags exist, so master is the only option |
| `pikvm/ustreamer` | `v6.66` | Verified on hardware |
| `pikvm/kvmd` | `v4.213` | Verified on hardware |

The latter two are pinned so that repeated builds produce the same thing. To
move up a version, edit `KVMD_REF` / `USTREAMER_REF` at the top of
`prepare-sources.sh`, re-run `make sources`, and check that the patches under
`patches/kvmd` and `patches/ustreamer` still apply.

Every run does `git checkout -- .` to restore the working tree before
applying patches, so it is idempotent. It deliberately **does not
`git clean`** — the BSP is an in-tree build, and removing untracked files
would throw away the entire kernel build.

## Build flow

`scripts/build-rootfs.sh` does everything inside an arm64 container (under
binfmt emulation):

1. Install the base system and PiKVM's runtime dependencies (all from apt,
   nothing via pip)
2. Install build packages → run `scripts/rootfs-pikvm.sh` → purge the build
   packages again
3. Unpack the kernel modules and generate an initramfs
4. System configuration (hostname, fstab, networkd, locale, …)
5. Lay `overlay/` on top and enable `bpikvm-firstboot.service`

`scripts/rootfs-pikvm.sh` only runs inside the container; do not run it
directly on the host.

### Why it installs into `/usr` rather than `/usr/local`

kvmd's upstream `configs/os/services/*.service` files hardcode
`/usr/bin/kvmd` and `/usr/bin/kvmd-nginx-mkconf`. Debian's pip and
`python -m installer` default to the `posix_local` scheme and would install
into `/usr/local/lib/python3.13/dist-packages` and `/usr/local/bin`, where
the unit files cannot find the executables.

The fix is `export DEB_PYTHON_INSTALL_LAYOUT=deb`, which switches Debian's
python to the `deb_system` scheme:

```
/usr/lib/python3/dist-packages   ← packages
/usr/bin                          ← console_scripts
```

That matches the layout of the upstream Arch package, so no pile of symlinks
under `/usr/bin` is needed.

> Early hardware debugging used the default `/usr/local` plus hand-made
> symlinks. That approach is obsolete.

### What gets installed

`rootfs-pikvm.sh` deliberately mirrors `package_kvmd()` from kvmd's upstream
`PKGBUILD` and `post_install()` from `kvmd.install`:

| Path | Source |
|------|--------|
| `/usr/bin/ustreamer`, `ustreamer-dump` | `make PREFIX=/usr install-strip` |
| `/usr/lib/python3/dist-packages/ustreamer*.so` | `WITH_PYTHON=1`; kvmd's memsink imports it |
| `/usr/bin/kvmd*` + `/usr/lib/python3/dist-packages/kvmd` | the kvmd wheel |
| `/usr/lib/systemd/system/kvmd*.service` | `configs/os/services` |
| `/usr/lib/sysusers.d/kvmd.conf`, `tmpfiles.d`, `sysctl.d` | `configs/os` |
| `/usr/lib/udev/rules.d/99-kvmd-common.rules` | `configs/os/udev/common.rules` |
| `/etc/sudoers.d/99_kvmd` | `configs/os/sudoers/v2-hdmi` |
| `/usr/share/kvmd/{web,hid,extras,keymaps,firmware}` | Web UI and data |
| `/usr/share/kvmd/configs.default` | the whole of `configs/` |
| `/etc/kvmd/*` | installed from `configs.default`, permissions per upstream |

ustreamer build options:

- `WITH_PYTHON=1` — kvmd needs `import ustreamer` (memsink)
- no `WITH_JANUS` — the kvmd-media / WebRTC path is not enabled on this
  platform
- no `WITH_GPIO` — no GPIO indicator LEDs are wired

## Dependencies

kvmd upstream is an Arch package. `build-rootfs.sh` maps its `depends` onto
the equivalently named Debian packages, all from apt, with nothing pulled by
pip — that avoids fighting the system python.

A few things learned the hard way:

- **`libevent-2.1-7t64` must be listed explicitly.** ustreamer links against
  `libevent-2.1.so.7`. With only `libevent-core` and `libevent-pthreads`
  installed, `apt-get autoremove` treats `libevent-2.1-7t64` as an orphan and
  removes it, and ustreamer then dies at boot with
  `error while loading shared libraries`.
- **Build tools must be purged by name, one at a time.** `gcc → cpp →
  cpp-<arch> → gcc` forms a dependency cycle, so purging only
  `build-essential` makes apt's autoremove conservatively keep the whole
  chain — 100 MB of dead weight. `BUILD_DEPS` therefore lists `gcc g++ cpp
  make dpkg-dev libc6-dev binutils` individually.
- **`python3-pyusb` is called `python3-usb` in Debian.**

Installed but currently unused — the corresponding services are not enabled,
but installing them now saves doing it later: `python3-pyghmi` (IPMI),
`python3-pyrad` (RADIUS auth), `python3-ldap`, `python3-smbc`,
`python3-paramiko` (remote MSD sources).

Deliberately not installed:

- `python3-luma.core` / `python3-luma.oled` — there is no OLED on the board
  and `kvmd-oled` is not enabled.
- `tesseract` — an optdepend upstream (on-screen OCR). Without it kvmd emits
  a single `Can't load libtesseract` RuntimeWarning at startup, which is
  expected.

## What this project overrides (`overlay/`)

| File | Purpose |
|------|---------|
| `usr/lib/kvmd/main.yaml` | Platform configuration, equivalent to upstream's `kvmd-platform-*` package |
| `usr/lib/kvmd/platform` | Model string, also from `kvmd-platform-*` |
| `etc/kvmd/override.yaml` | Local overrides (points `vcgencmd_cmd` at `bpikvm-vcgencmd`) |
| `usr/lib/udev/rules.d/99-kvmd-bpi-w2.rules` | Creates `/dev/kvmd-video` based on `hdmirx_video_info` |
| `usr/local/bin/hdmirx-info`, `hdmirx-capture` | Debug tools |
| `usr/local/bin/bpikvm-firstboot` + service | Generates per-board keys on first boot |
| `etc/systemd/system/bpikvm-ustreamer.service` | Test service that runs ustreamer standalone (not enabled by default) |

The overlay must be applied **after** kvmd is installed, otherwise
`/etc/kvmd/override.yaml` gets overwritten by upstream's defaults.

The overlay also **must not be copied with `cp -a`**: that carries the
builder's uid/gid and umask onto existing directories like `/etc`, `/usr`,
`/usr/lib` and `/etc/systemd/system` (leaving them `1000:1000 775`), and at
boot systemd-tmpfiles fails en masse with `Detected unsafe path transition`.
`build-rootfs.sh` instead `mkdir -p`s the directories and installs each file
with `install -o root -g root`, then sweeps the whole rootfs to confirm no
file carries a uid/gid ≥ 1000. `modules.tar` must likewise be created with
`--owner=0 --group=0`, or unpacking it chowns `/usr/lib` along the way.

### A wrong key in override.yaml produces no warning whatsoever

kvmd **silently ignores** override keys it does not recognise. We initially
put `vcgencmd_cmd` under `kvmd.info.health.` (the correct path is
`kvmd.info.hw.`). The setting had no effect at all, the board accumulated
7965 lines of `FileNotFoundError: '/usr/bin/vcgencmd'`, and kvmd itself said
nothing.

After changing an override, always confirm with `kvmd -M` — it prints only
the fields that differ from the defaults, so `{}` means the entire override
was ignored. The correct key paths are in `kvmd/apps/_scheme.py`.

### Platform constraints encoded in `main.yaml`

- `hid: otg` — the RTD1296's Type-C port runs dwc3 in dual-role
- `atx: disabled`, `msd: disabled` — not wired / not verified
- streamer uses `--format=NV16` — rtk_hdmirx only emits NV16 / NV12 / BGR32,
  none of which ustreamer supports natively as a packed format. NV16 is the
  smallest option that does not lose vertical chroma resolution (requires
  `patches/ustreamer/0001-add-semi-planar-NV12-NV21-NV16-support.patch`)
- streamer hardcodes `--resolution=1920x1080` — the driver does not implement
  `VIDIOC_QUERY_DV_TIMINGS`, so `--dv-timings` is unavailable. The EDID we
  serve prefers 1080p60, so normal negotiation lands here; confirm with
  `hdmirx-info`
- no `--encoder=m2m-image` — none of the SoC's codec engines (JPU / VE1 /
  VE3) expose a V4L2 M2M interface; the BSP provides vendor ioctls instead,
  so encoding is on the CPU. `--workers=3` measured best (4 costs about
  10%). What hardware exists and what is missing is surveyed in §10 of
  `06-changes.md`

## Intermittent SIGSEGV under qemu emulation

The rootfs is built inside a binfmt/qemu-emulated arm64 container, and the
emulated `python3` occasionally SIGSEGVs for no reason. Two observed
symptoms:

```
Exception: ('python3.13', '-c', '...') failed with status code -11
ERROR Backend subprocess exited when trying to invoke get_requires_for_build_wheel
```

Re-running usually succeeds, so both `apt-get install` and the build steps
are wrapped in retries (`apt_install()` in `build-rootfs.sh`, `retry()` in
`rootfs-pikvm.sh`). When a build fails, check for this before hunting for a
real dependency problem.

## USB OTG (HID)

`kvmd-otg.service` creates the USB gadget before kvmd starts. It needs a UDC
to exist, which is what `patches/kernel/0006` provides by pinning the Type-C
port to peripheral mode.

The RTD1296's dwc3 has only **2 IN + 2 OUT** usable endpoints, and kernel
4.9's `f_hid` always takes 1 IN + 1 OUT per function, so at most two
functions fit:

- default: keyboard + absolute-positioning mouse
- neither `mouse_alt` (relative positioning) nor MSD fits

`main.yaml` handles this with three settings:

```yaml
kvmd:
    hid:
        type: otg
        mouse_alt:
            device: ""      # do not even create hid.usb2
otg:
    endpoints: 2            # let kvmd skip functions that will not fit
```

`otg.endpoints` matters: without it kvmd adds all three HIDs to the config,
the third `hidg_bind` fails, and that takes **the entire gadget** down with
it — no HID at all.

The udev rule (`99-kvmd-bpi-w2.rules`) links `hidg0` / `hidg1` to the names
kvmd expects, `/dev/kvmd-hid-keyboard` and `/dev/kvmd-hid-mouse`. There is
no `hidg2`.

## Card expansion

`bpikvm-expand-rootfs` grows the root partition and filesystem to fill the
card. It is invoked by `bpikvm-firstboot.service` on the first boot and can
be re-run by hand.

root is the last partition in the image, so this works online: `growpart`
rewrites the partition table, `partx -u` tells the kernel, and `resize2fs`
grows the mounted ext4 — no reboot needed.

The device name is not hardcoded (`findmnt` plus `lsblk -no PKNAME`), because
the same image might boot from SD (`mmcblk0p2`) or USB (`sda2`).

Mind `growpart`'s exit codes: **0 = changed, 1 = no change needed (NOCHANGE),
2 = failed**. You cannot rely on `set -e`'s default behaviour here.

## First boot

`bpikvm-firstboot.service` runs once, before `ssh.service` and
`kvmd-nginx.service`:

- `bpikvm-expand-rootfs` grows root to fill the card
- `ssh-keygen -A` generates the SSH host keys
- `kvmd-gencert` generates the KVMD-Nginx and KVMD-VNC TLS certificates

Expansion runs first, but a failure there does not block key generation —
without keys you cannot reach the machine at all, whereas an unexpanded card
merely has less space.

**These keys are deliberately not baked into the image** — otherwise every
card flashed from it would share one private key. The build deletes the host
keys that openssh's installation generates, and asserts that no SSH
credential survives into the image.

When it finishes it writes `/var/lib/bpikvm/firstboot-done` and never runs
again.

## Default credentials

| Purpose | User | Password |
|---------|------|----------|
| Web UI | `admin` | `admin` (kvmd upstream's default) |
| SSH / console | `root` | `pikvm` |

Change both before real use:

```
kvmd-htpasswd set admin
passwd
```

## Enabled services

| Service | State | Note |
|---------|-------|------|
| `kvmd` | enabled | The main daemon |
| `kvmd-nginx` | enabled | HTTP entry point (80/443) |
| `kvmd-otg` | enabled | Creates the USB gadget (keyboard + mouse) |
| `systemd-timesyncd` | enabled | The board has no RTC battery and boots in 2014 |
| `nginx` (Debian's) | **disabled** | Would occupy port 80 and clash with kvmd-nginx |
| `bpikvm-firstboot` | enabled | Runs once |
| `bpikvm-ustreamer` | not enabled | Standalone testing only; normally kvmd manages the streamer |

The other `kvmd-*` units (ipmi, vnc, janus, media, otg, pst, oled, …) are
installed but not enabled.

## What `docker export` drags in

The rootfs comes out of `docker export`, which carries several
docker-specific things into the image. `build-rootfs.sh` deals with all of
them (details in "Pitfalls fixed along the way" in `06-changes.md`):

| Artifact | Effect |
|----------|--------|
| `/etc/{hostname,hosts,resolv.conf}` are bind mounts | The export contains empty files → hostname becomes `localhost` and DNS breaks |
| `/.dockerenv` | systemd decides the whole system is a container and skips every unit with `ConditionVirtualization=!container` |
| `/usr/sbin/policy-rc.d` | Services from packages installed later never start |
| `/etc/machine-id` has a fixed value | Every card shares one machine identifier |

## Not done yet

The full list is in section 9 of `06-changes.md`. The parts that touch
userspace directly:

- MSD and ATX are still `disabled`
- `kvmd.streamer.forever` stays at upstream's default (on-demand)
