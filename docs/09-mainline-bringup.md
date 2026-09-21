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
| **M1** | `smp_spin_table.c` hunk, `irq-rtd129x.c`, drop `reg` from rbus | not started |
| **M2** | `NET_VENDOR_REALTEK` Kconfig unlock + `r8169soc.c` | not started |
| **M3** | USB DT + the two probe quirks, Type-C as peripheral | not started |
| **M4** | kvmd + ustreamer on 6.18 | not started |
| **M5** | hdmirx port | not started |
| **M6** | mmc host driver | not started |

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
  /soc@0/interrupt-controller@ff011000: Fixed dependency cycle(s) with /soc@0/interrupt-controller@ff011000
  ```

  That dependency cycle is the IRQ mux, i.e. the `irq-rtd129x.c` half of M1.

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

## 6. Sources

| Source | Used for |
|--------|----------|
| `vendor/linux-mainline` @ `v6.18.52` | `rtd129x.dtsi`, `rtd1296.dtsi`, `rtd1296-ds418.dts`, `arch/arm64/configs/defconfig` |
| `vendor/bpi-w2-bsp/u-boot-rtk` | `boot_from_sd()`, the load addresses, the bootargs |
| `vendor/bpi-w2-bsp/linux-rtk/include/soc/realtek/memory.h` | `ACPU_IDMEM_PHYS`/`_SIZE` |
| `08-kernel-uplift.md` | The milestone definitions and the list of things to copy |
