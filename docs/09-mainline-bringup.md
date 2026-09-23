# Bringing the BPI-W2 up on a mainline LTS kernel

`07-mainline.md` asked what blocks a move to mainline. `08-kernel-uplift.md`
picked a version and wrote the milestones. **This document is the log of
actually doing it**, on the `kernel-6.18` branch.

Target: **Linux 6.18 LTS**, pinned at `v6.18.52`.

> **This branch does not produce a working PiKVM.** `main` remains the
> deliverable. Until M5 (HDMI capture) lands here, this line has less
> function than the BSP 4.9 one, by design -- see §7 of `08-kernel-uplift.md`.

---

## 1. How this branch is laid out

The BSP kernel does not go away. u-boot, the audio firmware blob and the
vendor initramfs still come from `vendor/bpi-w2-bsp`, so the two kernel trees
sit side by side and the build picks one.

| Path | What it is |
|------|------------|
| `vendor/linux-mainline` | The upstream tree, shallow-cloned at `v6.18.52`. Never edited in place |
| `patches/linux-mainline/` | Changes to files that already exist upstream: the dts Makefile line, and the board compatible in the bindings |
| `kernel/mainline/rtd1296-bananapi-w2.dts` | Our board DTS |
| `kernel/mainline/bpiw2.config` | Kconfig fragment merged onto `arm64 defconfig` |
| `docker/builder-mainline.Dockerfile` | Trixie + `gcc-aarch64-linux-gnu` 14.2 |
| `scripts/build-kernel-mainline.sh` | Copies the DTS in, configures, builds |

```sh
make builder-mainline     # trixie compile container
make sources-mainline     # fetch the kernel (~2 GB) and apply patches/linux-mainline
make kernel-mainline      # Image + dtbs + modules
make image-mainline       # SD image with that kernel instead of the BSP one

# narrower runs, useful while iterating on the DTS
CHECK_DTBS=1 TARGETS='realtek/rtd1296-bananapi-w2.dtb' scripts/build-kernel-mainline.sh
```

`make sources-mainline` is `WITH_MAINLINE=1 scripts/prepare-sources.sh`; the
plain `make sources` deliberately skips the 2 GB clone.

### Why the DTS is a file and the Makefile line is a patch

The project rule is "upstream is never edited in place; every change is a
patch". A board `.dts` is a *new* file that changes on nearly every bring-up
step, and regenerating a patch after every edit is friction with no benefit.
So the DTS is a normal file in this repo, copied into the tree by the build
script, while the one line in
`arch/arm64/boot/dts/realtek/Makefile` that builds it -- an edit to an
existing upstream file, and one that will not change again -- is a patch.

### Why a second build container

`docker/builder.Dockerfile` is pinned to bullseye for reasons that are
specific to kernel 4.9: OpenSSL 1.1, make 4.3, python2. It also builds with
the BSP's bundled gcc-linaro 7.3.1. None of that suits a 6.x kernel, and
none of it can be changed without breaking `main`. The mainline container is
trixie with the distro's own cross toolchain:

| | BSP line | Mainline line |
|---|---|---|
| Base | debian:bullseye-slim | debian:trixie-slim |
| Toolchain | gcc-linaro 7.3.1 (from the BSP tree) | gcc-aarch64-linux-gnu 14.2 (distro) |
| Image tag | `bpiw2-pikvm/builder:bullseye` | `bpiw2-pikvm/builder-mainline:trixie` |

u-boot is still built by the bullseye container.

### Why `arm64 defconfig` plus a fragment

`arm64 defconfig` already sets `CONFIG_ARCH_REALTEK=y`, `SERIAL_8250_DW=y`
and `BLK_DEV_INITRD=y` -- everything M0 needs. It is also what every other
arm64 port is developed against, so a bug that only shows up here is a real
bug and not a config accident. The fragment restates the handful of symbols
this board depends on, so an upstream defconfig change cannot silently
remove them.

The cost is size: see §3.

---

## 2. The boot chain is unchanged

This is worth writing down because it is what makes swapping kernels cheap.
The BSP u-boot 2015.07 is kept exactly as it is. Its `CONFIG_BOOTCOMMAND`
runs `set_sdbootargs && gosd`, and `gosd` is `boot_from_sd()` in
`u-boot-rtk/common/cmd_boot.c`, which loads four fixed paths off the FAT
partition and then calls `booti`:

| File on p1 | Load address | u-boot macro |
|------------|--------------|--------------|
| `bananapi/bpi-w2/linux/bpi-w2.dtb` | `0x02100000` | `CONFIG_BOOT_FROM_SD_DTB` |
| `bananapi/bpi-w2/linux/root.sd.cpio.gz_pad.img` | `0x02200000` | `CONFIG_BOOT_FROM_SD_ROOTFS` |
| `bananapi/bpi-w2/linux/uImage` | `0x03000000` | `CONFIG_BOOT_FROM_SD_VMLINUX` |
| `bananapi/bpi-w2/linux/bluecore.audio` | `0x0f900000` | `CONFIG_BOOT_FROM_SD_AUDIO_CORE` |

(`uImage` is a misnomer: it is a raw arm64 `Image`, and `booti` is what
consumes it.)

Two consequences:

1. **Swapping kernels means replacing two files on p1**, `uImage` and
   `bpi-w2.dtb`. That is all `KERNEL_FLAVOUR=mainline` does in
   `scripts/build-image.sh`.
2. **The board DTS must not set `bootargs` or `linux,initrd-start/end`.**
   `booti` goes through `image_setup_libfdt()`, which calls `fdt_chosen()`
   -- overwriting `bootargs` from the u-boot environment -- and
   `fdt_initrd()`. The BSP DTS hardcodes both and they are simply ignored.
   The command line the kernel actually receives is:

   ```
   earlycon=uart8250,mmio32,0x98007800 fbcon=map:0 console=ttyS0,115200 \
   loglevel=7 board=bpi-w2 rootwait root=/dev/mmcblk0p2 rw
   ```

All four files must be present or `boot_from_sd()` bails out, so the mainline
image still ships the vendor `bluecore.audio` and initramfs even though
nothing on this kernel line uses them.

---

## 3. The board DTS

`kernel/mainline/rtd1296-bananapi-w2.dts` is 66 lines against the BSP's ~400,
because mainline already carries `rtd1296.dtsi` (Andreas Färber, 2017-2019)
with the CPUs, GIC, timer, the CRT/ISO/SB2/MISC syscons, the reset
controllers and `uart0`. The board file adds only what is board-specific:

- `compatible = "bananapi,bpi-w2", "realtek,rtd1296"`
- 2 GiB of memory, starting after the boot ROM at `0x1f000` -- the same
  layout upstream's `rtd1296-ds418.dts` uses
- `serial0 = &uart0` plus `stdout-path`, and `uart0` set to `okay`
- two `reserved-memory` entries that mainline does not have but this board's
  bootloader needs: the audio firmware at `0x0f900000` (u-boot's
  `CONFIG_FW_LOADADDR`) and the ACPU instruction memory at `0x10000000`
  (`ACPU_IDMEM_PHYS`/`_SIZE` in the BSP's `include/soc/realtek/memory.h`).
  Nothing here uses the audio CPU, but letting Linux allocate over a core
  that may already be running is not worth the debugging.

Everything else in the BSP board file is vendor bindings with no driver in
mainline, and comes back node by node as the milestones land.

---

## 4. Milestone status

Milestones are the ones defined in §6 of `08-kernel-uplift.md`.

| Stage | What it needs | Status |
|-------|---------------|--------|
| **M0** | BSP u-boot + clean 6.18.x + board DTS; serial console, single core | **done**, booted 2026-09-21 -- see §5 |
| **M1** | `smp_spin_table.c` hunk, `irq-rtd129x.c`; the rbus `reg` turned out not to need dropping | **done**, booted 2026-09-22 -- see §6 |
| **M2** | `NET_VENDOR_REALTEK` Kconfig unlock + `r8169soc.c` | not started |
| **M3** | USB DT + the two probe quirks, Type-C as peripheral | **host half done**, root on USB 2026-09-22 -- see §7. Type-C/gadget not started |
| **M4** | kvmd + ustreamer on 6.18 | not started |
| **M5** | hdmirx port | not started |
| **M6** | mmc host driver | **stage 1**, driver probes; the card is not identified yet -- see §8 |

### Traps carried over from §3 of `08-kernel-uplift.md`

Both were predictions from someone else's port. M0 settled the first one:

- ~~**`rbus: bus@98000000` carries `reg` in mainline's `rtd129x.dtsi`.**~~
  **Did not happen.** The claim was that the node claims the whole 2 MiB
  window and makes every child's own MMIO request return `-EBUSY`. On this
  board `uart0` maps and works with the `reg` left exactly as upstream has
  it:

  ```
  98007800.serial: ttyS0 at MMIO 0x98007800 (irq = 0, base_baud = 1687500) is a 16550A
  printk: legacy console [ttyS0] enabled
  ```

  M1 no longer needs to drop that property. Whatever the original porter hit,
  it was not this.
- **SMP.** `rtd1296.dtsi` declares four CPUs with **no `enable-method` at
  all**, so mainline brings up CPU0 only. That matches M0's "single core"
  acceptance; M1 is where `spin-table` plus the release-address hunk goes.

---

## 5. M0

### What is built

```
Linux version 6.18.52-bpiw2 (bpiw2-pikvm@builder)
  (aarch64-linux-gnu-gcc (Debian 14.2.0-19) 14.2.0, GNU ld 2.44) # SMP PREEMPT
```

| | BSP 4.9 | 6.18.52 |
|---|---|---|
| `Image` | 22.2 MB | 41.1 MB |
| board dtb | 40.6 KB | 4.2 KB |
| build time (32 threads, `Image dtbs modules`) | -- | 3m40s |

The dtb is a tenth of the size because almost none of the vendor nodes are
there yet.

`CONFIG_LOCALVERSION="-bpiw2"` is worth one note. `CONFIG_LOCALVERSION_AUTO=n`
on its own is **not** enough to stop `-dirty` being appended: with
`LOCALVERSION` unset in the environment, `scripts/setlocalversion` still runs
`scm_version --short`, which appends `-dirty` for a dirty tree -- and this
tree is always dirty, because the build script copies the DTS in and the dts
Makefile is patched. Exporting `LOCALVERSION=""` (set, but empty) is what
suppresses it. Both are needed, and the version string matters because
modules install under `/lib/modules/<version>`.

### Image size: the boot partition had to grow

41 MB of `Image` plus the vendor `bluecore.audio` and two copies of the
vendor initramfs does not fit in the 60 MiB boot partition the BSP image
uses. `scripts/build-image.sh` now sizes p1 from what was actually staged,
rounded up to a multiple of 32 MiB, with 60 MiB as the floor -- so the BSP
image keeps exactly the layout that was verified on hardware, and the
mainline image gets a bigger p1 and a later p2 start.

### Validated against the devicetree bindings

`CHECK_DTBS=1 make kernel-mainline` runs the dtb through `dt-validate`. The
board dtb comes out clean. Two things came of turning it on:

- **`bananapi,bpi-w2` was not a legal compatible.**
  `Documentation/devicetree/bindings/arm/realtek.yaml` lists the RTD1296
  boards and only had the Synology DS418.
  `patches/linux-mainline/0002-*` adds ours, following the `bananapi,bpi-m4`
  entry already in the RTD1395 list. This is the kind of patch that goes
  upstream as-is.
- **Five remaining warnings are upstream's, not ours.** The `syscon@*` nodes
  in `rtd129x.dtsi` are `compatible = "syscon", "simple-mfd"`, and
  `syscon-common.yaml` wants a device-specific string in front. Running the
  same check on the untouched `rtd1296-ds418.dtb` produces the identical five
  lines, which is how we know they are inherited and not something this board
  file introduced.

`CHECK_DTBS` is off by default, and the build script deliberately passes the
variable only when it is on: the kernel tests it with
`ifneq ($(CHECK_DTBS),)`, so `CHECK_DTBS=0` would turn checking **on**.

### The one thing that stopped it booting: `text_offset`

The first boot attempt produced **no kernel output at all**, and the reason
was neither the kernel nor the DTS.

Both of this board's bootloaders read the load address straight out of the
arm64 Image header and ignore bit 3 of `flags`, the "this kernel may be
placed at any 2 MiB aligned address" bit. The BSP u-boot is explicit about
it, in `booti_setup()`:

```c
/* common/cmd_bootm.c:715 */
dst = gd->bd->bi_dram[0].start + le32_to_cpu(ih->text_offset);
if (images->ep != dst)
        memmove((void *)dst, src, le32_to_cpu(Image_Size));
```

`CONFIG_SYS_SDRAM_BASE` is `0x0` for this board
(`include/configs/rtd1296_qa_sd_bananapi.h:112`), so the load address *is*
`text_offset`. LK does the same thing.

Mainline has hardcoded `text_offset` to 0 since 5.8. The BSP 4.9 kernel still
carried `0x280000`, which is why it boots:

| | `text_offset` | `image_size` |
|---|---|---|
| BSP 4.9 | `0x0000000000280000` | `0x015bf000` |
| mainline 6.18 | `0x0000000000000000` | `0x027f0000` |

So the bootloader dutifully put the kernel at physical `0x00000000` -- the
boot ROM, not RAM -- and it died without a character. LK says so out loud,
and the size it prints is exactly the mainline header's:

```
Boot image target addr:0x00000000, size:0x027f0000
```

The kernel never reads this field itself; it is position independent, and
the field is only a hint to the bootloader. So `scripts/build-image.sh`
patches the eight bytes while staging the boot files, and nothing in
`vendor/linux-mainline` is touched:

```
0x08000000..0x0a7f0000   where the kernel lands (2 MiB aligned)
0x03000000..0x0572fa00   where the bootloader read the Image to
0x02100000, 0x02200000   dtb, initrd
0x0f900000               bluecore.audio / the acpu_fw reserved-memory region
```

Override with `KERNEL_TEXT_OFFSET=` if any of those move.

### Verified on hardware, 2026-09-21

Booted from a USB card reader through LK (see "How to test it" below).

```
Boot image target addr:0x08000000, size:0x027f0000
[    0.000000] Booting Linux on physical CPU 0x0000000000 [0x410fd034]
[    0.000000] Linux version 6.18.52-bpiw2 (bpiw2-pikvm@builder) (aarch64-linux-gnu-gcc (Debian 14.2.0-19) 14.2.0, ...)
[    0.000000] Machine model: Banana Pi BPI-W2
[    0.000000] OF: reserved mem: 0x000000000f900000..0x000000000fcfffff (4096 KiB) nomap non-reusable audio-firmware@f900000
[    0.000000] OF: reserved mem: 0x0000000010000000..0x0000000010013fff (80 KiB) nomap non-reusable acpu-idmem@10000000
[    0.000000] NUMA: Faking a node at [mem 0x000000000001f000-0x000000007fffffff]
[    0.000000] /cpus/cpu@1: missing enable-method property
[    0.204514] 98007800.serial: ttyS0 at MMIO 0x98007800 (irq = 0, base_baud = 1687500) is a 16550A
[    0.204705] printk: legacy console [ttyS0] enabled
[    0.005068] smp: Brought up 1 node, 1 CPU
[    1.526613] VFS: Cannot open root device "" or unknown-block(0,0): error -6
[    1.558508] Kernel panic - not syncing: VFS: Unable to mount root fs on unknown-block(0,0)
[    1.575127] Hardware name: Banana Pi BPI-W2 (DT)
```

1.6 seconds from `Booting Linux` to the panic, with no abort and no oops on
the way. The panic **is** the M0 result: mainline has no mmc host driver for
this SoC, so there is no root device to find.

What this confirms, item by item:

| Written in the board DTS | What the kernel did with it |
|---|---|
| `memory@1f000`, `reg = <0x1f000 0x7ffe1000>` | `NUMA: Faking a node at [mem 0x1f000-0x7fffffff]` |
| `acpu_fw: audio-firmware@f900000` | reserved, `nomap` |
| `acpu_idmem: acpu-idmem@10000000` | reserved, `nomap` |
| `compatible = "bananapi,bpi-w2"` | `Machine model: Banana Pi BPI-W2` |
| `&uart0 { status = "okay"; }` | `ttyS0 at MMIO 0x98007800` |
| `stdout-path = "serial0:115200n8"` | console came up **without** a command line |

That last row matters more than it looks. LK overwrites `/chosen/bootargs`
with its own value after we set ours, so the kernel received an empty command
line:

```
[    0.000000] Kernel command line: 
```

Everything above was printed anyway, because `stdout-path` in the board DTS
is enough on its own. The console does not depend on the bootloader
cooperating.

### What M0 leaves open

Both are expected, and both are M1:

- **Single core.** `rtd1296.dtsi` declares four CPUs with no `enable-method`,
  so `smp: Brought up 1 node, 1 CPU`. Worth noting for when M1 starts: LK
  patches the DT on its way past, and announces it --
  `[FDT] add boot-secondary-addr to /cpus/cpu@1` -- so the release address it
  writes is worth reading before inventing one.
- **The UART has no interrupt**, and runs polled:

  ```
  dw-apb-uart 98007800.serial: error -ENXIO: IRQ index 0 not found
  ```

  Not a mux failure -- there is nothing to fail yet. `uart0` in
  `rtd129x.dtsi` has no `interrupts` property at all, and the whole file
  carries exactly two: the GIC's own maintenance PPI and the arch timer.
  Mainline's RTD129x has no interrupt routing below the GIC. Supplying it is
  the `irq-rtd129x.c` half of M1.

  (The `Fixed dependency cycle(s) with /soc@0/interrupt-controller@ff011000`
  line in the same log is **not** related. `ff011000` is the GIC-400 itself,
  and the cycle is it referencing its own maintenance interrupt -- a routine
  fw_devlink message.)

### How to test it

```sh
make kernel-mainline
make image-mainline          # -> build/bpiw2-pikvm-mainline.img
```

**On this particular board, do not expect the SD slot to boot it.** The slot
is faulty (see `README.md` and §11 of `06-changes.md`); across six power-ons
with a byte-verified card it never once reached u-boot, falling through to
the eMMC Android firmware every time. The SPI ROM bootcode is clear about
what it does find:

```
C3h
SD card is not detected !!          <- slot empty
BPI: try get_builtin_hwsetting !!
hwsetting size: 00000000            <- with a card in the slot: 00000740
BPI: try bootcode_from_emmc !!
```

The hwsetting blob comes off the SD card when there is one, which is why
pulling the card mid-diagnosis hangs the bootcode at `C3h` rather than
falling straight through.

So boot it through LK instead. Put the card in a **USB card reader** -- the
image needs no changes, LK `fatload`s the same files off p1 -- plug that into
the board, set **SW4 = 0** so the board lands at the `Realtek>` prompt, and:

```sh
scripts/lk-boot-usb.sh "" 150
```

SW4 = 0 is the reliable way to reach `Realtek>`: it boots the eMMC Android
firmware, whose `FW Image sha FAILED` drops it into LK's console loop.

To put the image on a card the board can actually boot from, a different
BPI-W2 would be needed; nothing about the image is at fault.

---

## 6. M1

Four cores and an interrupt-driven UART. Two independent pieces, built and
tested together because each test cycle needs a human to power-cycle the
board.

### The interrupt mux

`rtd129x.dtsi` carries exactly two `interrupts` properties -- the GIC's own
maintenance PPI and the arch timer. Everything below the GIC is
interruptless, which is why M0 saw:

```
dw-apb-uart 98007800.serial: error -ENXIO: IRQ index 0 not found
```

The SoC folds peripheral interrupts onto two GIC SPIs, one for the MISC
register block and one for ISO. Each has a status register and an enable
register, and the two are **not bit-aligned**:

| Source | status bit | enable bit |
|---|---|---|
| MISC UART1 | 3 | 3 |
| MISC UART2 | 8 | **7** |
| MISC UART2 timeout | 13 | **6** |
| MISC I2C3 | 23 | **28** |
| ISO UART0 | 2 | 2 |

So the mapping has to live in the driver, and a device's `interrupts`
property carries only the status bit.

`kernel/mainline/irq-rtd129x.c` is a rewrite rather than a copy of the BSP's
`drivers/irqchip/irq-rtd129x.c`. The BSP's `irq_chip` is wired the wrong way
round: its `.irq_mask` writes the *status* register, which acknowledges
rather than masks, and only `.irq_disable` touches the enable bits. Here
`.irq_mask`/`.irq_unmask` gate the source and `.irq_ack` does the
write-one-to-clear, which is what genirq expects of a level chip. The chained
handler skips status bits whose enable bit is clear, so a stale bit from a
masked source is not dispatched.

Two other differences from the BSP:

- **One node per mux**, not one node describing both. Mainline's
  `realtek,rtd-gpio.yaml` -- already merged -- has an example that references
  `<&iso_irq_mux>` with `#interrupt-cells = <1>`, a controller that exists
  nowhere in the tree. This driver supplies that dangling reference, with the
  cell count the merged binding already assumes.
- **The node claims only the two registers it uses**, `reg = <0x0 0x4>, <0x40
  0x4>`, rather than 0x100 of its syscon. That keeps it clear of
  `iso_reset@88` and the UARTs.

### Four cores

`rtd1296.dtsi` declares four Cortex-A53s with no `enable-method`, so mainline
brought up CPU0 and said so about the rest. The BSP uses
`enable-method = "rtk-spin-table"` with `cpu-release-addr = <0x0 0x9801aa44>`
-- and that address is a **register in the SB2 block, not RAM**, which is the
whole reason it needed its own method. Realtek's
`drivers/soc/realtek/rtd129x/rtd129x_spin_table.c` differs from mainline's
`smp_spin_table.c` in exactly two ways: `ioremap` instead of `ioremap_cache`,
and a 32-bit `writel_relaxed` instead of a 64-bit `writeq_relaxed`.

So rather than carrying a second cpu_ops implementation, patch 0005 teaches
mainline's spin-table to recognise the case, keyed on
`memblock_is_map_memory()`: a release address that is not mapped RAM is
mapped as device memory and written 32 bits wide. Platforms whose release
address really is RAM take the existing path untouched, and the DT uses the
standard `"spin-table"`.

### Verified on hardware, 2026-09-22

```
[    0.005404] smp: Bringing up secondary CPUs ...
[    0.006138] CPU1: Booted secondary processor 0x0000000001 [0x410fd034]
[    0.006996] CPU2: Booted secondary processor 0x0000000002 [0x410fd034]
[    0.007794] CPU3: Booted secondary processor 0x0000000003 [0x410fd034]
[    0.007938] smp: Brought up 1 node, 4 CPUs
[    0.008048] SMP: Total of 4 processors activated.
[    0.211633] 98007800.serial: ttyS0 at MMIO 0x98007800 (irq = 16, base_baud = 1687500) is a 16550A
[    0.211822] printk: legacy console [ttyS0] enabled
...
[    1.650773] SMP: stopping secondary CPUs
[    1.667445] ---[ end Kernel panic - not syncing: VFS: Unable to mount root fs on unknown-block(0,0) ]---
```

`IRQ index 0 not found` and `missing enable-method` are both gone, `irq = 16`
is a real mapping out of the ISO mux, and no `nobody cared`, no spurious
interrupt, no warning anywhere in the boot. `SMP: stopping secondary CPUs` on
the way into the panic is the confirmation that the other three were still
running at that point.

The panic is unchanged from M0 and still expected: there is no mmc host
driver, so there is no root device. M3 is what gives this kernel a root, on
USB.

`dtbs_check` on the board dtb reports the same five `syscon ... is too short`
warnings as at M0, all from upstream `rtd129x.dtsi`. The mux nodes, the
binding and the cpu nodes add none.

---

## 7. M3, the host half

Root on USB, and with it the first real userspace on this kernel line.

### Mainline already had the drivers

Nothing had to be written. 6.18 carries `dwc3-rtk.c`, `phy-rtk-usb2.c`,
`phy-rtk-usb3.c` and `extcon-rtk-type-c.c`, and every one of them lists an
`rtd1295` compatible. What it does not carry is a single **device tree** node
using them: no `.dtsi` under `arch/arm64/boot/dts/realtek/` mentions dwc3 at
all. So M3 is DT work.

`arm64 defconfig` already sets `USB_DWC3`, `USB_DWC3_RTK`, xhci, usb-storage,
SCSI and ext4. Only the two phys had to be turned on, and they are what dwc3
waits for:

```
CONFIG_PHY_RTK_RTD_USB2PHY=y
CONFIG_PHY_RTK_RTD_USB3PHY=y
```

Ports 1 (USB 2.0 host) and 3 (USB 3.0 host) are described; port 0 is the
Type-C/DRD port and port 2 the EHCI/OHCI pair, neither of which is here yet.
The addresses come from the BSP's `rtd-129x-usb.dtsi`, with two corrections:

- **The usb2phy `reg` order is reversed from the BSP's.** Mainline's binding
  is `<PHY data>, <PHY control>` and `phy-rtk-usb2.c` maps index 0 to
  `reg_wrap_vstatus` and index 1 to `reg_gusb2phyacc0`. The BSP lists them
  the other way round.
- **The dwc3 wrapper length is 0x140, not the BSP's 0x200.**

### The -EBUSY that was my own fault

With `reg = <0x13c00 0x200>, <0x13d60 0x4>` the first entry runs to 0x13e00
and swallows the second, so the driver's own second request collides with its
first:

```
rtk-dwc3 98013c00.usb: error -EBUSY: can't request region for resource [mem 0x98013d60-0x98013d63]
rtk-dwc3 98013c00.usb: probe with driver rtk-dwc3 failed with error -16
```

The upstream binding example uses 0x140 for exactly this reason.

This was first misdiagnosed as the rbus `reg` trap from §3 of
`08-kernel-uplift.md`, and a patch was written to drop that property. It was
wrong: `simple-bus` has no driver that requests its region, so the `reg`
there cannot cause `-EBUSY`. The patch was dropped again rather than carried
as an unjustified deviation from upstream, and a test with the rbus `reg` in
place and the length corrected confirmed it is not needed. **The prediction
in `08` §3 that this property breaks child MMIO requests is wrong.**

### LK hands the kernel an empty command line

Setting `/chosen/bootargs` from the LK console does not survive -- LK
overwrites it on the way past, which is why M0 saw `Kernel command line:`
with nothing after it. `root=` never arrived, so `rootwait` never waited and
the kernel gave up before the USB device had finished enumerating.

The command line is therefore compiled in with `CONFIG_CMDLINE_FORCE`. That
also made the SD path work (see below), because it no longer matters what a
bootloader does or does not pass.

### Where the kernel is allowed to live

`text_offset` decides more than whether the kernel boots: it decides which
physical memory the kernel *occupies*, and on this SoC a lot of memory
belongs to the Realtek firmware, which keeps running alongside Linux. The
BSP reserves it with `/memreserve/`; mainline reserves none of it.

`0x08000000`, used through the first half of M3, sits in the middle of ION
media heap 1. The firmware's video path -- `VO_SetVideoStandard`, HDMI
infoframes, all of it visible in the boot log -- writes there. The kernel now
loads at `0x1c000000`, above every firmware region, and the board DTS
reserves the heaps:

| Region | Range |
|---|---|
| ION audio heap | `0x02600000..0x03200000` |
| ION media heap 1 | `0x03200000..0x0ea00000` |
| bluecore.audio / acpu_fw | `0x0f900000..0x0fd00000` |
| TEE (from `rtd129x.dtsi`) | `0x10100000..0x11000000` |
| ION media heap 2 | `0x11000000..0x1a200000` |
| **kernel** | `0x1c000000..0x1e7f0000` |

### Verified on hardware, 2026-09-22

```
xhci-hcd xhci-hcd.0.auto: irq 17, io mem 0x98029000
xhci-hcd xhci-hcd.1.auto: irq 17, io mem 0x981f0000
xhci-hcd xhci-hcd.1.auto: Host supports USB 3.0 SuperSpeed
usb 3-1: new SuperSpeed USB device number 2 using xhci-hcd
usb-storage 3-1:1.0: USB Mass Storage device detected
sd 0:0:0:0: [sda] 61120512 512-byte logical blocks: (31.3 GB/29.1 GiB)
 sda: sda1 sda2
EXT4-fs (sda2): mounted filesystem ... r/w with ordered data mode.
VFS: Mounted root (ext4 filesystem) on device 8:2.
systemd[1]: systemd 257.13-1~deb13u1 running in system mode
Welcome to Debian GNU/Linux 13 (trixie)!
...
[  OK  ] Reached target getty.target - Login Prompts.
[  OK  ] Started ssh.service - OpenBSD Secure Shell server.
[FAILED] Failed to start kvmd-otg.service - PiKVM - OTG setup.
[  OK  ] Started kvmd.service - PiKVM - The main daemon.

Debian GNU/Linux 13 bpi-w2-pikvm ttyS0
bpi-w2-pikvm login:
```

19 targets, a login prompt, and `kvmd.service` running -- most of M4 comes
free, because the rootfs userspace was already built and waiting. `kvmd-otg`
is the Type-C gadget, which is the half of M3 still to do.

The `irq 17` on both controllers comes out of the ISO mux built in M1.

Logged in over the serial console, the running system confirms M1 and M3
together:

```
# uname -a
Linux bpi-w2-pikvm 6.18.52-bpiw2 #15 SMP PREEMPT aarch64 GNU/Linux
# nproc
4
# cat /proc/interrupts
           CPU0       CPU1       CPU2       CPU3
 13:      10561      21341      15574      13855    GICv2  30 Level     arch_timer
 16:       1061          0          0          1 rtd129x-irq-mux   2 Edge      ttyS0
 17:      10019          0          0          0    GICv2  53 Level     xhci-hcd:usb1, xhci-hcd:usb2
# free -m
               total        used        free      shared  buff/cache   available
Mem:            1591         201        1338           0         119        1389
# lsblk
sda    29.1G disk
|-sda1   96M part /boot
`-sda2   29G part /
# systemctl --failed
kvmd-otg.service loaded failed failed PiKVM - OTG setup
```

`rtd129x-irq-mux 2 Edge ttyS0` with a thousand interrupts on it is the M1
driver doing real work, on the ISO status bit the DTS names. `GICv2 53` for
xhci is `GIC_SPI 21` translated. `total 1591` MiB is 2 GiB less the 361 MiB
of firmware reservations, as designed. One failed unit, and it is the Type-C
gadget.

The login banner still advertises the BSP kernel and `hdmirx-*` helpers --
it is baked into the rootfs and has not caught up with this branch.

### Resolved: the corruption belongs to the eMMC/LK boot path

For a while every boot died a few seconds into userspace -- in SLUB, in the
page allocator, in `xas_find`, somewhere different each time. It is not the
kernel. **The same kernel with the same DTB is clean when booted by the BSP
u-boot from the SD slot, and fails every time when booted by LK from eMMC.**

The decisive runs used a diagnostic kernel with a built-in initramfs
(`kernel/mainline/diag/`; `scripts/build-diag-initramfs.sh`, then
`EXTRA_CONFIG=kernel/mainline/diag/initramfs.config make kernel-mainline`), so userspace ran with no
storage, USB, SD host or other DMA source at all. Its `/init` runs
`aliastest`, which writes every 8-byte word of a growing buffer with its own
physical address (from `/proc/self/pagemap`) and reads it all back -- unlike
the kernel's `memtest=`, which writes one fixed pattern everywhere and so
cannot see two addresses that are really the same DRAM.

| Boot path | 256 MiB | 512 MiB | 1024 MiB |
|---|---|---|---|
| eMMC → FSBL → OP-TEE/BL31 → LK | clean | **kernel dies**, every time, within a millisecond of the same point | -- |
| SD → BSP u-boot | clean | clean | clean |

Then, on the u-boot path with the **normal** kernel and no `slub_debug`: two
250 MiB random buffers each copied once (1 GiB resident in tmpfs), ten rounds
of four parallel `urandom | gzip | gunzip` pipelines at 64 MiB each, then a
byte-compare of every copy. All identical, no oops, 540 s uptime. On the LK
path the same kind of load died inside 15 s.

Ruled out on the LK path, each with a boot behind it:

| Hypothesis | Test | Result |
|---|---|---|
| SMP / the M1 spin-table | `nr_cpus=1` | Same crash |
| RAM corrupted before Linux | `memtest=4` | Clean -- but a fixed pattern cannot see aliasing |
| DMA of any kind | no USB core, no dwc3, no SD host (initcall blacklist) | Same crash |
| The audio core | `SKIP_BOOT_A=1`, then held in reset via `SOFT_RESET2` bit 0 | Same crash, to the millisecond |
| Unreserved firmware ION heaps | all reserved, kernel moved above them | Same crash |
| ION secure heap (300 MiB) | reserved | Same crash |
| What u-boot reserves and LK does not | mirrored both regions | Same crash |
| DRAM smaller than declared | LK `bdinfo`: DDR4 2133, 16 Gb = 2 GiB | Size is right |

What differs between the two paths and was *not* isolated: the LK path runs
FSBL, Android's OP-TEE ("TEE OS v2.1") and BL31 from the eMMC boot area, and
its DRAM parameters come from the built-in hwsetting
(`hwsetting size: 00000000`) instead of the SD card's. The failures are all
reads of zero -- NULL dereferences at small offsets -- which fits either a
region the secure world treats as read-as-zero, or DRAM misbehaving under
load. Telling those apart means taking apart the Android firmware on the
eMMC, which is not the product: the deliverable boots from SD.

Consequences:

- **Development moves to the SD/u-boot path.** The LK route
  (`scripts/lk-boot-usb.sh`) stays in the tree but is not a valid way to run
  this kernel.
- **Installing to eMMC later** would hit the same problem if the eMMC keeps
  that Android boot chain; replacing it with the BSP u-boot is the thing to
  verify first when that work starts.
- `slub_debug` is gone from the command line. It never fixed anything: it
  moved the slab layout so the damage landed somewhere less fatal, and
  reported nothing.
- The board DTS keeps the two regions u-boot reserves (`0x1f000..0xfffff`,
  `0x1b00000..0x1fbdfff`) -- harmless, and correct whichever bootloader runs --
  but not the secure heap, which only mattered as a hypothesis for the LK
  path and cost 300 MiB.

Two traps met on the way that are worth knowing about:

- The BSP u-boot passes `0x31400000` as the ramdisk argument to `booti` even
  when no initrd was loaded, and then refuses to boot. Renaming the initrd
  away does not skip it; `booti 0x03000000 - 0x02100000` at the `BPI-W2>`
  prompt does.
- The vendor initramfs's busybox has no `md5sum`, `cksum` or `free`, and its
  `dd` rejects `bs=1M`. Stress scripts that hide `dd`'s stderr will run happily
  and test nothing.

### The SD slot boots after all

Unrelated to USB, but found on the way. With the card in the board's own slot
and SW4 = 1, the BSP u-boot loaded and ran the 6.18 kernel:

```
SD card is detected !!
BPI: try bootcode_from_sdcard !!
U-Boot 2015.07 (Sep 16 2026 - 01:49:38 +0000)
Loading "bananapi/bpi-w2/linux/uImage" to 0x03000000 is OK.
Starting Kernel ...
[    0.000000] Kernel command line: earlycon=... root=/dev/sda2 rootwait rw
Run /init as init process
Begin: Mounting root file system ...
```

Six earlier attempts had never got past `C3h`, so the slot is intermittent
rather than dead. It stopped at `rootwait` because the card was in the slot
and so there was no `/dev/sda2`; mainline has no mmc host driver, so SD boot
cannot yet supply its own root. That is M6, and this makes M6 considerably
more attractive than its position at the end of the list suggests: the
bootloader side already works, and finishing it would end the
reader-swapping that every test cycle currently needs.

---

## 8. M6, stage 1

### The controller is the rtsx card reader core

See the note in §6 of `08-kernel-uplift.md`. In short: the BSP's
`rtk-sdmmc-reg.h` register names are mainline's rtsx names at a constant
offset per block, so `drivers/mmc/host/rtsx_pci_sdmmc.c` is a working
reference for the SD protocol, the tuning and the bit meanings. What mainline
lacks is a platform transport -- `drivers/misc/cardreader/` has PCI and USB
and nothing else -- and the SoC-specific DMA engine, PLL and pad settings,
which come from the BSP.

`kernel/mainline/sdmmc-rtd129x.c` is the result. Stage 1 deliberately does
card detection, command submission and responses only, including the 136-bit
R2 that this core returns by DMA rather than in registers. Block data
transfer is stage 2.

### Only the core's own registers are mapped

The BSP's node maps four windows: the CRT/PLL block, the card reader core,
SB2, and the DMA engine shared with the eMMC controller. Three of those
overlap nodes `rtd129x.dtsi` already owns, and `crt: syscon@0` requests its
region, so asking for it again fails:

```
rtd129x-sdmmc 98000000.mmc: error -EBUSY: can't request region for resource [mem 0x98000000-0x980003ff]
rtd129x-sdmmc 98000000.mmc: probe with driver rtd129x-sdmmc failed with error -16
```

Stage 1 needs none of them -- the clock generator it uses, `CR_SD_CKGEN_CTL`,
is inside the core at offset 0x78 -- so the node claims one window. When a
later stage needs the PLL it should come through a syscon phandle rather than
a second mapping.

Note for §3 of `08-kernel-uplift.md`: this is the second time an `-EBUSY`
here has had nothing to do with the rbus `reg`. A `simple-bus` has no driver
to request its region; nodes with drivers, like syscon, do.

### Verified on hardware, 2026-09-23

```
rtd129x-sdmmc 98010400.mmc: RTD129x SD host, card present
```

The driver probes and the register mapping is right. `MMC_CAP_NEEDS_POLL` is
set because the card detect line is not wired to an interrupt, so a card
inserted after boot is noticed.

It has **not** been shown to talk to a card. Booted from the SD slot by
u-boot, with the card itself sitting in the slot, `mmc0` registers but no card
appears under `/sys/bus/mmc/devices/`, and the host's interrupt ran away:

```
 17:     100000          0          0          0    GICv2  76 Level     98010400.mmc
```

100000 is the kernel giving up on an interrupt line whose handler keeps
returning `IRQ_NONE`: some status bit this driver neither recognises nor
clears stays asserted. That is the next thing to fix. Stage 1 cannot be called
done until a `mmc0: new ... SD card` line appears.

The u-boot path also makes this the fast test loop for M6: the kernel boots in
seconds, finds no `/dev/sda2`, and the vendor initramfs drops to an
`(initramfs)` shell on the console, with `dmesg` and `/sys` to look at.

### What M6 is worth

Every test cycle in this document costs a human two card swaps and about ten
minutes, because the boot media is a card in a USB reader. Once the SD slot
works that becomes "power on". The bootloader side is already proven -- §7
records u-boot loading and running this kernel from the slot -- so M6 is the
only thing between here and `CLAUDE.md`'s "flash a card and go".

---

## 9. Sources

| Source | Used for |
|--------|----------|
| `vendor/linux-mainline` @ `v6.18.52` | `rtd129x.dtsi`, `rtd1296.dtsi`, `rtd1296-ds418.dts`, `arch/arm64/configs/defconfig` |
| `vendor/bpi-w2-bsp/u-boot-rtk` | `boot_from_sd()`, the load addresses, the bootargs |
| `vendor/bpi-w2-bsp/linux-rtk/include/soc/realtek/memory.h` | `ACPU_IDMEM_PHYS`/`_SIZE` |
| `08-kernel-uplift.md` | The milestone definitions and the list of things to copy |
| `vendor/bpi-w2-bsp/linux-rtk/drivers/irqchip/irq-rtd129x.[ch]` | The interrupt mux register layout and the status-bit -> enable-bit tables |
| `vendor/bpi-w2-bsp/linux-rtk/drivers/soc/realtek/rtd129x/rtd129x_spin_table.c` | How the secondary CPUs are released |
