# Image layout and the boot chain

This document covers the physical layout of the SD image, the BPI-W2 boot
chain, and a handful of undocumented constraints that could only be worked
out from BSP sources and from the board itself.

> This file started life as the write-up for the first verification image,
> back when it carried neither kvmd nor ustreamer. The image is now the
> complete, usable article — see `../README.md` for how to use it and
> `05-userspace.md` for the userspace side.

## How it is produced

```
make all
```

or step by step: `make builder sources kernel uboot rootfs image`.
Output: `build/bpiw2-pikvm.img` (3 GB).

## Flashing

```
sudo dd if=build/bpiw2-pikvm.img of=/dev/sdX bs=4M conv=fsync status=progress
```

## Image layout

Derived from `scripts/dd_download.sh` and `bootloader.sh` **inside the BSP
tree** (not this project's `scripts/`):

| Offset | Contents |
|---|---|
| 40 KiB | `u-boot.bin` |
| sectors 204800–327679 (60 MiB) | p1, vfat, `BPI-BOOT` |
| sector 327680– | p2, ext4, `BPI-ROOT` |

Contents of p1 (**both sets of filenames are present** — see below):

```
bananapi/bpi-w2/linux/uImage                       ← arch/arm64/boot/Image
bananapi/bpi-w2/linux/bpi-w2.dtb                   ← path compiled into u-boot
bananapi/bpi-w2/linux/rtd-1296-bananapi-w2-2GB.dtb ← path uEnv.txt uses (same file)
bananapi/bpi-w2/linux/root.sd.cpio.gz_pad.img      ← path compiled into u-boot
bananapi/bpi-w2/linux/uInitrd                      ← path uEnv.txt uses (same file)
bananapi/bpi-w2/linux/bluecore.audio
bananapi/bpi-w2/linux/uEnv.txt
spirom-bpi-w2.bin                                  ← SPI ROM updater, kept for rescue
```

### Why two sets of filenames

The BSP's `u-boot-rtk` **does not read `uEnv.txt`**. Booting is driven by the
compiled-in `CONFIG_BOOTCOMMAND = "run set_sdbootargs && gosd;"`, and `gosd`
→ `boot_from_sd()` (`common/cmd_boot.c:500`) uses the default paths from
`include/configs/rtd1296_qa_sd_bananapi.h:123-126`:

```c
#define CONFIG_BOOT_FROM_SD_DTB        "bananapi/bpi-w2/linux/bpi-w2.dtb"
#define CONFIG_BOOT_FROM_SD_ROOTFS     "bananapi/bpi-w2/linux/root.sd.cpio.gz_pad.img"
#define CONFIG_BOOT_FROM_SD_VMLINUX    "bananapi/bpi-w2/linux/uImage"
#define CONFIG_BOOT_FROM_SD_AUDIO_CORE "bananapi/bpi-w2/linux/bluecore.audio"
```

The BSP's own `build.sh`, however, lays files out using `uEnv.txt`'s names,
and the two disagree. A board carrying a stale u-boot environment (with
`sd_boot_dtb` and friends set) will take the uEnv.txt path, so shipping both
sets is the safe option.

Note that **a failure to load `bluecore.audio` aborts the boot outright** —
that branch in `cmd_boot.c` has no `#if 0` guard — whereas a failed initrd
load is non-fatal.

### bootargs

Compiled into u-boot (`include/configs/rtd1295_common.h:149-151`):

```
earlycon=uart8250,mmio32,0x98007800 fbcon=map:0 console=ttyS0,115200 loglevel=7
board=bpi-w2 rootwait root=/dev/mmcblk0p2 rw
```

The serial console is **ttyS0 at 115200**.

### The initrd memory constraint

`CONFIG_ROOTFS_LOADADDR = 0x02200000` and the kernel loads at `0x03000000`,
leaving only 14 MiB between them (the DT's `ROOTFS_NORMAL_SIZE` reserves a
mere 4 MiB).

initramfs-tools' default `MODULES=most` produces a **27 MB** initrd, which
lands straight on top of the kernel. `scripts/build-rootfs.sh` therefore sets
`MODULES=list` (an empty list), which yields about 10.8 MB. This image still
ships the BSP's original 7.5 MB `uInitrd`, which is the Debian
initramfs-tools initrd built for `4.9.119-BPI-W2-Kernel` and the safest
choice.

In practice SD/MMC and ext4 are both built into this kernel
(`CONFIG_MMC_RTK_SDMMC=y`, `CONFIG_EXT4_FS=y`), so root mounts without an
initramfs anyway.

## Logging in

- Serial `ttyS0` at 115200, or SSH
- User `root`, password `pikvm` (**change it before real use**)
- Hostname `bpi-w2-pikvm`, networking via systemd-networkd DHCP

The serial console gets flooded by the Realtek audio driver's
`[AO][_AO_if_video_HDMI_mode]HDMI not enabled`, which frequently buries the
login prompt. Run `dmesg -n 1` first to quiet it.

## Verifying after boot

The full hardware verification results live in section 7 of
`06-changes.md`. Quick self-check:

```sh
hdmirx-info              # input timings and V4L2 caps
df -h /                  # did the card get expanded
ls -l /dev/kvmd-video /dev/kvmd-hid-*
systemctl status kvmd kvmd-nginx kvmd-otg
```

With a 1080p60 source attached, `hdmirx-info` should report:

```
Type:HDMIRx
Status:Ready
Width:1920
Height:1080
ScanMode:Progressive
Color:RGB
Fps:60
```

> The driver asks for minor 250, but `video_register_device()` falls back to
> the first free number when it cannot have it (on real hardware, `video0`),
> so **do not hardcode `/dev/video250`**. Both the udev rule and
> `hdmirx-info` identify the device by the driver-specific sysfs attribute
> `hdmirx_video_info`.

## What to collect when something breaks

1. The complete serial boot log, starting from u-boot
2. `dmesg | grep -i -E 'hdmi|mipi|ion'`
3. Full output of `hdmirx-info`
4. `v4l2-ctl -d /dev/kvmd-video --all`
5. `journalctl -b -p err --no-pager`
