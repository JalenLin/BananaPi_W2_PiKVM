#!/bin/bash
# Assemble a flashable SD card image -- or, with IMAGE_TARGET=emmc, one for
# the eMMC.
#
# Layout (from the BSP's scripts/dd_download.sh and bootloader.sh):
#   offset 40 KiB          u-boot.bin
#   sector 204800..        p1 vfat  "BPI-BOOT"  -- 60 MiB, grown if the staged
#                                                  boot files need more
#   after p1               p2 ext4  rootfs
#   after p2               p3 ext4  "BPI-MSD" -- kvmd's virtual media store
#                                     (the mainline SD image only, as PiKVM's
#                                     own images do it); root is then a fixed
#                                     size and p3 grows to fill the card
#
# The eMMC image has the same partitions, labelled EMMC-BOOT and EMMC-ROOT so
# that a board with both never mixes them up, and nothing before p1 but the
# partition table: the eMMC's first MiBs hold its own boot loader and
# environment (docs/10 §5). bpikvm-install-emmc writes it from a running SD
# system and leaves those alone; do not dd it whole.
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BSP="$PROJECT_ROOT/vendor/bpi-w2-bsp"
BUILD="$PROJECT_ROOT/build"

# Which kernel line to put in the image. u-boot, the audio firmware blob and
# the vendor initramfs come from the BSP either way -- only the Image and the
# dtb differ. See docs/09-mainline-bringup.md.
FLAVOUR="${KERNEL_FLAVOUR:-bsp}"
case "$FLAVOUR" in
    bsp)
        KIMAGE="$BSP/linux-rtk/arch/arm64/boot/Image"
        KDTB="$BSP/linux-rtk/arch/arm64/boot/dts/realtek/rtd129x/rtd-1296-bananapi-w2-2GB.dtb"
        OUT="$BUILD/bpiw2-pikvm.img"
        # Debian 13 (make rootfs): Arch's systemd cannot run on 4.9
        ROOTFS=rootfs.tar
        ;;
    mainline)
        LINUX="$PROJECT_ROOT/vendor/linux-mainline"
        KIMAGE="$LINUX/arch/arm64/boot/Image"
        KDTB="$LINUX/arch/arm64/boot/dts/realtek/rtd1296-bananapi-w2.dtb"
        OUT="$BUILD/bpiw2-pikvm-mainline.img"
        # Arch Linux ARM + PiKVM packages (make rootfs-arch)
        ROOTFS=rootfs-arch.tar
        ;;
    *)
        echo "KERNEL_FLAVOUR must be 'bsp' or 'mainline', got '$FLAVOUR'" >&2
        exit 1
        ;;
esac
echo ">>> kernel flavour: $FLAVOUR"

TARGET="${IMAGE_TARGET:-sd}"
case "$TARGET" in
    sd)   BOOT_LABEL=BPI-BOOT;  ROOT_LABEL=BPI-ROOT ;;
    emmc) BOOT_LABEL=EMMC-BOOT; ROOT_LABEL=EMMC-ROOT
          # the BSP kernel has no eMMC driver that this boot loader can use
          [ "$FLAVOUR" = mainline ] || { echo "IMAGE_TARGET=emmc needs KERNEL_FLAVOUR=mainline" >&2; exit 1; }
          OUT="${OUT%.img}-emmc.img" ;;
    *)    echo "IMAGE_TARGET must be 'sd' or 'emmc', got '$TARGET'" >&2; exit 1 ;;
esac
echo ">>> target: $TARGET"

IMG_MB=3072          # total size
P1_START=204800      # sector
ROOT_MB=0            # 0: the rootfs takes the rest of the image
MSD_LABEL=

# PiKVM keeps its virtual media on a partition of its own, so that filling it
# with ISOs cannot fill the root filesystem, and the user can see how much is
# left for images. The mainline SD image does the same: root is a fixed size
# and p3 takes the rest of the card (bpikvm-expand grows it on the first
# boot). The eMMC image has no room to spare on a 7.3 GiB eMMC, so there the
# store stays a directory on the rootfs, with an SD card as an option
# (bpikvm-msd-sd). The BSP image keeps the layout verified on main.
if [ "$TARGET" = sd ] && [ "$FLAVOUR" = mainline ]; then
    IMG_MB=3584
    ROOT_MB=3072
    MSD_LABEL=BPI-MSD
fi

for f in "$BSP/u-boot-rtk/u-boot.bin" \
         "$KIMAGE" \
         "$KDTB" \
         "$BSP/rtk-pack/rtk/bpi-w2/configs/default/linux/bluecore.audio" \
         "$BSP/rtk-pack/rtk/bpi-w2/configs/default/linux/uInitrd" \
         "$BUILD/$ROOTFS"; do
    [ -f "$f" ] || { echo "missing: $f"; exit 1; }
done

echo ">>> staging boot files"
rm -rf "$BUILD/bootfs" && mkdir -p "$BUILD/bootfs/bananapi/bpi-w2/linux"
L="$BUILD/bootfs/bananapi/bpi-w2/linux"
cp "$KIMAGE" "$L/uImage"
if [ "$FLAVOUR" = "mainline" ]; then
    # Both of this board's bootloaders take text_offset straight out of the
    # arm64 Image header and load the kernel at
    # `gd->bd->bi_dram[0].start + text_offset` -- see booti_setup() in the
    # BSP u-boot's common/cmd_bootm.c:715. Neither looks at bit 3 of `flags`,
    # the "this kernel may be placed at any 2 MiB aligned address" bit, and
    # CONFIG_SYS_SDRAM_BASE is 0 here, so the load address *is* text_offset.
    #
    # Mainline has hardcoded text_offset to 0 since 5.8, which puts the
    # kernel at physical 0 -- the boot ROM, not RAM -- and it dies without a
    # single character of output. LK prints exactly that:
    #     Boot image target addr:0x00000000, size:0x027f0000
    # The BSP 4.9 kernel still carried text_offset 0x280000 and so booted.
    #
    # The kernel never reads this field itself, so patching it only steers
    # the bootloader. It has to be 2 MiB aligned and clear of every region
    # the Realtek firmware owns -- the ACPU keeps running alongside Linux and
    # writes into its ION heaps:
    #     0x1c000000..0x1e7f0000  where the kernel lands
    #     0x03000000..0x0572fa00  where the bootloader read the Image to
    #     0x02100000, 0x02200000  dtb, initrd
    #     0x02600000..0x03200000  ION audio heap
    #     0x03200000..0x0ea00000  ION media heap 1
    #     0x0f900000..0x0fd00000  bluecore.audio / acpu_fw
    #     0x10100000..0x11000000  TEE
    #     0x11000000..0x1a200000  ION media heap 2
    #
    # 0x08000000 was used through M3 and sits inside media heap 1, where the
    # ACPU's video path overwrote the running kernel. It showed up as SLUB
    # taking a fault on a pointer of 0x80000000 a few seconds into userspace.
    TEXT_OFFSET="${KERNEL_TEXT_OFFSET:-0x1c000000}"
    esc=""
    for i in 0 1 2 3 4 5 6 7; do
        esc="$esc\\x$(printf '%02x' $(( ($TEXT_OFFSET >> (8 * i)) & 0xff )))"
    done
    printf "$esc" | dd of="$L/uImage" bs=1 seek=8 conv=notrunc status=none
    echo ">>> patched arm64 text_offset to $TEXT_OFFSET (old bootloaders ignore the relocatable flag)"
fi
if [ "$FLAVOUR" = "mainline" ]; then
    # The rootfs has no kernel modules (or the BSP's). Stage the mainline
    # ones; the assembly step below puts them in. Stripped, because
    # arm64 defconfig builds several hundred modules with full debug info.
    echo ">>> installing the mainline modules"
    rm -rf "$BUILD/modules-mainline"
    BUILDER_IMAGE=bpiw2-pikvm/builder-mainline:trixie \
    "$PROJECT_ROOT/scripts/in-docker.sh" bash -c '
        cd /work/vendor/linux-mainline &&
        make -s ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- LOCALVERSION="" \
             INSTALL_MOD_PATH=/work/build/modules-mainline INSTALL_MOD_STRIP=1 \
             modules_install'
    ls "$BUILD/modules-mainline/lib/modules"
    # VE1's firmware for the coda driver (H.264), downloaded, not shipped
    "$PROJECT_ROOT/scripts/fetch-vpu-firmware.sh" "$BUILD/firmware-mainline"
fi
cp "$KDTB" "$L/bpi-w2.dtb"
cp "$L/bpi-w2.dtb" "$L/rtd-1296-bananapi-w2-2GB.dtb"
cp "$BSP/rtk-pack/rtk/bpi-w2/configs/default/linux/bluecore.audio" "$L/"
# u-boot's compiled-in name is root.sd.cpio.gz_pad.img; the uEnv.txt set uses
# uInitrd. This u-boot does not read uEnv.txt, but a board may carry a stale
# environment, so ship both names.
cp "$BSP/rtk-pack/rtk/bpi-w2/configs/default/linux/uInitrd" "$L/root.sd.cpio.gz_pad.img"
cp "$L/root.sd.cpio.gz_pad.img" "$L/uInitrd"
cp "$BSP/rtk-pack/rtk/bpi-w2/configs/default/linux/uEnv.txt" "$L/"
cp "$BSP/rtk-pack/rtk/bpi-w2/bin/spirom-bpi-w2.bin" "$BUILD/bootfs/"
du -sh "$BUILD/bootfs"

# The boot partition is 60 MiB, which is what the BSP kernel needs with room
# to spare. A mainline Image built from arm64 defconfig is roughly twice the
# size of the BSP one, so size p1 from what was actually staged and round up
# to a multiple of 32 MiB. 60 MiB is the floor so the BSP image keeps exactly
# the layout that was verified on hardware.
#
# The mainline image also has to take new kernels over the network
# (scripts/push-kernel-mainline.sh), which briefly holds three of them --
# uImage, uImage.prev and the incoming uImage.new, ~42 MiB each -- so its
# floor is 256 MiB.
P1_MB=60
[ "$FLAVOUR" = "mainline" ] && P1_MB=256
staged_mb="$(du -sm "$BUILD/bootfs" | cut -f1)"
if [ "$((staged_mb + 12))" -gt "$P1_MB" ]; then
    P1_MB=$(( ((staged_mb + 12 + 31) / 32) * 32 ))
fi
P1_SECTORS=$(( P1_MB * 2048 ))
P2_START=$(( P1_START + P1_SECTORS ))
echo ">>> boot partition: ${P1_MB} MiB (${staged_mb} MiB staged), rootfs starts at sector $P2_START"

PART_TABLE="start=$P1_START, size=$P1_SECTORS, type=c, bootable"
if [ -n "$MSD_LABEL" ]; then
    P2_SECTORS=$(( ROOT_MB * 2048 ))
    P3_START=$(( P2_START + P2_SECTORS ))
    P3_SECTORS=$(( IMG_MB * 2048 - P3_START ))
    [ "$P3_SECTORS" -gt 0 ] || { echo "no room left for the ISO partition" >&2; exit 1; }
    PART_TABLE="$PART_TABLE
start=$P2_START, size=$P2_SECTORS, type=83
start=$P3_START, type=83"
    echo ">>> rootfs: ${ROOT_MB} MiB, ISO store: $(( P3_SECTORS / 2048 )) MiB (grown on the first boot)"
else
    P2_SECTORS=$(( IMG_MB * 2048 - P2_START ))
    P3_START=0
    P3_SECTORS=0
    PART_TABLE="$PART_TABLE
start=$P2_START, type=83"
fi

# /etc/fstab. The rootfs builds write one for an SD card without an ISO
# partition; this is the image's own, by label so that the same image may boot
# from SD, the eMMC or USB.
{
    echo "LABEL=$ROOT_LABEL  /      ext4  defaults,noatime         0 1"
    echo "LABEL=$BOOT_LABEL  /boot  vfat  defaults,noatime,nofail  0 2"
    if [ -n "$MSD_LABEL" ]; then
        # As PiKVM's own fstab: read-only, kvmd's remount helper takes it
        # read-write while it writes an image. nofail: a board must boot even
        # with this partition damaged or absent.
        echo "LABEL=$MSD_LABEL  /var/lib/kvmd/msd  ext4  nodev,nosuid,noexec,ro,errors=remount-ro,nofail,X-kvmd.otgmsd-user=kvmd  0 2"
    elif [ "$FLAVOUR" = mainline ]; then
        # A directory on the rootfs, bind-mounted read-only. The source is a
        # mount point of its own so that bpikvm-msd-sd can put an SD card
        # under it; requires-mounts-for keeps the two in order.
        echo "/var/lib/kvmd/msd.data  /var/lib/kvmd/msd  none  bind,nodev,nosuid,noexec,ro,X-kvmd.otgmsd-user=kvmd,x-systemd.requires-mounts-for=/var/lib/kvmd/msd.data  0 0"
    else
        echo "/var/lib/kvmd/msd.data  /var/lib/kvmd/msd  none  bind,nodev,nosuid,noexec,ro,X-kvmd.otgmsd-user=kvmd  0 0"
    fi
} > "$BUILD/fstab"

rm -rf "$BUILD/overlay" "$BUILD/overlay-bsp"
cp -r "$PROJECT_ROOT/overlay" "$BUILD/overlay"
cp -r "$PROJECT_ROOT/overlay-bsp" "$BUILD/overlay-bsp"

# /etc/motd describes this image only: its kernel, rootfs, target and the
# commit it was built from, not every combination the tree can build.
GIT_BRANCH="$(git -C "$PROJECT_ROOT" rev-parse --abbrev-ref HEAD 2>/dev/null || echo unknown)"
GIT_REV="$(git -C "$PROJECT_ROOT" describe --always --dirty 2>/dev/null || echo unknown)"
if [ "$FLAVOUR" = mainline ]; then
    MOTD_KERNEL="Linux $(ls "$BUILD/modules-mainline/lib/modules" | head -1) (mainline LTS)"
    MOTD_ROOTFS="Arch Linux ARM + PiKVM packages"
    MOTD_VIDEO="MJPEG, H.264 (direct and WebRTC), HDMI audio; a terminal in the Web UI"
else
    MOTD_KERNEL="Linux 4.9.119 (Realtek BSP)"
    MOTD_ROOTFS="Debian 13"
    MOTD_VIDEO="MJPEG"
fi
case "$TARGET" in
    sd)   MOTD_TARGET="SD card image (/boot is ${BOOT_LABEL}, / is ${ROOT_LABEL})" ;;
    emmc) MOTD_TARGET="eMMC image (/boot is ${BOOT_LABEL}, / is ${ROOT_LABEL});
          u-boot boots raw copies of /boot, kept in step by bpikvm-emmc-bootsync" ;;
esac
if [ -n "$MSD_LABEL" ]; then
    MOTD_MSD="/var/lib/kvmd/msd, a partition of its own (${MSD_LABEL})"
else
    MOTD_MSD="/var/lib/kvmd/msd, a directory on the root filesystem
          \"bpikvm-msd-sd\" dedicates an SD card to it instead"
fi
cat > "$BUILD/motd" <<MOTD

  BPI-W2 PiKVM
  ────────────────────────────────────────────────
  kernel  ${MOTD_KERNEL}
  rootfs  ${MOTD_ROOTFS}
  image   ${MOTD_TARGET}
  ISOs    ${MOTD_MSD}
  built   ${GIT_BRANCH} @ ${GIT_REV}, $(date -u +%Y-%m-%d)

  Web UI    https://<this board's IP>/   admin / admin
  Console   root / pikvm

  ** Change both default passwords before real use **
    kvmd-htpasswd set admin
    passwd

  Handy commands:
    hdmirx-info                 detected input timings and V4L2 caps
    hdmirx-capture 30 NV12      grab 30 frames to /tmp/hdmirx.raw
    systemctl status kvmd kvmd-nginx

  The board's Type-C port goes to the target machine: keyboard, mouse and
  virtual media (MSD). ATX is not wired.
  Video: ${MOTD_VIDEO}.

MOTD
echo ">>> motd:"; sed -n '4,7p' "$BUILD/motd"

echo ">>> assembling the image (in a container, no loop device needed)"
docker run --rm --entrypoint bash \
    -v "$BUILD:/b" -v "$BSP:/bsp:ro" debian:bookworm -euxc "
apt-get update -qq
apt-get install -qq -y e2fsprogs dosfstools mtools fdisk >/dev/null

cd /b
rm -f $(basename "$OUT")
truncate -s ${IMG_MB}M /b/$(basename "$OUT")

# Partition table
sfdisk /b/$(basename "$OUT") <<EOF
label: dos
unit: sectors
$PART_TABLE
EOF

# p1: vfat plus the boot files
truncate -s $((P1_SECTORS * 512)) /b/p1.img
mkfs.vfat -F 32 -n $BOOT_LABEL /b/p1.img
mcopy -i /b/p1.img -s /b/bootfs/* ::/

# p2: ext4 + rootfs
rm -rf /b/rootfs && mkdir -p /b/rootfs
tar xf /b/$ROOTFS -C /b/rootfs
if [ -d /b/modules-mainline/lib/modules ] && [ $FLAVOUR = mainline ]; then
    # The BSP modules cannot load into this kernel; replace them. Plain cp,
    # not cp -a: the staged tree belongs to the build uid, and the rootfs
    # must stay root-owned (see the uid check in build-rootfs.sh).
    rm -rf /b/rootfs/usr/lib/modules/*
    cp -r /b/modules-mainline/lib/modules/. /b/rootfs/usr/lib/modules/
    mkdir -p /b/rootfs/usr/lib/firmware
    cp /b/firmware-mainline/* /b/rootfs/usr/lib/firmware/
fi
# overlay/ again: the rootfs builds put it in, but a change to it should not
# need a rootfs rebuild (which takes the better part of an hour) to reach the
# image. Modes as rootfs-arch.sh sets them: 755 for executables, else 644.
(cd /b/overlay && find . ! -type d -printf '%P\\n') | while read -r f; do
    if [ -x /b/overlay/\$f ]; then m=755; else m=644; fi
    install -D -m \$m /b/overlay/\$f /b/rootfs/\$f
done
if [ $FLAVOUR = bsp ]; then
    # Files for the BSP kernel only (overlay-bsp/)
    cp -r /b/overlay-bsp/. /b/rootfs/
fi
install -m 644 /b/motd /b/rootfs/etc/motd
# Units added to overlay/ after the rootfs was built (rootfs-arch.sh enables
# the rest when it builds it)
mkdir -p /b/rootfs/etc/systemd/system/multi-user.target.wants
ln -sf /etc/systemd/system/bpikvm-emmc-bootsync.path /b/rootfs/etc/systemd/system/multi-user.target.wants/
install -m 644 /b/fstab /b/rootfs/etc/fstab
cat /b/rootfs/etc/fstab
truncate -s $(( P2_SECTORS * 512 )) /b/p2.img
mkfs.ext4 -q -F -L $ROOT_LABEL -d /b/rootfs /b/p2.img

# p3: kvmd's virtual media store, owned by kvmd so it can write images
if [ $P3_SECTORS -gt 0 ]; then
    owner=\$(grep '^kvmd:' /b/rootfs/etc/passwd | cut -d: -f3,4)
    [ -n "\$owner" ] || { echo 'no kvmd user in the rootfs'; exit 1; }
    truncate -s $(( P3_SECTORS * 512 )) /b/p3.img
    mkfs.ext4 -q -F -L $MSD_LABEL -E root_owner=\$owner /b/p3.img
    dd if=/b/p3.img of=/b/$(basename "$OUT") bs=512 seek=$P3_START conv=notrunc status=none
    rm -f /b/p3.img
fi

# Write into the image
dd if=/b/p1.img of=/b/$(basename "$OUT") bs=512 seek=$P1_START conv=notrunc status=none
dd if=/b/p2.img of=/b/$(basename "$OUT") bs=512 seek=$P2_START conv=notrunc status=none

if [ $TARGET = sd ]; then
# Bootloader: u-boot.bin goes at 40 KiB
dd if=/bsp/u-boot-rtk/u-boot.bin of=/b/$(basename "$OUT") bs=1024 seek=40 conv=notrunc status=none

# The SDMMC_BOOT header (the first 0x1B8 bytes of LBA0)
#
# This layout was measured from a BPI-W2 SD card known to boot:
#   0x000  "SDMMC_BOOT"     10 bytes ASCII
#   0x00A  00 00
#   0x00C  01 00 00 00
#   0x010  00 02 00 00      (0x200)
#   0x014..0x1B7  0xFF padding
#   0x1B8  disk signature / 0x1BE partition table / 0x1FE 55 AA  <- written by sfdisk, must be preserved
#
# The string appears neither in the BSP sources nor inside spirom-bpi-w2.bin,
# which means whatever checks for it is lower down (the SoC mask ROM, or a
# convention of BPI's imaging tool) and the BSP build never produces it.
# sfdisk clears the first 446 bytes, so this has to be written back after the
# partition table.
printf 'SDMMC_BOOT\x00\x00\x01\x00\x00\x00\x00\x02\x00\x00' > /b/sdmmc_hdr.bin
# Pad 0x014 .. 0x1B7 with 0xFF
head -c $((0x1B8 - 0x14)) /dev/zero | tr '\000' '\377' >> /b/sdmmc_hdr.bin
dd if=/b/sdmmc_hdr.bin of=/b/$(basename "$OUT") bs=1 count=440 conv=notrunc status=none
rm -f /b/sdmmc_hdr.bin
fi

rm -f /b/p1.img /b/p2.img
rm -rf /b/rootfs
chown $(id -u):$(id -g) /b/$(basename "$OUT")
sfdisk -l /b/$(basename "$OUT")
"

echo
echo ">>> done: $OUT ($(du -h "$OUT" | cut -f1))"
if [ "$TARGET" = emmc ]; then
    echo ">>> install from the board, booted from SD: bpikvm-install-emmc <image>"
    echo ">>>   or from here: xz -T0 -c $OUT | ssh root@<board> 'xz -dc | bpikvm-install-emmc -y -'"
else
    echo ">>> flash with: sudo dd if=$OUT of=/dev/sdX bs=4M conv=fsync status=progress"
fi
