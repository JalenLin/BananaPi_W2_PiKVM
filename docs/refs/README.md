# External reference material

## `bpi-w2-v1_1-pub.pdf`

> **This file is not distributed with the project** (`.gitignore` covers it).
> Download it yourself and drop it in this directory. It is BPI's document,
> not our output.

BPI-W2 v1.1 public schematic (15 pages, published on the official BPI forum).

Source: <https://forum.banana-pi.org/t/bpi-w2-hardware-schematics/6173>
(Google Drive id `1Xn7-nlY0kVJacnRh_EpuS5Fw92_b6qqu`)

Pages that matter:

| Page | Contents |
|------|----------|
| 1, 2 | RTD1296 pinout |
| 6 | **HDMI IN / OUT** |
| 13 | 40-pin GPIO |

Page 6 is what cracked the EDID/DDC problem: the HDMI IN connector's pins
15/16 (SCL/SDA) go through R81/R82 (0R) to the SoC's `I2C6_SCL` /
`I2C6_SDA` (ISO pad 20 / pad 26) — not to dedicated HDMI RX pins. See
"Blocker 3" in `../04-hdmi-rx-bringup.md`.

HDMI OUT is wired to `TX_I2C1_SCL/SDA` → `I2C1_SCL/SDA` (R83/R80).

## Vendor reference images

> **Not distributed with the project** (`.gitignore` covers `/refer_images/`).
> They total ~450 MB compressed. Download them yourself into `refer_images/`
> if you want to repeat the analysis.

Two images from the BPI wiki were unpacked to answer the second-Ethernet-port
question in `../06-changes.md` §11. Both are listed at
<https://wiki.banana-pi.org/Banana_Pi_BPI-W2>.

| File | Kernel | Contains hwnat? |
|------|--------|-----------------|
| `bpi-w2-openwrt-lede` (`openwrt.zip`) | 4.9.119 — same as ours | **no**, NAS build; DT node disabled, no driver in the image |
| `2020-07-23-bpi-w2-android7-router.img.zip` | 4.1.35 | **yes**, built in (`=y`) |

Neither is a shortcut; §11 explains why. What they are good for is ground
truth about what the vendor actually ships.

Both are tar archives, not disk images, despite the `.img` name:

```sh
tar xf 2020-07-23-bpi-w2-android7-router.img omv/emmc.uImage omv/android.emmc.dtb
tar xf install.img                                    # the openwrt one
```

Useful things to pull out of them:

```sh
# the shipped router kernel's .config (CONFIG_IKCONFIG is on)
vendor/bpi-w2-bsp/linux-rtk/scripts/extract-ikconfig omv/emmc.uImage > router.config

# the DT the vendor actually boots, including the hwnat node's parameters
vendor/bpi-w2-bsp/linux-rtk/scripts/dtc/dtc -I dtb -O dts omv/android.emmc.dtb

# which drivers are built in
strings -a omv/emmc.uImage | grep -i '^Realtek,' | sort -u
```

The matching source for the 4.1.35 router kernel is **not** in the BPI-W2
BSP. It is in `BPI-1296-Android7` under `Openwrt/linux-4.1.7/` (the directory
name is wrong; the `Makefile` says `SUBLEVEL = 35`). That clone is sparse, so
use `git ls-tree -r HEAD --name-only` rather than `find` to see what is in it.
