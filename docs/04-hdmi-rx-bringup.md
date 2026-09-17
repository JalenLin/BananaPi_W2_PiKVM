# HDMI RX bring-up log

## Conclusion: HDMI IN captures at 1080p60, and EDID is served correctly

State on real hardware once all three blockers were cleared (source is a
Raspberry Pi 3 running Kodi, which negotiates 1080p from boot):

```
$ hdmirx-info
== switch state ==
  rx_video/state = 1
== video info ==
  Type:HDMIRx     Status:Ready
  Width:1920      Height:1080
  ScanMode:Progressive   Color:RGB   Fps:60
```

The driver's detection sequence — note `VIC(16)` rather than the earlier
`DIV` / DVI fallback:

```
[HDMI RX]Use cbus 5V detect
[HDMI RX]Cable Plugged
[HDMI RX]Set HPD(1)
[HDMI RX]Set Physical Addr = 0x1100
[HDMI RX]Set hotplug pulse
[HDMI RX]Check resolution match => Width(1920) Height(1080) VIC(16)
[HDMI RX]Polarity detect done: hor(1920) ver(1080) color(RGB) I/P(Prog)
[HDMI RX] switch hdmi rx state to 1
[HDMI RX]skip 0/600 in 2500 jiffies          ← 600 frames per 10 s, zero dropped
```

The sections below record the earlier state — when EDID was still not being
served and format negotiation was still broken — for comparison.

---

## Blocker 1: a HPD deadlock in the DT (fixed)

`rtd-1296-bananapi-common.dtsi` points `gpio-5v-detect` and
`gpio-rx-hpd-ctrl` at the same GPIO 22, in opposite directions. The driver's
`rx5v_do_task()`:

```c
if (hdmi.gpio_5v_det < 0)
    state = Cbus_GetRx5v();                   /* the correct path */
else
    state = gpio_get_value(hdmi.gpio_5v_det); /* reads HPD back from itself */
```

which deadlocks:

```
probe → Hdmi_SetHPD(0)     pin drives low
poll  → gpio_get_value()   reads back its own 0 → decides no source is present
      → Hdmi_SetHPD(1) never runs → the source never sees HPD → sends nothing
```

**Fix**: remove `gpio-5v-detect` (patch `0002`). No other board in the BSP
has this property (`rtd-1296.dtsi`), or it is commented out and uses a
different pin (`rtd-1295.dtsi` uses GPIO 21). Once removed, the driver takes
the `Cbus_GetRx5v()` path, which reads the real `REGO_5V_DETECT` bit in
`ISO_CBUS_TX_PHY_CTRL5` and works correctly on this board.

Side note: `Hdmi_SetHPD()` is inverted — asking for `high` drives the GPIO
**low**, which implies an inverting buffer on the board.

---

## Blocker 2: the driver never implemented VIDIOC_G_FMT (fixed)

```
$ grep -c "VIDIOC_G_FMT\|VIDIOC_TRY_FMT" hdmirx_video_dev.c
0
```

The driver has `VIDIOC_S_FMT` but **no `G_FMT` / `TRY_FMT`**. Both `v4l2-ctl`
and ustreamer call G_FMT before S_FMT and give up on S_FMT when it fails:

```
[HDMI RX ERR]Unknown ioctl TYPE(0x56) NR(4) SIZE(208)   ← NR(4) = VIDIOC_G_FMT
[HDMI RX] ioctl VIDIOC_STREAMON                          ← jumps straight to STREAMON
[HDMI RX][set_video_DDR_start_addr] addr1y(0x0414e000) addr1uv(0x0414e000)
                                    addr2y(0x0414e000) addr2uv(0x0414e000)
[HDMI RX]hsd_out=0x0,hsd_delta=0x0
[HDMI RX]vsd_out=0x0,vsd_delta=0x0
```

All four addresses being identical comes from `set_video_DDR_start_addr()`:

```c
offset = roundup16(mipi_top.pitch) * roundup16(mipi_top.v_output_len);
addruv = ion_buf->phys_addr + offset;
```

`pitch` and `v_output_len` are only set inside S_FMT. Without that call they
stay 0 → offset 0 → the Y and UV planes overlap → the MIPI wrapper never
produces a frame (`qcnt:4 rcnt:0`).

Android's `tv_input` HAL issues S_FMT directly without a preceding G_FMT,
which is how it sidesteps this hole.

**How it was verified**: `tools/hdmirx-test.c` follows the HAL's order and
calls S_FMT directly, and successfully grabs frames.

**Follow-up**: add `VIDIOC_G_FMT` (and consider `TRY_FMT`) to the driver,
otherwise ustreamer cannot use it.

---

## Blocker 3: EDID was never served at all (fixed)

**Symptom**: the source could not read EDID and fell back to 1024x768 DVI.
Everything on the driver side looked perfect — `Set Physical Addr` ran,
`edid_version` was 1.4, the EDID SRAM contents verified via `/dev/mem`
matched the DTS exactly, `DDC2_EDID_CR.edid_en` was 1, and HPD really was
asserted (the source's `/sys/class/drm/card0-HDMI-A-1/status` showed
`connected`).

**How it was narrowed down**:

1. Reading address 0x50 on the source's `/dev/i2c-2` (the HDMI DDC bus)
   directly: 400 attempts, all `EIO` (no ACK). Scanning the whole bus from
   0x03 to 0x77 produced no response at any address.
2. Simultaneously polling the DDC status registers on the board at high rate
   (`DDC2_I2C_SR1` / `DDC2_EDID_CR` / `DDC2_DDC_SIR` / `DDC2_EDID_IR`):
   820,000 samples showed exactly **one** state, with `cmderr` / `finish` /
   `timeout` never moving.
   → The DDC engine never saw a single I2C transaction.
3. Swept all 8 combinations of `HDMI_VCR`'s `cbus_ddc_chsel` /
   `hdcp_ddc_chsel`. No effect.
4. Opened the schematic, `bpi-w2-v1_1-pub.pdf` page 6, and found the answer.

**Root cause**: pins 15/16 of the HDMI IN connector (SCL/SDA) go through
R81/R82 (0R) to the SoC's `I2C6_SCL` / `I2C6_SDA`, i.e. ISO pad 20 and pad
26. The pinmux for those two pads lives in bits [1:0] and [3:2] of ISO
MUXPAD `0x98007314`, and resets to function 0 = `gpio`.

`pinctrl-rtd129x` does have an `i2c_pins_6` group (function `i2c6`), but
BPI's `pinctrl-0` list never references it, so those two pads stay GPIO from
power-on to power-off. However correctly rtk_hdmirx is configured, the
physical pins simply are not connected to the DDC block.

### Side discovery: this board's DT pinmux has never taken effect

Adding `<&i2c_pins_6>` to `pinctrl-0` of `pinctrl@9801A000` **does not
work**. Boot messages:

```
unsupported function sdio on pin sdio_clk
pinctrl core: failed to register map default (10): invalid type given
```

`RTK_pctrl_dt_node_to_map()`
(`drivers/pinctrl/realtek/pinctrl-rtd129x.c`) precomputes
`nmaps = count * 2` from the pin count and allocates the array, but when the
loop hits an unknown pin or an unsupported function it merely `continue`s
without incrementing `i` — and then unconditionally sets
`*num_maps = nmaps` at the end. The trailing entries are uninitialised
`kmalloc_array()` memory with a garbage `type`, so `pinctrl_register_map()`
rejects **the entire default state**.

This board's `sdio_pins` happens to trigger it (`sdio_clk` does not support
the `sdio` function), which means **no DT pinmux has ever been applied on
this board**. Everything that works today works purely because the
bootloader left the pads configured.

Changing `*num_maps = nmaps` to `*num_maps = i` would fix the bug, but it
would also apply 17 other groups (SD, RGMII, UART, PWM, …) for the first
time. That is too much risk for a board that currently boots, so **it is
deliberately not done**.

> Also note: the `pinctrl@9801A000` in `rtd-1296-bananapi-common.dtsi` is
> `status = "disabled"`. The one that actually takes effect is in
> `rtd-1296-bananapi-w2-2GB.dts`.

**Fix**:
`patches/kernel/0005-arm64-dts-bpi-w2-mux-i2c6-pads-for-hdmi-rx-ddc.patch`
attaches `pinctrl-names = "default"; pinctrl-0 = <&i2c_pins_6>;` to the
`hdmirx@98034000` node instead. The driver core's `pinctrl_bind_pins()`
builds and applies a map for that single device when hdmirx probes, which is
unaffected by the broken default state and touches no other pins.

**Verification** (done via `/dev/mem` at the time; the shipping image's
kernel no longer enables `CONFIG_DEVMEM`, so reproducing this needs a debug
defconfig layered on top — see `0003` in §1 of `06-changes.md`): after
switching the pinmux — either with the patch before boot, or live by writing
bits [3:0] of `0x98007314` to `0b0101` — the source immediately reads a
complete 256-byte EDID with both block checksums correct, the CEA extension
block (tag 0x02) and the HDMI VSDB present, and `1920x1080` appears in
`/sys/class/drm/card0-HDMI-A-1/modes`.

**This also explains the "720p only, force the resolution by hand"
workaround that circulates on the BPI forum**: DDC never worked, so the only
option was to hardcode it on the source.

If you still need to force it on the source (e.g. a device that ignores
EDID):

```
hdmi_force_hotplug=1
hdmi_group=1
hdmi_mode=16      # 1080p60
hdmi_drive=2      # force HDMI rather than DVI
```

On a Pi 4/5 using KMS, add `video=HDMI-A-1:1920x1080@60D` to `cmdline.txt`.

## Other problems, now solved

### The buffer was sized at 4 bytes/pixel (fixed)

1024x768 NV16 should be 1572864 bytes; 3145728 was allocated — exactly
double. `out_color_to_bpp()`'s switch had no `OUT_8BIT_YUV422` case and fell
through to a default of 32. Patch 0004 adds
`case OUT_8BIT_YUV422: bpp = 16;`.
The real buffer is 1920 x 1088 x 2 (plane height aligned to 16).

> **Correction (2026-09-17)**: this section used to claim "patch 0004 makes
> `S_FMT` fill in the real `bytesperline`". **That was wrong.** 0004 only
> routed `G_FMT` / `TRY_FMT` through `fill_pix_format()`; `S_FMT` was never
> touched, and returned whatever `bytesperline` the caller passed in. So the
> "bottom half of the picture is solid green" problem was never actually
> fixed — nobody looked at the pixels again. The real fix is patch `0007`;
> see `0007` and section 7 item 3 of `06-changes.md`.

### Frame rate was half of what it should be (solved)

Early testing at 1024x768 only reached 33.4 fps. Once format negotiation was
fixed the capture side reaches 57.9 fps raw, and `skip_frame` is not
deliberately dropping anything.

> **Correction.** This used to claim "60 fps through kvmd" as well. That was
> measured before the encoder problem was found and is wrong -- at the time
> the CPU encoder needed 830 ms per frame. What the capture side produces and
> what reaches a browser are different numbers; see section 7 item 6 of
> `06-changes.md` for the measured 22–24 fps delivered.

### The system froze after a capture test — cause found

Kernel oops, call trace:

```
Unable to handle kernel NULL pointer dereference at virtual address 00000000
Internal error: Oops: 96000006 [#1] PREEMPT SMP
PC is at __wake_up_common+0x38/0xa0
[<...>] __wake_up+0x50/0x70
[<...>] __vb2_queue_cancel+0x7c/0x188
[<...>] vb2_core_streamoff+0x54/0xb8
[<...>] vb2_streamoff+0x54/0x88
[<...>] v4l2_hdmi_do_ioctl+0x600/0x6f8      ← VIDIOC_STREAMOFF
```

**Cause**: this driver only calls `vb2_queue_init()` inside its
`VIDIOC_REQBUFS` handler. Issue `VIDIOC_STREAMOFF` before that and
`vb2_streamoff()` operates on a queue that was never initialised. `hdmi_dev`
comes from `kzalloc()`, so `q->done_wq.task_list` is all zeros, and
`wake_up_all()` inside `__vb2_queue_cancel()` walks the list, hits
`next == NULL`, and oopses.

After the oops the process is stuck in D state inside `_vb2_fop_release`,
holding `/dev/video0` open, and only a power cycle recovers it.

**This is a driver bug too**: `VIDIOC_STREAMOFF` (and `QBUF`/`DQBUF`/
`QUERYBUF`) should return `-EINVAL` rather than pass through when the queue
is not initialised. Patch 0004 adds a `queue_inited` guard.

The userspace workaround (already applied in `tools/hdmirx-test.c`) is to
issue `REQBUFS(count=0)` first so the queue initialises, and only use
`STREAMOFF` when that returns `-EBUSY`.

---

## Environment gotchas

| Symptom | Explanation |
|---|---|
| ~~`reboot` cannot be used~~ | **Corrected**: `reboot` works when booting from the on-board micro SD. `wait rtk_check_system_ready_to_suspend` also appears in successful reboots and is not where it hangs. At the time we were booting via a card reader and LK's manual USB boot, which drops back to LK and waits for a human — that is what looked like a hang |
| The MAC changes on every boot | It has no fixed source, so the DHCP lease moves with it (`.93` → `.94`) |
| `/dev/video250` does not exist | The driver asks for minor 250 and falls back to `video0`; tools must identify the device by the `hdmirx_video_info` attribute |
| dmesg floods when the SD slot is empty | `rtk_sdmmc` retries about 4 times a second and buries the kernel ring buffer |
| The serial console gets flooded | The Realtek audio driver continuously prints `[AO][_AO_if_video_HDMI_mode]HDMI not enabled`, burying the login prompt. `dmesg -n 1` quiets it |
| The board has no RTC battery | It boots in 2014 and needs `systemd-timesyncd`; a wrong clock also breaks apt's signature validation |
