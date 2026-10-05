# The kernel-6.18 line: what it delivers and how to build it

This is the summary of the `kernel-6.18` branch: PiKVM on the BPI-W2 with
Linux 6.18 LTS, Arch Linux ARM and PiKVM's own packages. The bring-up
story, with every finding and how it was verified, is in
`09-mainline-bringup.md`. Section numbers below (§n) refer to that file.

**To install and run it, read `11-install-and-use.md`.** This file is what
the images are and how they are built.

The `main` branch keeps the BSP 4.9 line (Debian 13); this branch can build
both, and the two are not merged.

## 1. What works

All of the following was verified on the board (BPI-W2, 2 GiB, Kodi on a
Raspberry Pi 3 as the HDMI source, a PC on the Type-C port) on 2026-10-01;
the eMMC rows on 2026-10-02/03.

**Release check, 2026-10-04, evening** (`kernel-6.18` @ `387db27`, then
`6a5e4bd`): eMMC HS200, the HDMI output, the mini DP output, the ATX GPIO
lines, and two fixes the check itself turned up.

- **The images carried a shared private key.** It was pacman's master
  signing key, from `pacman-key --init` at rootfs build time
  (`/etc/pacman.d/gnupg/private-keys-v1.d`). The earlier checks looked only
  for SSH and TLS keys by name. Now the image carries just the PiKVM
  repository's public key, and `bpikvm-firstboot` builds each board's own
  keyring, offline. After that, `pacman -Sy` verified its signatures on
  both paths.
- **The reset button hung the board** from an idle system: the PMIC keeps
  the CPU rail at Linux's last voltage, and the boot ROM does not start on
  0.8 V. The CPU rail now never goes below 1.0 V (`09` §16).

Both images were inspected: no SSH host keys, TLS keys, pacman keyring,
private key files or `authorized_keys`. The dtb in them is byte for byte
the one the reset test ran on.

| Path | Verified by |
|---|---|
| eMMC alone (SW4 = 0, no card), `387db27` | own keys and keyring made on the first boot; root on `mmcblk1p2` grown to 6.8 GiB; eMMC at HS200 (TX 18, RX 21); 1080p60 capture of the board's own HDMI output through a cable to its HDMI IN (the login console); HID online; 16 MiB ISO uploaded through kvmd, md5 equal; governor `performance` while a client streams and back after; `arecord` on `hw:hdmirx`; the mini DP output on a VGA monitor through an ATEN VC920 |
| SD card, the eMMC's u-boot (SW4 = 0, card in), `387db27` | the image written from the eMMC system, read back, md5 equal; first boot grew the ISO partition to 232 GiB; root on `mmcblk0p2`; SD 50 MHz 4-bit, eMMC HS200; the same checks as above (ISO onto `BPI-MSD`); DP on the VGA monitor |
| Reset button, SD system | 0.8 V: hang; 1.0125 V: boots; `6a5e4bd` (1.0 V floor), idle at 300 MHz: boots |

The first power-on of the freshly written card did not boot. It came
after a press of the reset button with the system idle, which is the
0.8 V hang above. The journal holds no record of it.

The `6a5e4bd` eMMC image went onto the eMMC afterwards, from the SD
system. The first try stopped at 1.25 GB when `eth0` stopped sending (found
and fixed afterwards, `docs/09` §21). After `ip link set eth0 down/up` the
second try went through and read back md5 equal. Only the dtb differs
from `387db27`. The images were rebuilt once more at `745d2e5`, with the
eth0 fixes. That eMMC image is on the board's eMMC, read back md5 equal,
and the 3 GB went over without a stall. They were rebuilt again at `c371e00`, after
the stress tests: SB2 drained on every `writel()` (patch 0018), and coda
and the HDMI receiver on their own memory pools. Both images were checked
for keys (none). The eMMC image is on the board's eMMC and read back md5
equal; the 3 GB went over eth0 without a stall. Booted from the eMMC alone
(SW4 = 0, no card): first boot made its own keys and keyring and grew
root to 6.8 GiB; HS200; the three memory pools assigned; no failed unit.
1080p60 capture, H.264 at 24 fps, HID online. An MSD upload compared md5
equal, and 4 upload/remove rounds went through. The MAC came from the
machine ID and stayed the same, with the same address, over a reboot. A
line printed on tty1 showed up in the capture of the board's own HDMI
output, and the DP output showed on the monitor.

The released SD image (`v2.0-kernel6.18`, the same bytes as `c371e00`'s)
was then checked on its own, on 2026-10-05:
- **Written and read back.** The board, running from the eMMC, downloaded
  `bpiw2-pikvm-mainline.img.xz` from the GitHub release and wrote it to an
  SD card. The download and the card's readback both matched SHA256SUMS.
- **Booted.** The eMMC's u-boot booted the card (SW4 = 0, card in).
- **First boot.** It made its own keys and keyring, and grew `BPI-MSD` to
  232 GB. No unit failed, and the three memory pools were assigned.
- **Video.** 1080p60 capture, and H.264 at 27 fps through kvmd-media.
- **Keyboard and mouse.** Both reached the Pi, as report IDs 1 and 2 on
  the one HID interface.
- **Virtual media.** A 16 MiB image went through kvmd as a flash drive, and
  the Pi read it back md5 equal.

**Release check, 2026-10-04** (`kernel-6.18` @ `061531b`: Linux 6.18.55,
kvmd 4.219, ustreamer 6.67, and the temperature, cpufreq/PMIC and CMA
work): both images rebuilt from a clean tree and inspected (kernel and
modules 6.18.55, the three new drivers and their DT nodes, kvmd-pm
enabled, motd naming the commit; no SSH host keys, TLS keys, private keys
or `authorized_keys`). The eMMC image went on with the SD system's
`bpikvm-install-emmc`; the SD image was then written onto the card from the
eMMC system (the card hot-plugged) and read back, md5 equal.

| Path | Verified by |
|---|---|
| eMMC alone (SW4 = 0, no card) | root on `mmcblk1p2` grown to 6.8 GiB, no `mmcblk0` |
| SD card, the eMMC's u-boot (SW4 = 0, card in) | bootargs from `bootsd`, root on `mmcblk0p2`, ISO partition grown to 232 GiB, SD bus tuned to 50 MHz 4-bit |

On both: no failed unit; 12 OPPs to 1.4 GHz with the PMIC driving the CPU
and L2 rails; the governor switched to `performance` while a client
watched and back to `schedutil` after; 1080p60 capture, a kvmd snapshot,
24 fps of MJPEG with Kodi playing video; HID online; HDMI audio through
`arecord` with Kodi playing (peaks of -10 and -5 dBFS, not silence); a
16 MiB ISO uploaded through kvmd's API onto the store, md5 equal (the
directory on the eMMC image, `BPI-MSD` on the SD one); no CMA failure. The
SD card's own u-boot (SW4 = 1) was not booted again: that u-boot did not
change, and the kernel and rootfs it loads are the same as on the other
paths.

**Release check, 2026-10-03** (`kernel-6.18` @ `40f058e`): both images
rebuilt from the tree, inspected (no keys of any kind; partitions, labels,
fstab, motd, tools, and the driver fixes present in both the kernel and
the modules) and booted from a fresh write -- the SD image `dd`'d from the
build machine onto the card, the eMMC image installed by the
`bpikvm-install-emmc` the SD image carries, each read back and compared.

All three boot paths passed, each with its own first boot (partition grown,
host keys made), no failed unit, 1080p capture, a kvmd snapshot, HDMI audio
through `arecord`, and the HID gadget `configured`:

| Path | Verified by |
|---|---|
| SD card, its own u-boot (SW4 = 1) | bootargs with `rootfstype=ext4 sdmmc_on=1`, root on `mmcblk0p2`, ISO partition grown to 232 GiB |
| SD card, the eMMC's u-boot (SW4 = 0, card in) | bootargs from the saved environment's `bootsd`, same root, SD bus tuned to 50 MHz 4-bit |
| eMMC alone (SW4 = 0, no card) | root on `mmcblk1p2` grown to 6.8 GiB, no `mmcblk0` |

kvmd uploaded ISOs onto the store through its API on both images (16 MiB
and 8 MiB, md5 compared), and `bpikvm-msd-sd` set up a card for the eMMC
system, handed it to kvmd and undid it again. The SD card from the eMMC
system reads at 29.7 MB/s (4-bit, 50 MHz).

| Feature | State | How it was verified | Where |
|---|---|---|---|
| Boot from SD | done | BSP u-boot from SPI + SD (SW4 = 1), or the eMMC's u-boot with an SD card in (SW4 = 0); 6.18 kernel, root on `mmcblk0p2`; no failed units | §5, §8 |
| Boot from eMMC | done | BPI's eMMC u-boot, no SD card, root on `mmcblk1p2`; capture, stream, Janus and audio as from the SD card | §5 of this file |
| Four cores, interrupts | done | spin-table release through MMIO; RTD129x ISO/MISC interrupt muxes | §6 |
| SD card (root) | done | own driver for the rtsx-style SD core, 50 MHz, ~25 MB/s read | §8 |
| SD card from the eMMC system | done | the driver powers the card, starts the controller and tunes the bus itself, so an SD card works as a data disk while the root is the eMMC's -- 4-bit 50 MHz, 29.7 MB/s; hot-plug too (the host polls) | §5 of this file |
| Virtual media store | done | its own partition on the SD image (`BPI-MSD`, the rest of the card), as PiKVM does it; on the eMMC image a directory, or a card dedicated with `bpikvm-msd-sd`. kvmd uploads an ISO onto either through its API | §6 of this file |
| eMMC (root) | done | own driver (DW MSHC + Realtek wrapper), HS200: 200 MHz 8-bit with the BSP's phase tuning, 113 MB/s read, 39 MB/s write; boots from the eMMC alone with the full PiKVM stack | §5 of this file, `09` §19 |
| Gigabit Ethernet | done | DHCP, ssh, kernels installed over the network. Descriptors synced through SB2 and a TX watchdog: no stall in ~77 GB of network + eMMC/SD load, where without the sync it stopped six times. MAC from the machine ID, the same over reboots | §9, `docs/09` §21 |
| Reboot | done | watchdog restart handler | §9 |
| USB host ports | done | hub and a card reader enumerate | §7 |
| USB OTG (Type-C) | done | the target enumerates keyboard, mouse and mass storage | §7, §10 |
| HDMI capture | done | 1080p60 from the receiver, EDID over DDC, NV12; after either u-boot (§4 of this file) | §10 |
| MJPEG stream | done | kvmd's ustreamer, ~22 fps of JPEG to a client | §10 |
| H.264 encoding (VE1) | done | CODA980 through the mainline `coda` driver, 1080p, ~70 fps capacity. Its buffers and the capture buffers come from pools of their own, not CMA: with the SD card busy a CMA allocation once failed and H.264 stayed off for the session. 20 restarts under page-cache pressure, all fine | §11, `docs/09` §22 |
| Direct H.264 (kvmd-media) | done | the Web UI's H.264 mode | §11 |
| WebRTC (Janus) | done | 1920x1080 at ~30 fps to a headless WebRTC client | §11 |
| HDMI audio | done | ALSA card `hdmirx`, 48 kHz stereo; music from Kodi over WebRTC (Opus) | §13 |
| Keyboard, mouse | done | through kvmd to the target | §10 |
| Virtual media (MSD) | done | image upload and connect through kvmd. Under load (network + eMMC + SD + H.264 + HID), a Pi 3 host read 45 GiB and wrote 1.5 GiB raw, every MiB checked, no error and no bus reset. That needs the SB2 drain on every `writel()`: without it the host gave up on a READ within minutes | §10, `docs/09` §21 |
| Web terminal | done | `kvmd-webterm` (ttyd) in the Web UI | §12 |
| Temperature | done | own driver for the sensor next to the CPUs, `thermal_zone0` (and hwmon `cpu_thermal`); kvmd shows it in the Web UI. Throttles the CPUs from 105 C, shuts down at 130 C | `docs/09` §15 |
| CPU frequency and voltage | done | own drivers for the CPU PLL and the G2227 PMIC; `cpufreq-dt` with schedutil, 300 MHz - 1.4 GHz, at the BSP's voltages from 1.2 GHz up and at 1.0 V below (a reset button or watchdog reset keeps the PMIC's voltage, and the boot ROM hangs on the BSP's 0.8 V), the L2 rail following (the boot loader leaves 800 MHz at 1.0 V). Every step checked (clock, both rails, PWM mode, speed); 4 cores at 1.4 GHz for 5 min with checked results, throttled to 1.2-1.3 GHz at 105 C | `docs/09` §16 |
| DisplayPort output (mini DP) | done | the same picture as HDMI, 1080p60 on 2 lanes: the driver sets up the transmitter and trains the link on hot plug, then the firmware mirrors HDMI to it. Seen on a VGA monitor through an ATEN VC920. Passive DP++ adapters cannot work (no dual mode on this board) | `docs/09` §20 |
| HDMI output (the board's own) | done | own DRM driver on the audio firmware's video output: one plane at 1080p60 (the mode the boot loader sets), fbdev emulation, so `tty1` with a login prompt is on HDMI. Checked by cabling HDMI OUT to the board's own HDMI IN and capturing it: colour bars, the console, and a KMS client's SETCRTC and page flip (with its flip event) | `docs/09` §20 |
| ATX lines (GPIO) | prepared, not tried | the MISC GPIO controller (mainline `gpio-rtd`) as `/dev/kvmd-gpio`; four header pins chosen and checked (GPIO function, inputs, pull-ups, edge detection can be requested); kvmd accepts the config. ATX stays off: no ATX board was connected | §7 of this file |
| Package updates | done | `pacman -Syu` works; ustreamer and kvmd are held back (IgnorePkg) | §12 |

Not done:

- **An eMMC install without the serial port.** Each board needs the eMMC
  boot loader flashed (`romflash.py`) and its u-boot environment set
  (`uboot-env.txt` through `ubstop.py`) over the serial console once,
  before `bpikvm-install-emmc` gives a bootable eMMC (§5).
- **Passive mini DP adapters**: the DP output works with a sink that
  speaks DP (a monitor, or an adapter with a converter chip such as the
  ATEN VC920, mini DP to VGA). Passive mini DP to HDMI adapters (ATEN
  VC980) cannot work: the board has no DP++ dual mode (`docs/09` §20).
- **Other HDMI output modes**: the output runs at the mode the boot loader
  set (1080p60); the driver does not change it or read the monitor's EDID.
- **Audio to the target and the webcam (Janus aplay/vplay)**: not possible
  next to the keyboard, mouse and virtual media. Both are USB gadget
  functions on the Type-C port, and its dwc3 has six endpoints of which
  three are IN, ep0 included (`GHWPARAMS3` 0x030c6485: `NUM_EPS` 6,
  `NUM_IN_EPS` 3). The combined HID takes one IN (interrupt) and the mass
  storage the other (bulk). kvmd's microphone (`uac2`, playback towards the
  target) needs an isochronous IN, plus an interrupt IN for its volume and
  mute controls; the webcam (`uvc`) needs IN endpoints too. Without the
  mass storage, one IN would be free -- enough for the microphone only if
  kvmd also turned the UAC2 controls off, which it does not. Left as is:
  virtual media is the more useful of the two.
- **ATX power control**: the GPIO side is ready (§7) but nothing was driven
  or connected; it is off in kvmd's config.
- **The second RJ45 (the hwnat switch), SATA, PCIe, IR**: no drivers, and
  set aside for a project of their own (2026-10-04): PiKVM does not need
  them. The hwnat survey is in `docs/06-changes.md` §11 (on `main`). One
  more finding from the vendor's router code
  (`hw_nat/AsicDriver/rtd129x_clk.c`): it switches the embedded gigabit
  PHY over to the NAT engine (`ISO_POWERCUT_ETN`, `etn_gphy_switch_nat`).
  That PHY is the one `eth0` uses, so a driver for the second port must
  not do that step. A USB 3.0 Ethernet adapter works meanwhile: the image
  has `r8152` and `ax88179_178a`.

Seen, not yet looked into:

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
make image-mainline             # -> build/bpiw2-pikvm-mainline.img (3.5 GiB)
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

- The image is 3.5 GiB: 256 MiB of boot files, a 3 GiB root and the ISO
  store, which grows to the rest of the card on the first boot (§6).
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
| `bpikvm-msd-sd`, `bpikvm-expand` (on the board) | Dedicate an SD card to the ISO store (§6); grow the last partition to fill the medium |

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
| 0015 | The thermal sensor |
| 0016 | The CPU clock |
| 0017 | The G2227 PMIC (CPU and L2 rails) |

### Out-of-tree files (`kernel/mainline`), copied in at build time

| File | What |
|---|---|
| `rtd1296-bananapi-w2.dts` | The board: reserved memory for the Realtek firmware, every device |
| `bpiw2.config` | The config fragment on top of arm64 defconfig |
| `irq-rtd129x.c`, `sdmmc-rtd129x.c`, `r8169soc.c`, `clk-rtd129x-crt.c`, `emmc-rtd129x.c`, `rtd129x-thermal.c`, `clk-rtd129x-scpu.c`, `g2227-regulator.c` | Drivers behind patches 0003, 0007, 0008, 0010, 0014, 0015, 0016, 0017 |
| `hdmirx/` | The HDMI receiver: Realtek's BSP driver with a new V4L2 side |
| `acpu/` | `rtd129x-acpu` (RPC to the audio CPU firmware), `snd-rtd129x-hdmirx` (ALSA capture) and `rtd129x-vo` (the HDMI output, DRM) |
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
dmesg.

HS200 (2026-10-04, `09` §19): the PLL at 200 MHz and the BSP's three
phase scans, done with the tuning block (CMD21) instead of the BSP's
write to block 0xfe. Two more things the hardware needs:

- **No DMA under 512 bytes.** After a DMA of the 128-byte tuning block
  the DMAC goes wrong on the next transfer. Shorter blocks are read from
  the FIFO.
- **The FIFO is 64 bits wide, read 32 bits at a time:** low half at
  0x200, high half at 0x204.

A bad phase can hang the data state machine with no timeout, so tuning
commands time out in software after 50 ms, and a data error resets the
whole controller. If tuning fails, the driver drops HS200 and the core's
second try (from `f_min`, 300 kHz) comes up at High Speed. Result: TX 18
and RX 21, as the boot loader's own tuning, 113 MB/s read and 39 MB/s
write, 5 minutes of write/readback equal.

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

## 6. The virtual media store (ISOs)

kvmd serves virtual media (MSD) out of `/var/lib/kvmd/msd`, mounted
read-only; its remount helper takes it read-write only while it writes an
image. Where that comes from differs per image.

**The SD image** gives it a partition of its own, `BPI-MSD`, as PiKVM's own
images do: root is a fixed 3 GiB and the ISO partition takes the rest of
the card (`bpikvm-expand` grows it on the first boot -- 156 MiB in the
image to 232 GiB on a 256 GB card). ISOs therefore cannot fill the root
filesystem, and the Web UI shows how much room is left for images. The
filesystem's root is owned by `kvmd`, so uploads need nothing else.

**The eMMC image** keeps it a directory on the root filesystem
(`/var/lib/kvmd/msd.data`, bind-mounted), because a 7.3 GiB eMMC has
little to spare. To put the ISOs on an SD card instead:

```sh
bpikvm-msd-sd            # erase /dev/mmcblk0, label it BPI-MSD and use it
bpikvm-msd-sd --status   # where the store is now
bpikvm-msd-sd --undo     # back to the eMMC's own directory
```

The card is mounted at `/var/lib/kvmd/msd.data`, under the bind mount kvmd
already has, so kvmd itself needs no reconfiguration. The fstab entry
carries `nofail`: with no card in, the store is the empty directory on the
eMMC again. Such a card is not bootable, so the board still boots from the
eMMC with one in -- its u-boot tries the card, finds nothing and falls
back. A card carrying the SD *system* has a `BPI-MSD` partition too (its
p3), and is picked up as the store, which is the point: the ISOs are on
the card either way.

## 7. ATX power control (GPIO)

PiKVM's ATX plugin needs four lines, all on one GPIO chip: two outputs
(power and reset buttons) and two inputs (power and HDD LEDs). This is
worked out and the GPIO side is in place, but **it has not been tried with
an ATX board**: nothing was connected and no line was driven. The plugin
stays `type: disabled`.

### The 40-pin header

From the schematic (sheet "15 GPIO", page 13 of the PDF, `CON2`) and the
BPI wiki's table, which agree on the GPIO numbers. (The two disagree on
which of pins 3 and 5 is I2C5's SDA.) The schematic marks ISO GPIO nets
`IGPIO`; the rest are MISC GPIOs:

| Pin | GPIO | Also | Pin | GPIO | Also |
|---|---|---|---|---|---|
| 3 | MISC 13 | I2C5 | 4 | 5 V | |
| 5 | MISC 14 | I2C5 | 6 | GND | |
| 7 | ISO 21 | PWM0 | 8 | ISO 3 | UART2 TX |
| 9 | GND | | 10 | ISO 2 | UART2 RX |
| 11 | MISC 17 | | 12 | MISC 58 | AO_BCK |
| 13 | MISC 25 | I2C3 | 14 | GND | |
| 15 | MISC 27 | I2C3 | 16 | ISO 5 | UART2 RTS |
| 17 | 3.3 V | | 18 | ISO 4 | UART2 CTS |
| 19 | MISC 7 | SPI MOSI | 20 | GND | |
| 21 | MISC 4 | SPI MISO | 22 | ISO 9 | IR TX |
| 23 | MISC 5 | SPI CLK | 24 | MISC 6 | SPI CS |
| 25 | GND | | 26 | MISC 8 | |
| 27 | MISC 12 | I2C4 | 28 | MISC 11 | I2C4 |
| 29 | MISC 100 | | 30 | GND | |
| 31 | MISC 21 | | 32 | MISC 54 | SPDIF |
| 33 | MISC 59 | AO_CK | 34 | GND | |
| 35 | MISC 57 | AO_LRCK | 36 | MISC 22 | |
| 37 | MISC 23 | | 38 | MISC 24 | |
| 39 | GND | | 40 | MISC 60 | AO_D0 |

Pin 1 is 3.3 V and pin 2 is 5 V. The kernel numbers MISC lines 0-100 on
their own chip (`gpiochip0`, `9801b100.gpio`).

### The lines chosen

A Raspberry Pi PiKVM uses header pins 18 and 15 for the LEDs and 16 and 13
for the buttons. On the W2, 16 and 18 are ISO GPIOs. The ISO controller is
not in the device tree: mainline's driver claims 0x98007000-0x980070ff for
its interrupt status, and that overlaps the ISO reset and clock
controllers. So all four lines are MISC GPIOs, on plain pins that have no
use on this board. Two of them are where the Pi has them:

| kvmd option | GPIO | Header pin | Pi PiKVM pin |
|---|---|---|---|
| `power_led_pin` | 8 | 26 | 18 |
| `hdd_led_pin` | 27 | 15 | 15 |
| `power_switch_pin` | 17 | 11 | 16 |
| `reset_switch_pin` | 25 | 13 | 13 |

Checked on the board:

- All four are GPIOs from reset: function 0 in `MUXPAD` 0x9801a908
  (GPIO 25, 27), 0x9801a90c (GPIO 17) and 0x9801a910 (GPIO 8). Read
  after the eMMC's u-boot; the SD card's u-boot was not checked.
- All four are inputs and read high: their pads have the pull-up on
  (0x3 in `PCONF` 0x9801a92c and 0x9801a938).
- `gpiomon` can request edge detection on them, which goes through the
  MISC interrupt mux.
- With these four pins in an override file, kvmd accepts and resolves
  the config (`kvmd -m`). kvmd was not run with it.

### What is in the image

- The MISC GPIO node in the board DTS: `realtek,rtd1295-misc-gpio`,
  interrupts 19/20 of the MISC mux.
- A udev rule that points `/dev/kvmd-gpio` at it (kvmd is in the `gpio`
  group).
- The config to switch it on, as a comment in `/usr/lib/kvmd/main.yaml`.
  It goes into `/etc/kvmd/override.yaml`:

```yaml
kvmd:
    atx:
        type: gpio
        power_led_pin: 8
        hdd_led_pin: 27
        power_switch_pin: 17
        reset_switch_pin: 25
```

### Electrical notes, for whoever wires it

- 3.3 V logic, like the Pi.
- **The pads pull up**, where a Pi's default for these pins is pull-down,
  and kvmd sets no bias. An LED stage that pulls the pin low when the LED
  is on works with `*_led_inverted: true`. One that drives the pin high (a
  board made for a Pi's pull-downs) needs pull-down resistors on the board,
  or the pads changed to pull-down (`PCONF` bit 0 of each pin's nibble).
- **Until kvmd starts, the button pins are inputs with the pull-up on.**
  They float at 3.3 V through tens of kilohms. An optocoupler LED behind a
  resistor will not light on that, but a high-impedance gate driver could
  see a press while the board boots.

