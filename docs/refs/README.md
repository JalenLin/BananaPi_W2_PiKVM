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
