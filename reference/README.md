# Reference material

## `known-good-bootarea-1MiB.bin` (not in this repository)

The first 1 MiB of a BPI-W2 SD card that is known to boot: the MBR, the
`SDMMC_BOOT` header and Realtek's u-boot binary.

It was used to work out the layout of the `SDMMC_BOOT` header, which appears
nowhere in the BSP sources. That layout is now documented in the comments of
`scripts/build-image.sh`, and `build-image.sh` writes the header itself, so
nothing in the build depends on this file.

It is **not committed** (`.gitignore` covers `reference/*.bin`) because it is
a copy of somebody else's bootloader and the knowledge it carried is already
captured in the build script.

To recreate it from a card that boots:

```sh
sudo dd if=/dev/sdX of=reference/known-good-bootarea-1MiB.bin bs=1M count=1
```

Useful checks against it:

```sh
xxd -l 32 reference/known-good-bootarea-1MiB.bin      # SDMMC_BOOT magic at 0x000
xxd -s 0x1B8 -l 72 reference/known-good-bootarea-1MiB.bin  # signature + partition table
```
