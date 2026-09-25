#!/bin/bash
# Assemble a flashable SD card image.
#
# Layout (from the BSP's scripts/dd_download.sh and bootloader.sh):
#   offset 40 KiB          u-boot.bin
#   sector 204800..        p1 vfat  "BPI-BOOT"  -- 60 MiB, grown if the staged
#                                                  boot files need more
#   after p1               p2 ext4  rootfs
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
        ;;
    mainline)
        LINUX="$PROJECT_ROOT/vendor/linux-mainline"
        KIMAGE="$LINUX/arch/arm64/boot/Image"
        KDTB="$LINUX/arch/arm64/boot/dts/realtek/rtd1296-bananapi-w2.dtb"
        OUT="$BUILD/bpiw2-pikvm-mainline.img"
        ;;
    *)
        echo "KERNEL_FLAVOUR must be 'bsp' or 'mainline', got '$FLAVOUR'" >&2
        exit 1
        ;;
esac
echo ">>> kernel flavour: $FLAVOUR"

IMG_MB=3072          # total size
P1_START=204800      # sector

for f in "$BSP/u-boot-rtk/u-boot.bin" \
         "$KIMAGE" \
         "$KDTB" \
         "$BSP/rtk-pack/rtk/bpi-w2/configs/default/linux/bluecore.audio" \
         "$BSP/rtk-pack/rtk/bpi-w2/configs/default/linux/uInitrd" \
         "$BUILD/rootfs.tar"; do
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
    # rootfs.tar carries the BSP kernel's modules. Stage the mainline ones
    # next to it; the assembly step below swaps them in. Stripped, because
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
start=$P1_START, size=$P1_SECTORS, type=c, bootable
start=$P2_START, type=83
EOF

# p1: vfat plus the boot files
truncate -s $((P1_SECTORS * 512)) /b/p1.img
mkfs.vfat -F 32 -n BPI-BOOT /b/p1.img
mcopy -i /b/p1.img -s /b/bootfs/* ::/

# p2: ext4 + rootfs
rm -rf /b/rootfs && mkdir -p /b/rootfs
tar xf /b/rootfs.tar -C /b/rootfs
if [ -d /b/modules-mainline/lib/modules ] && [ $FLAVOUR = mainline ]; then
    # The BSP modules cannot load into this kernel; replace them. Plain cp,
    # not cp -a: the staged tree belongs to the build uid, and the rootfs
    # must stay root-owned (see the uid check in build-rootfs.sh).
    rm -rf /b/rootfs/usr/lib/modules/*
    cp -r /b/modules-mainline/lib/modules/. /b/rootfs/usr/lib/modules/
fi
P2_SECTORS=\$(( ${IMG_MB} * 2048 - $P2_START ))
truncate -s \$(( P2_SECTORS * 512 )) /b/p2.img
mkfs.ext4 -q -F -L BPI-ROOT -d /b/rootfs /b/p2.img

# Write into the image
dd if=/b/p1.img of=/b/$(basename "$OUT") bs=512 seek=$P1_START conv=notrunc status=none
dd if=/b/p2.img of=/b/$(basename "$OUT") bs=512 seek=$P2_START conv=notrunc status=none

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

rm -f /b/p1.img /b/p2.img
rm -rf /b/rootfs
chown $(id -u):$(id -g) /b/$(basename "$OUT")
sfdisk -l /b/$(basename "$OUT")
"

echo
echo ">>> done: $OUT ($(du -h "$OUT" | cut -f1))"
echo ">>> flash with: sudo dd if=$OUT of=/dev/sdX bs=4M conv=fsync status=progress"
