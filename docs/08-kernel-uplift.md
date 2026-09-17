# Not chasing the newest: which kernel version is worth moving to

`07-mainline.md` asks "what blocks a move to mainline", and the answer is
"the RTD129x in mainline is essentially an empty shell". This document asks
the more practical question:

> Forget the newest. **Which version, what does it buy, and at what cost?**

Reference point: 2026-09-17, mainline v7.3-rc3 / latest stable 7.2.6, with
6.18 / 6.12 / 6.6 / 6.1 / 5.15 / 5.10 as the active longterm series.

---

## 1. Version thresholds: what each step up buys

Four things matter to this project. When each landed in mainline:

| Component | Landed in | What it means here |
|-----------|-----------|--------------------|
| `no_out_endpoint` in `f_hid` | **v5.15** | Frees an OUT endpoint on the mouse → MSD might fit inside the 2-IN budget |
| `drivers/phy/realtek/phy-rtk-usb2.c` (with `realtek,rtd1295-usb2phy`) | **v6.6** | No need to carry the USB 2.0 PHY over from the BSP |
| `drivers/usb/dwc3/dwc3-rtk.c` (`realtek,rtd-dwc3`) | **v6.7** | dwc3 glue plus the RTD globals-offset quirk (see §3) |
| `drivers/pinctrl/realtek/pinctrl-rtd.c` shared framework | **v6.7** | The base to build a 129x pinctrl on later |
| `drivers/phy/realtek/phy-rtk-usb3.c` (with `realtek,rtd1295-usb3phy`) | **v6.9** | USB 3.0 PHY |

The `no_out_endpoint` threshold is worth stating precisely, because it is
easy to get wrong: checking `drivers/usb/gadget/function/f_hid.c` at each
tag, v5.14 does **not** have it and v5.15 does. The floor is 5.15, and 5.15
is itself a longterm series.

(The `pinctrl/realtek/` directory was only created in 2023-09 and **has never
carried 129x pin tables** — only 1315E / 1319D / 1619B, plus 1625 added in
2026-03. Realtek does upstream steadily, but only for new SoCs; the 129x is
an old part to them.)

Mapped onto the longterm series:

| Target | Gets you | Still missing |
|--------|----------|---------------|
| 5.15 LTS | `no_out_endpoint` | All three Realtek USB pieces still have to come from the BSP |
| 6.6 LTS | the above + USB2 PHY | dwc3 glue, USB3 PHY, pinctrl framework |
| **6.12 LTS** | the above + dwc3 glue + USB3 PHY + pinctrl framework | **The first LTS where the whole Realtek USB family is present** |
| **6.18 LTS** | the above + someone has verified it on RTD1295 hardware (§2) | — |

**Conclusion: 6.12 is the lowest threshold that buys anything meaningful,
but 6.18 is the one to actually pick.** The next section explains why.

---

## 2. The key find: someone already runs an RTD1295 on 6.18 LTS

`Fireblossom/wd-mch-kernel` — **Linux 6.18.40 LTS ported to the WD My Cloud
Home (Realtek RTD1295)**, still being updated as of 2026-08. That repository
carries a 765-line `docs/PORTING_GUIDE_4.9_to_6.18.md` giving a per-file
account (it is their file, not one of ours).

What its author reports as verified on hardware (confirmed across three cold
power cycles): **four cores, interrupt-driven UART, gigabit ethernet, USB
3.0, Docker, md array assembly, a 15-second hardware watchdog,
pstore/ramoops, three-point cpufreq with an 85 °C passive throttle,
CE-accelerated dm-crypt, and temperature via both thermal and hwmon.**

The RTD1295 and our RTD1296 are the same family (the 1296 adds usable extra
cores and dual SATA) and share the `rtd129x` register layer. **That port
turns Route B in §7 of `07-mainline.md` from "possibly a dead end" into
"someone walked it and left a map".**

The author's own conclusion is worth repeating:

> Mainline support for the RTD1295 is **far more complete than it first
> appears.** […] The single most repeated trap: the driver source is in the
> mainline tree, the Makefile references it, but the Kconfig prompt is gated
> behind an unsatisfiable condition.

One of the three Kconfig deadlocks he hit is exactly the
`NET_VENDOR_REALTEK depends on PCI` found in `07-mainline.md` — **that is not
"no driver exists", it is "the symbol cannot be selected"**. It was
originally judged fatal and "you would have to write it yourself"; in
reality it is one `|| ARCH_REALTEK` in Kconfig plus carrying `r8169soc.c`
over from the BSP (8,903 lines, only 450 lines different from the vendor
original).

---

## 3. What can be copied directly

Each item below has a verified implementation on the RTD1295. The BPI-W2
shares that register layer, so these should carry over (to be confirmed on
hardware):

| Item | Approach | For the BPI-W2 |
|------|----------|----------------|
| **Four cores** | One hunk in `arch/arm64/kernel/smp_spin_table.c`: when the release address is not RAM, use `ioremap` + a 32-bit `writel_relaxed` instead of mainline's default `ioremap_cache` + 64-bit `writeq`. Decided by `memblock_is_map_memory()` | The BSP's release address is **also `0x9801AA44`** — the same value. In the DTS, change `enable-method` from `rtk-spin-table` to the standard `spin-table` |
| **Second-level IRQ mux** | Carry `irq-rtd129x.c` (303 lines) over from the BSP and add "W1C ack before dispatch" | The BSP's `rtd-1296.dtsi:150` already has `Realtek,rtk-irq-mux` — the same compatible |
| **GbE** | Add `\|\| ARCH_REALTEK` to Kconfig's `NET_VENDOR_REALTEK`; carry `r8169soc.c`; route all 23 `clk_get` call sites through one `rtl_clk_get_optional()` helper that **returns NULL rather than an ERR_PTR**, which sends the driver down its own direct-register bring-up path | The BPI-W2's `nic: gmac@98016000` also has compatible `Realtek,r8168` — the same part |
| **USB** | No driver writing needed; it is DT plus two probe-time quirks: ① `clk_en_usb` (CRT `0x9800000C` bit 4) is left closed by the bootloader ② the Type-C lane switch at `0x9801334C` resets to "disconnected", and without setting it the link only reaches High-Speed | Our Type-C has to be a **peripheral** (on the BSP that is `/delete-property/ drd_mode`, patch 0006), but it is the same register and the same bits (`TYPE_C_EN_SWITCH` = BIT(29), `TYPE_C_TxRX_sel` = BIT(28)\|BIT(27)) |
| **SATA** | Rewritten `phy-rtk-sata.c` (431 lines vs. the vendor's 677) plus mainline AHCI | The BPI-W2 also has `Realtek,rtk-sata-phy` / `Realtek,ahci-sata`. **This is the fallback for a root device — see §5** |
| **thermal / watchdog restart** | A new ~79-line thermal driver; add `.restart` to `rtd119x_wdt.c` (the only reset channel on that board) | Use as-is |
| **The rbus trap in `rtd129x.dtsi`** | Mainline's `bus@98000000` is a `simple-bus` **that carries `reg`**, which claims the whole 2 MiB window as a resource and makes every child device's own MMIO request return `-EBUSY`. **Drop the `reg` and keep only `ranges`** | **This one matters especially here**: hdmirx's regs are `0x98034000` / `0x98037000` / `0x98004000` … — **all inside that 2 MiB window**. Without removing `reg` first, hdmirx is guaranteed to fail probe |

One more useful observation: that port writes neither a pinctrl driver nor a
clock driver. It relies on **whatever state the bootloader left, plus
probe-time register writes**. That is exactly the mindset correction needed
here (see §4).

---

## 4. Three gaps specific to the BPI-W2

After copying the above, three things remain that the WD port never touched.

### 4.1 The DDC pinmux — a full pinctrl driver is not required

`07-mainline.md` puts "write `pinctrl-rtd1295.c`" first and estimates weeks.
Having seen the WD approach, that should be revised: **for DDC alone, no
pinctrl driver is needed.**

The exact address and value are already known (`04-hdmi-rx-bringup.md`):

```
ISO MUXPAD 0x98007314
  bits[1:0] = i2c_scl_6  (ISO pad 20)
  bits[3:2] = i2c_sda_6  (ISO pad 26)
  function 0x1 = i2c6
  → bits[3:0] = 0b0101
```

A probe-time `ioremap` plus read-modify-write, five lines, exactly the same
pattern the WD port uses for `clk_en_usb`. **A full pinctrl driver is still
the *right* thing to do** (SD and GbE pins need it too), but it is not a
prerequisite for the HDMI path.

### 4.2 hdmirx's clocks, resets and power domains

`rtk_hdmirx` wants 9 named clocks, 7 named resets and 2 power-controls.
Mainline has no RTD129x clock driver, so all those `clk_get` calls return
ERR_PTR — which is precisely where the WD port oopsed
(`__clk_is_enabled` receiving an ERR_PTR).

The fix is the same `rtl_clk_get_optional` pattern from §3: return NULL, then
open the gates yourself. All the bit numbers are constants in the BSP's
dt-bindings, and combining them with the node addresses in
`rtd-129x-common.dtsi` reconstructs the raw registers:

| Register | Address | Bits needed |
|----------|---------|-------------|
| `clk_en_1` | `0x9800000C` | `CLK_EN_MIPI`=27, `CLK_EN_TP`=21, `CLK_EN_CP`=19 |
| `clk_en_2` | `0x98000010` | `CLK_EN_HDMIRX`=24, `CLK_EN_CBUS_TX`=7 |
| `iclk_en` | `0x9800708C` | `CLK_EN_CBUS_OSC`=6, `CLK_EN_CBUS_SYS`=5, `CLK_EN_CBUSTX_SYS`=4, `CLK_EN_CBUSRX_SYS`=3 |
| `rst4` | `0x98000050` | `RSTN_HDMIRX`=5, `RSTN_HDMIRX_WRAP`=12 |

(The resets do not actually need doing by hand: mainline's
`snps,dw-low-reset` / reset-simple is already in `rtd129x.dtsi`, so adding
`resets = <...>` is enough.)

Still unresolved are the two SRAM power domains (`pctrl_disp_hdmi_rx`,
`pctrl_disp_mipi`) — their registers have to be found in the BSP's
`soc/realtek`.

What remains after that is 4.9 → 6.x V4L2 API drift, and cutting out the
MIPI / HDCP / ION paths we do not use (`ion/ion.h` was removed from mainline
in 5.11, but it only appears in `mipi_wrapper.c`; our capture path uses
`vb2_dma_contig_memops` and is portable).

### 4.3 The root device — the real wall

The WD My Cloud Home has its root on SATA (`/dev/md1`), so that port **never
touched MMC**. Mainline still has no RTD129x mmc host driver of any kind
(`drivers/mmc/host/Kconfig` has zero realtek hits).

And this project's stated goal is, in writing, "**produce an SD card image
you can flash and use directly**".

---

## 5. Three options for the root device

| Option | Feasibility | Cost |
|--------|-------------|------|
| **A. SATA** | The WD port verified `phy-rtk-sata` + AHCI working on 6.18; the BPI-W2 also has two SATA ports | Requires an external drive, which directly contradicts "flash a card and go" |
| **B. USB** | The WD port verified the DRD block in host mode (its initramfs raises VBUS through a GPIO) | The BPI-W2's Type-C is reserved for the HID gadget, so a separate USB host port is needed. "Flash a USB stick and go" is a one-step-back version of the goal |
| **C. Write an mmc host driver** | The only option that fully meets the project goal | The biggest unknown of the three. The BSP's `Realtek,rtk1295-sdmmc` / `rtk1295-emmc` is a starting point, but there is no precedent to copy |

**B is the pragmatic middle**: it lets "PiKVM on a mainline kernel" run and
lets the HDMI path be validated without writing an mmc driver first, leaving
C for later.

---

## 6. Suggested milestones

Target version: **6.18 LTS** — there is a hardware-verified RTD1295
precedent, the whole Realtek USB family is present, and it has the longest
LTS life ahead of it.

| Stage | Contents | Acceptance | New code needed |
|-------|----------|------------|-----------------|
| **M0** | BSP u-boot + clean 6.18.x + `rtd1296.dtsi` + a 30-line `rtd1296-bananapi-w2.dts` | Characters on the serial console (single core, initramfs) | None |
| **M1** | Copy the WD `smp_spin_table.c` hunk + `irq-rtd129x.c` + drop `reg` from rbus | Four cores, interrupt-driven UART | 1 file carried over + 2 hunks |
| **M2** | Unlock Kconfig + carry `r8169soc.c` (with the `clk_get_optional` pattern) | ping works, ssh works | 1 file carried over |
| **M3** | USB DT + two probe quirks, Type-C set to peripheral | Boots from a USB host port (root on USB) and `/dev/hidg*` appears | ~40 lines |
| **M4** | kvmd + ustreamer on 6.18, `no_out_endpoint` in effect | Web UI works, **MSD may become enableable** | None (userspace is ready) |
| **M5** | hdmirx port: MUXPAD quirk + clock gate quirk + V4L2 API catch-up | 1080p60 capture | The biggest piece |
| **M6** | mmc host driver | Back to "flash an SD card and go" | The biggest unknown |

After M0–M4 you already have **a PiKVM running on 6.18 LTS (no video, root on
USB)**, and the MSD limitation may be gone. M5 is where the real cost of
changing kernels sits, and M6 is what restores the project's intended
deliverable.

If the files from M0–M2 are cleaned up they are upstreamable: `ARM/REALTEK`
accepts patches to `arch/arm64/boot/dts/realtek/`, and `rtd1395-bpi-m4.dts`
is already in mainline as precedent for a Banana Pi board.

---

## 7. The current decision

**Do nothing for now.** Recorded here with reasons:

1. The BSP 4.9 path is functionally complete (HDMI + HID + Web UI), so
   moving before M5 is a **functional regression**
2. The benefits of changing kernels (MSD; four cores, which we already have;
   better userspace compatibility) are not yet painful enough to force the
   issue
3. The MSD limitation, which is the thing actually worth solving, only needs
   **5.15** — if that were the sole goal, there may be cheaper options than a
   full mainline move

Triggers that would restart this:

- 4.9 blocking a userspace component we need (Debian 13 still copes today)
- MSD or multiple mouse modes becoming a hard requirement
- The WD port's author, or anyone else, producing an RTD129x clock or mmc
  driver — that would turn M6 from "the biggest unknown" into "copy it"

---

## 8. Sources

| Source | Used for |
|--------|----------|
| `Fireblossom/wd-mch-kernel` (GitHub, updated 2026-08) | The 6.18.40 LTS port for RTD1295, including the 765-line `docs/PORTING_GUIDE_4.9_to_6.18.md` |
| torvalds/linux v5.14 – v7.3-rc3 (files compared tag by tag) | Which release each component landed in |
| kernel.org `releases.json` | The active longterm list |
| `vendor/bpi-w2-bsp/linux-rtk` | Register addresses, clock/reset bit numbers, compatible strings |
| `04-hdmi-rx-bringup.md` | The MUXPAD address and value |

Related documents: `07-mainline.md` (the full inventory of blockers against
current mainline) and `02-decisions.md` D1 (why the BSP kernel was chosen
originally).
