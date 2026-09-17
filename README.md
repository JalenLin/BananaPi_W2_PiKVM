# BPI-W2 PiKVM

[PiKVM](https://github.com/pikvm/pikvm) ported to the Banana Pi BPI-W2
(Realtek RTD1296), using **the board's own HDMI INPUT** for video capture.
The deliverable is an SD card image you can flash and boot.

Status: **1080p60 HDMI IN capture, EDID served over DDC, USB HID
(keyboard + mouse), kvmd and the stock PiKVM web UI — all verified on real
hardware.**

It matters *how* each thing was verified, so:

| Item | How it was verified |
|------|---------------------|
| Everything below | **The shipping image was flashed to a blank card and booted** (2026-09-17). The English-only rebuild that followed was flashed and checked too |
| Security posture | No `/dev/mem` in the image, zero `authorized_keys`, SSH host keys and TLS certs generated per-board on first boot |
| HDMI capture, EDID | 1920x1080@60, `Status: Ready`. The source negotiating 1080p60 *is* the proof that DDC works — without EDID it falls back to 1024x768 |
| Picture correctness | Snapshot decoded and checked pixel-wise: 1920×1080, **zero corrupted rows**. Not just "the JPEG was the right size" |
| Streaming performance | **22–27 fps delivered** with moving video on the source, 0.083 s per frame encode, 58% CPU (40% idle). `--workers=3` measured better than 4 — a 4th worker fights the capture and HTTP threads and costs ~10% |
| USB HID against a real PC | Verified with the Type-C port connected to a target machine |
| Card expansion, first-boot keys | root grown to fill the card; 3 SSH host keys + TLS cert all generated on the board |
| Boot integrity | No failed units; kvmd / kvmd-nginx / kvmd-otg / timesyncd all active; NTP synchronised |

Virtual media (MSD) is limited by the SoC's USB endpoint budget and ATX is
not wired up, so neither is enabled.

---

## Building

You need `docker`, `git` and `bash` on the host. Everything compiles inside
containers.

```sh
make all
```

= `builder → sources → kernel → uboot → rootfs → image`

Produces `build/bpiw2-pikvm.img`. Flash it with:

```sh
sudo dd if=build/bpiw2-pikvm.img of=/dev/sdX bs=4M conv=fsync status=progress
```

Individual steps:

| Command | What it does |
|---------|--------------|
| `make builder` | Build the docker image used for compiling |
| `make sources` | Fetch upstream sources (BSP / kvmd / ustreamer) and apply `patches/` |
| `make kernel` | Build kernel + dtb + modules |
| `make uboot` | Build u-boot |
| `make rootfs` | Build the Debian 13 arm64 rootfs (with ustreamer + kvmd) |
| `make image` | Assemble the SD image |

The arm64 rootfs is built under binfmt/qemu emulation. It is slow, and the
emulated Python occasionally SIGSEGVs for no reason — the scripts retry.

## After booting

| | Address / credentials |
|------|-------------|
| Web UI | `https://<board IP>/` — `admin` / `admin` |
| SSH / serial console | `root` / `pikvm` (serial is `ttyS0,115200`) |

**Change both default passwords before real use**
(`kvmd-htpasswd set admin`, `passwd`).

On the first boot `bpikvm-firstboot.service` does three things:

1. Grows the root partition and filesystem to fill the card
2. Generates SSH host keys unique to this board
3. Generates the KVMD TLS certificates

Keys are deliberately *not* baked into the image — otherwise every card
flashed from it would share one private key. The build refuses to produce an
image that contains any SSH credential.

Sanity checks:

```sh
hdmirx-info            # input timings and V4L2 caps
df -h /                # did the card get expanded
ls -l /dev/kvmd-hid-*  # USB HID gadget
```

### USB HID (keyboard / mouse)

Connect the board's **Type-C port** to the target machine and it enumerates
as a USB keyboard plus an absolute-positioning mouse. The Type-C port is
pinned to peripheral mode and can no longer act as a USB host (the board has
separate USB host ports).

The RTD1296's dwc3 only has 2 usable IN endpoints, so **keyboard + mouse
already fills the budget**:

- the relative-positioning mouse (`mouse_alt`) does not fit and is disabled
- virtual media (MSD) does not fit either. If you need MSD more than the
  mouse you can swap them — see the `msd` section of
  `/usr/lib/kvmd/main.yaml`

---

## What this project changes

Upstream sources live in `vendor/` and are **never edited in place**. Every
change is a patch under `patches/`, applied by
`scripts/prepare-sources.sh`.

Four fixes are what actually make HDMI IN usable:

1. **A HPD deadlock in the DTS.** BPI pointed `gpio-5v-detect` and
   `gpio-rx-hpd-ctrl` at the same pin in opposite directions, so the driver
   read back the level it was driving itself and concluded forever that no
   source was attached.
2. **The driver was missing the standard V4L2 entry points.** It had only
   ever been used by Android's tv_input HAL, so `G_FMT` / `TRY_FMT` /
   `ENUMINPUT` were absent, and an ioctl issued before the queue was
   initialised would oops.
3. **The DDC pins were never muxed.** HDMI-IN's SCL/SDA go to the SoC's
   `I2C6`, but the pinmux for those two pads stayed on `gpio`, so EDID was
   never served and sources fell back to 1024x768. This is also the real
   cause behind the "720p only" workaround people pass around on the BPI
   forum.
4. **`S_FMT` never wrote the accepted format back.** The driver left
   `bytesperline` untouched, so the caller got its own value returned.
   ustreamer assumes packed YUV and asks for double the stride, which
   squashed the top half of the picture to 2× height, turned the bottom half
   solid green, and then read past the mmap and SIGSEGV'd. `G_FMT` reported
   the correct value all along — only `S_FMT`'s return was wrong.

The full list is in `docs/06-changes.md`.

---

## Documentation

**If you want to know what was changed, start with
[`docs/06-changes.md`](docs/06-changes.md)** — it is the single source of
truth for the change list. The other documents cover *why*, and how things
were worked out.

| File | Contents | Kind |
|------|----------|------|
| [`docs/06-changes.md`](docs/06-changes.md) | **Every change, its reason, and the hardware verification results**, plus surveys of the SoC's hardware encoders (its §10) and its second Ethernet port (its §11) | Current state |
| [`docs/05-userspace.md`](docs/05-userspace.md) | rootfs build flow, install layout, dependencies, USB OTG, card expansion | Current state |
| [`docs/04-hdmi-rx-bringup.md`](docs/04-hdmi-rx-bringup.md) | The full debugging story behind the three HDMI RX blockers | Current state + process |
| [`docs/03-image-and-boot.md`](docs/03-image-and-boot.md) | Image layout, boot chain, u-boot's constraints | Current state |
| [`docs/02-decisions.md`](docs/02-decisions.md) | Kernel / rootfs / build environment choices, with later corrections | Decision record |
| [`docs/01-research-findings.md`](docs/01-research-findings.md) | Fact-finding and evidence gathered before any code was written | **Snapshot as of 2026-09-04** |
| [`docs/08-kernel-uplift.md`](docs/08-kernel-uplift.md) | Not chasing the latest: which LTS is worth targeting, and an existing port to crib from | Analysis + plan |
| [`docs/07-mainline.md`](docs/07-mainline.md) | What blocks a move to current mainline | Analysis |
| [`docs/refs/`](docs/refs/) | The official BPI-W2 schematic and where it came from | Reference |

First time here? Read README → `06-changes` → whichever detail chapter
interests you.

## Layout

```
patches/        changes against upstream (kernel / ustreamer / kvmd)
scripts/        build and debug scripts
overlay/        files this project ships into the rootfs
tools/          debug programs used during development; not in the image
docs/           documentation
vendor/         upstream sources (fetched by make sources; do not edit)
build/          build outputs
```

## Hardware notes

- The board's MAC address **changes on every boot**, so its IP changes too.
- `reboot` works when booting from the on-board micro SD. An early note
  claiming it hangs at `wait rtk_check_system_ready_to_suspend` was wrong —
  that line also appears in successful reboots. The real cause back then was
  booting via a card reader and LK's manual USB boot, which drops back to LK
  and waits for a human.
- The SD card slot makes poor contact; occasionally the card is not detected.
- **Only one of the two Ethernet sockets works.** The second one is an
  RTL8211F on RGMII0 belonging to the hwnat block, whose DT node is disabled
  and whose driver exists in no public BSP. See §11 of
  `docs/06-changes.md`.
- **Capture buffers are mapped uncached.** Anything that touches the V4L2
  mmap buffer must `memcpy` it out before working on it. Reading it byte by
  byte is ten times slower — ustreamer's JPEG encoder hit exactly this and a
  single 1080p frame went from 79 ms to 830 ms.
- After an OTG test the board once fell into a boot loop, resetting hard at
  the instant initramfs started. The diagnosis is in `docs/06-changes.md`
  §8; unplug the Type-C cable and power-cycle.
- SW4: `0` boots from eMMC, `1` boots from SPI + SD.
- **There is no RTC battery.** Every boot starts in 2014 until
  `systemd-timesyncd` syncs. Until then apt fails because signatures are
  "not valid yet".
- The serial console gets flooded by the Realtek audio driver's
  `[AO][_AO_if_video_HDMI_mode]HDMI not enabled`. `dmesg -n 1` quiets it.

## Branches

| Ref | What it is |
|-----|------------|
| `main` | The BSP 4.9.119 line. This is the working deliverable and what the documentation describes |
| `v1.0-bsp4.9` (tag) | An immutable snapshot of the verified BSP 4.9 state |
| `mainline` | Research towards a mainline/LTS kernel. Diverges heavily and is not expected to be usable until it reaches M5 in `docs/08-kernel-uplift.md` |

The two kernel lines are kept apart deliberately. `main` stays functional
while `mainline` is worked on, and until that work passes M5 (HDMI capture)
switching to it would be a functional regression -- see section 7 of
`docs/08-kernel-uplift.md`.

## Licensing and upstreams

- Kernel: [BPI-SINOVOIP/BPI-W2-bsp](https://github.com/BPI-SINOVOIP/BPI-W2-bsp) (GPL-2.0)
- [pikvm/kvmd](https://github.com/pikvm/kvmd) (GPL-3.0) v4.213
- [pikvm/ustreamer](https://github.com/pikvm/ustreamer) (GPL-3.0) v6.66
