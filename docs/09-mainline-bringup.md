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
| **M0** | BSP u-boot + clean 6.18.x + board DTS; serial console, single core | see §5 |
| **M1** | `smp_spin_table.c` hunk, `irq-rtd129x.c`, drop `reg` from rbus | not started |
| **M2** | `NET_VENDOR_REALTEK` Kconfig unlock + `r8169soc.c` | not started |
| **M3** | USB DT + the two probe quirks, Type-C as peripheral | not started |
| **M4** | kvmd + ustreamer on 6.18 | not started |
| **M5** | hdmirx port | not started |
| **M6** | mmc host driver | not started |

### Traps carried over from §3 of `08-kernel-uplift.md`

Two of them are predictions from someone else's port and are **not yet
confirmed on this board**. Recording them here so the confirmation, when it
comes, is attached to evidence rather than to the prediction:

- **`rbus: bus@98000000` carries `reg` in mainline's `rtd129x.dtsi`.** The
  claim is that this claims the whole 2 MiB window and makes every child's
  own MMIO request return `-EBUSY`. Against that: `rtd1296-ds418.dts` is
  upstream and boots with a working `uart0`, whose ancestor is that node.
  Whatever the true scope of the problem is, it has not bitten yet.
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

### Not yet verified on hardware

**Everything above is a build result. The image has not been booted on the
board.** M0's acceptance is characters on the serial console, and that needs
a card and a person.

### How to test it

```sh
make kernel-mainline
make image-mainline          # -> build/bpiw2-pikvm-mainline.img
sudo dd if=build/bpiw2-pikvm-mainline.img of=/dev/sdX bs=4M conv=fsync status=progress
```

Watch the console with `LISTEN=180 scripts/uboot-cmd.sh ""` (SW4 = 1 for
SPI + SD).

What should appear, in order: u-boot's four `Loading ... is OK` lines, then
earlycon output from the kernel. Anything after
`Booting Linux on physical CPU 0x0` is new ground.

Expect it to stop somewhere in userspace, and that is fine at M0:

- **only CPU0 comes up.** `rtd1296.dtsi` declares four CPUs with no
  `enable-method`, so this is expected, not a fault. M1 fixes it
- **root will not mount.** There is no RTD129x mmc host driver in mainline
  (§4.3 of `08-kernel-uplift.md`), so `/dev/mmcblk0p2` never appears
- **the rootfs on the card still carries BSP 4.9 modules**, under
  `/lib/modules/4.9.119-BPI-W2-Kernel`. Nothing will load them. Building a
  matching module set into the rootfs is M4 work

The vendor initramfs is a Debian initramfs-tools image (klibc + busybox), so
it is actually useful here: to get a shell instead of watching it hunt for a
root device, break into u-boot and boot with `break=premount`:

```
setenv sdroot_args 'break=premount'
run set_sdbootargs
gosd
```

That lands on `(initramfs)`, where `dmesg`, `cat /proc/cpuinfo` and
`ls /proc/device-tree` answer most of the questions M0 raises.

---

## 6. Sources

| Source | Used for |
|--------|----------|
| `vendor/linux-mainline` @ `v6.18.52` | `rtd129x.dtsi`, `rtd1296.dtsi`, `rtd1296-ds418.dts`, `arch/arm64/configs/defconfig` |
| `vendor/bpi-w2-bsp/u-boot-rtk` | `boot_from_sd()`, the load addresses, the bootargs |
| `vendor/bpi-w2-bsp/linux-rtk/include/soc/realtek/memory.h` | `ACPU_IDMEM_PHYS`/`_SIZE` |
| `08-kernel-uplift.md` | The milestone definitions and the list of things to copy |
