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
#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/reset.h>

#include "hdmirx_clk_ctrl.h"

#define SRAM_PWR2		0x368
#define SRAM_PWR2_HDMIRX	GENMASK(14, 11)
#define SRAM_PWR2_MIPI		BIT(8)

static struct reset_control *reset_mipi;
static struct reset_control *reset_rxwrap;
static struct reset_control *reset_hdmirx;
static struct reset_control *reset_cbustx;
static struct reset_control *reset_cbus_iso;
static struct reset_control *reset_cbustx_iso;
static struct reset_control *reset_cbusrx_iso;
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
