# The kernel-6.18 line: what it delivers and how to build it

This is the summary of the `kernel-6.18` branch: PiKVM on the BPI-W2 with
Linux 6.18 LTS, Arch Linux ARM and PiKVM's own packages. The bring-up
story, with every finding and how it was verified, is in
`09-mainline-bringup.md`. Section numbers below (§n) refer to that file.

The `main` branch keeps the BSP 4.9 line (Debian 13); this branch can build
both, and the two are not merged.

## 1. What works

All of the following was verified on the board (BPI-W2, 2 GiB, Kodi on a
Raspberry Pi as the HDMI source, a PC on the Type-C port) on 2026-10-01.

| Feature | State | How it was verified | Where |
|---|---|---|---|
| Boot from SD | done | BSP u-boot from SPI + SD, 6.18 kernel, root on `mmcblk0p2`; no failed units | §5, §8 |
| Four cores, interrupts | done | spin-table release through MMIO; RTD129x ISO/MISC interrupt muxes | §6 |
| SD card (root) | done | own driver for the rtsx-style SD core, 50 MHz, ~25 MB/s read | §8 |
| Gigabit Ethernet | done | DHCP, ssh, kernels installed over the network | §9 |
| Reboot | done | watchdog restart handler | §9 |
| USB host ports | done | hub and a card reader enumerate | §7 |
| USB OTG (Type-C) | done | the target enumerates keyboard, mouse and mass storage | §7, §10 |
| HDMI capture | done | 1080p60 from the receiver, EDID over DDC, NV12 | §10 |
| MJPEG stream | done | kvmd's ustreamer, ~22 fps of JPEG to a client | §10 |
| H.264 encoding (VE1) | done | CODA980 through the mainline `coda` driver, 1080p, ~70 fps capacity | §11 |
| Direct H.264 (kvmd-media) | done | the Web UI's H.264 mode | §11 |
| WebRTC (Janus) | done | 1920x1080 at ~30 fps to a headless WebRTC client | §11 |
| HDMI audio | done | ALSA card `hdmirx`, 48 kHz stereo; music from Kodi over WebRTC (Opus) | §13 |
| Keyboard, mouse | done | through kvmd to the target | §10 |
| Virtual media (MSD) | done | image upload and connect through kvmd | §10 |
| Web terminal | done | `kvmd-webterm` (ttyd) in the Web UI | §12 |
| Package updates | done | `pacman -Syu` works; ustreamer and kvmd are held back (IgnorePkg) | §12 |

Not done:

- **eMMC**: no eMMC host driver on this line, and no image that boots from
  the eMMC (see §5 of this file).
- **ATX power control**: not wired on this board.
- **Audio to the target and the webcam (Janus aplay/vplay)**: not wired.
- **The board's own HDMI output**: no driver; the console is the serial port
  and the network.

## 2. Building

The host needs `docker`, `git`, `bash` and `make`. Everything else runs in
containers. The arm64 rootfs build uses qemu-user (binfmt) through docker.

```sh
make builder builder-mainline   # the two build containers
make sources sources-mainline   # BSP, kvmd, ustreamer, Janus, Linux 6.18; applies patches/
make uboot                      # the BSP u-boot (also used by the mainline image)
make kernel-mainline            # Image, dtb and modules
make rootfs-arch                # Arch Linux ARM + PiKVM packages -> build/rootfs-arch.tar
make image-mainline             # -> build/bpiw2-pikvm-mainline.img (3 GiB)
```

| Target | What it does | Output |
|---|---|---|
| `builder-mainline` | Debian trixie with the arm64 cross toolchain | docker image `bpiw2-pikvm/builder-mainline:trixie` |
| `sources-mainline` | Fetches Linux 6.18.y and applies `patches/linux-mainline` | `vendor/linux-mainline` |
| `kernel-mainline` | Copies `kernel/mainline/*` into the tree, merges `bpiw2.config` on top of arm64 defconfig, builds | `arch/arm64/boot/Image`, the board dtb, modules |
| `rootfs-arch` | ALARM aarch64 tarball, PiKVM's repository, ustreamer and kvmd built from our patched trees, Janus and kvmd-webterm from PiKVM | `build/rootfs-arch.tar` |
| `image-mainline` | Boot partition (u-boot's files, the kernel with its text offset patched, the audio firmware), rootfs, modules, VPU firmware | `build/bpiw2-pikvm-mainline.img` |

`make image` and the targets without `-mainline` build the BSP 4.9 line
with its Debian rootfs, as on `main`.

### Flashing and first boot

```sh
sudo dd if=build/bpiw2-pikvm-mainline.img of=/dev/sdX bs=4M conv=fsync status=progress
```

- SW4 = 1 (SPI + SD). The Type-C port goes to the target machine.
- The first boot grows the root partition and generates the SSH host keys
  and the TLS certificate. The image itself carries no keys.
- Web UI: `https://<board IP>/`, `admin` / `admin`. Console (serial
  115200 or ssh): `root` / `pikvm`. Change both.

### Working on a running board

| Script | Use |
|---|---|
| `scripts/push-kernel-mainline.sh [--modules] [--reboot]` | Install a new kernel, dtb and modules over ssh (`BOARD_HOST=<ip>`); the old ones stay as `*.prev` |
| `scripts/board-ssh.sh '<cmd>'`, `--put <local> <remote>` | Run a command on the board, copy a file to it |
| `scripts/boot-prev-kernel.sh [--restore]` | Over the serial console, boot `uImage.prev` once |
| `scripts/uboot-cmd.sh`, `scripts/serial-cmd.sh` | Drive u-boot or a shell on the serial port |

A module can be rebuilt and replaced without a reboot: `make kernel-mainline`,
copy the `.ko` into `/usr/lib/modules/6.18.*/`, and `rmmod`/`modprobe`
(stop kvmd first for hdmirx and coda). `rtd129x-acpu` is the exception: it
cannot be unloaded.

## 3. What the branch consists of

### Kernel patches (`patches/linux-mainline`)

| Patch | What |
|---|---|
| 0001, 0002 | Build the BPI-W2 dtb; its binding |
| 0003, 0004 | The ISO/MISC peripheral interrupt muxes and their DT nodes |
| 0005, 0006 | MMIO `cpu-release-addr` for the spin table; all four cores |
| 0007 | The SD host |
| 0008 | The embedded gigabit MAC (`r8169soc`) |
| 0009 | Watchdog restart handler |
| 0010 | CRT clock gates |
| 0011 | Hooks for the HDMI receiver driver |
| 0012 | The CODA980 (VE1) in the `coda` driver |
| 0013 | Hooks for the audio CPU and HDMI audio drivers |

### Out-of-tree files (`kernel/mainline`), copied in at build time

| File | What |
|---|---|
| `rtd1296-bananapi-w2.dts` | The board: reserved memory for the Realtek firmware, every device |
| `bpiw2.config` | The config fragment on top of arm64 defconfig |
| `irq-rtd129x.c`, `sdmmc-rtd129x.c`, `r8169soc.c`, `clk-rtd129x-crt.c` | Drivers behind patches 0003, 0007, 0008, 0010 |
| `hdmirx/` | The HDMI receiver: Realtek's BSP driver with a new V4L2 side |
| `acpu/` | `rtd129x-acpu` (RPC to the audio CPU firmware) and `snd-rtd129x-hdmirx` (ALSA capture) |
| `diag/` | A diagnostic initramfs used during bring-up |

### Userspace

| Path | What |
|---|---|
| `scripts/build-rootfs-arch.sh`, `scripts/rootfs-arch.sh` | The Arch rootfs build |
| `patches/ustreamer` | NV12/NV16 capture; single-planar M2M encoders (coda) |
| `patches/kvmd` | Python 3.13 fix; one HID function for keyboard and mouse, MSD on mainline |
| `patches/janus` | PiKVM's `janus.js` change (used by the Debian line's own Janus build) |
| `overlay/usr/lib/kvmd/main.yaml`, `overlay/usr/lib/kvmd/platform` | kvmd's platform configuration for this board |
| `overlay/etc/kvmd/janus/janus.plugin.ustreamer.jcfg` | Janus: the H.264 sink and the `hdmirx` audio |
| `overlay/usr/lib/udev/rules.d/99-kvmd-bpi-w2.rules` | Device names kvmd expects (`/dev/kvmd-video`, `/dev/kvmd-h264`, ...) |
| `overlay/usr/local/bin/bpikvm-*` | First boot: grow the root, generate keys |
| `overlay-bsp/` | What only the BSP image gets (no H.264, NV16 capture) |

### Firmware the image carries, not built here

- u-boot (`vendor/bpi-w2-bsp/u-boot-rtk`, built by `make uboot`) and the
  SPI bootcode from the BSP.
- `bluecore.audio`, the audio CPU's firmware, from the BSP.
- The CODA980 firmware (`vpu_fw`), downloaded by
  `scripts/fetch-vpu-firmware.sh`.

## 4. Things to know

- **The audio CPU.** u-boot starts the audio firmware before the kernel.
  `rtd129x-acpu` completes its start-up; without that module nothing breaks,
  but there is no audio. The firmware's debug output ("HDMI not enabled"
  every 3 s on the serial console) is turned off by the driver, by clearing
  bit 0 of the firmware's debug flag once it has started; `fw_debug=1` keeps
  it. Verified from a fresh flash: none in 45 s after boot, and audio over
  WebRTC unaffected.
- **Memory the firmware owns.** The DTS reserves the audio firmware (5 MiB at
  `0x0f900000`), its RPC pages, the audio and media heaps and the regions u-boot
  reserves. The kernel loads at `0x1c000000`, above all of them (§5).
- **The HDMI source decides.** Audio needs the source to send it over
  HDMI. A Kodi set to Bluetooth output sends none. `docs/09` §13 shows how
  to see which packets arrive.
- **The serial console is shared** with the audio firmware.
- **No RTC battery**: the clock is right only after NTP.

## 5. eMMC

There is no eMMC image yet. Two things stand in the way:

1. **Booting.** With SW4 = 0, or with no SD card, the SPI bootcode loads
   the eMMC's own boot chain: FSBL, OP-TEE, BL31 and LK from the Android
   install. On that path this kernel corrupts memory under load (§5). An
   eMMC that boots this system needs the BSP u-boot in a layout the bootcode
   accepts from eMMC. The BSP has a config for it
   (`rtd1295_qa_emmc_bananapi.h`), but no tool that writes it.
2. **The root filesystem.** Mainline has no driver for the RTD129x eMMC
   controller (`rtk1295-emmc` in the BSP). It needs one, as the SD host
   did (§8).
