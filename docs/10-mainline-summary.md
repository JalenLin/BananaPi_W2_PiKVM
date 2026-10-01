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

- **eMMC as the system disk**: the board boots from the eMMC's own
  bootloader now, but the kernel's eMMC driver does not transfer data yet,
  so the system still lives on the SD card (§5 of this file).
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

Status on 2026-10-02: **the board boots from the eMMC's bootloader**. The
**kernel cannot use the eMMC yet**, so the root filesystem stays on the SD
card.

### The bootloader on the eMMC (done)

The SPI flash bootcode loads u-boot from the SD card when there is one (SW4
= 1). Otherwise, and always with SW4 = 0, it loads the bootcode from the
eMMC. The BSP sources cannot build that bootcode: the flash writer needs
Realtek blobs that are not in the tree (`bootmon-new.bin`, the FSBL
loader). The eMMC bootloader therefore comes from BPI's own release, the
"Hardware files" zip on the BPI-W2 wiki: `dvrboot.exe.bin` plus
`RTD1296_hwsetting_BOOT_4DDR4_4Gb_s1866_padding.bin`. It is written over
the serial port through the SoC ROM's download mode:

```sh
# board running, SW4 = 1, SD card in: tools/emmc/romflash.py sends ctrl+q
# while the board reboots, lands in the ROM's "d/g/r" prompt, sends the
# hwsetting (h + Y-modem), sets 0x98007058 = 0x01500000, sends dvrboot
# (d + Y-modem) and runs it (g), which programs the eMMC.
docker run --rm --device /dev/ttyUSB0 -v $PWD/tools/emmc:/io -v <release dir>:/r \
    python:3-slim python3 -u /io/romflash.py /r/<hwsetting>.bin /r/dvrboot.exe.bin
# then reboot the board (ssh) -- stage 1 stops after the hwsetting;
# rerun with --stage2 for the bootloader itself
```

The ROM loader's Y-modem receiver sends `C` without pause, and the text
before it (`download to 0x80006C30`) contains a `C`. romflash.py waits for
the `Ymodem:` line and drops stray `C`s before each block.

The flash writer puts everything in the first 1.3 MiB of the user area,
below the first partition of our image at 100 MiB:

| What | Block |
|---|---|
| hwsetting | 0x100 |
| u-boot 2015.07 (BPI, 2018-04-27) | 0x107 |
| FSBL, OP-TEE (`fsbl_os`), BL31 | 0x4F6, 0x583, 0x962 |
| u-boot's environment (factory area) | 0x1100 |

The chain is FSBL, then OP-TEE 2.1, then BL31, then u-boot. The kernel
corrupted memory under the Android/LK chain (`09` §5); under this one it
does not. Over 7 minutes of load (two 250 MiB copies, 40 parallel
gzip/gunzip round trips) every byte compared equal, with no oops.

The environment, saved with `saveenv`, boots our files from FAT partition
1. It tries the SD card first, then the eMMC, then falls back to the
vendor's `bootr`:

```
audio_loadaddr=0x0f900000            (the firmware's link address; the default 0x01b00000 breaks the ACPU)
initrd_high=0xffffffffffffffff       (else: "ramdisk - allocation error")
bpidir=bananapi/bpi-w2/linux
bpiload=fatload ${bdev} 0:1 0x02100000 ${bpidir}/bpi-w2.dtb && fatload ${bdev} 0:1 0x0f900000 ${bpidir}/bluecore.audio && fatload ${bdev} 0:1 0x03000000 ${bpidir}/uImage && fatload ${bdev} 0:1 0x31400000 ${bpidir}/uInitrd
bpiboot=go a; booti 0x03000000 0x31400000 0x02100000
bootsd=setenv bdev sd; setenv bootargs ${console_args} root=/dev/mmcblk0p2 rw rootwait; run bpiload && run bpiboot
bootemmc=setenv bdev mmc; setenv bootargs ${console_args} root=/dev/mmcblk1p2 rw rootwait; run bpiload && run bpiboot
bootcmd=run bootsd; run bootemmc; run set_emmcbootargs; bootr
bootprev=   (as bootsd, with uImage.prev and bpi-w2.dtb.prev)
bootnoemmc= (as bootsd, with the eMMC node set disabled by "fdt set" first)
```

`bootdelay` is 0. To reach the prompt, send Esc while it boots
(`tools/emmc/ubstop.py` does that, then runs the commands given).
`tools/emmc/ubcmd.py` runs commands at a prompt that is already waiting.
This u-boot's hush expands `${...}` inside double quotes when setting a
variable, so set them with single quotes. It also has no `boot` command;
use `run bootcmd`.

### The kernel driver (in progress)

The eMMC controller at `0x98012000` is a Synopsys DesignWare MSHC. The
register map, VERID `0x270a`, IDMAC and 32-bit addressing all match. It
has a Realtek wrapper at +0x400. `kernel/mainline/dw_mmc-rtd129x.c` puts
it on mainline's `dw_mmc` (patch 0014). The DT node is **disabled** until
data transfers work.

What works, and what was learned:

- **Interrupts.** The wrapper's ISR register (`+0x424`) gates the core's
  interrupt. Its own `DMA_DONE` status latches on transfers, and dw_mmc
  does not know it: left unmasked, it is an interrupt storm that hangs the
  board. dw_mmc's handler always returns `IRQ_HANDLED`, so the kernel
  cannot notice. Mask the wrapper's DMA and descriptor bits, clear
  `DMA_DONE`, and pass only the core's interrupt (bit 4).
- **1.8 V.** The pads are 1.8 V (`UHS_REG` bit 0). The MMC core asks for
  3.3 V first, which would clear it; the glue refuses 3.3 V.
- **The card answers.** CMD0/1/2/3/9/7 and the EXT_CSD read go through, at
  25 MHz: `mmcblk1: mmc1:0001 8GME4R 7.28 GiB`. At 50 MHz, without the
  HS200 phase tuning u-boot did for 200 MHz, the switch to the 8-bit bus
  fails.
- **Data transfers do not complete.** After the first one, every transfer
  finds its descriptor still owned by the IDMAC. The IDMAC fetches it (the
  wrapper's `IP_DESC0..3` mirror it), but `IDSTS` never shows RI/TI. The
  wrapper's `DMA_DONE` is what latches instead. The BSP writes fresh
  descriptors for every transfer and never looks at OWN. Ignoring OWN
  makes it worse: the next transfer starts on a busy IDMAC and the board
  hangs. Not the cause: fixed bursts, FIFOTH's burst size, the wrapper's
  `SWC_SEL*`/`CP` settings (the glue sets them as the BSP does). The
  chip's revision is past the one that needs the BSP's `lockapi`
  workaround.

Next: complete transfers on the wrapper's `DMA_DONE`, as the BSP's
`rtkemmc_wait_opt_end` does. That means a change in dw_mmc's DMA
completion, or the BSP's sequence carried over in the glue. After that
come the eMMC image itself (our SD layout behind the first 1.3 MiB the
bootcode uses, `root=/dev/mmcblk1p2`) and HS200.

Testing traps:

- Debug output from the MMC core also covers `mmc0`, the root
  filesystem; at 115200 baud on the console it stalls the whole system.
- A hung eMMC transfer blocks `sync`, so `sync; reboot` hangs. Use
  `echo b > /proc/sysrq-trigger` (with `kernel.sysrq` set to 1 first).
- Keep the driver a module (`=m`) while working on it, and load it by
  hand.
