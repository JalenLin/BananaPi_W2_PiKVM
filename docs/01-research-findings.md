# Pre-work research findings (2026-09-04)

This document records the fact-finding done **before any code was written**.
It is a snapshot of what was known at the time. Every conclusion carries a
reproducible source, so if someone later questions a design decision, start
here and look at the evidence.

> **This is not the current state.** Every blocker listed here was
> subsequently solved. For what was actually done, and what was discovered
> later, see `06-changes.md`.

## What we concluded then vs. how it turned out

| Conclusion at the time | Outcome |
|------------------------|---------|
| Mainline kernel is not viable | Held up; the whole project runs on BSP 4.9.119 |
| HDMI IN has no 720p30 hardware limit — it is a usage problem | **Held up**; 1080p60 on hardware with zero dropped frames |
| Blocker 3.1: the DT node is disabled | Opened by patch 0002, which also uncovered the HPD deadlock |
| Blocker 3.2: zero overlap between pixel formats and ustreamer | ustreamer patch 0001 adds NV12/NV21/NV16 |
| Blocker 3.3: systemd vs. 4.9 compatibility | Debian 13 trixie chosen, verified on hardware |
| "The built-in EDID already advertises 1080p60" | The EDID content was indeed fine, but **it was never actually served** — the DDC pins had never been muxed (patch 0005) |
| "Only `CONFIG_USB_CONFIGFS_F_HID` needs turning on" | Not only. The Type-C port was also switched to host by `drd_mode`, so no UDC existed at all (patch 0006), and dwc3's endpoint budget only fits two gadget functions |

---

## 1. Mainline kernel is not viable

### Evidence

Extent of Realtek support in `torvalds/linux` master:

| Item | State |
|---|---|
| `arch/arm64/boot/dts/realtek/rtd1296.dtsi` | Present |
| `arch/arm64/boot/dts/realtek/rtd1296-bananapi-w2.dts` | **Absent** (only Synology's `rtd1296-ds418.dts`) |
| Nodes in `rtd129x.dtsi` | Only GIC, 5 syscons, 5 `snps,dw-low-reset`, watchdog, 3 UARTs |
| eMMC / SD host driver | **None** |
| Ethernet (the SoC's internal GMAC) | **None** (`drivers/net/ethernet/realtek/` has only the PCIe parts: r8169/8139/rtase) |
| clk driver | **None** (no realtek directory under `drivers/clk/`) |
| pinctrl | Only RTD1315E / 1319D / 1619B / 1625 — **no rtd129x** |
| Coverage of MAINTAINERS' `ARM/REALTEK ARCHITECTURE` | Only `arch/arm/boot/dts/realtek/`, `arch/arm/mach-realtek/`, `arch/arm64/boot/dts/realtek/`, `drivers/pinctrl/realtek/` |

Conclusion: on a BPI-W2, mainline gets as far as a boot console and
**cannot mount a rootfs**.

### The cost of forward-porting

Measured line count of Realtek out-of-tree code in the BSP kernel:

```
find drivers arch/arm64 -type d \( -iname '*rtk*' -o -iname '*realtek*' -o -iname 'rtd*' \) \
  | xargs -I{} find {} -name '*.c' -o -name '*.h' | sort -u | xargs wc -l
→ 1,943,330 total
```

Spread across `drivers/soc/realtek/rtd129x` (clk / reset / power-domain /
VE), `drivers/clk/realtek`, `drivers/pinctrl/realtek`,
`drivers/staging/android/ion/realtek`, `drivers/net/ethernet/realtek`
(the r8168 SoC GMAC), `drivers/media/platform/rtk_*` and
`drivers/video/fbdev/rtk`.

### How community attempts went

- A 5.9-rc4 rebased mainline posted on the BPI forum: the kernel crashed
  immediately after loading with no UART or display output, and the author
  never published the patches.
- Attempts at 6.18-rc5 in 2025–2026 stalled on the same thing — eMMC / SATA
  / PCIe all need Realtek code that is not public.

**Decision: use the BSP's `linux-rtk` 4.9.119 plus our own patches.**
CLAUDE.md's "prefer new, mainline code" is honoured in userspace instead
(Debian, ustreamer and kvmd are all current).

---

## 2. How HDMI IN is meant to be driven (reverse-engineered from the Android HAL)

### Source

`BPI-SINOVOIP/BPI-1296-Android7` → `android/hardware/realtek/tv_input/`.
The implementation is a prebuilt `tv_input.kylin.so` (ELF 32-bit ARM,
stripped, 67 KB). Key strings pulled out with `strings`:

```
/sys/devices/virtual/switch/rx_video/state
/sys/devices/virtual/video4linux/video250/hdmirx_video_info
/dev/video250
HDMI Rx video %dx%d%c%d Color:%d %sReady
VIDIOC_REQBUFS / QUERYBUF / QBUF / STREAMON / DQBUF
```

### Cross-checked against BSP kernel sources (everything matches)

| Observed in Android | Corresponding BSP kernel location |
|---|---|
| `/dev/video250` | `rtk_hdmirx/rtd129x/hdmirx_video_dev.c:641`, `video_register_device(video_dev, VFL_TYPE_GRABBER, 250)` |
| `hdmirx_video_info` | Same file, `:570`, `show_hdmirx_video_info()`, emitting `Type/Status/Width/Height/ScanMode/Color/Fps` |
| `switch/rx_video/state` | `v4l2_hdmi_dev.c:560`, `sdev->name = "rx_video"; switch_dev_register(sdev)` |

### What Android actually does

1. Watch `/sys/devices/virtual/switch/rx_video/state` (netlink uevent) for a
   signal to appear
2. Read `hdmirx_video_info` to get the **timings actually detected** —
   width / height / fps / colour
3. `VIDIOC_S_FMT` with those dimensions
4. `REQBUFS` → `QUERYBUF` → `QBUF` → `STREAMON` → `DQBUF`

### The driver itself has no 720p30 limit

- `v4l2_hdmi_dev.c:30-31`: `MAX_WIDTH 4096` / `MAX_HEIGHT 2160`
- `hdmirx_video_dev.c`'s `VIDIOC_ENUM_FRAMESIZES` returns
  `V4L2_FRMSIZE_TYPE_CONTINUOUS`, with bounds taken directly from the
  detected input timings `mipi_top.h_input_len` / `v_input_len`
- `VIDIOC_S_FMT` only checks that the output is not larger than the input,
  i.e. downscaling is supported

### The built-in EDID already advertises 1080p60

`arch/arm64/boot/dts/realtek/rtd129x/rtd-1295-hdmirxEDID.dtsi`:

- First DTD: pixel clock `0x3A02` = 14850 → **148.50 MHz = 1920x1080p60**
- Second DTD: `0x1D01` = 7425 → 74.25 MHz = 1280x720p60
- CEA Video Data Block SVD sequence: `0x90` (VIC16 1080p60, native),
  `0x1F` (VIC31 1080p50), `0x22` (VIC34 1080p30), `0x20` (VIC32 1080p24),
  `0x05` (VIC5 1080i60), `0x14` (VIC20 1080i50), `0x04` (VIC4 720p60),
  `0x11` (VIC17 576p50), `0x02` (VIC2 480p60)

**Conclusion: only getting 720p30 was a usage problem, not a hardware or
EDID limit.** You have to follow Android's flow — read the real timings from
sysfs first, then `S_FMT` at those dimensions.

---

## 3. Three blockers to clear before starting

### 3.1 The DT node is disabled

`arch/arm64/boot/dts/realtek/rtd129x/rtd-1296-bananapi-common.dtsi:143`

```dts
hdmirx@98034000 {
        compatible = "Realtek,rtk-mipi-top";
        status = "disabled";          /* ← must become okay */
        gpio-rx-hpd-ctrl = <&rtk_iso_gpio 22 1 0>;
        gpio-5v-detect = <&rtk_iso_gpio 22 0 0>;
};
```

`# CONFIG_RTK_HDMIRX is not set` in `rtd129x_bpi_defconfig` also has to be
turned on.

### 3.2 Zero overlap between pixel formats and ustreamer

| Side | Supported formats |
|---|---|
| rtk_hdmirx driver | `NV12` `NV21` `NV16` `RGB32` `BGR32` `SGRBG8` `SGRBG10` |
| ustreamer (`src/libs/capture.c`) | `YUYV` `YVYU` `UYVY` `YUV420` `YVU420` `GREY` `RGB565` `RGB24` `BGR24` `MJPEG` `JPEG` |

**The intersection is empty**, so code has to be written. On the hardware
side `MIPI_OUT_COLOR_SPACE_T` (`mipi_wrapper.h:44`) only offers YUV422/YUV420
semi-planar and assorted 32-bit ARGB orderings — it cannot emit packed
YUYV — so ustreamer is what has to change, not the driver's output format.

### 3.3 systemd compatibility with kernel 4.9

- systemd 258 dropped cgroup v1 and raised its kernel baseline to **5.4**
- Arch Linux ARM is rolling and will inevitably reach 258+, so **it cannot
  be used on a 4.9 kernel**
- kvmd 4.213's PKGBUILD requires `python>=3.14`

---

## 4. What the BSP already provides (nothing to do)

Confirmed from `arch/arm64/configs/rtd129x_bpi_defconfig`:

```
CONFIG_ION=y                        # rtk_hdmirx's buffer allocation depends on it
CONFIG_ION_RTK=y
CONFIG_USB_DWC3=y
CONFIG_USB_DWC3_RTK=y
CONFIG_USB_RTK_DWC3_DRD_MODE=y      # Type-C can go device mode (but defaults back to host; see patch 0006)
CONFIG_USB_DWC3_DUAL_ROLE=y
CONFIG_USB_GADGET=y
CONFIG_USB_CONFIGFS=y
CONFIG_USB_F_MASS_STORAGE=y         # virtual media / USB stick
CONFIG_USB_CONFIGFS_MASS_STORAGE=y
CONFIG_AHCI_RTK=y
CONFIG_R8169SOC=y
CONFIG_MMC_BLOCK=y
```

Only one defconfig option needs turning on:

```
# CONFIG_USB_CONFIGFS_F_HID is not set    ← needed for keyboard/mouse emulation (kvmd hid: otg)
```

> It turned out that "only one defconfig option" is not the same as "USB
> gadget will now work". Patch 0006 is also required to pin the Type-C port
> to peripheral mode, and the RTD1296's dwc3 has only 2 usable IN endpoints,
> so at most two gadget functions can coexist. See `0006` in
> `06-changes.md`.

---

## 5. Upstream sources and versions

| Component | Source | Version / state |
|---|---|---|
| kernel | `BPI-SINOVOIP/BPI-W2-bsp`, `linux-rtk` | 4.9.119; repo's last commit 2019-10-22 (frozen) |
| u-boot | Same repo, `u-boot-rtk` | 2015.07 |
| toolchain | Bundled with the BSP | `gcc-linaro-7.3.1-2018.05-x86_64_aarch64-linux-gnu` |
| Android reference | `BPI-SINOVOIP/BPI-1296-Android7` | Last pushed 2020-07-27 |
| ustreamer | `pikvm/ustreamer` | Latest |
| kvmd | `pikvm/kvmd` | 4.213 |

Defaults produced by the BSP's `configure`:
`UBOOT_CONFIG=rtd1296_sd_bananapi_defconfig`,
`KERNEL_CONFIG=rtd129x_bpi_defconfig`, `ARCH=arm64`.
Note that the `BOARD=BPI-W2-720P` baked into `build.sh` is just the BSP's own
board-variant name and has nothing to do with HDMI IN resolution.
