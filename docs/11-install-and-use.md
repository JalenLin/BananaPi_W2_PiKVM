# Installing and using the BPI-W2 PiKVM (kernel-6.18)

How to put this on a board and run it. What the images contain and how they
were built is `10-mainline-summary.md`; how any of it was worked out is
`09-mainline-bringup.md`.

Everything here was done on the board on 2026-10-03, on the images built
from `kernel-6.18` at `40f058e`; the eMMC install and the SD image again on
2026-10-04, at `061531b`, and that evening at `387db27`/`6a5e4bd`.

## 1. Which image

| | `build/bpiw2-pikvm-mainline.img` | `build/bpiw2-pikvm-mainline-emmc.img` |
|---|---|---|
| Runs from | the SD card | the eMMC |
| Written with | `dd` on a PC | `bpikvm-install-emmc`, on the board |
| Needs | nothing | the eMMC boot loader, flashed once per board (§3) |
| Root | 3 GiB | grows to the whole eMMC (6.8 GiB) |
| ISOs | a partition of its own, the rest of the card | a directory on the root, or an SD card (§5) |
| The SD slot is then | the system | free, usable for ISOs or data |

Both are the same system: same kernel, same packages, same Web UI.
Prebuilt, xz-compressed copies are on the GitHub Releases page
(`v2.1-kernel6.18`), or build them with `make image-mainline` and
`make image-emmc` (§2 of `10-mainline-summary.md`).

A board can hold both. The eMMC's boot loader tries the SD card first, so
with a bootable card in, the card is what boots; take it out and the board
boots the eMMC.

## 2. The SD card

```sh
sudo dd if=build/bpiw2-pikvm-mainline.img of=/dev/sdX bs=4M conv=fsync status=progress
# or, from the release
xzcat bpiw2-pikvm-mainline.img.xz | sudo dd of=/dev/sdX bs=4M conv=fsync status=progress
```

- **SW4 = 1** boots the SD card's own u-boot (from the SPI flash). **SW4 = 0**
  boots the eMMC's, which then boots the SD card if one is in. Both work.
- The Type-C port goes to the machine being controlled; HDMI IN takes its
  video.
- The first boot grows the ISO partition to the rest of the card, generates
  the SSH host keys and the TLS certificates, and takes about a minute.

Then:

| | |
|---|---|
| Web UI | `https://<board IP>/`, `admin` / `admin` |
| Console (ssh, or serial at 115200) | `root` / `pikvm` |
| Hostname | `bpi-w2-pikvm`, announced over mDNS |

**Change both passwords** (`kvmd-htpasswd set admin`, `passwd`). The image
carries no keys of its own: each board makes its own on its first boot.

Every flash gives the board a new machine-id, so a new MAC address and a
new DHCP lease. `/etc/motd` says which image, which kernel and which commit
a board is running.

## 3. The eMMC

### Once per board: the boot loader

The eMMC needs a boot loader of its own before it can boot anything, and
that cannot be built from the BSP sources (§5 of `10-mainline-summary.md`).
It comes from BPI's release and goes on over the serial port, which is the
only part of the whole procedure that needs a serial cable.

Take `dvrboot.exe.bin` and `RTD1296_hwsetting_BOOT_4DDR4_4Gb_s1866_padding.bin`
from the "Hardware files" zip on the BPI-W2 wiki, put them in a directory,
and with the board running (any SW4, SD card in):

```sh
# stage 1: the hwsetting. Reboot the board when it asks.
docker run --rm --device /dev/ttyUSB0 -v $PWD/tools/emmc:/io -v <release dir>:/r \
    python:3-slim python3 -u /io/romflash.py /r/RTD1296_hwsetting_*.bin /r/dvrboot.exe.bin
# stage 2: the boot loader itself
docker run --rm --device /dev/ttyUSB0 -v $PWD/tools/emmc:/io -v <release dir>:/r \
    python:3-slim python3 -u /io/romflash.py --stage2 /r/RTD1296_hwsetting_*.bin /r/dvrboot.exe.bin
```

Then its environment, once, the same way:

```sh
docker run --rm --device /dev/ttyUSB0 -v $PWD/tools/emmc:/t -v /tmp:/io python:3-slim \
    sh -c "grep -v '^#' /t/uboot-env.txt | xargs -d '\n' python3 -u /t/ubstop.py"
# reboot the board while that waits for the prompt
```

`tools/emmc/uboot-env.txt` is that environment, with a comment per line.

### The system

With the board running the SD image, from the build machine:

```sh
xz -T0 -c build/bpiw2-pikvm-mainline-emmc.img |
    ssh root@<board> 'xz -dc | bpikvm-install-emmc -y -'
# or, from the release
ssh root@<board> 'xz -dc | bpikvm-install-emmc -y -' < bpiw2-pikvm-mainline-emmc.img.xz
```

Or copy the image to the board and run `bpikvm-install-emmc <image>` there.
It writes the partition table and everything from 100 MiB on, so the boot
loader and its environment are left alone, and then fills the raw copies of
the boot files that this u-boot loads instead of reading them from a
filesystem (§5 of `10-mainline-summary.md`).

```
power off, take the SD card out, SW4 = 0, power on
```

The first boot grows the root to the whole eMMC. Everything in §2 about
passwords, keys and the Web UI applies.

## 4. Updating a board

```sh
BOARD_HOST=<ip> scripts/push-kernel-mainline.sh --modules --reboot
```

installs a kernel, dtb and modules over ssh, keeping the previous ones as
`*.prev`. On a board running from the eMMC it also refreshes the raw boot
copies before it reboots.

`pacman -Syu` works for the rest of the system; `ustreamer` and `kvmd` are
held back (`IgnorePkg`) because they are built from this tree's patched
sources.

To grow a filesystem after moving to a bigger card: `bpikvm-expand`.

## 5. Where the ISOs go

kvmd serves virtual media from `/var/lib/kvmd/msd`, which is mounted
read-only; kvmd takes it read-write itself for as long as it is writing an
image. Upload images from the Web UI (Virtual Media), which is the way
meant for it.

To copy one in by hand, write where the store really lives rather than
remounting the read-only view:

```sh
# the SD image: the store's own partition
mount -o remount,rw /var/lib/kvmd/msd
cp something.iso /var/lib/kvmd/msd/ && chown kvmd: /var/lib/kvmd/msd/something.iso
mount -o remount,ro /var/lib/kvmd/msd

# the eMMC image: /var/lib/kvmd/msd is a read-only view of msd.data
cp something.iso /var/lib/kvmd/msd.data/ && chown kvmd: /var/lib/kvmd/msd.data/something.iso
```

**On the SD image** that is a partition of its own, `BPI-MSD`, holding the
rest of the card -- 218 GiB of a 256 GB card. Nothing to set up.

**On the eMMC image** it is a directory on the root filesystem, which
leaves about 5 GiB. To use an SD card for images instead:

```sh
bpikvm-msd-sd              # erase /dev/mmcblk0 and use it for ISOs
bpikvm-msd-sd --status     # where the store is now
bpikvm-msd-sd --undo       # back to the eMMC's own directory
```

Such a card is not bootable, so the board still boots from the eMMC with
one in. It can be taken out and put back; with no card the store is the
directory on the eMMC again, and kvmd keeps working.

## 6. Recovery

| Trouble | What to do |
|---|---|
| A pushed kernel does not boot | On the serial console, `scripts/boot-prev-kernel.sh` boots `uImage.prev` once; `--restore` makes it permanent |
| The eMMC system does not boot | Put a bootable SD card in: its u-boot is tried first. Install the eMMC image again from there |
| A board is unreachable | Serial console at 115200, `root` / `pikvm` |
| The eMMC's u-boot prompt | `bootdelay` is 0; `tools/emmc/ubstop.py` sends Esc until it answers, then runs the commands given |

Two things worth knowing before they surprise anyone:

- **A boot stopped at the u-boot prompt has no HDMI audio** until the next
  normal boot: that u-boot starts the audio firmware on its way past, and
  stopping interrupts it.
- **Which u-boot runs, and with which environment, follows SW4.** SW4 = 1
  loads u-boot from the SD card (the one `make uboot` builds). Its
  environment would live in the card's factory area, which the image does
  not write, so it runs on its compiled-in default: the card's own files,
  with the bootargs of `docs/03`. SW4 = 0 loads the eMMC's u-boot, which
  does have the saved environment of §3 -- the one that tries the SD card
  and then the eMMC. So the commands of §3 (`bootemmc` and the rest) exist
  only on the SW4 = 0 path; at the SW4 = 1 prompt they are "not defined".

## 7. The board's own HDMI output

The HDMI port next to the HDMI input shows a Linux console (`tty1`, with a
login prompt) at 1080p60, the mode the boot loader sets. A USB keyboard on
the board's USB ports types into it. It is a DRM device
(`/dev/dri/card0`, `/dev/fb0`), so KMS programs can use it as well.

The mini DisplayPort output shows the same console, at 1080p60, when a
DisplayPort sink is plugged in: a DP monitor, or an adapter with a
converter chip (checked with an ATEN VC920, mini DP to VGA). Passive mini
DP to HDMI adapters (most cheap ones, e.g. ATEN VC980) cannot work on this
board: it has no DP++ dual mode.

## 8. Using it

The Web UI works as on any PiKVM. What is particular to this board:

- **Video.** The input takes what the EDID offers (1080p at 60, 50, 30
  and 24 Hz, 1080i, 720p, 576p, 480p, and the VGA modes), and any other
  size up to 4096x2160 a computer sends (1366x768, 1440x900, 1920x1200,
  ...). H.264 (the WebRTC and Direct H.264 modes) goes up to 1920x1088;
  above that, use MJPEG.
- **Keyboard and mouse** are one USB device, so that virtual media fits
  the board's USB endpoints. In the target's firmware setup (BIOS/UEFI)
  the keyboard may not work, as it cannot offer the boot protocol; to
  trade virtual media for that, see `docs/09` §10. The mouse is absolute
  only.
- **Virtual media.** Besides uploading, an image can be downloaded into
  the store from a URL, or attached straight from an HTTP, SMB or SFTP
  server without copying it (Virtual Media, remote image). Such a server
  should stay up while the target reads: if it goes away for longer than
  the target's USB timeout, keyboard and mouse stop too, until the
  target's USB is reset (replug, or reboot it).
- **VNC** is off, as on PiKVM OS: `systemctl enable --now kvmd-vnc`, port
  5900, the Web UI's users and passwords (VeNCrypt).
- **Text recognition (OCR)** of the screen is in the Web UI's menu, in
  English.

## 9. What is not there

ATX power control is prepared but off (`10-mainline-summary.md` §7). Not
there: the second Ethernet port, and audio
towards the controlled machine. The full list, with what each would need,
is in §1 of `10-mainline-summary.md`.
