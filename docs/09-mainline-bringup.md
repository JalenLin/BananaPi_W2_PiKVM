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
| `kernel/mainline/r8169soc.c` | The gigabit MAC driver (M2), copied in at build time |
| `scripts/push-kernel-mainline.sh` | Installs a new kernel on a running board over ssh |
| `docker/builder-mainline.Dockerfile` | Trixie + `gcc-aarch64-linux-gnu` 14.2 |
| `scripts/build-kernel-mainline.sh` | Copies the DTS in, configures, builds |
| `scripts/build-rootfs-arch.sh`, `scripts/rootfs-arch.sh` | The Arch Linux ARM + PiKVM rootfs this kernel line uses (§12) |

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
| **M2** | `NET_VENDOR_REALTEK` Kconfig unlock + `r8169soc.c` | **done** 2026-10-01: gigabit, DHCP, ssh, kernels installed over the network, reboot via the watchdog -- see §9 |
| **M3** | USB DT + the two probe quirks, Type-C as peripheral | **done**: host ports 2026-09-22 (and on an SD boot 2026-10-01); Type-C gadget enumerated by the target and kvmd-otg up 2026-10-01 -- see §7 |
| **M4** | kvmd + ustreamer on 6.18 | **done** 2026-10-01: kvmd starts ustreamer on demand, keyboard input reaches the target and shows up in the capture -- see §10 |
| **M5** | hdmirx port | **done** 2026-10-01: 1080p60 captured, 60 fps from the driver, ~22 fps of JPEG to a client (the BSP's figure) -- see §10 |
| **M6** | mmc host driver | **done** 2026-09-23: one card in the slot boots to a login with root on `mmcblk0p2`; 512 MiB write/read-back verified -- see §8. High speed (50 MHz, ~25 MB/s read) 2026-10-01 |
| **H.264** | the `coda` driver on VE1, firmware, ustreamer's single-planar M2M, Janus | **done** 2026-10-01: 1080p H.264 from the HDMI capture, in kvmd as direct H.264 (kvmd-media) and WebRTC (kvmd-janus) -- see §11 |
| **Audio** | RPC to the audio CPU's firmware, an ALSA capture device, Janus | **works** 2026-10-01: the firmware runs its full start-up, the capture device delivers 48 kHz stereo, and WebRTC carries it as Opus; music from Kodi comes through clean -- see §13 |

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

### USB on an SD boot: the clock gate nobody opened (2026-10-01)

Everything above ran by way of LK, and LK runs `usb start` before it jumps.
Once the board booted from the SD slot through the BSP u-boot, which does
not, both host controllers failed:

```
dwc3 98029000.usb: this is not a DesignWare USB3 DRD Core
rtk-dwc3 98013c00.usb: failed to find dwc3 core
```

The registers showed why. `CLK_EN1` (0x9800000c) was `0x93fe8561`, so
`CLK_EN_USB` (bit 4) was off. Every USB line in `SOFT_RESET1`/`SOFT_RESET2`
was still asserted, so the core answered GSNPSID with garbage. Replaying
u-boot's `rtk_usb_clock_init()` by hand with `devmem` and rebinding
`rtk-dwc3` brought both xHCIs and the onboard hub up, so clock and resets
were all that was missing. VBUS was already on.

The fix is in the kernel, not the bootloader:

- `kernel/mainline/clk-rtd129x-crt.c` (patch 0010) exposes CLK_EN1/CLK_EN2
  as 64 plain gates, specifier = bit number, as the BSP numbers them.
  Mainline had no clock driver for this SoC at all. Every gate is
  `CLK_IGNORE_UNUSED`: UART, SD and ETN are left open by the bootloader and
  their drivers do not claim a clock, so `clk_disable_unused()` would
  otherwise shut them.
- Each dwc3 core node gets `clocks = <&crt_clk 4>` (`bus_early`) and the
  nine reset lines `usb start` releases. dwc3 takes those as a shared array
  and releases them itself.

Result on an SD boot: both xHCIs, `hub 1-1` (the onboard Terminus hub), and
`crt_clk_en1_4` held by the controllers. The SD host and the network were
unaffected. `CLK_EN2` is covered too because the HDMI receiver's gate
(`CLK_EN_HDMIRX`) lives there.

### The other half of M3: Type-C as a USB device (2026-10-01)

PiKVM reaches the target through a USB gadget on the Type-C port: a HID
keyboard and mouse that `kvmd-otg` builds in configfs. On this bench the
port is cabled to the HDMI source's USB-A port (a Raspberry Pi running
Kodi), which is the host.

- **DTS:** port 0 as a third `realtek,rtd1295-dwc3` wrapper (`0x13200`), its
  core at `0x20000`, both PHYs, `dr_mode = "peripheral"`, and the same USB
  clock and reset lines as the host ports. There is no role switching and no
  CC handling. The port is meant to face the target, which supplies VBUS,
  and the board is powered from the DC jack. The main branch reached the
  same arrangement on the BSP kernel, after that kernel's Type-C driver
  kept flipping the port to host (docs/06-changes.md, patch 0006).
  `dwc3-rtk` switches the USB2 PHY to device itself when `dr_mode` says so.
- **Kconfig:** arm64 defconfig builds the configfs gadget as modules and
  leaves out the HID function. `kvmd-otg` does not load `libcomposite`, so
  `/sys/kernel/config/usb_gadget` never appeared, and it died with ENOENT
  on `mkdir .../usb_gadget/kvmd`. `USB_LIBCOMPOSITE`, `USB_CONFIGFS` and
  `USB_CONFIGFS_F_HID` are now built in, as on the BSP kernel.

Result, with no manual step after boot:

```
# ls /sys/class/udc
98020000.usb
# cat /sys/class/udc/*/state /sys/class/udc/*/current_speed
configured
high-speed
# ls /sys/kernel/config/usb_gadget/kvmd/functions/ ; ls /dev/hidg*
hid.usb0  hid.usb1
/dev/hidg0  /dev/hidg1
# systemctl is-active kvmd-otg kvmd kvmd-nginx
active
active
active
```

`configured` means the Pi enumerated the gadget and selected its
configuration. `systemctl --failed` is empty for the first time on this
branch. Whether keystrokes arrive at the target is the next thing to check,
and it can be seen on the target's screen once HDMI capture (M5) works.

## 8. M6, the SD slot

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

It now talks to the card. Booted from the SD slot by u-boot, with that card
in the slot and the driver's `dev_dbg` plus the MMC core's switched on through
`/sys/kernel/debug/dynamic_debug/control`:

```
mmc0: starting CMD0 arg 00000000 flags 000000c0
mmc0: req done (CMD0): 0
mmc0: starting CMD8 arg 000001aa flags 000002f5
mmc0: req done (CMD8): 0: 000001aa                           <- SD 2.0
mmc0: starting CMD41 arg 48300000 flags 000000e1
mmc0: req done (CMD41): 0: c0ff8000                          <- ready, SDHC
mmc0: starting CMD2 arg 00000000 flags 00000007
mmc0: req done (CMD2): 0: 744a6053 44000000 00000000 05011821 <- CID, by DMA
mmc0: starting CMD3 arg 00000000 flags 00000075
mmc0: req done (CMD3): 0: 00010520                           <- RCA 1
mmc0: starting CMD9 arg 00010000 flags 00000007
mmc0: req done (CMD9): 0: 400e0032 5b590000 e9277f80 0a4000cb <- CSD, by DMA
mmc0: starting CMD7 arg 00010000 flags 00000015
mmc0: req done (CMD7): 0
mmc0: starting CMD51 arg 00000000 flags 000000b5
rtd129x-sdmmc 98010400.mmc: cmd51: data transfer not implemented
```

CMD51 (read the SCR) is the first command that moves data, and stage 1 stops
there by design. The CSD is the proof that the R2 path is right end to end:
`C_SIZE` = 0xe927, so (0xe927 + 1) x 512 KiB = 29.1 GiB, which is this card.

What it took to get here, all found by reading registers from the
`(initramfs)` shell with `busybox devmem`:

- **Bit 0 of `CR_SD_ISR`/`CR_SD_ISREN` is a data bit, not a status bit.** A
  write sets every other bit in the mask to bit 0's value: 0x07 enables END
  and ERR, 0x16 clears END, ERR and DMA_DONE. The first version wrote 0x16 to
  ISREN to "enable" and so disabled everything, and acknowledged interrupts
  by writing back what it read, which re-asserts them.
- **`SD_CONFIGURE2` bit 7 stops the core generating the command's CRC7.** The
  first version used it to mean "don't check the response's CRC" (that is bit
  2), so CMD0 and every R3 command went out with no valid CRC.
- **R2 comes back by DMA with `DDR_WR` set** -- DMA *into* DDR -- and is
  complete on DMA_DONE, not on END. Fifteen bytes land in the buffer after
  the 0x3f header; the sixteenth is in `SD_CMD5`.
- **The card-detect interrupt shares the line.** u-boot leaves
  `CR_SD_INT_EN`/`CARD_INT_PEND` (0x120/0x121) at 0x04 with the card in, so
  the line is asserted the moment the handler is installed and the kernel
  gives up after 100000 unhandled interrupts. The driver polls for the card,
  so it turns this off. The BSP never touches these registers; its handler
  returns `IRQ_HANDLED` unconditionally, which hides a storm.
- **`CARD_CLOCK_EN_CTL` must have bit 2 set.** The `SD_*` registers from
  0x180 up are clocked by the SD module; with that clock off they read back
  their last value and drop every write, while the `CARD_*` registers below
  0x180 and the `CR_SD_*` ones below 0x100 keep working -- which looks like a
  mapping problem until you try writing each block. The first version wrote
  0x3b there, copied from the BSP's *card-removal* path, and so switched the
  clock off itself. Init now follows the BSP's `rtk_sdmmc_hw_reset()`.

`CONFIG_DYNAMIC_DEBUG` is now on in `bpiw2.config` for this.

### Stage 2: data, verified on hardware, 2026-09-23

```
mmc0: new SDHC card at address 0001
mmcblk0: mmc0:0001 SD 29.1 GiB
 mmcblk0: p1 p2
```

Both partitions mount. Reads are checked two ways, because the vendor
initramfs has no checksum tool: the 7.5 MB gzip stream inside `uInitrd`
passes `gzip -t` (CRC32 over all of it, after `drop_caches`), and so do 300
`.gz` files from the rootfs. Mounting the ext4 partition replayed its journal,
which exercised the write path with no errors. The MMC core runs the card at
4-bit; 16 MiB of raw reads take about 3 s.

Data follows the BSP's per-opcode table: CMD17/18 use AUTOREAD2/AUTOREAD1,
CMD24/25 AUTOWRITE2/AUTOWRITE1 (the "1" modes send CMD12 themselves, so
`mrq->stop` is never issued), and the short reads -- SCR, SD status, switch
-- use NORMALREAD with `RSP64_SEL` and a 64-byte count. Everything goes
through a 64 KiB coherent bounce buffer, so a request is one DMA.

Three things stood between stage 1 and this:

- **The interrupt is not the end of the transfer.** DMA_DONE, and END for
  plain commands, can fire while the core is still clocking. The BSP's
  `rtk_sdmmc_int_wait()` then polls `SD_TRANSFER` for END and IDLE before it
  touches the core again. Without that the next command was queued onto a
  busy core, and from then on every command timed out with no interrupt at
  all -- `SD_TRANSFER` read 0xa8, started and never finishing. Any error now
  also resets the core the way the BSP's `rtk_sdmmc_reset()` does, so one bad
  transfer cannot wedge the ones after it.
- **Data does not work behind the /256 divider.** At 400 kHz the SCR read
  completed its DMA with 64 bytes of zeros, and `SD_TRANSFER` stopped at 0x2c
  -- idle, END never set. That looks like DAT0 stuck low, but the pads are
  untouched (mainline has no RTD1295 pinctrl, so u-boot's muxing stands). The
  BSP simply never does it: it moves to 6.2 MHz as soon as CMD7 selects the
  card. The driver now does the same for any data transfer that finds the
  clock at 400 kHz, and the SCR came back as `02 b5 80 02`, the value u-boot
  prints for the same card.
- **The block layer wants at least a page per request.** With
  `max_req_size` at 512 `blk_validate_limits()` warns and `mmcblk` fails with
  -EINVAL, hence the multi-block modes and the larger buffer.

### One card boots the whole system

With `root=/dev/mmcblk0p2` in `bpiw2.config`, a single card in the slot
(SW4=1, no USB storage) goes u-boot -> kernel -> vendor initramfs -> fsck ->
systemd -> `bpi-w2-pikvm login:`, root mounted `rw` from the card. Logged in,
256 MiB of `/dev/urandom` was written to the card, synced, caches dropped and
read back with a matching md5; a second copy compared byte-identical with
`cmp`. 512 MiB written in all, no MMC or ext4 errors. Writing ran at about
5.1 MB/s and reading at 5.2 MB/s.

### High speed, 2026-10-01

Left for later at the time: the clock ceiling, and scatter-gather DMA instead
of the bounce copy. The clock is now done.

What the clock really was. The card clock is the SD PLL / 2 / 2^n in SD 2.0
mode, with n from `CR_SD_CKGEN_CTL` bits 1:0 and a further /256 from the
`SD_CONFIGURE1` divider. The PLL is `(ssc_div_n + 3) * 4.5 / 4` MHz, with
`ssc_div_n` in CRT `PLL_SD3` bits 23:16 (the BSP's own comment). u-boot
leaves the PLL wherever the card it booted from needed it. After this SDR104
card that was `0x00b64388`, i.e. 208 MHz, so "0x2103" had been running the
bus at about 13 MHz -- not the 6.2 MHz its BSP name suggests, and not a rate
anybody chose. Confirmed before changing anything: switching to 0x2102 by
devmem read the boot partition 1.76x faster, with the same md5.

The driver now:

- sets the PLL to 100 MHz (`0x00564388`, the BSP u-boot's default) at
  probe, with the BSP's sequence: the core on its 4 MHz source and the PLL
  held in reset around the write. CRT is reached through the `crt` syscon
  (`realtek,crt = <&crt>`), not a second mapping.
- derives n from the requested rate: 50 MHz is 0x2100, 25 MHz 0x2101, and
  identification uses 0x2100 plus /256 = 195 kHz. It also reports
  `actual_clock`.
- advertises `MMC_CAP_SD_HIGHSPEED` with `f_max` = 50 MHz. The BSP sets no
  sample or push point for high speed, only the clock. The UHS modes need
  1.8 V signalling and tuning, and are not done.

Without `realtek,crt` it keeps the old conservative settings.

| | before (~13 MHz) | high speed (50 MHz) |
|---|---|---|
| read | 5.1 MB/s | **24.7-25.5 MB/s** |
| write | ~5 MB/s | **14.4 MB/s** |

The card comes up as `new high speed SDHC card`, and `ios` shows
`timing spec: 2 (sd high-speed)`, `actual clock: 50000000 Hz`. 256 MiB of
random data written and read back matched by md5 and by `cmp`. The kernel
pushed onto `/boot` matched the build byte for byte. No mmc or I/O errors.

### What M6 is worth

Every test cycle in this document costs a human two card swaps and about ten
minutes, because the boot media is a card in a USB reader. Once the SD slot
works that becomes "power on". The bootloader side is already proven -- §7
records u-boot loading and running this kernel from the slot -- so M6 is the
only thing between here and `CLAUDE.md`'s "flash a card and go".

---

## 9. M2, the gigabit port

**Done, verified on hardware 2026-10-01.** The point of doing it next was the
test loop: with the network up, a new kernel goes onto the running board over
ssh (`scripts/push-kernel-mainline.sh`) and the card stays in the slot. That
now works end to end, reboot included.

### The driver: Realtek's, by way of a working 6.18 port

Mainline has no driver for the RTD129x's embedded MAC. It is an RTL8168-family
MAC and PHY, but on the SoC's internal bus rather than PCIe, so `r8169` cannot
bind. The BSP's `r8169soc.c` is Realtek's platform variant of it.

`Fireblossom/wd-mch-kernel` already carried that file to 6.18.40 and runs it
on an RTD1295 (the WD My Cloud Home) at gigabit. Their copy is the starting
point -- `kernel/mainline/r8169soc.c`, copied into the tree by
`build-kernel-mainline.sh` like the SD host -- with three changes for this
board:

- **The ETN clocks are gated by hand.** There is no RTD129x clock driver, so
  every `clk_get()` fails. Their copy turns those into NULL clocks, which
  makes `clk_prepare_enable()` a no-op; that only works because the WD's
  bootloader leaves the clocks on. Here, when the clocks are NULL, the
  driver sets `ISO_CLK_EN` (0x9800708c) bits 12:11 itself at the same three
  points in Realtek's power-up sequence. Probe prints both registers, so the
  first boot shows what u-boot left:

  ```
  r8169 98016000.ethernet: chip revision ..., ETN clocks ..., resets ...
  ```

- **The chip revision is read, not assumed.** It selects PHY calibration
  values, and their `rtk_chip.h` stub always said A00, which would apply
  the A00-only AFE fix to every chip. It now comes from SB2 0x9801a204 bits
  17:16, where the BSP's `rtk_chip.c` gets it.

- **No shared MAC address.** The driver takes the address u-boot left in the
  MAC registers, and the BSP u-boot's is `CONFIG_ETHADDR`,
  `00:10:20:30:40:50`, on every board. That value (or an invalid one) is
  replaced with a random address, which marks it `NET_ADDR_RANDOM`; udev's
  default `MACAddressPolicy=persistent` then derives one from `machine-id`.
  It is stable across boots, so DHCP keeps handing out the same lease.

`NET_VENDOR_REALTEK` depends on PCI, which this SoC does not have, so the
whole Realtek menu was unreachable. Patch 0008 adds `ARCH_REALTEK` to that
line and adds the `R8169SOC` symbol. It is built in, not a module, so ssh
still works when `/lib/modules` does not match a freshly pushed kernel.

The DT node is the BSP's, minus its clocks. The second `reg` is the ISO
block, which `rtd129x.dtsi` already has as `iso: syscon@7000`. That does not
collide the way the SD host's CRT window did, because this driver maps it with
`of_iomap()`, which does not request the region.

The board DTS also gains `ethernet0 = &gmac`. With that alias, systemd names
the interface `end0` (an onboard device found through the devicetree) rather
than `eth0`. `10-wired.network` matches on `Type=ether`, so DHCP does not care.

### The image side

- `make image-mainline` now installs the mainline kernel's modules
  (stripped) in place of the BSP's 4.9 set.
- Its boot partition is at least 256 MiB. A mainline Image is ~42 MiB, and a
  push over the network briefly holds three (`uImage`, `uImage.prev`,
  `uImage.new`); at the old size, 36 MiB was free.
- The rootfs answers mDNS (systemd-resolved, `MulticastDNS=yes`), so a newly
  flashed board is `bpi-w2-pikvm.local`. `scripts/board-ssh.sh` defaults to
  that name. It resolves it on the host, falling back to
  `scripts/mdns-resolve.py` when the host has no nss-mdns.
- ssh was already there: `openssh-server`, root/pikvm (a test image only),
  and host keys generated on first boot by `bpikvm-firstboot`. The image still
  ships no keys; `build-rootfs.sh` fails the build if one appears.

### Verified on hardware, 2026-10-01

A freshly flashed card, first boot:

```
r8169 98016000.ethernet: chip revision b01, ETN clocks 00001f01, resets 00001f80
r8169 98016000.ethernet: no usable MAC address (00:10:20:30:40:50), using a random one
r8169 98016000.ethernet eth0: RTL8169SOC, XID 10900800 IRQ 17
r8169 98016000.ethernet end0: renamed from eth0
r8169 98016000.ethernet end0: link up
bpi-w2-pikvm login:
```

- The chip is a **B01**, so the old always-A00 stub would have applied the
  wrong PHY calibration. u-boot had left the ETN clocks on (bits 12:11 of
  `0x1f01`) and both resets released (bits 10:9 of `0x1f80`), so the
  already-running branch was taken this time. The hand-gating branch is
  still there for a bootloader that does not.
- u-boot does hand over its placeholder MAC. `networkctl` shows
  `72:3b:2b:df:33:a8` from `99-default.link`, and the same address held
  across all four boots so far (three power-ons, one watchdog reboot).
- `end0`: 1000 Mb/s full duplex, DHCPv4 lease, `routable`. ssh logs in with
  host keys that `bpikvm-firstboot` generated on the board. 300 MB over ssh
  took 7.3 s (~41 MB/s, bounded by ssh itself). The only failed unit is
  `kvmd-otg` (the Type-C gadget, M3's other half).
- `push-kernel-mainline.sh --modules` on a 42 MiB kernel plus modules:
  50 s, md5 checked before the swap.
- The two `rtl_csiar_cond` lines at link-up are the harmless noise the
  wd-mch-kernel port also reports.

mDNS works on the board (`resolvectl query bpi-w2-pikvm.local` answers on
`end0`). It did not reach this bench's PC because the PC sits on a different
routed subnet, and mDNS is link-local. On a flat LAN the `.local` name works.
Elsewhere, pass `BOARD_HOST=<ip>`.

### Reboot: the watchdog had no restart handler

The first `--reboot` ended with

```
reboot: Restarting system
Reboot failed -- System halted
```

Nothing in mainline can reset the RTD129x. `rtd119x_wdt` probes (systemd even
picks it up as `/dev/watchdog0` on the way down) but has no `.restart`. Patch
0009 adds one, with the sequence from the BSP's `rtd129x_restart.c`: the
watchdog fires after 0x800000 ticks of 27 MHz, ~310 ms. Now `systemctl reboot`
goes `Restarting system` -> `U-Boot 2015.07` -> back on the network in about
50 s.

The kernel that performs a reboot is the one already running, not the one
just pushed. So the first time this fix is installed over the network, one
power cycle is still needed.

### Recovery without a human

Once kernels go over the network, whoever is testing may not be near the
board. A bad kernel should cost a reboot, not a trip to the card reader.
Three layers, each tested on 2026-10-01:

| Failure | What recovers it | Tested by |
|---------|------------------|-----------|
| Kernel panics, oopses or soft-locks | `PANIC_TIMEOUT=10`, `PANIC_ON_OOPS`, `BOOTPARAM_SOFTLOCKUP_PANIC` in `bpiw2.config`; the reset goes through patch 0009 | `echo c > /proc/sysrq-trigger`: `Kernel panic` -> `Rebooting in 10 seconds..` -> u-boot -> back on ssh |
| Userspace hangs | systemd feeds `/dev/watchdog0` (`overlay/etc/systemd/system.conf.d/10-watchdog.conf`, 30 s) | boot log: `Watchdog running with a hardware timeout of 30s` |
| The new kernel never boots | `scripts/boot-prev-kernel.sh` stops u-boot over serial and boots `uImage.prev`/`bpi-w2.dtb.prev`; `--restore` then makes them the default again | reboot, run it: `Loading ".../uImage.prev"`, and `uname -v` showed the previous build |

Patch 0009 gained a second hunk for the watchdog layer. `rtd119x_wdt`
implements ping and set_timeout but advertised neither in `options`, so
the watchdog core refused `WDIOC_KEEPALIVE`, and systemd reported
`Failed to ping hardware watchdog ... Operation not supported`.

`boot-prev-kernel.sh` relies on u-boot reading `uEnv.txt`, after which
`boot_from_sd()` loads whatever `sd_vmlinux`/`sd_boot_dtb` name. The script
replays u-boot's own `boot_normal` by hand and overrides those two after
the import.

What is left is a hang so early that neither a panic nor systemd is
running. After the first second of boot, that means a power cycle.

### How to test it

```sh
make kernel-mainline && make image-mainline
# flash build/bpiw2-pikvm-mainline.img, boot with SW4=1, cable in the RJ45
# next to the USB ports

scripts/board-ssh.sh 'ip -br addr; grep . /sys/class/net/end0/speed; dmesg | grep 98016000'

# from then on, kernels go over the network:
make kernel-mainline && scripts/push-kernel-mainline.sh --modules --reboot
```

If probe misbehaves, the serial console has the `chip revision ... ETN
clocks ... resets ...` line. The vendor's `/proc` register dumps are compiled
out (`RTL_PROC`; their `proc_ops` conversion was never done).

---

## 10. M5, HDMI capture (and M4, kvmd on top of it)

**Done, verified on hardware 2026-10-01.** The source is the bench's
Raspberry Pi 3 running Kodi (LibreELEC). Its HDMI output goes to the board's
HDMI IN, and its USB-A port goes to the board's Type-C, where it is the
target for the HID gadget.

### What was carried and what was rewritten

`kernel/mainline/hdmirx` is Realtek's BSP `rtk_hdmirx` (rtd129x). About
5,600 lines of C plus 15,000 lines of register headers. The split:

- **Kept as the BSP has it:** everything below V4L2. That is `rx_drv/` (the
  HDMI MAC/PHY state machines, EDID, measurement), the RX and MIPI wrappers
  (the MIPI wrapper is the DMA engine that writes frames to memory), and the
  order of the clock, reset and power steps.
- **Rewritten:** the V4L2 side. The BSP ran every ioctl through its own
  switch statement with its own 32-bit compat layer, created the vb2 queue
  inside REQBUFS, and reached into vb2-vmalloc's private structs for
  USERPTR. It is now `v4l2_ioctl_ops` plus the `vb2_ioctl_*` helpers, and a
  queue set up once at probe with dma-contig, MMAP and DMABUF. The main
  branch's fixes carried over as behaviour: G_FMT reports the detected
  input, S_FMT returns bytesperline/sizeimage, and the output only scales
  down.
- **New:** `QUERY_DV_TIMINGS`, with the full CEA timings from the AVI VIC
  through `v4l2_find_dv_timings_cea861_vic()`, and
  `V4L2_EVENT_SOURCE_CHANGE`. ustreamer now runs with `--dv-timings` and
  follows the source by itself, where the main branch had to hardcode
  1920x1080.
- **Replaced:**
  - ION, used only for a scratch frame the DMA writes when no buffer is
    queued, became `dma_alloc_coherent`.
  - Android's switch class became sysfs attributes next to
    `hdmirx_video_info`, plus the event above.
  - HDCP 1.4's SHA-1 now comes from lib/crypto.
  - HDCP 2.2 and the TEE path are gone.
- **remove() tears down.** The BSP's did nothing, so the module can now be
  reloaded. It is built as a module (`CONFIG_VIDEO_RTD129X_HDMIRX=m`), and a
  change to it is one `.ko` upload and an rmmod/modprobe.

### Clocks, resets, power, pins

- **Clocks.** The CRT gate driver from the USB work, extended to take any
  run of 32-bit gate registers, also covers ISO `0x9800708c` for the four
  CBUS clocks. The node asks for 7 clocks and 7 resets by name, as the BSP
  did.
- **SRAM power.** The BSP asked its power-control core for
  `pctrl_disp_hdmi_rx`/`pctrl_disp_mipi`, but those register as
  `pctrl_hdmirx_pd`/`pctrl_mipi_pd`. The lookups returned NULL, so every
  power_on/off was a no-op. CRT `SRAM_PWR2` (0x98000368) reads 0 (all
  channels on) on this board. The driver clears its channels on enable and
  never powers them off, which is what the BSP really did.
- **HPD.** ISO GPIO 22, through an inverter. Mainline's `gpio-rtd` would
  drive it, but it claims the ISO interrupt-status window (0x000-0x0e7) as a
  second resource. That window overlaps `iso_reset@88` and the gate register
  at 0x8c, so it cannot be instantiated as `rtd129x.dtsi` stands. The driver
  writes the two GPIO registers itself.
- **DDC pins.** ISO MUXPAD 0x314 bits 3:0 are set to I2C6 at probe, the fix
  for the main branch's blocker 3. Mainline has no RTD129x pinctrl either.
- **Property naming.** The property is `realtek,hpd-iso-pin`, not
  `...-gpio`. fw_devlink parses any `-gpio` property as a phandle and
  complained `could not find phandle 22`.
- **EDID.** Realtek's default table and its HDMI 2.0 variant, moved from the
  BSP's one-byte-per-cell property into DTS byte strings (`realtek,edid`,
  `realtek,edid-hdmi20`). Both block checksums were verified when converting.

### The detour: the source had stopped sending

The first loads probed cleanly: `/dev/video0` appeared, CBUS saw 5V and HPD
went high. But nothing was detected. The PHY's clock measurement
(`REGD43.p0_ck_md_count`) read 0-8, where the driver needs more than 116.
Clocks, resets, LDO and termination all read back as configured.

An A/B test settled it. The BSP 4.9 kernel went back on the card (over ssh)
and showed exactly the same `Cable Plugged` / `Set HPD(1)` / nothing. Then
the Pi alone was rebooted while the board held HPD high, and the BSP
detected 1080p60 within two seconds. Kodi on this Pi stops driving HDMI
after enough hotplug churn, and today's many board reboots were exactly
that. Back on the mainline kernel, with the Pi outputting again:

```
[HDMI RX]PLL Setting b(1407) cd(0) TMDS(148MHz) P(1) 2X(1) 6G_flag(0)
[HDMI RX]Check resolution match => Width(1920) Height(1080) VIC(16)
[HDMI RX]Polarity detect done: hor(1920) ver(1080) color(RGB) I/P(Prog)
[HDMI RX]video state 1

# v4l2-ctl --query-dv-timings
	Active width: 1920
	Active height: 1080
	Total width: 2200
	Total height: 1125
	Pixelclock: 148500000 Hz (60.00 frames per second)
```

A frame captured with `v4l2-ctl --stream-mmap` and converted from NV16 is
the Kodi home screen, with correct colours and text.

### Throughput

| | |
|---|---|
| Capture (driver, `captured_fps`) | 58-60 fps |
| JPEG to one client, 3 workers, q80 | **22.3 fps**, 49% CPU |
| JPEG to one client, 4 workers | 24.0 fps, 56% CPU |

That matches the main branch on BSP 4.9 (22-24 fps, 58% CPU). Getting there
took one fix. vb2-dma-contig's default MMAP buffers are coherent
allocations, which arm64 maps into userspace uncached, and a CPU JPEG
encoder reading 4 MB frames out of uncached memory ran at 15 fps. The
driver now asks for `V4L2_MEMORY_FLAG_NON_COHERENT` on the client's behalf
(`allow_cache_hints`, REQBUFS/CREATE_BUFS wrappers). vb2 then hands out
cached buffers and invalidates them on DQBUF, and that brought 20-22 fps.
It must not be combined with `GFP_DMA32` in `gfp_flags`: the non-coherent
allocator refuses zone flags (`dma alloc of size 4177920 failed`). The DMA
mask already keeps buffers below 4 GiB.

### M4: kvmd

kvmd needed nothing new. The udev rule that maps the device carrying
`hdmirx_video_info` to `/dev/kvmd-video` works unchanged. With
`--dv-timings` added to the streamer command in `main.yaml` (and
`--resolution` kept as the fallback for the BSP kernel), acting as a Web UI
client -- a `/api/ws?stream=1` session -- makes kvmd start ustreamer, and
`/api/streamer/snapshot` returns the screen. Then the whole loop:
`POST /api/hid/events/send_key?key=ArrowDown` goes out the Type-C gadget to
the Pi, and the next snapshot shows Kodi's menu highlight moved from
Add-ons to Pictures.

The absolute mouse works too. Walking it to the centre through
`send_mouse_move` puts Kodi's pointer in the middle of the next snapshot.
(`/api/hid` still says `mouse online: false`. That is kvmd's flag, not the
device.)

### Keyboard, mouse and virtual media at once: one HID function

The endpoint budget is the hard limit. This dwc3 has two IN endpoints
besides ep0 (`ep1in`, `ep2in` in debugfs; `GHWPARAMS3` is the same as on
4.9), and a separate keyboard and mouse hold one each. 6.18's `f_hid` has
`no_out_endpoint`, and kvmd-otg sets it, which frees the OUT endpoints, but
mass storage also needs an IN endpoint. So on the main branch the choice was
mouse or MSD.

`patches/kvmd/0002` merges the keyboard and the mouse into **one HID
function**. Its descriptor is the keyboard's with Report ID 1 followed by
the mouse's with Report ID 2. (`make_keyboard_hid()`/`make_mouse_hid()`
upstream already took a `report_id`; nothing used it.) The HID plugin
writes both through `/dev/hidg0` with the ID in front of each report, and
strips the ID from the keyboard LED reports it reads back. One IN endpoint
for HID, one IN + one OUT for MSD: kvmd's budget is `otg.endpoints: 3`.

Verified with the Pi as the target, all three at once:

- Gadget: `hid.usb0` + `mass_storage.usb0`, UDC `configured`. `/api/hid`
  reports keyboard and mouse online.
- Keyboard: ArrowLeft, ArrowUp moves Kodi's focus to the side menu and up
  to Add-ons.
- Mouse: an absolute move puts the pointer where it was sent, and Kodi
  highlights what is under it.
- MSD: a 16 MB FAT image uploaded through `/api/msd/write` and attached as
  a flash drive. LibreELEC pops up "Mounted removable storage device --
  PIKVMTEST".

**The trade-off, and why it is a setting.** A HID device with Report IDs
cannot be a boot-protocol keyboard, and much BIOS/UEFI setup firmware only
understands boot protocol. In this mode, typing in the target's firmware
menus may not work; in the OS it does. (Not testable here: the Pi has no
firmware setup screen.) The image defaults to `kvmd.hid.combined: true`
with MSD on. `main.yaml` carries the four-line override back to a separate
boot keyboard and mouse without MSD.

Getting MSD itself working on this image took three more fixes, all in
0002:

- kvmd-otg wrote `lun.0/inquiry_string_cdrom`, a Raspberry Pi kernel
  attribute that mainline `f_mass_storage` does not have. It died with
  EACCES halfway through building the gadget. The write is now optional.
- There is no MSD partition. The storage is `/var/lib/kvmd/msd.data`,
  bind-mounted read-only onto `/var/lib/kvmd/msd` by `/etc/fstab` with
  `X-kvmd.otgmsd-user=kvmd` (`build-rootfs.sh`). The remount helper did
  `mount -o remount,ro` on it, which remounts the filesystem underneath (the
  rootfs) and fails as busy. It now adds `bind` when mountinfo shows a bind
  mount.
- kvmd's MSD plugin finds the storage's mountpoint with
  `os.path.ismount()`, which cannot see a bind mount on the same
  filesystem. It therefore skipped remount-rw before an upload, which failed
  with EROFS. It now also consults `/proc/self/mountinfo`.

### The image, flashed to a fresh card (2026-10-01)

Everything above was verified by pushing kernels and files to one running
card. The deliverable was then checked as a user would get it:
`make rootfs && make image-mainline`, `build/bpiw2-pikvm-mainline.img`
written to a new card, booted with no other step.

- First boot: `bpikvm-firstboot` generated three ssh host keys (the image
  ships none), the rootfs grew to fill the card (29 GB), and DHCP gave a new
  address. The card has its own machine-id, so it has its own MAC as well.
- `systemctl --failed` is empty. kvmd, kvmd-otg, kvmd-nginx and ssh are all
  active.
- Without anything done by hand: the gadget is `hid.usb0` (combined) plus
  `mass_storage.usb0` and `configured` by the target, HDMI is 1080p60 Ready,
  and the MSD storage is mounted read-only.
- Through kvmd, with the same scripts as before: snapshot; keyboard (Kodi
  woke up and moved focus); mouse (hover highlight); and an uploaded image
  that the Pi mounted as `NEWCARD`.

The very first power-on went silent on the serial console in the middle of
u-boot's countdown. The second power-on booted normally. Linux had in fact
come up the first time too, because `firstboot-done` was already there on
the second boot. The silence was on the console path, not a hang, and is
put down to the card slot and serial capture rather than the image.

## 11. H.264: VE1, the CODA980

PiKVM sends audio only over WebRTC, and its WebRTC video is H.264, so H.264
comes first. The SoC has two Chips&Media video engines at `0x98040000`. VE1
is a **CODA980**: the product code at `+0x1044` reads `0x9800`. VE2, at
`+0x4000`, is a WAVE410 HEVC decoder and is not used. Mainline already has a
driver for the CODA9 family, `coda`, written for the i.MX6's CODA960. That
driver now runs VE1 as a V4L2 mem2mem H.264 encoder
(`patches/linux-mainline/0012`, `CONFIG_VIDEO_CODA=m`, the `vpu` node in the
DTS, `/dev/kvmd-h264` from the udev rule).

### Waking it up

With the clock, SRAM power domain, reset and isolation all set exactly as the
BSP sets them, every core register still read `0xdeadbeef`, and SB2 logged an
invalid access. The way through was bisecting on the BSP kernel itself: it
booted from an initramfs with `root=/dev/ram` and the DRD-disabled DTB, and a
test module repeated the BSP's steps one at a time. Its own `power_control`
and `clk` calls still gave `0xdeadbeef`. Only the ioctl path woke the core,
and that path ends in `ve1_wrapper_setup()`. That function turns on the
Realtek wrapper's command interface: `VE_CTRL` (`+0x3000`) bit 1, and the CTI
command depth (`+0x3004` bits 29:24 = `0x1a`, "for 1296 timing issue").
After those two writes, the core answers.

**Do not dump the SB2 block blindly.** `0x9801a000`-`0x9801a01f` and
`0x9801a620`-`0x9801a63c` are hardware semaphores, and a read acquires them.
Write 0 to release one taken by accident.

### What differs from a CODA960

The firmware is Realtek's `ve1.bin` from BPI's Android 7 tree. It holds the
BIT processor's code words as hex text. `scripts/fetch-vpu-firmware.sh`
downloads it from a pinned commit, checks its hash and converts it. The blob
is not in the repo; `make image-mainline` fetches it into the image. The
firmware reports product `0xe428`, version 3.0.1.

Each item below was found by comparing against Realtek's BSP driver or by
disassembling `libvpu.so` (32-bit ARM, with symbols) from the same Android
tree:

| Symptom | Cause | Fix |
|---|---|---|
| Encoded fine, but the SPS was High profile with a 10-bit luma depth | The header command reads CABAC, 8x8, chroma format, field and profile parameters at `0x194`-`0x1a8`, which still held SEQ_INIT's frame rate, GOP size and so on | Zero `0x180`-`0x1fc` before each command's parameters |
| Every pixel of every frame was the frame's first pixel | `FRAME_MEM_CTRL` bits 12:9 are the frame map type on the CODA980. The CODA960 BWB bit (12) selected a tiled field map | BWB is bit 15 there (`Coda9VpuEncSetup`) |
| Writing the GDI tables made it worse | The CODA980 has GDI 2.0: `0x1800`/`0x1880`/`0x1900` are x/y-to-AXI maps and a config register, not GDI 1.0's tables | Leave the GDI alone for linear frames, as libvpu does. No tiled maps |
| The second encoding session hung in SEQ_INIT (BIT busy, PC `0x100`) | `coda_hw_reset()` runs after every SEQ_END on a CODA960. It stops the GDI bus, then calls `reset_control_reset()`, which our `snps,dw-low-reset` controller does not implement. The reset returned early, leaving the bus stopped | Assert/deassert, then redo the wrapper |
| 1080p came out 1088 lines tall | The SPS crop flag is bit 2 on the CODA980, not 3 (`GetEncHeader`) | |
| | No subsampled ME frames (their registers are the slice buffer there); a 470 KiB temp buffer (`coda9_vpuconfig.h`); no JPEG engine; no IRAM | |
| ustreamer's forced keyframe failed with EINVAL | `coda_s_ctrl()` handles `FORCE_KEY_FRAME`, but the control was never created | Create it |
| Colours were wrong in places and a ghost of other picture content showed through (Kodi's "OK" button text, in its text field) | NV12's chroma is interleaved, and the CODA980 takes that from a per-picture flag, not from `FRAME_MEM_CTRL`. Without it, Cr was read from a planar offset, inside the CbCr plane | `ENC_PIC_ROT_MODE` bit 18 for NV12 (libvpu, `Coda9VpuEncode`: `cbcrInterleave << 18`) |
| Coloured edges (the focused button, a highlighted field) drifted further from the source with every P-frame: invisible with a short GOP, obvious at kvmd's default of 0 | The CODA960 frame cache settings (`SET_FRAME_CACHE_SIZE/CONFIG`) make the CODA980 read stale reference data | Leave the cache alone, as libvpu does here. 1080p still encodes at about 70 fps |
| Early in boot, `VE1 SRAM power-on not acked` | The first power-on of the SRAM domain is not acked, not even after 50 ms; off and on again it acks in ~50 us | Retry; clear the stale off-ack first |
| The last 8 lines of 1080p were green | The receiver lays NV12 out at 1088 lines and never writes the padding. Zero chroma there (U = V = 0) makes the CODA980 garble the whole last macroblock row, visible lines included. Zero luma does no harm | The receiver driver fills the padding with black (Y 16, UV 128) when it allocates a buffer (`hdmi_buffer_init`) |

Only the H.264 encoder is registered, because nothing else has been tried.

### ustreamer

ustreamer's M2M encoder only knew the multi-planar API of the Pi's
bcm2835-codec, and coda is single-planar. `patches/ustreamer/0002` picks the
API from `VIDIOC_QUERYCAP`. It also makes controls optional where not every
encoder has them: coda has `GOP_SIZE` instead of `H264_I_PERIOD`, and has no
`REPEAT_SEQ_HEADER` or `H264_MIN_QP`. coda only takes 4:2:0 input
(NV12/YU12/YV12), so the capture format has to be NV12 rather than NV16. The
receiver starts NV12's chroma plane after 1088 lines, so ustreamer gives the
encoder a 1088-line buffer and crops it to 1080 with `VIDIOC_S_SELECTION`.
The capture buffers go to the encoder as DMA-BUFs, with no copy.

### kvmd: direct H.264 and WebRTC

`main.yaml` gives ustreamer the H.264 sink options of PiKVM V4. Two readers
use that sink, as on a V4:

- **kvmd-media**: H.264 over kvmd's own websocket. The Web UI's direct H.264
  mode decodes it in the browser. No Janus is involved.
- **kvmd-janus**: Janus with ustreamer's plugin, for WebRTC. PiKVM sends
  audio only this way. Debian 13 has no Janus package, so the rootfs build
  compiles v1.4.2 with PiKVM's `janus.js` patch (`patches/janus`), configured
  as PiKVM's `janus-gateway-pikvm`, and builds ustreamer `WITH_JANUS=1`. The
  plugin config (`overlay/etc/kvmd/janus`) has video only: there is no ALSA
  device for the HDMI input's audio yet.

An image with the BSP kernel removes the H.264 options again
(`overlay-bsp/`), because that kernel has no encoder device.

The plugin asks ustreamer for a keyframe only on a PLI/FIR, or on the UI's
`key_required` message. ustreamer keeps encoding for 10 s after a sink client
leaves (the client TTL), so a client that reconnects within that time joins
mid-GOP. A browser sends a PLI for a new stream, and kvmd's UI also sends
`key_required` when bytes arrive but no frames decode.

### Verified on hardware, 2026-10-01

- Test patterns: a striped frame and a moving gradient decode back
  pixel-exact (ffmpeg, on the PC).
- `v4l2-ctl`, 40 frames of captured 1080p NV12: 0.7 s, including setup.
- ustreamer: HDMI (Kodi on a Pi) to NV12 to DMA-BUF to VE1 to the H.264
  sink. 1920x1080, 30 fps (ustreamer's own limit above 720p). The CPU is 92%
  idle. The stream decodes cleanly as 1920x1080.
- Encoder sessions opened back to back keep working.
- After the three fixes above (2026-10-01, Arch image): a captured Kodi frame
  encodes with a mean chroma error of 0.12 against the source, libx264 at the
  same QP 0.08; 60 frames at GOP 0 show no drift; and Kodi's coloured
  buttons are right through kvmd-media and WebRTC. AMD's VAAPI decoder agrees
  with ffmpeg's, so the earlier errors were in the bitstream, not in one
  decoder.
- Testing traps met on the way: `v4l2-ctl --stream-from` with a crop
  selection reads the file in frames of the visible size, so every frame
  after the first is misaligned; a decoded stream is 1088 lines tall when
  no crop was set; and Kodi dims its screen when idle, which stops the
  cursor blinking and hides colour problems. Wake it with a key first.
- Through kvmd: `/api/media/ws` (kvmd-media) delivers the H.264 at 24 fps.
  A headless WebRTC client (aiortc, on the PC) goes through `/janus/ws` and
  `janus.plugin.ustreamer` the way the UI does, and receives 1920x1080 at
  30-36 fps with the last lines intact. A client that reconnects within
  ustreamer's 10 s gets video after one `key_required`, as the UI would send.

Audio: see §13.

## 12. The rootfs: Arch Linux ARM with PiKVM's packages

Debian was chosen because of the BSP kernel: systemd 258 and later need
Linux 5.4, and Arch's systemd is past that (`02-decisions.md`, D3). The 6.18
line does not have that problem, so it uses what PiKVM OS itself is built
from. `make rootfs-arch` produces `build/rootfs-arch.tar`, and `make
image-mainline` uses it. The BSP image keeps the Debian rootfs.

| Part | Source |
|---|---|
| Base system | Arch Linux ARM's generic aarch64 tarball, imported as a docker image and upgraded in it |
| Janus, the Web UI's terminal (`kvmd-webterm`, ttyd), and kvmd's dependencies Arch lacks (`raspberrypi-io-access`, `raspberrypi-utils`, ...) | PiKVM's repository, `files.pikvm.org/repos/arch/rpi4-aarch64`, signed with key `912C773ABBD1B584` (the one PiKVM's own builder uses) |
| ustreamer, kvmd | Upstream's PKGBUILDs, built from `vendor/` with our patches. They are in `IgnorePkg`, so `pacman -Syu` keeps them |
| Platform config | `overlay/`, as before. No `kvmd-platform-*` package is for this board; the sysctl, udev and sudoers files such a package installs are copied from kvmd's `configs.default` |
| Kernel | None installed. Arch Linux ARM's `linux-aarch64` and `linux-firmware` are removed; the image step adds our modules and VE1's firmware |

Running pacman and makepkg under qemu-user needs four adjustments:

- pacman 7's download sandbox needs Landlock, which qemu-user does not have:
  `DisableSandbox*`, as PiKVM's builder does. The image gets the sandbox
  (and `CheckSpace`) back.
- setuid does not work, so makepkg cannot install dependencies through sudo.
  The script reads them from `makepkg --printsrcinfo` and installs the ones
  `pacman -T` reports missing, as root.
- `vendor/ustreamer` can hold object files from a Debian build, whose
  dependency files name Debian's header paths. `prepare()` runs `make
  clean`.
- gcc 16's `cc1` crashes under qemu often enough that the ustreamer package
  failed three makepkg attempts in a row (each attempt starts from scratch).
  During the build `/usr/local/bin/gcc` and `cc` wrap the compiler and retry
  a single compilation that dies of a signal or an internal compiler error.

pacman 7 downloads inside a Landlock sandbox and refuses to download
without it, so the kernel needs `CONFIG_SECURITY_LANDLOCK` (arm64 defconfig
leaves it out). Until it was added, `pacman -Sy` on the board failed with
"switching to sandbox user 'alpm' failed".

Arch Linux ARM masks udev's predictable interface names
(`/etc/systemd/network/99-default.link`), so the NIC is `eth0` here, not
`end0` as on the Debian image. The network file matches `Type=ether`, so
either works.

Upstream's ustreamer PKGBUILD does not list `speexdsp`, which the Janus
plugin needs; the script installs it explicitly.

## 13. HDMI audio: the audio CPU

The HDMI receiver hands the audio it decodes to the audio input (AI) block,
and AI belongs to the audio CPU (ACPU), a big-endian MIPS-compatible core
running Realtek's firmware, `bluecore.audio`. The BSP u-boot loads it and
starts the ACPU on it ("go a") before every kernel, ours included. The BSP
kernel talks to it over Realtek's RPC (`drivers/soc/realtek/common/rpc`)
and its ALSA driver (`sound/arm/snd-realtek*`) asks it for the capture.
`kernel/mainline/acpu` does the same for mainline, in two modules:

- `rtd129x-acpu`: the RPC. It cannot be unloaded: the firmware keeps the
  memory it was given.
- `snd-rtd129x-hdmirx`: ALSA card `hdmirx`, one capture device, S16_LE
  stereo at 44.1 or 48 kHz. It attaches to the first over the auxiliary bus.

### How the firmware starts, and what it waits for

The firmware starts in two stages. The first runs while u-boot is still
loading the kernel: it brings up the audio hardware and prints the
`[AO]aio_...` lines. Then it waits, sleeping 10 ticks at a time, for
`audio_rpc_flag` in the IPC block (`0x1f0d0`) to become non-zero. That is
the system CPU saying the RPC rings are ready. This was found by disassembly
(`mips-linux-gnu-objdump -EB`, load address `0x8f900000`), not documented
anywhere.

The second stage opens the rings (`[ROS: openRPC()`), then asks the system
CPU for memory over that same RPC (`gloabl malloc`, `do Remote Malloc`) and
waits for the answer before it creates its agents. The BSP answers out of
its ION media heap. Here the reply comes from media heap 1 (`0x03200000`),
already reserved. It gets the same addresses the BSP gave it, starting at
`0x83200000`, which is KSEG0 for `0x03200000`.

### The RPC

| Part | Where |
|---|---|
| Rings and their records | `0x01ffe000`, 16 KiB, a layout the firmware hardcodes: poll, intr and kern rings of 512 bytes, records of five words (buffer, start, end, in, out) in the CPU's byte order, holding ACPU (KSEG1) addresses |
| IPC block | `0x1f0c4`, big-endian: `audio_rpc_flag` at +0x0c, `vo_int_sync` at +0x40 |
| Interrupts | SB2 `CPU_INT` `0x9801a104` (bit 1 system to audio, bit 3 back, bit 0 "write 1"), SPI 33. `vo_int_sync` bit 9 marks one as RPC; the same line carries display sync |
| Kernel calls | kern ring 0, replies on kern ring 1: `{98, 98, 0, task, 0, 0, 12, 0}` then command, argument address, result address |
| Firmware requests | intr ring 1, replies on intr ring 0: program 98 is memory (1 alloc, 2 free, 3 secure alloc), the reply `phys + 0x80000000` |

Messages and everything the firmware reads are big-endian. Argument
structures are passed as `phys | 0xa0000000`.

### Two things mainline broke

Both left the firmware stuck in its first stage. It looked alive, with its
memory changing, but it never answered.

| Symptom | Cause | Fix |
|---|---|---|
| Its task delays (TCB +32, list at `0x8fc63940`) never counted down: the OS tick had stopped, so the 10-tick sleep never returned | `irq-rtd129x.c` cleared every bit of the ISO and MISC status registers at init. The enable registers are the system CPU's own, but the status registers are shared with the ACPU | Leave the status registers alone, as the BSP's driver does |
| Its BSS, heap and stacks were overwritten | The image is 3.8 MiB at `0x0f900000` and its data runs on to `ACPU_FIREWARE_SIZE`, 5 MiB; the DTS reserved 4 | Reserve 5 MiB, and the MIPS reset vector page at `0x1fc00000` |

How it was narrowed down: reading the firmware's TCBs through `/dev/mem`;
setting the flag from the u-boot console instead (with the dcache off,
since u-boot's `mw` otherwise sits in the cache until `booti` flushes it),
where the firmware went straight on; and booting the BSP kernel from an
initramfs to see the same firmware print `openRPC` with a working kernel.
`rpc@1f000` and `rpc@1ffe000` are now `no-map`, so the driver maps them
uncached, as the firmware sees them.

### Capture

This is the BSP's `snd_card_capture_prepare_LPCM()` sequence:

1. `CHECK_READY`.
2. `CREATE_AGENT(AUDIO_IN)`.
3. Two per-channel PCM rings and one LPCM ring, with `INIT_RINGBUF`.
4. `PRIVATEINFO(AI_CONNECT_ALSA, 16-bit LE)`.
5. `ADC0_CONFIG` with the rate.
6. `PAUSE`, then `RUN`.

The AI agent's default source is the HDMI receiver. The firmware resamples
to the rate asked for (`AI SRC 44100 => 48000`), so the stream rate stays
fixed.

An hrtimer copies whole periods from the LPCM ring into the ALSA buffer.
When the source sends no audio, the firmware writes nothing. The driver
then fills in silence in real time, so Janus sees a quiet input instead of
a stalled one.

On the HDMI side the receiver's audio state machine (`Hdmi_AudioModeDetect`,
already in the hdmirx port) sets up the audio PLL once ACR packets arrive.

### kvmd

`janus.plugin.ustreamer.jcfg` has `acap { device = "hw:hdmirx,0";
sampling_rate = 48000 }`. A TC358743 is not needed to learn the rate, since
the firmware resamples. With the BSP kernel there is no such card, and the
plugin leaves audio out.

### Verified on hardware, 2026-10-01

- From a cold boot: the firmware's second stage runs completely (global AO,
  PP, connections) with the memory this driver hands it.
- `arecord -D hw:hdmirx` delivers 48 kHz stereo in real time.
- A headless WebRTC client gets `features.audio = true`, an Opus track, and
  480 samples per 10 ms.
- With Kodi on the Pi playing a video: the receiver sees the audio
  infoframe, ACR (N = 6144, 48 kHz) and audio sample packets, and logs
  `audio state 1`. Five seconds from `arecord` have no silent 100 ms block
  (peak about -11 dBFS), and the spectrogram shows continuous harmonics up
  to the source's 21 kHz, with no periodic clicks. Over WebRTC the decoded
  Opus peaks at a similar level.
- A trap on the way: Kodi had its audio output set to Bluetooth, so the Pi
  sent no audio over HDMI at all. To see what a source really sends, point
  the receiver's packet slot 2 (`HDMI_PTRSV1` bits 15:8, `0x980340b0`) at
  a packet type, clear `HDMI_GPVS` bit 6, and see whether it comes back:
  0x84 audio infoframe, 0x01 ACR, 0x02 audio sample.

### The serial console noise

The firmware prints `[AO][_AO_if_video_HDMI_mode]HDMI not enabled` on the
serial console about every 3 s. That is the board's own HDMI output, for
which there is no driver; the BSP image printed it too. The function that
prints it (`0x8f9e4814`) does so only when bit 0 of `*ptrDebugFlag`
(pointer at `0x8fc5daa4`) is set, and the firmware sets that flag to 1 when
it starts its second stage. There is no RPC to change it, so `rtd129x-acpu`
clears the bit in the firmware's data, after the firmware's first memory
request (by then the flag is set up). It does so only if the message
string sits where this firmware build has it, and not with `fw_debug=1`.

Verified from a freshly flashed card: no such line in the 45 s after the
boot, and WebRTC audio as before. The first version compared the string
without its trailing newline, did not recognise the build, and said so
(`unknown firmware build, debug output left on`).

## 14. Sources

| Source | Used for |
|--------|----------|
| `vendor/linux-mainline` @ `v6.18.52` | `rtd129x.dtsi`, `rtd1296.dtsi`, `rtd1296-ds418.dts`, `arch/arm64/configs/defconfig` |
| `vendor/bpi-w2-bsp/u-boot-rtk` | `boot_from_sd()`, the load addresses, the bootargs |
| `vendor/bpi-w2-bsp/linux-rtk/include/soc/realtek/memory.h` | `ACPU_IDMEM_PHYS`/`_SIZE` |
| `08-kernel-uplift.md` | The milestone definitions and the list of things to copy |
| `vendor/bpi-w2-bsp/linux-rtk/drivers/irqchip/irq-rtd129x.[ch]` | The interrupt mux register layout and the status-bit -> enable-bit tables |
| `vendor/bpi-w2-bsp/linux-rtk/drivers/soc/realtek/rtd129x/rtd129x_spin_table.c` | How the secondary CPUs are released |
| `Fireblossom/wd-mch-kernel` @ `947374d` (`linux-6.18.40/drivers/net/ethernet/realtek/r8169soc.c`) | The 6.18 port of Realtek's `r8169soc.c` that M2 starts from |
| `vendor/bpi-w2-bsp/linux-rtk/drivers/soc/realtek/rtd129x/rtk_chip.c` | Where the chip revision lives |
| `vendor/bpi-w2-bsp/u-boot-rtk/include/configs/rtd1295_common.h` | `CONFIG_ETHADDR`, the MAC every board shares |
| `vendor/bpi-w2-bsp/linux-rtk/drivers/soc/realtek/rtd129x/rtk_ve/ve1/ve1.c` | VE1 power-on and the wrapper setup |
| `BPI-SINOVOIP/BPI-1296-Android7` @ `d377aa6` | `ve1.bin`; `libvpu.so` and the `vpuapi` headers (CODA980 register use) |
