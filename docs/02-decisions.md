# Architecture decisions

## D1 — Kernel: BSP linux-rtk 4.9.119

See section 1 of `01-research-findings.md`. Mainline has no eMMC, network,
clk or pinctrl driver for rtd129x, every previous community porting attempt
failed, and the BSP carries roughly 1.94 million lines of Realtek
out-of-tree code.

CLAUDE.md's "prefer new, mainline code" is satisfied in userspace instead.

This judgement was re-checked against mainline v7.3-rc3 on 2026-09-17. The
conclusion did not change, and is now sharper: mainline's `rtd129x.dtsi` is
195 lines total (CPUs / GIC / timer / a single 27 MHz fixed clock / syscons
/ resets / watchdog / 3 UARTs) and does not even carry an SMP
`enable-method`. The item-by-item comparison and a suggested ordering for a
move to mainline are in `07-mainline.md`; "don't chase the newest — which
LTS is actually worth targeting", plus an existing RTD1295 port to crib
from, are in `08-kernel-uplift.md`.

## D2 — Video source: the built-in HDMI RX, driven the way Android does

Read the driver's `hdmirx_video_info` sysfs attribute first to learn the
timings it actually detected, then `VIDIOC_S_FMT` with those dimensions.

The driver asks `video_register_device()` for minor 250, but falls back to
the first free number when it cannot have it (on real hardware that is
`video0`), so **the node name must not be hardcoded**. Both the udev rule
and `hdmirx-info` identify the device by the `hdmirx_video_info` attribute
and map it to `/dev/kvmd-video`.

## D3 — Rootfs: Debian 13 (trixie) arm64

**Revised on 2026-09-04 based on QEMU testing. This previously said Debian
12 plus a self-built Python, which was the long way round.**

### Test evidence

See `tests/README.md`. Each userspace was booted under QEMU on vanilla
linux-4.9.337 arm64, with cgroup / namespace / seccomp options matched to
the BSP defconfig:

| userspace | systemd | Python | Result |
|---|---|---|---|
| Arch Linux ARM aarch64 | 261.2 | — | **Fails**, PID 1 freezes |
| Debian 13 trixie | 257.13 | 3.13.5 | Boots, 0 failed units, unified cgroup v2 |
| Debian 12 bookworm | 252.39 | 3.11.2 | Boots, 0 failed units |

Arch fails while systemd mounts the early API filesystems:

```
systemd[1]: Failed to determine whether /proc is a mount point: Invalid argument
[!!!!!!] Failed to mount early API filesystems.
systemd[1]: Freezing execution.
```

systemd 258 dropped cgroup v1 and raised its kernel floor to 5.4. ALARM is
rolling, so it inevitably reaches 258+, and there is no official archive to
pin an older build against — **Arch is a dead end here**. systemd 257's
"4.15 minimum" is a soft recommendation by comparison, and works fine on 4.9
in practice.

### Why trixie and not bookworm

Both boot, but trixie's Python 3.13 clears kvmd's syntax floor (3.12+) while
bookworm's 3.11 does not. Choosing trixie means:

- **kvmd needs no patches** and runs on the distro's python3
- no self-built or bundled Python runtime
- all 44 of kvmd's dependencies (including `async-lru`, `dbus-next`,
  `pyghmi`, `luma.core`, `luma.oled`, `spidev`, `serial-asyncio`, `hid`,
  `pyrad`, `smbc`) **exist as apt packages in trixie — not one is missing**
- `libgpiod` 2.2.1 (kvmd wants ≥2.1) and `v4l-utils` 1.30.1 (wants ≥1.22.1)
  both satisfy their floors
- it is also the closest fit to CLAUDE.md's "prefer new code"

ustreamer is still built from source: trixie ships 5.4 (kvmd wants ≥6.47),
and we need to patch it for pixel formats anyway.

### About kvmd's Python requirement (corrected 2026-09-16)

**This section previously claimed "kvmd needs no patches on Python 3.13".
That was wrong.**

The original check only validated syntax with `compile()`, which cannot
catch runtime annotation evaluation. Running kvmd 4.213 on the board
actually failed with:

```python
# kvmd/apps/kvmd/switch/proto.py:133
def compare_edid(self, ch: int, edid: ("Edid" | None)) -> bool:
TypeError: unsupported operand type(s) for |: 'str' and 'NoneType'
```

`"Edid" | None` is a string forward reference combined with `|`. Under
Python 3.14's PEP 649 (deferred annotation evaluation) it would never be
evaluated; 3.13 evaluates it at class creation time and raises TypeError.

Measured impact is **one file, two occurrences**, fixed by a single
`from __future__ import annotations` (`patches/kvmd/0001-*.patch`). The
3.12+ runtime APIs (`itertools.batched`, `typing.override`,
`TypeAliasType`, `sys.monitoring`) were also checked for, and are not used.

So the choice of trixie still stands, but the price is **one 1-line kvmd
patch**, not the zero patches originally claimed.

**Lesson: when checking language version compatibility, a syntax check is
not a runtime check. Import and run the code.**

### Non-Python dependencies that were missed at first

All 44 Python dependencies exist in trixie. The non-Python ones from the
PKGBUILD did not all make it in initially:

- `libxkbcommon0` — kvmd's keymap code dlopens it; without it you get a
  straight `RuntimeError`
- `libevent-core-2.1-7t64` / `libevent-pthreads-2.1-7t64` — Debian splits
  libevent into three packages and the main one alone is not enough

### Known limitations

1. ~~The tests above used vanilla 4.9.337 under QEMU virt, not the BSP's
   4.9.119 on real hardware; this must be re-verified once the board
   arrives.~~
   **Re-verified 2026-09-17**: Debian 13 trixie boots correctly on the BSP's
   4.9.119 on real hardware. systemd 257.13 comes up fully with no failed
   units, and kvmd / kvmd-nginx / kvmd-otg / systemd-timesyncd all work.
2. Debian 14 will ship systemd ≥258 — **trixie is the end of the line for
   this board**. Trixie is supported until roughly 2030, which is long
   enough for this project.
3. If kvmd ever requires Python >3.13, the fallback is a bundled CPython
   under `/opt` (`astral-sh/python-build-standalone` publishes prebuilt
   aarch64 binaries, no compilation needed).

### Things only real hardware revealed

QEMU could not surface these:

- A rootfs produced by `docker export` carries docker-specific artifacts
  (`/.dockerenv`, `policy-rc.d`, the bind-mounted
  `/etc/{hostname,hosts,resolv.conf}`, a baked-in `machine-id`).
  `/.dockerenv` in particular makes systemd treat the whole system as a
  container and skip `systemd-timesyncd` entirely.
- The board has no RTC battery, so it boots in 2014, and a wrong clock makes
  apt's signature validation fail (`Not live until ...`). That becomes a
  deadlock — you need the network to set the time, and packages to get on
  the network — so `systemd-timesyncd` has to be in the image from the
  start.

Details are in "Pitfalls fixed along the way" in `06-changes.md`.

### On tracking PiKVM package updates

Not possible, and this has nothing to do with the distro choice. kvmd's Arch
PKGBUILD depends on `raspberrypi-io-access`, `raspberrypi-utils` and
`janus-gateway-pikvm`, and upstream has no `kvmd-platform-*-bpi-w2` variant.
Even on Arch you could not just install it. What is achievable is **tracking
upstream source releases with zero patches**, and trixie delivers that.

## D4 — Build environment: a Debian bullseye container

`docker/builder.Dockerfile` is pinned to bullseye because:

- OpenSSL 1.1.1 — the in-tree `linux-rtk/scripts/sign-file` of kernel 4.9
  uses the 1.1 API and will not compile against OpenSSL 3
- GNU make 4.3 — make 4.4 changed `$(shell)` behaviour
- python2.7 — some u-boot 2015.07 scripts need it

The host only needs docker, git and bash; nothing is installed system-wide.
