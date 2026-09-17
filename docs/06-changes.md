# Change overview

**This document is the single source of truth for the change list** — every
modification this project makes to upstream, why, and the hardware
verification results. Start here for *what* changed; the other documents
cover *how it works*:

- `01-research-findings.md` — why the BSP kernel rather than mainline
- `02-decisions.md` — kernel / rootfs / build environment choices and their
  corrections
- `03-image-and-boot.md` — image layout, boot chain, u-boot's constraints
- `04-hdmi-rx-bringup.md` — how the three HDMI RX blockers were cleared
- `05-userspace.md` — building and installing ustreamer / kvmd

All upstream sources live in `vendor/`, fetched by
`scripts/prepare-sources.sh`, which then applies the patches under
`patches/`. **We never edit `vendor/` directly**; every change exists as a
patch, and `make sources` is idempotent.

| Source | ref |
|--------|-----|
| `BPI-SINOVOIP/BPI-W2-bsp` | `master` (no tags exist) |
| `pikvm/ustreamer` | `v6.66` |
| `pikvm/kvmd` | `v4.213` |

---

## 1. Kernel patches (`patches/kernel/`)

### `0001` Duplicate definition of `yylloc` in dtc

**Problem**: building the BSP kernel with gcc 10 or newer stalls on the host
tool `dtc`:

```
scripts/dtc/dtc-lexer.lex.c: multiple definition of 'yylloc'
```

**Cause**: gcc 10 defaults to `-fno-common`, so the tentative definition
`YYLTYPE yylloc;` in `dtc-lexer.l` is no longer merged.

**Fix**: change it to `extern YYLTYPE yylloc;` in both `dtc-lexer.l` and
`dtc-lexer.lex.c_shipped`. This is the same fix upstream Linux made.

---

### `0002` Enable hdmirx in the DTS and remove `gpio-5v-detect`

File: `arch/arm64/boot/dts/realtek/rtd129x/rtd-1296-bananapi-common.dtsi`

1. Change `hdmirx@98034000`'s `status` from `disabled` to `okay`.
2. **Remove `gpio-5v-detect = <&rtk_iso_gpio 22 0 0>;`**

Item 2 is a bug in BPI's own DTS. It refers to **the same pin** as the
node's `gpio-rx-hpd-ctrl = <&rtk_iso_gpio 22 1 0>`, but in the opposite
direction (HPD is an output, 5 V detect is an input). The driver's
`rx5v_do_task()` calls `gpio_get_value(5v_det)` and therefore reads back the
level HPD is driving itself:

```
probe : Hdmi_SetHPD(0)     → pin drives low
poll  : gpio_get_value()   → reads 0 → decides "no source present"
                           → Hdmi_SetHPD(1) never runs
```

That deadlocks: the source never sees HPD, so it sends nothing, and `Status`
stays `NotReady` forever. No other board in the BSP has this property, and
when it is absent the driver takes the correct `Use cbus 5V detect` path.

---

### `0003` defconfig

File: `arch/arm64/configs/rtd129x_bpi_defconfig`

| Option | Was | Now | Why |
|--------|-----|-----|-----|
| `CONFIG_RTK_HDMIRX` | not set | `y` | The HDMI capture driver |
| `CONFIG_USB_CONFIGFS_F_HID` | not set | `y` | kvmd's `hid: otg` needs USB gadget HID |

> **`CONFIG_DEVMEM` used to be here and has been removed.**
>
> During debugging we enabled `CONFIG_DEVMEM=y` and disabled
> `STRICT_DEVMEM` so that `tools/rdreg.py` could read and write the pinmux
> and HDMI RX registers directly — that is how the whole DDC problem was
> uncovered. But a KVM sits on a management network and holds the console of
> somebody else's machine; a shipping image should not leave an unrestricted
> `/dev/mem` lying around, so it was removed on 2026-09-17.
>
> To make a debug build, layer this on top of patch `0003`:
> ```
> CONFIG_DEVMEM=y
> # CONFIG_STRICT_DEVMEM is not set
> ```
> Routine pinmux verification does not need it — whether the source
> negotiated 1080p60 (`hdmirx-info`'s Width/Height/Fps) is sufficient proof
> that DDC works.

---

### `0004` rtk_hdmirx driver fixes (437 lines)

Files: `hdmirx_video_dev.{c,h}` and `v4l2_hdmi_dev.{c,h}` under
`drivers/media/platform/rtk_hdmirx/rtd129x/`.

This driver had only ever been exercised by Android's tv_input HAL and never
through the standard V4L2 flow, so a fair amount was missing. Eight fixes in
total:

| # | Problem | Fix |
|---|---------|-----|
| 1 | No `VIDIOC_G_FMT` | Added; reports the detected input timings when `!dev->fmt_set` |
| 2 | No `VIDIOC_TRY_FMT` | Added |
| 3 | No `VIDIOC_ENUMINPUT` / `G_INPUT` / `S_INPUT` | Added; input named `HDMI RX`, `V4L2_INPUT_TYPE_CAMERA` |
| 4 | `S_FMT` did not report the accepted format | Call `fill_pix_format()` at the end so the real `bytesperline` is returned |
| 5 | `out_color_to_bpp()` had no `OUT_8BIT_YUV422` case | Add `bpp = 16` (it fell through to a default of 32, doubling the buffer) |
| 6 | An ioctl before queue initialisation oopsed the kernel | Add a `queue_inited` flag; `QUERYBUF`/`QBUF`/`DQBUF`/`STREAMON`/`STREAMOFF` return `-EINVAL` when uninitialised |
| 7 | `q->owner` was never set after `vb2_reqbufs` | Add `vq->owner = file->private_data` |
| 8 | No release handler | Add `v4l2_mipi_top_release()`, which calls `vb2_fop_release()` and clears `queue_inited` |

The oops in item 6 looked like this:

```
Unable to handle kernel NULL pointer dereference at virtual address 00000000
PC is at __wake_up_common+0x38/0xa0
 __vb2_queue_cancel+0x7c/0x188
 vb2_core_streamoff+0x54/0xb8
 v4l2_hdmi_do_ioctl+0x600/0x6f8      ← VIDIOC_STREAMOFF
```

The driver only calls `vb2_queue_init()` inside its `VIDIOC_REQBUFS`
handler. Issue `STREAMOFF` before that and `__vb2_queue_cancel()` runs
`wake_up_all()` on an all-zero `done_wq.task_list` → `next == NULL` → oops.
Afterwards the process is stuck in D state holding `/dev/video0`, and only a
power cycle recovers it.

The probe-time defaults were changed too:
`outfmt = OUT_8BIT_YUV422; bpp = 16; fmt_set = false;`

---

### `0005` Give the i2c6 pad mux to the hdmirx node

File: `rtd-1296-bananapi-common.dtsi`, in `hdmirx@98034000`, add:

```
pinctrl-names = "default";
pinctrl-0 = <&i2c_pins_6>;
```

**This is the root cause of EDID/DDC never working at all.** According to
BPI's official schematic (`docs/refs/bpi-w2-v1_1-pub.pdf`, page 6):

```
HDMI-IN pin 15 (SCL) ── RX_I2C6_SCL ──[R81 0R]── I2C6_SCL   (ISO pad 20)
HDMI-IN pin 16 (SDA) ── RX_I2C6_SDA ──[R82 0R]── I2C6_SDA   (ISO pad 26)
```

The pinmux for those pads is in bits [1:0] and [3:2] of ISO MUXPAD
`0x98007314`, which resets to function 0 = `gpio`. BPI's `pinctrl-0` list
never mentions `i2c_pins_6`, so they stay GPIO from power-on to power-off.
No matter how correct the rtk_hdmirx side is — right EDID SRAM contents,
`edid_en=1`, HPD asserted — the physical pins are simply not connected to the
DDC block, and the source gets a NAK every time it reads `0x50`.

**Why attach it to the hdmirx node rather than add it to `pinctrl-0`**:
`pinctrl-0` has never taken effect on this board. Boot messages:

```
unsupported function sdio on pin sdio_clk
pinctrl core: failed to register map default (10): invalid type given
```

`RTK_pctrl_dt_node_to_map()`
(`drivers/pinctrl/realtek/pinctrl-rtd129x.c`) precomputes
`nmaps = count * 2` from the pin count and allocates the array; when the loop
skips an unsupported pin it does not increment `i`, yet the function
unconditionally sets `*num_maps = nmaps` at the end. The trailing entries are
uninitialised `kmalloc_array()` memory with a garbage `type`, so the whole
batch is rejected. `sdio_pins` happens to trigger it.

**We do not fix that pinctrl driver bug** — changing `*num_maps = nmaps` to
`*num_maps = i` would apply 17 other groups (SD, RGMII, UART, PWM, …) for the
first time, which is too much risk for a board that currently boots. Instead
the hdmirx device applies just the group it needs through the driver core's
`pinctrl_bind_pins()`, which sidesteps the broken default state and touches
no other pins.

> This also explains the "720p only, force the resolution by hand"
> workaround on the BPI forum: DDC never worked, so hardcoding it on the
> source was the only option.

---

### `0006` Pin Type-C to peripheral mode (USB OTG)

File: `rtd-1296-bananapi-common.dtsi`, `/delete-property/ drd_mode;` under
`&dwc3_drd`.

PiKVM's HID goes through a USB gadget, which needs a UDC. The BSP's
`rtd-1296-usb.dtsi` already sets `dwc3_drd@98020000` to
`dr_mode = "peripheral"`, but it also puts `drd_mode` on
`rtk_dwc3_drd_type_c@0`, which makes the `dwc3-rtk-type_c` driver pick the
role dynamically from the Type-C CC pins:

```
[    6.5] dwc3_drd_to_device END....           ← starts as device
[   38.0] switch_dwc3_mode dr_mode=USB_DR_MODE_HOST
[   38.3] dwc3_drd_to_host END....             ← then switched to host
```

The result is an empty `/sys/class/udc/` and `kvmd-otg` failing to start.

With `drd_mode` removed, `switch_dwc3_mode()` takes the
`if (!type_c->is_drd_mode) dr_mode = type_c->dwc3_mode;` path and always uses
the peripheral mode the DTS specifies. Verified on hardware:

```
UDC: [98020000.dwc3_drd]
[6.465] switch_dwc3_mode dr_mode=USB_DR_MODE_PERIPHERAL
```

The cost: the Type-C port can no longer act as a USB host. For a KVM that is
what you want anyway — it goes to the target machine — and the board has
separate USB host ports. Power comes in through the DC jack and is
unaffected.

#### The hard limit on USB endpoints

`GHWPARAMS3 = 0x030c6485` → 6 endpoints total, 3 of them IN. After the
control endpoint ep0, that leaves **2 IN + 2 OUT** (debugfs likewise shows
only `ep1in/ep1out/ep2in/ep2out`).

Kernel 4.9's `f_hid` has no `no_out_endpoint` configfs attribute (kvmd tries
to write it and skips on failure), so every HID function takes 1 IN + 1 OUT.
With keyboard + mouse + mouse_alt, the third one fails to bind and takes the
whole gadget with it:

```
configfs-gadget gadget: hidg_bind FAILED
configfs-gadget 98020000.dwc3_drd: failed to start kvmd: -19
```

Adding mass_storage as the third function behaves the same way. **Two
functions at a time is the maximum**; how that is handled is in the
`main.yaml` part of section 5.

---

### `0007` S_FMT must write the accepted format back to userspace

File: `drivers/media/platform/rtk_hdmirx/rtd129x/hdmirx_video_dev.c`

`VIDIOC_S_FMT` is `_IOWR`: the driver is obliged to write the format it
actually accepted back into the caller's `struct v4l2_format`. The original
handler read `width` / `height` / `pixelformat` into `dev` and `mipi_top` and
then simply `break`ed, **never touching `bytesperline` or `sizeimage`**, so
whatever the caller passed in came straight back out.

ustreamer's `_capture_open_format()` always sets
`bytesperline = us_align_size(width, 32) << 1` (a packed-YUV assumption), so
it believed NV16's Y stride was **3840** rather than 1920:

| Consequence | Arithmetic |
|-------------|------------|
| Every row skips one → the top half of the picture is squashed to 2× height | `y_row = luma + 3840 * row` |
| By row 540 the chroma pointer has run through the entire chroma plane | `2088960 + 3840*540 = 4162560` = start of the zero padding |
| Reads zeros → Cb=Cr=0 → solid bright green | |
| Further down it exceeds `sizeimage` (4177920) | reads past the mmap → **SIGSEGV** |

Symptoms on hardware: kvmd restarting the streamer every 3 seconds
(`Process killed: retcode=-11`), with the top half of the Web UI picture
distorted and the bottom half solid green.

The `G_FMT` / `TRY_FMT` added by `0004` already went through
`fill_pix_format()`, which is why `v4l2-ctl --get-fmt-video` reported the
correct 1920 the whole time. That is what made this bug hard to spot:
**querying with G_FMT looks fine; only S_FMT's return value is wrong.**

The fix is one more `fill_pix_format()` call at the end of S_FMT's success
path, consistent with G_FMT and TRY_FMT.

Verification: with the patch applied, twelve consecutive snapshots taken with
an **unmodified** ustreamer all returned 200, with zero green rows and
correct geometry.

---

## 2. ustreamer patches (`patches/ustreamer/`)

### `0001` Semi-planar NV12 / NV21 / NV16 support

Files: `src/libs/capture.c`, `src/libs/frame.c`,
`src/ustreamer/encoders/cpu/encoder.c`

rtk_hdmirx only emits **NV16 / NV12 / BGR32**, none of which is a packed
format ustreamer supports natively. NV16 is the smallest of them that does
not lose vertical chroma resolution.

- `capture.c` — add NV12 / NV21 / NV16 to `_FORMATS[]`
- `frame.c` — set `bytes_per_pixel` to 1 for all three (the luma plane)
- `encoder.c` — add `_jpeg_write_scanlines_yuv_semiplanar()`, which
  reassembles semi-planar Y plus interleaved UV into packed YUV row by row
  for libjpeg, using `frame->format` to decide whether chroma is vertically
  subsampled (NV16 is not) and the U/V ordering (NV21 is swapped)

**A stride sanity check** (the same bug as kernel `0007`, blocked from the
userspace side as well):

`us_frame_get_padding()` was written for packed formats, and ustreamer always
requests `bytesperline = align(width,32) * 2` at S_FMT time. If a driver does
not rewrite that field as V4L2 requires — and rtk_hdmirx originally did not —
the semi-planar Y stride comes out doubled. So
`_jpeg_write_scanlines_yuv_semiplanar()` now:

- sanity-checks the stride against the plane size — `stride * height` must
  fit inside the Y plane, otherwise it falls back to `width`
- bounds-checks every row and fills unreadable parts with neutral grey
  (Y=0, UV=128) instead of reading memory that is not ours

Either layer alone fixes the problem (both verified on hardware). Both are
kept because the kernel layer addresses the root cause while the userspace
layer guards against *any* driver reporting a bogus bytesperline.

**Uncached mmap: reading byte by byte is 10× slower**

`frame->data` points at the V4L2 mmap buffer, and on this SoC vb2-dma-contig
hands back an **uncached** mapping. Every byte-sized access to that memory is
a separate bus transaction. The original implementation read `y_row[x]` and
`uv_row[uv]` directly inside the per-pixel loop, which is over six million
uncached reads for a single 1080p frame:

| | One 1080p frame |
|---|---|
| libjpeg itself (same CPU, measured with worst-case random noise) | 80 ms |
| The version reading the uncached mmap byte by byte | **830 ms** |

In other words 750 ms had nothing to do with JPEG compression and everything
to do with how memory was accessed. Bandwidth comparison:

| Reading 125 MB | Time | Throughput |
|---|---|---|
| Sequentially from the capture buffer (the memcpy path) | 1.72 s | 73 MB/s |
| tmpfs → tmpfs (ordinary cached memory) | 0.31 s | 410 MB/s |

Sequential reads still manage 73 MB/s; byte-at-a-time falls through the
floor.

The fix: `memcpy` each Y and UV row into a cached buffer first (memcpy reads
uncached memory sequentially with wide instructions), then do the
interleaving out of cache. NV12/NV21 share one chroma row between two luma
rows, so a small cache avoids copying it twice.

Measured:

| | Before | After |
|---|---|---|
| Per-frame encode (ustreamer's own `last_job_time`) | 830 ms | **79 ms** |
| Stream | 3 fps | **29.8 fps** (hits the `desired_fps` cap) |
| CPU | 273% (3 workers at 91% each, saturated) | 237%, 59% system-wide |

**This lesson is not specific to JPEG**: any code that touches the capture
buffer on this board must memcpy it out before working on it.

Verification: `tools/nv16-encode-test.c` round-trips a synthetic image and
measures 49.5 dB PSNR; on hardware, the old kernel plus the fixed ustreamer
streamed MJPEG for 90 seconds with zero errors.

---

## 3. kvmd patches (`patches/kvmd/`)

### `0001` Deferred annotation evaluation for Python 3.13

File: `kvmd/apps/kvmd/switch/proto.py`, adding one
`from __future__ import annotations`.

kvmd 4.213 requires Python 3.14 and relies on PEP 649's deferred annotation
evaluation. Debian 13 ships 3.13, which evaluates the annotation at import
time and blows up:

```
TypeError: unsupported operand type(s) for |: 'str' and 'NoneType'
```

> Lesson: when checking language version compatibility, a syntax check is not
> a runtime check. An earlier syntax-only scan led to the claim "3.13 needs
> no patches", which was wrong (see the correction in `02-decisions.md`).

---

## 4. Build scripts (`scripts/`)

| Script | Purpose |
|--------|---------|
| `prepare-sources.sh` | Fetch the three upstream trees at pinned refs and apply patches; idempotent |
| `build-kernel.sh` | Build kernel + dtb + modules inside a bullseye container |
| `build-rootfs.sh` | Build the Debian 13 rootfs (with the PiKVM userspace) inside an arm64 container |
| `rootfs-pikvm.sh` | Called by the above inside the container; builds and installs ustreamer + kvmd |
| `build-image.sh` | Assemble the flashable SD image |
| `in-docker.sh` | Shared wrapper for entering the build container |

Debug helpers (not part of the build):

| Script | Purpose |
|--------|---------|
| `board-ssh.sh` / `board-cmd.sh` | Reach the board; supports `--put` / `--get` |
| `pi-ssh.sh` | Reach the test source Pi. Credentials are **not in the file**; pass `PI_HOST` / `PI_PASS` as environment variables — the script is version-controlled, and hardcoding a password means committing it |
| `serial-cmd.sh` | Issue serial console commands via `/dev/ttyUSB0` |
| `uboot-cmd.sh` | When the board will not boot, break into the u-boot console during the boot loop and issue commands. The most useful trick is booting with `setenv root /dev/ram` — no rootfs is mounted at all, which cleanly separates "software problem" from "hardware/power problem" |
| `lk-boot-usb.sh` | Boot from USB via LK |
| `kvmd-fix-perms.sh` | Fix kvmd file permissions (a leftover from the hot-deploy era) |

`Makefile` flow: `make all` = `builder → sources → kernel → uboot → rootfs → image`

### Pitfalls fixed along the way

| Symptom | Cause | Fix |
|---------|-------|-----|
| The initrd lands on the kernel | `MODULES=most` produces 27 MB, but the initrd loads at `0x02200000` and the kernel at `0x03000000`, only 14 MB apart | `MODULES=list` with an empty list; SD/MMC and ext4 are built in anyway |
| Root will not mount when booting from USB | fstab hardcoded `/dev/mmcblk0p*` | Use `LABEL=BPI-ROOT` / `LABEL=BPI-BOOT`, with `nofail` on `/boot` |
| The SD card will not boot | LBA0 lacks the `SDMMC_BOOT` magic (which appears nowhere in the BSP sources) | `build-image.sh` writes it, byte-for-byte identical to a known-bootable card |
| u-boot cannot find the files | It does not read `uEnv.txt`; the names are compiled in, and the two sets differ | The image ships both sets of filenames |
| `/etc` and `/usr` become `1000:1000` | `cp -a overlay` and `modules.tar` carry the host's uid/gid in, and systemd-tmpfiles then fails en masse with `Detected unsafe path transition` | `mkdir -p` directories only, `install -o root -g root` each file, `--owner=0 --group=0` on `tar`, plus a final sweep of the whole rootfs as a backstop |
| ustreamer cannot find its library at boot | Only `libevent-core` / `libevent-pthreads` were listed, so `libevent-2.1-7t64` was autoremoved | List `libevent-2.1-7t64` explicitly |
| `python3-pyghmi` gets purged | It depends on `python3-setuptools` via `python3-pbr`, and setuptools was in BUILD_DEPS | Move setuptools to the runtime dependencies |
| The image is 100 MB larger than it should be | `gcc → cpp → cpp-<arch> → gcc` is a dependency cycle; purging only `build-essential` makes autoremove conservatively keep the whole chain | Name each of `gcc g++ cpp make dpkg-dev libc6-dev binutils` |
| Builds fail at random | `python3` SIGSEGVs occasionally under qemu emulation (`status code -11`, failing `get_requires_for_build_wheel`) | Wrap in `apt_install()` and `retry()` |
| hostname becomes `localhost` and DNS is dead | docker bind-mounts `/etc/{hostname,hosts,resolv.conf}` into the container; writes there are not part of the image layer, so `docker export` emits the empty files underneath | Append the correct contents with `tar --append` after export (later entries win; `--delete` was measured to corrupt the archive) |
| The clock is stuck in 2014 and apt does not work | `/.dockerenv` gets exported → systemd's `detect_container()` decides the whole system is a container → `systemd-timesyncd` and `fstrim.timer` are skipped by `ConditionVirtualization=!container`. The wrong clock then breaks apt's signature validation (`Not live until ...`), so you cannot even install timesyncd to fix it | `rm -f /.dockerenv`, and include `systemd-timesyncd` in the package list |
| Services from packages installed later never start | `/usr/sbin/policy-rc.d` (`exit 101`) gets exported | `rm -f /usr/sbin/policy-rc.d` |
| Every card shares one machine-id | `/etc/machine-id` carries the build-time value, and systemd-networkd derives its DHCP identifier from it | Empty `/etc/machine-id` and make `/var/lib/dbus/machine-id` a symlink |
| After emptying machine-id, the first boot may hang | `systemd-firstboot.service` (`ConditionFirstBoot=yes`, `StandardInput=tty`) passes `--prompt-keymap` and waits on the serial console when unset | Write `/etc/vconsole.conf` (locale, timezone and the root password are already configured) |
| Only 3 GB is usable on a large card | The image is a fixed size | `bpikvm-expand-rootfs`, invoked by firstboot on first boot; measured 2.8 G → 232 G |
| `/etc/vconsole.conf` gets written somewhere else | It is a symlink to `/etc/default/keyboard` (and a dangling one, since `keyboard-configuration` is not installed), so `echo >` follows it and creates `/etc/default/keyboard` | `rm -f` the symlink first, then write a real file |
| `vconsole.conf` ends up containing `KEYMAP=usn` | The container script is wrapped in `bash -eux -c '...'`, so writing `printf 'KEYMAP=us\n'` terminates the outer quoting early and `\n` is taken literally as `n` | Use double quotes or a heredoc inside container scripts; never single quotes |

---

## 5. `overlay/` — files this project ships

| File | Purpose |
|------|---------|
| `usr/lib/kvmd/main.yaml` | Platform configuration, equivalent to upstream's `kvmd-platform-*` |
| `usr/lib/kvmd/platform` | Model strings (`bpi-w2` / `hdmi` / `rtd1296`); without it kvmd raises `FileNotFoundError` |
| `etc/kvmd/override.yaml` | Points `kvmd.info.hw.vcgencmd_cmd` at `bpikvm-vcgencmd` |
| `usr/lib/udev/rules.d/99-kvmd-bpi-w2.rules` | Creates `/dev/kvmd-video` and `/dev/kvmd-hid-{keyboard,mouse}` |
| `usr/local/bin/hdmirx-info` | Show the currently detected input state |
| `usr/local/bin/hdmirx-capture` | Capture test |
| `usr/local/bin/bpikvm-expand-rootfs` | Grow root to fill the card; can be re-run by hand |
| `usr/local/bin/bpikvm-vcgencmd` | A `vcgencmd` stand-in that gives the health monitor a valid output |
| `usr/local/bin/bpikvm-firstboot` + `.service` | First boot: grow root, generate SSH host keys and kvmd TLS certificates |
| `etc/systemd/system/bpikvm-ustreamer.service` + `etc/default/bpikvm-ustreamer` | Test service that runs ustreamer standalone (not enabled by default) |
| `etc/motd` | Login banner |

### Platform constraints in `main.yaml`

- `hid: otg` — Type-C is pinned to peripheral mode by patch 0006; verified on
  hardware, with `/dev/kvmd-hid-keyboard` and `/dev/kvmd-hid-mouse` created
  correctly and kvmd reporting `hid.online: true`
- `hid.mouse_alt.device: ""` — disables the relative-positioning mouse;
  there are not enough endpoints (see patch 0006)
- `otg.endpoints: 2` — tells kvmd how many endpoints actually exist. Without
  it kvmd adds all three HIDs to the config, the third `hidg_bind` fails, and
  the whole gadget fails to bind to the UDC. With it set to 2, kvmd skips the
  extra function and logs
  `Function 'hid.usb2' not be started: No available endpoints`
- `atx: disabled`, `msd: disabled` — not wired / not verified
- `--format=NV16` — see the ustreamer patch above
- `--resolution=1920x1080` hardcoded — the driver does not implement
  `VIDIOC_QUERY_DV_TIMINGS`, so `--dv-timings` is unavailable
- no `--encoder=m2m-image` — none of the SoC's codec engines expose a V4L2
  M2M interface (the BSP provides vendor ioctls), so encoding is on the CPU.
  `--workers=3` measured best; 4 is worse — see §7 item 6. The full survey of
  the hardware encoders is §10

### Why the udev rule does not hardcode the node name

rtk_hdmirx asks for minor 250, but `video_register_device()` falls back to
the first free number when it cannot have it (on real hardware, `video0`).
The rule identifies the device by the driver-specific sysfs attribute
`hdmirx_video_info` instead. `hdmirx-info` does the same.

### A wrong key path in `override.yaml` produces no warning at all

kvmd **silently ignores** override keys it does not recognise. We initially
put `vcgencmd_cmd` under `kvmd.info.health.` (correct is `kvmd.info.hw.`).
The setting did nothing, the board accumulated 7965 lines of
`FileNotFoundError: '/usr/bin/vcgencmd'`, and kvmd said nothing.

After changing an override, always verify with `kvmd -M` — it prints only
fields that differ from the defaults, so `{}` means the whole override was
ignored. Correct paths are in `kvmd/apps/_scheme.py`.

### `/bin/true` is not a quiet fallback

Once the key path was right, `vcgencmd_cmd: ["/bin/true"]` merely swapped one
error for another, still once every 5 seconds:

```
Can't parse [ /bin/true get_throttled ] output: '': ValueError:
    invalid literal for int() with base 16: ''
```

kvmd's parser is `int(text.split("=")[-1].strip(), 16)` and needs valid
output. Hence `overlay/usr/local/bin/bpikvm-vcgencmd`, which answers
`get_throttled` with `throttled=0x0` (no undervoltage, no frequency capping,
no thermal throttling) and exits 0 for anything else. Verified on hardware:
zero new errors in the 15 seconds after a restart.

---

## 6. Debug tools (`tools/`)

Not shipped in the image; used during development.

| File | Purpose |
|------|---------|
| `hdmirx-test.c` | V4L2 capture following the Android HAL's order |
| `guard-test.c` | Exercise the `queue_inited` guard from patch 0004 |
| `nv16-encode-test.c` | Verify ustreamer's NV16 encoding (PSNR) |
| `rdreg.py` / `wrreg.py` | Read/write registers through `/dev/mem`. **The shipping image's kernel does not enable `CONFIG_DEVMEM`**; using these requires layering a debug defconfig (see `0003` in §1) |
| `dump-edid-sram.py` | Read EDID back out of the hardware SRAM |
| `edid-sram-probe.py` | Probe the EDID SRAM's read/write behaviour |
| `ddc-watch.py` | Poll the DDC status registers at high rate to see whether any I2C activity occurs |

`ddc-watch.py` is what settled the DDC question: while the source hammered
I2C, 820,000 samples on the board showed exactly **one** state, with
`cmderr` / `finish` / `timeout` never moving → the DDC engine never saw a
single I2C transaction → the problem was the pins, not the configuration.

---

## 7. Verified state on real hardware

Source: a Raspberry Pi 3 running Kodi (LibreELEC).

```
$ hdmirx-info
  rx_video/state = 1
  Type:HDMIRx     Status:Ready
  Width:1920      Height:1080
  ScanMode:Progressive   Color:RGB   Fps:60
```

```
[HDMI RX]Check resolution match => Width(1920) Height(1080) VIC(16)
[HDMI RX]Polarity detect done: hor(1920) ver(1080) color(RGB) I/P(Prog)
[HDMI RX]skip 0/600 in 2500 jiffies          ← 600 frames per 10 s, zero dropped
```

- **EDID is served correctly over DDC**: the source reads back a complete 256
  bytes, both block checksums are correct, and the CEA extension block (tag
  0x02) and HDMI VSDB are present
- **The source negotiates 1080p60 by itself**: `VIC(16)`, no longer a DVI
  fallback. The Pi has **no** forced-resolution settings and no custom EDID
  file
- kvmd and kvmd-nginx are running, and the stock PiKVM Web UI works
- 1080p60 capture: about 57.9 fps raw on the capture side

> **Correction.** This section used to add "60 fps through kvmd, about 75%
> CPU". That was measured before the encoder problem in §2 was found, and it
> was wrong: at the time the CPU encoder took 830 ms per frame and only about
> 3 fps ever reached a client. The authoritative numbers, measured with
> moving video after the fix, are in section 7 item 6 -- 22–24 fps delivered,
> 0.083 s per frame, 58% CPU. The capture side really does run at 53–60 fps;
> it is the encode and delivery side that was misreported.

### rootfs / image checks

Importing `build/rootfs.tar` as an arm64 container and running it directly:

- `ustreamer --version` = 6.66, `ldd` shows no missing libraries
- `kvmd.apps.{kvmd,ngxmkconf,otg,vnc,ipmi,edidconf,htpasswd}` all import
- `kvmd -M` confirms the override took effect; `kvmd -m` shows `hid: otg` /
  `atx: disabled` / `msd: disabled`
- `/`, `/etc`, `/usr`, `/usr/lib` and `/etc/systemd/system` are all
  `root:root 755`
- No pre-generated SSH host keys or TLS certificates
- Build tools are gone
- The DTB md5 inside `build/bpiw2-pikvm.img`'s (3.0 G) boot partition matches
  the one just built

### Hardware verification

**It matters how each thing was verified**, otherwise it is easy to
overestimate how much of the image has actually been exercised.

#### 1. First flash: verified by booting

The image was flashed to a 235.5 G micro SD and booted from the on-board
slot:

| Item | Result |
|------|--------|
| Boot | systemd comes up cleanly, no failed units |
| `reboot` | Works, back in about a minute |
| Install layout | `/usr/bin/kvmd`, `/usr/lib/python3/dist-packages/kvmd` |
| i2c6 pinmux | `0x98007314 = 0x00005005`, applied automatically at boot |
| HDMI RX | 1920x1080 @60, `Status: Ready` |
| Web UI | HTTP 80 → 301 to HTTPS, login returns 200 |
| Streaming | Once a ws session connects, kvmd starts ustreamer and a snapshot returns a 51 KB 1080p JPEG. **Calling this a pass was wrong** — only the response size was checked, not the pixels, and the picture was in fact broken (see item 3) |
| firstboot | 3 SSH host keys and the nginx TLS certificate, with correct ownership and permissions |

This round also **caught five image-level bugs** (`/etc` bind-mounted by
docker, `/.dockerenv`, `policy-rc.d`, a fixed machine-id, missing timesyncd),
plus our own mistake of pointing `vcgencmd_cmd` at `/bin/true`.

#### 2. Verified on the board first, then folded into the image

These were verified by hot-patching that card or deploying a new DTB, **and
the image itself had not yet been re-flashed at that point**:

| Item | Result |
|------|--------|
| `/etc` fixes | hostname `bpi-w2-pikvm`, DNS working, `systemd-detect-virt` = `none` |
| NTP | Clock synchronised (previously stuck in 2014) |
| kvmd health errors | After switching to `bpikvm-vcgencmd`, zero new errors within 15 seconds of a restart |
| Card expansion | 2.8 G → 232 G (online resize, no reboot) |
| USB OTG | After deploying patch 0006's DTB and rebooting, UDC `98020000.dwc3_drd` registers |
| USB HID | `/dev/kvmd-hid-{keyboard,mouse}` created, kvmd reports `hid.online: true` |

#### 3. What the second flash (with OTG + expand) caught

The image including USB OTG and card expansion was flashed, and the report
came back as "the picture is not quite right".

| Item | Result |
|------|--------|
| Card expansion | ✅ root grew to 232 G automatically |
| Boot / SSH / Web UI | ✅ |
| **Stream picture** | ❌ Top half squashed to 2× height, everything below row 539 solid green; kvmd restarting the streamer every 3 seconds (SIGSEGV), 66 times since boot |

The root cause was kernel patch `0007` — `S_FMT` not writing `bytesperline`
back. The debugging order is worth recording, because the first two steps
went the wrong way:

1. Suspected ustreamer's semi-planar patch first → read the code, and the
   offset arithmetic looked correct
2. Ran ustreamer standalone with no client attached → **it does not crash**
   (`JPEG: Passed encoding because nobody is watching`), which pinned the
   problem to the JPEG encoding path
3. Grabbed raw NV16 with `hdmirx-capture` and decoded it by hand → **the raw
   frame is perfect**, ruling out the driver's capture path
4. Measured which row the green starts at in the JPEG → **539**, and
   `chroma + 2*stride*540` lands exactly on the zero padding at the end of
   the chroma plane → pointing at a doubled stride
5. Read ustreamer's `_capture_open_format()` and the driver's S_FMT handler →
   root cause found

**Lesson**: "the snapshot returned a 51 KB JPEG" is not the same as "the
picture is correct". Acceptance has to look at pixels — the cheapest way is
to pull the JPEG down, decode it, and count rows of anomalous colour.

After the fix, each layer was verified independently:

| Combination | Result |
|-------------|--------|
| Old kernel + fixed ustreamer | 10/10 snapshots correct, 90 seconds of continuous MJPEG with zero errors |
| **New kernel + unmodified ustreamer** | 12/12 snapshots, zero green rows, correct geometry |
| Reboot on the new kernel | Fine, back in 80 seconds |
| kvmd end to end (login → websocket → snapshot) | 200 / image-jpeg / correct picture |

#### 4. USB HID confirmed on hardware (2026-09-17)

The Type-C port was connected to a target machine and tested: **OTG works**.
This was the last unverified feature in §9 and can now be struck off.

The same round also surfaced the streaming performance problem — see the
"uncached mmap" part of the ustreamer patch in §2: 830 ms → 79 ms per 1080p
frame, 3 fps → 29.8 fps.

#### 5. Re-verification on a fresh card (2026-09-17)

This closes out the "the latest image has not been re-flashed" caveat. The
image was flashed to a brand-new card and booted, then checked item by item:

| Item | Result |
|------|--------|
| Failed units | None |
| Services | kvmd / kvmd-nginx / kvmd-otg / systemd-timesyncd / bpikvm-firstboot all active |
| Card expansion | 2.8 G → **232 G** |
| Clock | NTP synchronised (`NTPSynchronized=yes`), no longer stuck in 2014 |
| firstboot keys | 3 SSH host keys + the nginx TLS certificate, generated at boot with correct permissions |
| i2c6 pinmux | `0x98007314 = 0x00005005`, both pads on function 1 |
| HDMI RX | 1920x1080 Progressive RGB 60, `Status: Ready` |
| **Picture correctness** | snapshot 200 / 114 KB / 1920×1080, **zero rows of anomalous colour** |
| **Encode performance** | `last_job_time` = **79–80 ms**, capture side `captured_fps` = 60 |
| Streaming | With drop-same-frames off, 327 frames in 12 s = **27.2 fps** |
| USB HID | `/dev/kvmd-hid-{keyboard,mouse}` present, typing verified on hardware |
| Boot time | kernel 12.4 s + userspace 14.7 s = 27.1 s |

Note the acceptance method: **picture correctness means decoding the JPEG and
counting rows of anomalous colour**, not looking at the file size — checking
only the size is exactly how the first flash missed the `S_FMT` bug (items 1
and 3).

#### 6. Real frame rate on moving content, and parameter choices (2026-09-17)

With video playing on the source, so these are not static-screen artefacts:

| | |
|---|---|
| Capture | 53–54 fps |
| **Delivered to the browser** | **22–24 fps** (two clients watching simultaneously, each getting this) |
| CPU | 58%, 40% idle |
| Per-frame encode | 0.083 s |

**How many `--workers`: 3 measured best, and 4 is worse.**

| workers | quality | fps | per frame | bandwidth | encode | CPU |
|---:|---:|---:|---:|---:|---:|---:|
| **3** | 80 | **26.5** | 152 KB | 4045 KB/s | 0.083 s | 57.9% |
| 4 | 80 | 23.8 | 119 KB | 2832 KB/s | 0.081 s | 49.6% |
| **3** | 60 | **27.7** | 83 KB | 2305 KB/s | 0.081 s | 55.8% |
| 4 | 60 | 24.9 | 45 KB | 1129 KB/s | 0.079 s | 48.9% |

A fourth worker competes with the capture and HTTP threads for the four
cores, so throughput drops by about 10% and CPU utilisation falls with it —
that is not "cheaper", it is "cannot be scheduled". **Three workers fill the
four cores without fighting each other.**

(Per-frame size varies a lot at the same quality; that is the video content,
not the parameter. Only the fps column is comparable.)

Dropping `quality` from 80 to 60: **fps barely changes, bandwidth nearly
halves**. If the bottleneck is the network rather than the CPU, this is the
best knob available, and the Web UI has a slider for it — no config edit and
no restart needed.

#### 7. Shipping image flash verification (2026-09-17, after removing DEVMEM)

This is the final build — the image rebuilt after patch `0003` dropped
`CONFIG_DEVMEM` and `build-rootfs.sh` gained the SSH credential assertion —
flashed to another brand-new card:

| Item | Result |
|------|--------|
| kernel | `4.9.119-BPI-W2-Kernel #15` |
| Failed units | **None** |
| Services | kvmd / kvmd-nginx / kvmd-otg / timesyncd / firstboot all active |
| Card expansion | 2.8 G → **29 G** (this was a 32 GB card) |
| Clock | NTP synchronised |
| **`/dev/mem`** | **Does not exist** (`No such file or directory`) — DEVMEM really is off |
| **`authorized_keys`** | **Zero** system-wide |
| SSH host keys | 3, generated at boot |
| HDMI RX | `Status:Ready 1920x1080 Progressive RGB 60` |
| USB HID | `/dev/kvmd-hid-{keyboard,mouse}` both present |
| **Picture correctness** | snapshot 200 / 153 KB / 1920×1080, **zero rows of anomalous colour** |
| **Streaming** | 451 frames in 20 s = **22.6 fps**, 3449 KB/s |
| Capture side | `captured_fps` = **60** (full rate) |
| CPU when idle | 99.3% idle — with no client the streamer does not run, so on-demand works |

#### 8. English-only rebuild, flashed (2026-09-17)

After every document, script, patch and overlay file was translated to
English, the whole tree was rebuilt (`make sources` re-applied all 9 patches
cleanly) and flashed to another card. Confirmed on the board: the login
banner is English, and the kernel config is unchanged
(`# CONFIG_DEVMEM is not set`, `CONFIG_RTK_HDMIRX=y`,
`CONFIG_USB_CONFIGFS_F_HID=y`).

The only differences from item 7 are comment text and the motd; no logic
changed.

#### 9. Not yet verified

- Nothing outstanding. MSD and ATX are deliberately disabled (section 9 of
  this document), which is not the same as unverified.

## 8. Known deviations and landmines

| Symptom | Explanation |
|---------|-------------|
| The board's MAC changes on every boot | Its IP follows, so it has to be rescanned |
| ~~`reboot` hangs~~ | **Corrected**: `reboot` works when booting from the on-board micro SD. Verified over serial: `reboot: Restarting system` → `Starting Kernel ...` → back in a minute. `wait rtk_check_system_ready_to_suspend` also prints in successful reboots and is not the failure point. The early "hang" came from booting via a card reader and LK's manual USB boot, which drops back to LK and waits for a human |
| The SD slot makes poor contact | Occasionally not detected; reseat it or use a card reader |
| **A boot loop after OTG testing** | Seen on 2026-09-17: every boot **hard-reset instantly** at the moment the kernel handed off to initramfs (`Freeing unused kernel memory` → `starting version 237`), with the six occurrences within 0.04 s of each other. No panic, no oops, no kernel message at all.<br><br>Booting manually from u-boot with `root=/dev/ram` — mounting no rootfs whatsoever — produced **exactly the same symptom**, which rules out the root filesystem, fsck and the entire Debian userspace. The kernel also enumerated the SD card fine (236 GiB SDXC with p1 and p2 present), so the card was not at fault.<br><br>The reset point coincides with udev coldplug and the USB gadget binding to the UDC, and a Type-C cable to a target machine (which supplies VBUS) had just been connected. **Unplug the Type-C and power-cycle** is the first thing to try. |
| LK's `usb start` often fails the first time | `lk-boot-usb.sh` always issues it twice |
| The DT pinmux default state has never taken effect | The `*num_maps` bug in the pinctrl driver (see patch 0005). Pin configuration currently relies entirely on the bootloader; we only apply the one group hdmirx needs |
| Default credentials | Web UI `admin`/`admin`, SSH `root`/`pikvm`. Deliberately left at the defaults — an image should let the user change them (`kvmd-htpasswd set admin`, `passwd`) |
| **The second Ethernet port does not work** | It is not a PCIe NIC. It is an RTL8211F on RGMII0, belonging to the hwnat block (`gmac@98060000`), whose DT node is disabled and whose driver does not exist in any public BSP. Full analysis in §11 |
| The image contains no SSH credentials | `build-rootfs.sh` asserts this: if `authorized_keys` / `ssh_host_*_key` / `id_*` appears anywhere in the rootfs, the build fails outright. An `authorized_keys` would make every flashed card accept the same private key (a backdoor), and pre-generated host keys would make every card share one host identity |

---

## 9. Not done yet

- **MSD (virtual media)**: `disabled`. Limited by dwc3's 2 usable IN
  endpoints, it cannot coexist with the mouse; to use it you have to swap
  them, as described in `main.yaml`'s `msd` section
- **ATX**: `disabled`; the 40-pin control lines are not wired
- **`kvmd.streamer.forever`**: left at upstream's default (on-demand). During
  debugging it was temporarily set to `true` on the board to keep the
  streamer resident; that setting did not go into the overlay
- **Automatic resolution following**: `--resolution` is currently hardcoded.
  The driver has no `VIDIOC_QUERY_DV_TIMINGS`, so following the source
  automatically would need a service watching `hdmirx_video_info` and
  restarting the streamer
- **The `*num_maps` bug in the pinctrl driver**: located but deliberately not
  fixed; the reasoning is in patch 0005
- **Hardware encoding**: currently CPU encoding (79–80 ms per 1080p frame, 3
  workers, 30 fps). What hardware exists and what is missing is in §10

---

## 10. Survey of the hardware encoders (2026-09-17)

On paper this SoC has an encoder. BPI's own BPI-W2 specification page says:

> "The Video DSP of RTD1296 is dedicated to manipulating, **decoding and
> encoding** of video streams" / "Video decoding and **encoding can run
> simultaneously**."

And yet there is not one line of encoding code on the Linux side. This
section separates "what the hardware has" from "what is missing", so nobody
starts looking in the wrong place again.

### Four relevant blocks on the SoC

| Block | DT node | State on the board | What it is |
|-------|---------|--------------------|------------|
| **JPU** | `jpeg@9803e000`, IRQ 52 | `/dev/jpu`, `status=okay`, `JPEG_CODEC_IRQ` registered | Chips&Media JPEG codec |
| **VE1** | `ve1@98040000`, IRQ 53 | `/dev/vpu`, `status=okay`, `VE1_CODEC_IRQ` registered | Chips&Media CODA9 / WAVE4 family |
| **VE3** | `ve3@98048000` | `CONFIG_VE3_CODEC` not enabled | Hantro, **decode only** (`hantrodec`, `IOCS_DEC_PUSH_REG`; no `hantroenc`) |
| **Video DSP** | Reached over RPC | `/dev/rpc0`–`rpc7` all present | This is what the specification's "encoding" refers to |

`CONFIG_RTK_CODEC=y` / `VE1_CODEC=y` / `IMAGE_CODEC=y` were already in the
BSP's defconfig and had simply gone unnoticed — **the drivers have been built
in and probing all along**.

### But the kernel drivers are only a device interface

`drivers/soc/realtek/rtd129x/rtk_ve/` (8512 lines) contains no codec logic at
all. The ioctls are:

```
JDI_IOCTL_ALLOCATE_PHYSICAL_MEMORY / FREE_PHYSICALMEMORY
JDI_IOCTL_WAIT_INTERRUPT / ENABLE_INTERRUPT
JDI_IOCTL_SET_CLOCK_GATE / RESET
JDI_IOCTL_GET_INSTANCE_POOL / OPEN_INSTANCE / CLOSE_INSTANCE
```

That is "allocate contiguous memory, wait for an interrupt, gate the clock,
mmap the registers". **The actual register programming lives in userspace**,
which is Chips&Media's standard architecture.

### What is missing lives in the Android tree

Under `vendor/bpi-1296-android7/`:

```
android/device/realtek/kylin/common/prebuilt/vendor/modules/
    ve1.bin   583 KB      ← VE1's codec firmware
    ve2.bin    90 KB
android/device/realtek/kylin/common/prebuilt/system/etc/init/
    jpuinit.rc / vpuinit.rc      ← run /system/bin/jpuinit and vpuinit
android/hardware/realtek/VideoEngine/
    VE1/vpuapi/ + coda9/ + wave/wave4/ + wave/coda7q/
    VE1/vdi/
    JPEG/jpuapi/ + jdi/ + src/
    VE3/software/
    libvpu.so 481 KB / libjpu.so 55 KB / libvp9.so 80 KB
```

**`VideoEngine/` contains 149 `.h` files and zero `.c` files** — the register
definitions and API headers are complete, but the implementation is a blob.
For writing a driver the headers are in fact the half that matters
(`JPEG/jpuapi/regdefine.h` is 141 lines, `jpuapi.h` 344).

`ve1config.h` lists CODA7542 / CODA960 / CODA980 / WAVE320 / WAVE410 /
WAVE420 / WAVE412 as supported parts, and includes:

```c
/* for WAVE420 */
#define W4_MIN_ENC_PIC_WIDTH  256
#define W4_MAX_ENC_PIC_WIDTH  8192
```

WAVE420 is Chips&Media's H.265/H.264 **encoder** IP, and CODA960/980 can
encode H.264 as well. The kernel's `ve1.c` is a generic driver (131 `W4_`
registers plus 37 `BIT_` ones, covering both the WAVE and CODA9 families), so
**which part this actually is has to be read from the product ID at
runtime**.

### The Video DSP's firmware is nowhere in the BSP

u-boot says so itself during boot:

```
------------can't find tmp/factory/video_rpc.bin
```

The BSP only has `bluecore.audio` (the Audio DSP's firmware, which our image
does place in the boot partition); `video_rpc.bin` appears nowhere in the
tree. So the Video DSP the specification talks about never comes up at all.

### Conclusion: three routes of very different difficulty

| Route | What it takes | Assessment |
|-------|---------------|------------|
| **JPU (most promising)** | Read the 4 KB register block at `0x9803e000` and drive it per `regdefine.h` | **No firmware needed** (`request_firmware` has zero hits in `jdi/jpu.c`). And **we already use MJPEG**, so no janus and no change to kvmd's streaming mode. Success would eliminate the 79 ms per frame outright |
| **VE1 (H.264)** | Load `ve1.bin`, reverse-engineer `libvpu.so`'s RPC/register sequences, wrap it as V4L2 M2M, install janus-gateway + `kvmd-janus` | The firmware exists and the headers exist, but the implementation has to be reverse-engineered. ustreamer's `--h264-sink` also only speaks V4L2 M2M |
| **Video DSP** | Find `video_rpc.bin` first | Not in the BSP. Candidate sources: BPI's Android image, or firmware from other RTD1295/1296 devices (WD My Cloud Home, Zidoo X9S, Synology DS418 — DSM does hardware transcoding) |

### Probed on the board (2026-09-17)

Measured on the card that still had `CONFIG_DEVMEM`.

**Where the clocks and resets live** (all plain read-modify-write, not
write-1-to-clear; `CLK_MMIO_GATE_HAS_WRITE_EN` is only used by RTD16xx):

| | Location | State after boot |
|---|---|---|
| `clk_en_jpeg` | `clk_en_2` (`0x98000010`) bit 3 | **0 (off)** |
| `RSTN_JPEG` | `rst2` (`0x98000004`) bit 1 | **0 (held in reset)** |
| `clk_en_ve1` | **Not in `clk_en_2`** — bit 12 of that node is empty; VE1 goes through the `cc` clock controller (`<&cc CC_CLK_VE1>`) | — |

**The experiment**: enable `clk_en_jpeg`, release `RSTN_JPEG`, then read
`0x9803e000`:

```
before: clk_en_2=0xc73ee416 (jpeg bit3=0)  rst2=0x5f840e3d (RSTN_JPEG bit1=0)
after : clk_en_2=0xc73ee41e               rst2=0x5f840e3f
JPU 0x9803e000, first 0x40 bytes:
  +000: deadbeef deadbeef deadbeef deadbeef
  +010: deadbeef deadbeef deadbeef deadbeef
  ...
restored: clk_en_2=0xc73ee416 rst2=0x5f840e3d
```

Two conclusions:

1. **The board did not hang.** The rbus returns `0xdeadbeef` as a safe
   default when no device responds, rather than locking up the interconnect.
   So **probing registers on this SoC is safe** — the earlier worry that a
   read might hang was overcautious, and even a genuine hang only costs a
   power cycle. A register read cannot brick the board.
2. **Enabling the clock and releasing reset is not enough** — the block does
   not respond.

**The missing piece is most likely a power domain.** The BSP has these SRAM
power domains:

```
pctrl_ve1   pctrl_ve2   pctrl_ve3
pctrl_disp_hdmi_rx   pctrl_disp_mipi   pctrl_usb_*
```

HDMI RX only works because of `power_control_get("pctrl_disp_hdmi_rx")`
(`hdmirx_clk_ctrl.c`), and the VE blocks are presumably the same. Continuing
means finding the registers behind `pctrl_ve*` and powering the domain up
before reading.

**Another trap**: `JDI_IOCTL_SET_CLOCK_GATE` is a **no-op** in this BSP —
`JPU_SUPPORT_CLOCK_CONTROL` at `jpu.c:29` is commented out, so the ioctl
accepts its argument and does nothing. You cannot use it to enable the clock.

### What to do next

1. Find the registers behind `pctrl_ve1` / `pctrl_ve2` (the power-control
   implementation under `drivers/soc/realtek/`) and power the domain up
2. Read VE1's product ID to identify the part:
   `W4_PRODUCT_NAME = 0x98041040`, `W4_PRODUCT_NUMBER = 0x98041044`
3. If it is a CODA960/980 or WAVE420, it can encode H.264 and is worth
   pursuing
4. If it is only a WAVE410/412 (HEVC decode only), the H.264 route ends here
   and the JPU becomes the target

The shipping image no longer has `CONFIG_DEVMEM`, so any of this needs a
debug defconfig layered on top (see `0003` in §1), or a dedicated debug card.

### Why Realtek's Linux support looks like this

Mainline's `arch/arm64/boot/dts/realtek/` holds only Synology's
`rtd1296-ds418.dts` / `rtd1293-ds418j.dts` plus a few Android TV box board
files, all roughly 30-line stubs; MAINTAINERS' ARM/REALTEK covers only `dts/`
and `pinctrl/`; and `drivers/soc/realtek/` does not exist in mainline at all.

**Realtek's Linux support for this part is a shell that boots, with all the
media capability tied to Android's OMX / HAL.** That is the same conclusion
`07-mainline.md` and `08-kernel-uplift.md` reach, seen from another angle.

---

## 11. The second Ethernet port (2026-09-17)

The BPI-W2 has two RJ45 sockets. Only one of them works, and the reason is
worth recording because you cannot tell any of it from looking at the board.

### It is not a PCIe NIC

Read off BPI's official schematic:

```
RTD1296 -- RGMII0 (RXC/RXCTL/RXD0-3, TXC/TXCTL/TXD0-3, MDIO/MDC)
        +- RTL8211F-JA-CG (QFN40)
             +- magnetics-side nets are named NAT0_MDI0p/n, NAT0_MDI1p/n, NAT0_MDI2p/n
```

The `NAT0_` prefix is the giveaway: this PHY belongs to the SoC's hardware
NAT engine, not to the GMAC that drives the working port.

So the two sockets are driven by **two entirely different MACs**:

| Socket | MAC | PHY | State |
|--------|-----|-----|-------|
| Working | `nic: gmac@98016000` (`Realtek,r8168`, driven by `r8169soc.c`) | The SoC's **embedded** PHY (`ETN_MDIP/N0-3`) | `eth0` |
| Dead | `hwnat: gmac@98060000` (MAC0 of the hardware NAT engine) | External **RTL8211F** over RGMII0 | — |

### Two reasons it does not work

**The DT node is disabled.** `rtd-1296-bananapi-common.dtsi`:

```dts
hwnat: gmac@98060000 {
    compatible = "Realtek,rtd1295-hwnat";
    status = "disabled";
};
```

and the board file that actually takes effect,
`rtd-1296-bananapi-w2-2GB.dts`, overrides `nic`, `pcie@9804E000`,
`pcie2@9803B000` and `sdmmc` but never mentions `hwnat`.

**The driver does not exist.** Searching the whole BSP for `hwnat` — in
`*.c`, `*.h`, `Kconfig` and `Makefile` — returns nothing.
`drivers/net/ethernet/realtek/` contains only `8139*`, `atp`, `r8125`,
`r8168`, `r8169.c` and `r8169soc.c`. So `status = "disabled"` is not an
oversight: there is nothing for that node to bind to.

u-boot hints at the same thing during boot:

```
Unable to update property /gmac@0x98060000:local-mac-address, err=FDT_ERR_NOTFOUND
```

### Where the driver actually lives

`vendor/bpi-1296-android7/.build_config`:

```
CONFIG_TARGET_BUILD_TYPE   openwrt      <- this board ships as a router
CONFIG_GIT_SERVER_URL      rsgerrit     <- Realtek's internal Gerrit
CONFIG_IMAGE_TARGET_BOARD  bananapi
```

and `build_prepare.sh`:

```
OPENWRTDIR       = $SCRIPTDIR/Openwrt          <- not present in the repo
OPENWRTKERNELDIR = $OPENWRTDIR/linux-4.1.7     <- a second kernel
```

The RTD129x ships as an **Android + OpenWrt dual system**: Android runs the
4.9 kernel we use, while OpenWrt runs a separate 4.1.x kernel. Community
work on other RTD129x devices corroborates this (RTD1295/X9S on Linux
4.1.17, RTD1296/Z9S on 4.1.35). The router features -- hwnat, the second
MAC, the switch -- live in that 4.1.x tree, which is pulled from Realtek's
internal Gerrit and was never published.

BPI's wiki matches: the image described as "router mode android image with
hwnat and openwrt enabled" is a **kernel 4.1** image, and the Android 6.0
image is documented as "without second ethernet port enabled".

Cross-checked against three public BPI-W2 BSP trees -- the official one,
`jjm2473/BPI-W2-bsp-tmp` (by someone who ports OpenWrt to RTD129x) and
`minhng99/BPI-W2-bsp-4.4_public`. None contains hwnat, and jjm2473's fork
still has the node disabled: that port reuses the vendor's 4.1.x kernel and
only replaces the OpenWrt userspace, sidestepping the problem rather than
solving it.

### Would a prebuilt image help?

Only the kernel-4.9 "OpenWrt LEDE" image is even the right major.minor. Even
then:

- if hwnat is built in (`=y`), it is linked into vmlinux and cannot be
  extracted into another kernel build
- if it is a module, `vermagic` must match ours exactly
  (`4.9.119-BPI-W2-Kernel SMP preempt mod_unload aarch64`), and every symbol
  it imports must be exported by our kernel
- and it would still need the DT node enabled plus whatever clock, reset and
  power domain the block wants -- compare §10, where the JPU returned
  `0xdeadbeef` even with its clock enabled and reset released, because a
  power domain was still missing

Booting their kernel instead is not an option: that discards every HDMI RX
patch this project depends on.

### The theoretical alternative, and why it is not useful here

`r8169soc.c` can drive an external RGMII PHY:

```c
enum rtl_output_mode {
    OUTPUT_EMBEDDED_PHY,     /* what we use today */
    OUTPUT_RGMII_TO_MAC,
    OUTPUT_RGMII_TO_PHY,     /* this one */
};
#define RGMII0_PAD_CTRL_ADDR    0x9801a960
#define ISO_RGMII_MDIO_TO_GMAC  0x98007064
```

So the working MAC could in principle be switched to RGMII0 and drive the
RTL8211F (`output-mode = <2>` plus `ext-phy-id`). But that is **either/or**,
not both: one MAC can only serve one PHY. You would trade the working socket
for the dead one, which gains a KVM nothing. It would also need the DT
pinmux to work, and on this board it never has (see `04-hdmi-rx-bringup.md`).

### The pattern

This is the same shape as the hardware encoders in §10:

| | Codec (VE1 / JPU) | hwnat (second NIC) |
|---|---|---|
| Hardware | present | present (RTL8211F on RGMII0) |
| DT node | `status=okay`, probes | `status=disabled` |
| Kernel driver | **yes** (device interface only) | **none at all** |
| What is missing | the userspace implementation (blob found in the Android tree) | **the entire driver**, in Realtek's internal 4.1.x tree |
| Tractability | register definitions exist and can be worked from | not even register definitions exist |

hwnat is the harder of the two: §10 at least has 149 headers of register
definitions to work from, whereas `gmac@98060000`'s 0x170000-byte register
window is a complete black box.

**None of this is a blocker for PiKVM** -- one network port is all a KVM
needs. It is recorded here so nobody has to re-derive it.
