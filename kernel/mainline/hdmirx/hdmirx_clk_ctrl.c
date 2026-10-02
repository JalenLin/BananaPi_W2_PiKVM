// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * hdmirx_clk_ctrl.c - clocks, resets and SRAM power of the HDMI receiver
 *
 * Copyright (C) 2017 Realtek Semiconductor Corporation
 *
 * The grouping (HDMIRX / RXWRAP / MIPI / CBUS) and the order within each
 * group are the BSP's. For mainline the clocks come from the CRT and ISO
 * gate drivers (clk-rtd129x-crt.c) and the resets from reset-simple.
 *
 * SRAM power: the BSP asked its power-control core for "pctrl_disp_hdmi_rx"
 * and "pctrl_disp_mipi", but that core registers them as
 * "pctrl_hdmirx_pd"/"pctrl_mipi_pd", so both lookups returned NULL and
 * every power_on/off was a no-op; the HDMI RX SRAM was powered through the
 * node's power-domains instead. Measured on this board, CRT SRAM_PWR2
 * (0x98000368) reads 0, i.e. every channel on, before any of this runs.
 * The channels are still cleared on enable, from the BSP's pwrctrl tables
 * (hdmirx: bits 14:11, mipi: bit 8), and never turned off, which is what
 * the BSP effectively did.
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/reset.h>

#include "hdmirx_clk_ctrl.h"

#define SRAM_PWR2		0x368
#define SRAM_PWR2_HDMIRX	GENMASK(14, 11)
#define SRAM_PWR2_MIPI		BIT(8)

#define PLL_HDMI		0x190
#define  PLL_HDMI_CK_EN		BIT(0)
#define  PLL_HDMI_PLL_POW	BIT(1)
#define  PLL_HDMI_PLL_RSTB	BIT(2)
#define  PLL_HDMI_TMDS_POW	BIT(3)
#define PLL_HDMI_LDO1		0x230
#define  PLL_HDMI_LDO1_EXT_LDO_LV	BIT(6)
#define PLL_VODMA1		0x260	/* M, N, O, ... */
#define  PLL_VODMA1_GOOD	0x0010c166	/* as a good boot left it */
#define PLL_VODMA2		0x264
#define  PLL_VODMA2_POW		BIT(0)
#define  PLL_VODMA2_RSTB	BIT(1)
#define  PLL_VODMA2_OEB		BIT(2)

static struct reset_control *reset_mipi;
static struct reset_control *reset_rxwrap;
static struct reset_control *reset_hdmirx;
static struct reset_control *reset_cbustx;
static struct reset_control *reset_cbus_iso;
static struct reset_control *reset_cbustx_iso;
static struct reset_control *reset_cbusrx_iso;
static struct reset_control *reset_disp;
static struct clk *clk_mipi;
static struct clk *clk_hdmirx;
static struct clk *clk_cbustx;
static struct clk *clk_cbus_osc_iso;
static struct clk *clk_cbus_iso;
static struct clk *clk_cbustx_iso;
static struct clk *clk_cbusrx_iso;
static void __iomem *crt;
static unsigned int enabled;

unsigned char is_clock_enabled(HDMI_CLK_TYPE clk_type)
{
	return !!(enabled & clk_type & CLK_MIPI);
}

void hdmirx_reset_control(HDMI_CLK_TYPE clk_type, HDMI_CLK_CTL enable)
{
	if (clk_type & RST_MIPI) {
		if (enable)
			reset_control_deassert(reset_mipi);
		else
			reset_control_assert(reset_mipi);
	}
}

static void sram_power_on(u32 mask)
{
	writel(readl(crt + SRAM_PWR2) & ~mask, crt + SRAM_PWR2);
}

/*
 * The receiver depends on two things of the display side that nothing on
 * this kernel drives -- the boot loader and the audio firmware set them
 * up, or not. The BSP u-boot on the SD card leaves both on; BPI's eMMC
 * u-boot leaves both off:
 *
 *  - The HDMI PLL in CRT, the transmitter's (PLL_HDMI 0x80, LDO1 bit 6
 *    set). Without it the PHY's offset calibration times out on every lane
 *    ("Wait b lane koff timeout"). Both the power, reset and clock bits and
 *    LDO1 bit 6 cleared are needed -- what the BSP's DP driver also does
 *    before it uses this PLL (dptx_pixelpll_setting()).
 *  - The display block's reset (RSTN_DISP). Held in reset, the receiver
 *    calibrates and measures the TMDS clock, but never decodes a sync and
 *    goes round "PLL Setting" for good.
 *  - The VODMA PLL, the clock of the MIPI block that writes the frames to
 *    memory (PLL_VODMA2 0x4: off, output disabled). Without it the MIPI
 *    registers read 0xdeadbeef, timings are detected but no frame comes
 *    ("CAP: Device select() timeout" in ustreamer). Set up as a good boot
 *    left it.
 *
 * Found on the board by diffing CRT between a good and a bad boot and
 * putting the good values back a few at a time. Both are only ever turned
 * on here: the audio firmware drives the display side too.
 */
static void hdmi_pll_on(void)
{
	const u32 on = PLL_HDMI_CK_EN | PLL_HDMI_PLL_POW | PLL_HDMI_PLL_RSTB |
		       PLL_HDMI_TMDS_POW;
	u32 val;

	writel(readl(crt + PLL_HDMI_LDO1) & ~PLL_HDMI_LDO1_EXT_LDO_LV,
	       crt + PLL_HDMI_LDO1);
	val = readl(crt + PLL_HDMI);
	if ((val & on) == on)
		return;
	writel(val | PLL_HDMI_PLL_POW | PLL_HDMI_TMDS_POW, crt + PLL_HDMI);
	udelay(100);
	writel(val | on, crt + PLL_HDMI);
	pr_info("[HDMI RX]HDMI PLL was off (0x%08x), turned on\n", val);
}

static void vodma_pll_on(void)
{
	u32 val = readl(crt + PLL_VODMA2);

	if ((val & (PLL_VODMA2_POW | PLL_VODMA2_RSTB | PLL_VODMA2_OEB)) ==
	    (PLL_VODMA2_POW | PLL_VODMA2_RSTB))
		return;
	writel(PLL_VODMA1_GOOD, crt + PLL_VODMA1);
	writel(PLL_VODMA2_POW | PLL_VODMA2_OEB, crt + PLL_VODMA2);
	udelay(200);
	writel(PLL_VODMA2_POW | PLL_VODMA2_RSTB | PLL_VODMA2_OEB, crt + PLL_VODMA2);
	udelay(200);
	writel(PLL_VODMA2_POW | PLL_VODMA2_RSTB, crt + PLL_VODMA2);
	pr_info("[HDMI RX]VODMA PLL was off (0x%08x), turned on\n", val);
}

static void clk_on(struct clk *clk, HDMI_CLK_TYPE type)
{
	if (clk_prepare_enable(clk))
		HDMIRX_ERROR("clock for group 0x%x failed", type);
}

void hdmirx_clock_control(HDMI_CLK_TYPE clk_type, HDMI_CLK_CTL enable)
{
	/* Balance the clk refcounts: act only on a real change of state. */
	clk_type &= enable ? ~enabled : enabled;
	clk_type &= CLK_ALL;
	if (!clk_type)
		return;

	if (clk_type & CLK_HDMIRX) {
		if (enable) {
			hdmi_pll_on();
			vodma_pll_on();
			reset_control_deassert(reset_disp);
			reset_control_deassert(reset_hdmirx);
			clk_on(clk_hdmirx, CLK_HDMIRX);
			sram_power_on(SRAM_PWR2_HDMIRX);
		} else {
			clk_disable_unprepare(clk_hdmirx);
			reset_control_assert(reset_hdmirx);
		}
	}

	if (clk_type & CLK_RXWRAP) {
		if (enable)
			reset_control_deassert(reset_rxwrap);
		else
			reset_control_assert(reset_rxwrap);
	}

	if (clk_type & CLK_MIPI) {
		if (enable) {
			reset_control_deassert(reset_mipi);
			clk_on(clk_mipi, CLK_MIPI);
			sram_power_on(SRAM_PWR2_MIPI);
		} else {
			clk_disable_unprepare(clk_mipi);
			reset_control_assert(reset_mipi);
		}
	}

	if (clk_type & CLK_CBUS) {
		if (enable) {
			reset_control_deassert(reset_cbustx);
			reset_control_deassert(reset_cbus_iso);
			reset_control_deassert(reset_cbustx_iso);
			reset_control_deassert(reset_cbusrx_iso);
			clk_on(clk_cbustx, CLK_CBUS);
			clk_on(clk_cbus_osc_iso, CLK_CBUS);
			clk_on(clk_cbus_iso, CLK_CBUS);
			clk_on(clk_cbustx_iso, CLK_CBUS);
			clk_on(clk_cbusrx_iso, CLK_CBUS);
		} else {
			clk_disable_unprepare(clk_cbustx);
			clk_disable_unprepare(clk_cbus_osc_iso);
			clk_disable_unprepare(clk_cbus_iso);
			clk_disable_unprepare(clk_cbustx_iso);
			clk_disable_unprepare(clk_cbusrx_iso);
			reset_control_assert(reset_cbustx);
			reset_control_assert(reset_cbus_iso);
			reset_control_assert(reset_cbustx_iso);
			reset_control_assert(reset_cbusrx_iso);
		}
	}

	if (enable)
		enabled |= clk_type;
	else
		enabled &= ~clk_type;
}

#define GET_RESET(var, name)						\
	do {								\
		var = devm_reset_control_get_exclusive(dev, name);	\
		if (IS_ERR(var))					\
			return dev_err_probe(dev, PTR_ERR(var),		\
					     "reset %s\n", name);	\
	} while (0)

#define GET_CLK(var, name)						\
	do {								\
		var = devm_clk_get(dev, name);				\
		if (IS_ERR(var))					\
			return dev_err_probe(dev, PTR_ERR(var),		\
					     "clock %s\n", name);	\
	} while (0)

int hdmirx_clock_init(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct resource res;
	int idx;

	enabled = 0;

	GET_RESET(reset_mipi, "mipi");
	GET_RESET(reset_rxwrap, "hdmirx_wrap");
	GET_RESET(reset_hdmirx, "hdmirx");
	GET_RESET(reset_cbustx, "cbus_tx");
	GET_RESET(reset_cbus_iso, "cbus");
	GET_RESET(reset_cbustx_iso, "cbustx");
	GET_RESET(reset_cbusrx_iso, "cbusrx");
	/* shared: never asserted from here (see hdmi_pll_on()) */
	reset_disp = devm_reset_control_get_shared(dev, "disp");
	if (IS_ERR(reset_disp))
		return dev_err_probe(dev, PTR_ERR(reset_disp), "reset disp\n");

	GET_CLK(clk_mipi, "mipi");
	GET_CLK(clk_hdmirx, "hdmirx");
	GET_CLK(clk_cbustx, "cbus_tx");
	GET_CLK(clk_cbus_osc_iso, "cbus_osc");
	GET_CLK(clk_cbus_iso, "cbus_sys");
	GET_CLK(clk_cbustx_iso, "cbustx_sys");
	GET_CLK(clk_cbusrx_iso, "cbusrx_sys");

	/*
	 * Mapped without requesting it: the CRT block is already claimed by
	 * the crt syscon.
	 */
	idx = of_property_match_string(dev->of_node, "reg-names", "crt");
	if (idx < 0 || of_address_to_resource(dev->of_node, idx, &res))
		return dev_err_probe(dev, -EINVAL, "no \"crt\" reg\n");
	crt = devm_ioremap(dev, res.start, resource_size(&res));
	return crt ? 0 : -ENOMEM;
}
