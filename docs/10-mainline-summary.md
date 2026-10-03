# The kernel-6.18 line: what it delivers and how to build it

This is the summary of the `kernel-6.18` branch: PiKVM on the BPI-W2 with
Linux 6.18 LTS, Arch Linux ARM and PiKVM's own packages. The bring-up
story, with every finding and how it was verified, is in
`09-mainline-bringup.md`. Section numbers below (§n) refer to that file.

The `main` branch keeps the BSP 4.9 line (Debian 13); this branch can build
both, and the two are not merged.

## 1. What works

All of the following was verified on the board (BPI-W2, 2 GiB, Kodi on a
Raspberry Pi 3 as the HDMI source, a PC on the Type-C port) on 2026-10-01;
the eMMC rows on 2026-10-02/03.

**Release check, 2026-10-03** (`kernel-6.18` @ `3b99198`): both images
rebuilt from the tree, checked for keys (none) and content, and booted from
a fresh write -- the SD image `dd`'d on a PC, the eMMC image installed by
the `bpikvm-install-emmc` that the SD image carries. On each of the three
boot paths -- SD card through its own u-boot (SW4 = 1), SD card through the
eMMC's u-boot (SW4 = 0), eMMC alone -- the first boot grew the root and
made the host keys, no unit failed, and capture (1080p), the stream,
HDMI audio (`arecord`) and keyboard/mouse (`configured`) all worked.

The SD driver change that followed (`6f4f501`, the card's power and the
controller no longer left to u-boot) was verified from the eMMC system on
a 236 GiB card: found when inserted after boot and when in at power-on,
ext4 at 50 MHz 4-bit, 512 MiB written and read back equal, and then the
SD image written onto it from that same eMMC system and booted.

| Feature | State | How it was verified | Where |
|---|---|---|---|
| Boot from SD | done | BSP u-boot from SPI + SD (SW4 = 1), or the eMMC's u-boot with an SD card in (SW4 = 0); 6.18 kernel, root on `mmcblk0p2`; no failed units | §5, §8 |
| Boot from eMMC | done | BPI's eMMC u-boot, no SD card, root on `mmcblk1p2`; capture, stream, Janus and audio as from the SD card | §5 of this file |
| Four cores, interrupts | done | spin-table release through MMIO; RTD129x ISO/MISC interrupt muxes | §6 |
| SD card (root) | done | own driver for the rtsx-style SD core, 50 MHz, ~25 MB/s read | §8 |
| SD card from the eMMC system | done | the driver powers the card and starts the controller itself, so an SD card works as a data disk while the root is the eMMC's; hot-plug too (the host polls) | §5 of this file |
| eMMC (root) | done | own driver (DW MSHC + Realtek wrapper), HS 52 MHz 8-bit, 38 MB/s read, 27 MB/s write; boots from the eMMC alone with the full PiKVM stack | §5 of this file |
| Gigabit Ethernet | done | DHCP, ssh, kernels installed over the network | §9 |
| Reboot | done | watchdog restart handler | §9 |
| USB host ports | done | hub and a card reader enumerate | §7 |
| USB OTG (Type-C) | done | the target enumerates keyboard, mouse and mass storage | §7, §10 |
| HDMI capture | done | 1080p60 from the receiver, EDID over DDC, NV12; after either u-boot (§4 of this file) | §10 |
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

- **An eMMC install without the serial port.** Each board needs the eMMC
  boot loader flashed (`romflash.py`) and its u-boot environment set
  (`uboot-env.txt` through `ubstop.py`) over the serial console once,
  before `bpikvm-install-emmc` gives a bootable eMMC (§5).
- **The board's own HDMI output**: no driver; the console is the serial port
  and the network. PiKVM itself does not need it.
- **Temperature**: no thermal driver, `/sys/class/thermal` is empty. kvmd
  logs "Can't read CPU temp" every 5 s and the Web UI shows none.
- **CPU frequency scaling**: no cpufreq driver; the CPUs stay at the clock
  the boot loader set.
- **eMMC HS200**: the eMMC runs at High Speed (52 MHz, 38 MB/s read); HS200
  would need the BSP's phase tuning (§5 of this file).
- **Audio to the target and the webcam (Janus aplay/vplay)**: not wired.
- **ATX power control**: not wired on this board.
- **The second RJ45 (the hwnat switch), SATA, PCIe, IR**: no drivers. The
  hwnat survey is in `docs/06-changes.md` §11 (on `main`).

Seen, not yet looked into:

- `cma: __cma_alloc: reserved: alloc failed, req-size: 765 pages, ret: -16`
  (twice per boot so far, around when capture buffers are set up);
  nothing has failed with it.
- `r8169 98016000.ethernet eth0: rtl_csiar_cond == 0/1` lines from the
  Ethernet driver while the link comes up; the link works.

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
make image-emmc                 # -> build/bpiw2-pikvm-mainline-emmc.img (3 GiB), see §5
```

| Target | What it does | Output |
|---|---|---|
| `builder-mainline` | Debian trixie with the arm64 cross toolchain | docker image `bpiw2-pikvm/builder-mainline:trixie` |
| `sources-mainline` | Fetches Linux 6.18.y and applies `patches/linux-mainline` | `vendor/linux-mainline` |
| `kernel-mainline` | Copies `kernel/mainline/*` into the tree, merges `bpiw2.config` on top of arm64 defconfig, builds | `arch/arm64/boot/Image`, the board dtb, modules |
| `rootfs-arch` | ALARM aarch64 tarball, PiKVM's repository, ustreamer and kvmd built from our patched trees, Janus and kvmd-webterm from PiKVM | `build/rootfs-arch.tar` |
| `image-mainline` | Boot partition (u-boot's files, the kernel with its text offset patched, the audio firmware), rootfs, modules, VPU firmware | `build/bpiw2-pikvm-mainline.img` |
| `image-emmc` | The same system for the eMMC: partitions labelled `EMMC-BOOT`/`EMMC-ROOT`, nothing before the first partition; install it with `bpikvm-install-emmc` (§5) | `build/bpiw2-pikvm-mainline-emmc.img` |

`make image` and the targets without `-mainline` build the BSP 4.9 line
with its Debian rootfs, as on `main`.

### Flashing and first boot

```sh
sudo dd if=build/bpiw2-pikvm-mainline.img of=/dev/sdX bs=4M conv=fsync status=progress
```

- SW4 = 1 (SPI + SD) boots the SD card's u-boot. SW4 = 0 boots the eMMC's
  u-boot, which, once flashed and set up as in §5, also boots the SD card
  when one is in, and the eMMC system when not. The Type-C port goes to
  the target machine.
- The first boot grows the root partition and generates the SSH host keys
  and the TLS certificate. The image itself carries no keys.
- Web UI: `https://<board IP>/`, `admin` / `admin`. Console (serial
  115200 or ssh): `root` / `pikvm`. Change both.

### Working on a running board

| Script | Use |
|---|---|
| `scripts/push-kernel-mainline.sh [--modules] [--reboot]` | Install a new kernel, dtb and modules over ssh (`BOARD_HOST=<ip>`); the old ones stay as `*.prev`. On an eMMC system it also refreshes the raw boot slots (§5) |
| `scripts/board-ssh.sh '<cmd>'`, `--put <local> <remote>` | Run a command on the board, copy a file to it |
| `scripts/boot-prev-kernel.sh [--restore]` | Over the serial console, boot `uImage.prev` once |
| `scripts/uboot-cmd.sh`, `scripts/serial-cmd.sh` | Drive u-boot or a shell on the serial port |
| `tools/emmc/romflash.py` | Flash the eMMC boot loader through the SoC ROM's serial download mode (§5) |
| `tools/emmc/ubstop.py`, `ubcmd.py`, `uboot-env.txt` | Stop the eMMC's u-boot (bootdelay 0) and run commands; its environment (§5) |
| `bpikvm-install-emmc`, `bpikvm-emmc-bootsync` (on the board) | Install the eMMC image from a running SD system; keep the eMMC's raw boot slots and firmware table in step with `/boot` (§5) |

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
| 0014 | The eMMC host |

### Out-of-tree files (`kernel/mainline`), copied in at build time

| File | What |
|---|---|
| `rtd1296-bananapi-w2.dts` | The board: reserved memory for the Realtek firmware, every device |
| `bpiw2.config` | The config fragment on top of arm64 defconfig |
| `irq-rtd129x.c`, `sdmmc-rtd129x.c`, `r8169soc.c`, `clk-rtd129x-crt.c`, `emmc-rtd129x.c` | Drivers behind patches 0003, 0007, 0008, 0010, 0014 |
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
| `overlay/usr/local/bin/bpikvm-*` | First boot: grow the root, generate keys; the eMMC installer and boot-slot sync (with `bpikvm-emmc-bootsync.path`) |
| `tools/emmc/` | Flashing the eMMC boot loader, driving its u-boot, its environment |
| `overlay-bsp/` | What only the BSP image gets (no H.264, NV16 capture) |

### Firmware the image carries, not built here

- u-boot (`vendor/bpi-w2-bsp/u-boot-rtk`, built by `make uboot`) and the
  SPI bootcode from the BSP.
- `bluecore.audio`, the audio CPU's firmware, from the BSP.
- The CODA980 firmware (`vpu_fw`), downloaded by
  `scripts/fetch-vpu-firmware.sh`.
- Not in any image: the eMMC boot loader (BPI's `dvrboot.exe.bin` and
  hwsetting from the BPI-W2 wiki), flashed once per board (§5).

## 4. Things to know

- **The audio CPU.** u-boot starts the audio firmware before the kernel:
  the SD card's u-boot with `go a` from the files it loads, the eMMC's
  u-boot by itself from its firmware table, before `bootcmd` (§5).
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
- **Capture depends on display-side state the boot loader leaves.** The
  receiver needs the transmitter's HDMI PLL (`PLL_HDMI`, `PLL_HDMI_LDO1`),
  the display block out of reset (`RSTN_DISP`) and the VODMA PLL that
  clocks the MIPI block writing the frames to memory. No driver on this
  kernel owns any of them. The SD card's u-boot leaves all three on; BPI's
  eMMC u-boot leaves them off, and capture fails in three stages -- "Wait
  b/g/R lane koff timeout"; then timings never detected; then timings
  detected but no frame (MIPI registers read 0xdeadbeef, ustreamer "CAP:
  Device select() timeout"). The receiver driver turns them on when they
  are off (`hdmirx_clk_ctrl.c`, it logs "... PLL was off"). Found by
  diffing CRT between a good and a bad boot, then putting good values back
  a few at a time on a bad one.
- **The serial console is shared** with the audio firmware.
- **No RTC battery**: the clock is right only after NTP.

## 5. eMMC

Status on 2026-10-03: **the system runs from the eMMC**, with no SD card,
including capture and audio. (2026-10-02: driver, image, installer, boot
from the eMMC; 2026-10-03: capture and audio after the eMMC's u-boot.)
Three parts: a boot loader on the eMMC (flashed once, over the serial
port), a kernel driver for the eMMC, and an image plus an installer.

Installing, on a board whose eMMC has the boot loader and environment
below:

```sh
make image-emmc
# board booted from the SD image (any SW4), then from the build machine:
xz -T0 -c build/bpiw2-pikvm-mainline-emmc.img | ssh root@<board> 'xz -dc | bpikvm-install-emmc -y -'
# power off, take the SD card out, SW4 = 0, power on
```

The eMMC image has the SD image's partitions, labelled `EMMC-BOOT` and
`EMMC-ROOT` (the fstab follows), so a board with both never mixes them up,
and nothing before the first partition: `bpikvm-install-emmc` writes sector
0 and everything from 100 MiB on, and keeps the boot loader. It then fills
the raw boot slots (below). The root grows to fill the eMMC on the first
boot, as on the SD card. Verified: the installed image read back
identical, and the board booted from it (`root=/dev/mmcblk1p2`) into the
whole PiKVM stack -- capture, stream, Janus, audio (the last two only
after the fixes in §4 and below, 2026-10-03).

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

The environment, saved with `saveenv`, is in `tools/emmc/uboot-env.txt`,
which also shows how to set it through `ubstop.py`. It tries the SD card
first (fatload from its FAT partition), then the eMMC, then falls back to
the vendor's `bootr`. Two settings matter beyond the file names:
`audio_loadaddr=0x0f900000` (the firmware's link address; the default
0x01b00000 breaks the ACPU) and `initrd_high=0xffffffffffffffff` (else
"ramdisk - allocation error"). Left over from the bring-up, in the saved
environment but not in `uboot-env.txt`: `bootprev` (as bootsd, with
uImage.prev and bpi-w2.dtb.prev; set up before the audio finding below,
so expect it to boot without audio) and `bootnoemmc` (obsolete).

**This u-boot cannot read files from its eMMC.** BPI's eMMC driver in it
refuses any DMA below 0xe0000 -- `panic: dma_addr = 0x000cd0c0`, and the
board hangs with the watchdog already off -- and its FAT code keeps its
buffers there. So `bootemmc` reads raw blocks (`mmc read`, as BPI's own
`bootr` does) from fixed slots between the environment and the first
partition:

| File | At | Slot | Loaded to |
|---|---|---|---|
| bpi-w2.dtb | 16 MiB | 1 MiB | 0x02100000 |
| bluecore.audio | 17 MiB | 5 MiB | 0x0f900000, by u-boot itself (below) |
| uInitrd | 24 MiB | 16 MiB | 0x31400000 |
| uImage | 40 MiB | 56 MiB | 0x03000000 |

**This u-boot starts the audio CPU on its own**, before `bootcmd`, from the
AUDIO entry of the vendor firmware table at 0x620000 (a copy at 0x628000)
-- and `go a` does nothing after that. The entry pointed into what is now
the uImage slot: the audio CPU ran a piece of the kernel and every audio
RPC timed out. `bpikvm-emmc-bootsync` points the entry at the
bluecore.audio slot (offset, length, sha256, and the table's checksum, a
byte sum), and neither `emmcload` nor `bpiload` (the SD card's files)
loads bluecore.audio: loading it again lands on the running firmware and
kills it. A board with this boot loader but no eMMC system needs
`bpikvm-emmc-bootsync --force` once from its SD card system, so that the
entry points at a firmware this kernel knows. A boot stopped at the
u-boot prompt (Esc, `ubstop.py`) also leaves the audio CPU dead until the
next boot.

An SD card is usable from the eMMC system: the driver powers it and starts
the controller itself rather than relying on u-boot having read the card
(`09` §14). It is found whether it is in at power-on or inserted later, so
it can hold data -- MSD images, say -- while the system runs from the
eMMC.

`bpikvm-emmc-bootsync` fills them from `/boot`, writing only what
changed. On a system running from the eMMC it runs whenever the boot
files change (`bpikvm-emmc-bootsync.path`), and
`scripts/push-kernel-mainline.sh` runs it before it reboots; on one
running from the SD card it does nothing, unless given `--force` (as the
installer does).

`bootdelay` is 0. To reach the prompt, send Esc while it boots
(`tools/emmc/ubstop.py` does that, then runs the commands given).
`tools/emmc/ubcmd.py` runs commands at a prompt that is already waiting.
This u-boot's hush expands `${...}` inside double quotes when setting a
variable, so set them with single quotes. It has no `boot` or `printenv`
command; use `run bootcmd`.

### The kernel driver (done)

The eMMC controller at `0x98012000` is a Synopsys DesignWare MSHC (VERID
`0x270a`, IDMAC, 32-bit descriptors) with a Realtek wrapper at +0x400.
Commands work as mainline's `dw_mmc` expects; its DMA does not, and a glue
on top of `dw_mmc` was abandoned for a driver of its own,
`kernel/mainline/emmc-rtd129x.c` (patch 0014), which follows Realtek's BSP
and u-boot. What it has to deal with, found one at a time on the board:

- **SB2 holds CPU writes back.** The bus bridge keeps posted writes from
  DDR until it is told to sync (`0x9801a020`); the BSP does it after every
  register write. Without it the DMAC fetches the previous transfer's
  descriptors. The driver syncs once, after writing the descriptors.
- **The DMAC does not stop at the last descriptor.** It follows the next
  pointer anyway and runs whatever is there if it says OWN -- and it never
  clears OWN, so stale descriptors from longer, earlier transfers do. With
  no next pointer at all it fetches from address 0. The chain now ends on
  a descriptor it does not own, and the DMAC is restarted from DBADDR
  (DMA reset, BMOD software reset) for every transfer: parked on that last
  descriptor, it ignores a new DBADDR.
- **Data-transfer-over is early.** On a read the DMAC is still emptying
  the FIFO into DDR. The read completes when the DMAC has moved every byte
  (`TBBCNT`) and the wrapper has raised `DMA_DONE` -- which BSP and u-boot
  wait for, but which comes for every descriptor.
- **u-boot leaves the EMMC PLL tuned for HS200.** With its 200 MHz phases
  the first reads after probe got no data from the card at all, now and
  then. The driver sets the PLL to 100 MHz with the phases at 0, the BSP's
  steps (`SYS_PLL_EMMC1..4` in CRT, the `DUMMY_SYS` toggle), and divides.
- **Interrupts.** The wrapper's ISR (`+0x424`) gates the core's interrupt.
  Its own `DMA_DONE` latches on every transfer and is not the core's:
  unmasked, it is an interrupt storm that hangs the board.
- **Busy.** The core's data-busy status does not clear after an R1b
  command, so the driver leaves busy to the MMC core's CMD13 polling, and
  only block reads wait for the previous data (as the BSP).
- **1.8 V.** The pads are 1.8 V (`UHS_REG` bit 0); the MMC core asks for
  3.3 V first, which the driver refuses.

Before these, the eMMC tests corrupted memory -- slab crashes, and resets
of the whole SoC when a stale descriptor pointed into memory the TEE
guards. A theory that the DMAC uses only 28 bits of the next pointer came
out of that and was wrong.

Result: High Speed, 8 bits, 52 MHz: 38 MB/s read, 27 MB/s write. Ten
minutes of 512 MiB buffered writes with readback, 1 GiB reads compared
twice and four parallel memory checks all came out equal, with nothing in
dmesg. HS200 (200 MHz) would need the BSP's phase tuning.

Testing traps:

- Debug output from the MMC core also covers `mmc0`, the root
  filesystem; at 115200 baud on the console it stalls the whole system.
- A hung eMMC transfer blocks `sync`, so `sync; reboot` hangs. Use
  `echo b > /proc/sysrq-trigger` (with `kernel.sysrq` set to 1 first).
- Keep the driver a module (`=m`) while working on it, and load it by
  hand. Unloading it and loading it again is not a clean test: test after
  a reboot.
- A u-boot `panic` leaves the board hanging with the watchdog off: it needs
  a power cycle.
