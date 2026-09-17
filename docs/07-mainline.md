# What blocks a move to a mainline kernel

The project's goal statement says "prefer new, mainline code". The current
implementation runs the BSP's Linux 4.9.119. This document inventories what
actually stands in the way of switching to mainline.

Reference point: **mainline v7.3-rc3** (checked against torvalds/linux master
on 2026-09-17), and this project's `vendor/bpi-w2-bsp/linux-rtk` (4.9.119).

---

## 1. Conclusion

**The blocker is not the HDMI RX driver.**

Mainline's support for the RTD129x amounts to "a single-core system that
reaches a serial console". PiKVM needs four things — root storage,
networking, USB gadget and video capture — and **three of them do not exist
in mainline at all**. None of what is missing is PiKVM-specific either; it is
SoC plumbing (pinctrl, clk, mmc, GbE).

Put differently: this is not a job of "porting PiKVM to mainline", it is a
job of "bringing the RTD129x to mainline", with PiKVM as its first consumer.

---

## 2. What mainline has today

`arch/arm64/boot/dts/realtek/rtd129x.dtsi` is **195 lines** in total:

| Present | Note |
|---------|------|
| 4 × Cortex-A53 | **No `enable-method`** → effectively single-core (see §4.6) |
| GIC-400, armv8-timer, cortex-a53-pmu | |
| One 27 MHz `fixed-clock` | Just the one. No PLLs, no clock gates |
| 5 × `syscon` / `simple-mfd` | |
| 5 × `snps,dw-low-reset` | reset-simple |
| `realtek,rtd1295-watchdog` | |
| 3 × `snps,dw-apb-uart` | |

There is no board file for the BPI-W2. The only sibling is
`rtd1296-ds418.dts` (Synology DS418), whose 30 lines are a memory node, a
`stdout-path` and `uart0 status = "okay"`.

`MAINTAINERS`' `ARM/REALTEK ARCHITECTURE` covers exactly two paths:
`arch/arm64/boot/dts/realtek/` and `drivers/pinctrl/realtek/`.
`drivers/soc/realtek/` **does not exist**.

u-boot is the same story: mainline u-boot has no `arch/arm/mach-realtek` and
no `board/realtek`. The BSP's u-boot 2015.07 is not going anywhere soon, but
that is independent of the kernel (see §5).

---

## 3. PiKVM's four paths vs. mainline

| Path | What it uses today (BSP) | Mainline | Severity |
|------|--------------------------|----------|----------|
| Root storage, SD/eMMC | `Realtek,rtk1295-sdmmc` / `rtk1295-emmc` | **None** | Fatal |
| Networking, GbE | `Realtek,r8168` (platform device) | **None** | Fatal |
| USB HID gadget | dwc3 + `dwc3-rtk-type_c` | **All present** ✅ | None |
| HDMI capture | `rtk_hdmirx` (26,327 lines) | **None** | Major effort |
| — pinctrl (everything above needs it) | `pinctrl-rtd129x.c` | Framework present, **no 129x tables** | Prerequisite |
| — clk (everything above needs it) | `drivers/clk/realtek` (3,715 lines) | Only the 27 MHz fixed osc | Prerequisite |

---

## 4. Item by item

### 4.1 Root storage — fatal

`drivers/mmc/host/Kconfig` has zero realtek hits (the only `rtsx_*` entries
are PCIe card readers and unrelated to the SoC). **Without a host driver you
cannot mount a rootfs**, so the whole path dies at step one.

Every workaround leads back to another gap:

- boot from USB → needs USB first (this one is actually viable, see §6)
- NFS root → needs networking first (absent)

### 4.2 Networking — looks fatal, is actually a Kconfig lockout

The RTD1296 has an RTL8168-family MAC on an internal SoC bus, which the BSP
drives as the platform device `Realtek,r8168`.

Mainline's `drivers/net/ethernet/realtek/Kconfig` makes the entire directory
`depends on PCI` — r8169 only recognises PCIe cards, never the SoC's internal
MAC.

PiKVM without networking is pointless.

**But this turned out not to be "write your own driver"**: that
`depends on PCI` is an unsatisfiable Kconfig gate. Adding `|| ARCH_REALTEK`
and carrying `r8169soc.c` over from the BSP is enough, and someone has
already done exactly that in a 6.18 port for the RTD1295. See §4 of
`08-kernel-uplift.md`.

### 4.3 USB HID gadget — mainline is actually complete here

The one piece of good news, and all three parts are present:

| Component | Mainline file | Supports RTD1296? |
|-----------|---------------|-------------------|
| dwc3 glue | `drivers/usb/dwc3/dwc3-rtk.c` | `realtek,rtd-dwc3` (generic), `select USB_ROLE_SWITCH` |
| USB2 PHY | `drivers/phy/realtek/phy-rtk-usb2.c` | `realtek,rtd1295-usb2phy` ✅ |
| USB3 PHY | `drivers/phy/realtek/phy-rtk-usb3.c` | `realtek,rtd1295-usb3phy` ✅ |

All that is left is writing the DT nodes and pinning Type-C to peripheral
mode. On the BSP we do that with `/delete-property/ drd_mode` (patch 0006);
mainline's equivalent is `dr_mode = "peripheral"` or `usb-role-switch`, which
is cleaner.

**There is a real benefit to moving here**: the endpoint budget does not
change (`GHWPARAMS3` says 2 usable IN), but `f_hid` in kernel ≥ **5.15**
supports `no_out_endpoint`, which frees an OUT endpoint on the mouse. The
"keyboard + mouse fills the budget and MSD does not fit" limitation on BSP
4.9 (see README and `docs/06-changes.md`) could be relaxed on mainline.

### 4.4 HDMI capture — the biggest piece

Mainline has nothing, and nothing close enough to adapt. The only option is
porting the BSP's `drivers/media/platform/rtk_hdmirx`:

```
rtd129x/ + rx_drv/   26,327 lines of C/H
```

Its BSP-only dependencies:

| Dependency | Purpose | What to do on mainline |
|------------|---------|------------------------|
| `soc/realtek/power-control.h` | SRAM power domains for HDMI RX / MIPI (`pctrl_disp_hdmi_rx`) | `drivers/soc/realtek/` does not exist; rewrite as genpd or poke the syscon bits directly |
| 9 named clocks + 7 named resets | `hdmirx`/`mipi`/`cbus_*`/`tp`/`cp` | Resets can use mainline's reset-simple; clocks have no driver at all (§4.5) |
| `soc/realtek/rtk_sha1.h` | HDCP | We do not use it; the whole section can go |
| `ion/ion.h` | Only in `mipi_wrapper.c` | ION was removed from mainline in 5.11. Our capture path uses `vb2_dma_contig_memops`, so the MIPI path can be cut |

`vb2`'s mem_ops is `vb2_dma_contig_memops` — **that part is portable** and
the data path does not need changing.

There is also 4.9 → 7.x V4L2 API drift to absorb: `buf_prepare`/`buf_queue`
signatures, `vb2_queue` fields, the `v4l2_device` registration flow, and the
fact that new `media/platform` drivers are expected to have a media
controller.

**A realistic view on upstreaming**: this code is a large body of
undocumented register writes plus firmware tables (`DFE_fw.h`, `HDMI_fw.h`,
`HDMIRXDDC_fw.h`), and we had to add `G_FMT`/`TRY_FMT` ourselves (patch
0004). Making it conform to V4L2's DV-timings driver conventions
(`ENUM/QUERY_DV_TIMINGS`, `G_EDID`/`S_EDID`) is another large rewrite. The
pragmatic target is "compiles as an out-of-tree module on mainline", not
"merged".

### 4.5 pinctrl and clk — prerequisites for everything

**pinctrl** should be done first and is the easiest:

- Mainline already has `drivers/pinctrl/realtek/pinctrl-rtd.c`, a shared
  regmap-mmio framework, but only carries pin/function tables for
  **1315E / 1319D / 1619B / 1625** — **no 129x**.
- The BSP's `pinctrl-rtd129x.c` uses the old interface and cannot be dropped
  in as-is, but its pin tables can be carried over.
- This is the only area with an upstream maintainer actively taking patches
  (`MAINTAINERS` lists `drivers/pinctrl/realtek/` explicitly).
- Incidentally, the BSP's uninitialised-tail `*num_maps` bug in
  `RTK_pctrl_dt_node_to_map()` (see `docs/04-hdmi-rx-bringup.md`) does not
  exist in the mainline framework, which also makes our workaround of
  attaching pinctrl to the hdmirx device node (patch 0005) unnecessary.

**An important clarification about DDC**: our EDID fix muxes the pads to the
`i2c6` function. But the BSP's DTS **has no `i2c_6` bus node at all** (only
i2c0 through i2c5) — "i2c6" is purely a pinmux function name, and the DDC
slave logic lives inside the HDMI RX block (`HDMIRXDDC_fw.h`). So on
mainline this needs **only pinctrl, not an i2c controller driver**.

**clk** has no shortcut: mainline has one 27 MHz fixed oscillator, while the
BSP's `drivers/clk/realtek` (`cc-rtd129x.c`, `clk-pll.c`, `clk-mmio-gate.c`
and friends) is 3,715 lines. SD/eMMC, GbE and hdmirx all use named clocks.

### 4.6 SMP — you drop to a single core

Mainline's rtd1296 CPU nodes carry no `enable-method` or `psci` at all. The
BSP uses a private one:

```
enable-method = "rtk-spin-table";
cpu-release-addr = <0x0 0x9801AA44>;
```

`rtk-spin-table` is an arm64 SMP operation the BSP added itself and mainline
does not have. Either implement PSCI (which means touching BL31 / secure
firmware) or upstream the BSP's spin-table op.

For PiKVM this is a real regression: `main.yaml`'s ustreamer uses
`--workers=3` for JPEG encoding, and a single core directly cuts the stream
frame rate.

---

## 5. u-boot is not a blocker

Mainline u-boot has no RTD129x support, but that is independent of the
kernel — the BSP's u-boot 2015.07 only has to load an `Image` plus a dtb, and
it does not care what kernel version that is. Boot chain details are in
`docs/03-image-and-boot.md`.

---

## 6. If you do go for mainline, start here

Do not start with hdmirx. Start with a milestone you can validate **without
writing any new driver**:

**Milestone 0: a mainline kernel reaching the serial console**

1. BSP u-boot + mainline v7.3 + `rtd1296.dtsi`
2. Add `rtd1296-bananapi-w2.dts` in the same 30-line style as
   `rtd1296-ds418.dts`: `memory@1f000` + `aliases` +
   `chosen/stdout-path` + `&uart0 status = "okay"`
3. The goal is only "single core, serial console, initramfs" — no SD mount
4. Passing this proves the BSP's bootloader / secure firmware is willing to
   load a mainline kernel, which is what makes the rest worth the effort
5. That board dts can be sent upstream while you are at it — ARM/REALTEK
   accepts dts patches, and BPI-M4's `rtd1395-bpi-m4.dts` is already there as
   precedent

After that, ordered by how much each item unblocks:

| Order | Item | Why here | Rough estimate |
|-------|------|----------|----------------|
| 1 | `pinctrl-rtd1295.c` | Prerequisite for every peripheral; framework exists; the only area anyone is taking patches for | weeks |
| 2 | RTD129x clk driver | Prerequisite for every peripheral; no shortcut | weeks to months |
| 3 | USB (DT only) | Drivers all exist; gets USB boot and HID working | days |
| 4 | mmc host | Needed to boot from SD | months (needs a datasheet or reverse-engineering the BSP) |
| 5 | GbE | A hard requirement for PiKVM | months |
| 6 | `rtk_hdmirx` port | The biggest piece, but pointless before the rest works | months, and targeted at out-of-tree |

(Estimates are guesses, not commitments.)

A shortcut worth considering: **once item 3 is done, boot from USB with a USB
NIC** and defer items 4 and 5. That gets you to "PiKVM on a mainline kernel,
without video" using only pinctrl + clk + USB, and hdmirx can come later.

---

## 7. Three routes and their costs

| Route | What it is | Cost |
|-------|------------|------|
| **A. Stay put** | BSP 4.9.119 | The kernel is EOL (4.9 ended maintenance in 2023-01). A Debian 13 userspace on 4.9 is already at the edge — the py3.13, systemd and missing-`no_out_endpoint` problems we hit are all this cost |
| **B. Move to some LTS (not the newest)** | Target 6.18 LTS and crib from an existing RTD1295 port | **Someone has already done this** (`Fireblossom/wd-mch-kernel`: RTD1295 on 6.18.40 LTS, verified on hardware with four cores, GbE and USB3). Best return on effort; full inventory in `08-kernel-uplift.md` |
| **C. Real mainline** | The six steps in §6 | Sized as "bring the RTD129x to mainline", which is beyond one person on this project |

Measured against the "prefer new, mainline code" goal, the honest summary is:
**userspace already achieves it** (Debian 13, kvmd v4.213 and ustreamer v6.66
are all current), while **the kernel layer is blocked by the state of SoC
support** — and the reason it is blocked is that the RTD129x in mainline is
essentially an empty shell.

---

## 8. Related documents

- `docs/06-changes.md` — the complete change list (single source of truth)
- `docs/04-hdmi-rx-bringup.md` — the full debugging story for the pinctrl
  bug, DDC and HPD
- `docs/08-kernel-uplift.md` — **the pragmatic route that does not chase the
  newest**: version thresholds, an existing port to crib from, staged
  milestones
- `docs/02-decisions.md` — the original record of choosing the BSP kernel
  over mainline
