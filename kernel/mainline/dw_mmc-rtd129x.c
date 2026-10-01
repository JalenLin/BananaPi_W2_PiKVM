// SPDX-License-Identifier: GPL-2.0
/*
 * Realtek RTD1295/RTD1296 eMMC: a Synopsys DesignWare MSHC (VERID 2.70a,
 * internal DMAC, 32-bit addressing) with a Realtek wrapper behind it at
 * +0x400. Realtek's BSP drives it with a driver of its own (rtkemmc.c);
 * the core is what dw_mmc already supports.
 *
 * What the wrapper needs from us is little, because the boot loader has
 * set the eMMC up before Linux -- both the SD and the eMMC builds of the
 * BSP u-boot initialise it, pins, pad drive, the 1.8 V LDO and the
 * EMMC PLL included -- and that is what this relies on:
 *
 *  - The core's interrupt reaches the GIC through a mask in the wrapper's
 *    ISR register; clear it, and mask the wrapper's own DMA interrupts.
 *  - The pads are 1.8 V (UHS_REG bit 0, set by the boot loader). The MMC
 *    core first asks for 3.3 V, and dw_mmc would clear the bit for that:
 *    refuse 3.3 V so that the card is run at 1.8 V, as wired.
 *  - The card clock comes from the EMMC PLL through the wrapper's clock
 *    generator, at up to 200 MHz (HS200). The CRT clock gate has no rate,
 *    so the rate is given in the DT (clock-frequency) as the upper bound:
 *    a slower PLL only makes the card clock slower than asked for.
 *
 * The wrapper's own DMA engine and the HS200/HS400 phase tuning are not
 * used; the card runs at High Speed, 8 bits, SDR.
 */
#include <linux/clk.h>
#include <linux/mmc/host.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>

#include "dw_mmc.h"
#include "dw_mmc-pltfm.h"

/* The wrapper; offsets from the start of the DW core's registers */
#define RTD_EMMC_ISR			0x424
#define RTD_EMMC_ISR_WRITE_DATA		BIT(0)	/* 1: set the bits given, 0: clear */
#define RTD_EMMC_ISR_DMA_DONE		BIT(1)	/* status */
#define RTD_EMMC_ISR_DMA_INT_MASK	BIT(2)
#define RTD_EMMC_ISR_DESC_INT_MASK	BIT(3)
#define RTD_EMMC_ISR_IP_INT_MASK	BIT(4)	/* the DW core's interrupt */

#define RTD_EMMC_CP			0x41c
#define RTD_EMMC_SWC_SEL		0x4d4
#define RTD_EMMC_SWC_SEL1		0x4d8
#define RTD_EMMC_SWC_SEL2		0x4dc
#define RTD_EMMC_SWC_SEL3		0x4e0

#define RTD_UHS_REG_18V			BIT(0)

static int dw_mci_rtd129x_init(struct dw_mci *host)
{
	struct clk *ip;

	/* The core's clock gate (CLK_EN1 bit 28); "biu" is the bus side */
	ip = devm_clk_get_optional_enabled(host->dev, "ip");
	if (IS_ERR(ip))
		return dev_err_probe(host->dev, PTR_ERR(ip), "no ip clock\n");

	/*
	 * Only the core's interrupt goes to the GIC. The wrapper's own DMA
	 * status (DMA_DONE) also latches on the core's transfers, and dw_mmc
	 * knows nothing of it: left unmasked, it holds the line high for
	 * good, an interrupt storm that hung the board.
	 */
	writel(RTD_EMMC_ISR_WRITE_DATA | RTD_EMMC_ISR_DMA_INT_MASK |
	       RTD_EMMC_ISR_DESC_INT_MASK, host->regs + RTD_EMMC_ISR);
	writel(RTD_EMMC_ISR_DMA_DONE, host->regs + RTD_EMMC_ISR);
	writel(RTD_EMMC_ISR_IP_INT_MASK, host->regs + RTD_EMMC_ISR);

	/*
	 * The BSP sets these before every transfer (SD_Stream_Cmd()). Without
	 * SWC_SEL2 the internal DMAC never takes its descriptors back from
	 * memory -- "descriptor is still owned by IDMAC". They do not change
	 * under us, so once is enough.
	 */
	writel(0, host->regs + RTD_EMMC_SWC_SEL1);
	writel(1, host->regs + RTD_EMMC_SWC_SEL2);
	writel(0, host->regs + RTD_EMMC_SWC_SEL3);
	writel(0, host->regs + RTD_EMMC_SWC_SEL);
	writel(0, host->regs + RTD_EMMC_CP);

	mci_writel(host, UHS_REG,
		   mci_readl(host, UHS_REG) | RTD_UHS_REG_18V);
	return 0;
}

static int dw_mci_rtd129x_switch_voltage(struct mmc_host *mmc,
					 struct mmc_ios *ios)
{
	return ios->signal_voltage == MMC_SIGNAL_VOLTAGE_180 ? 0 : -EINVAL;
}

static const struct dw_mci_drv_data rtd129x_data = {
	.init			= dw_mci_rtd129x_init,
	.switch_voltage		= dw_mci_rtd129x_switch_voltage,
};

static const struct of_device_id dw_mci_rtd129x_match[] = {
	{ .compatible = "realtek,rtd1295-dw-mshc", .data = &rtd129x_data },
	{},
};
MODULE_DEVICE_TABLE(of, dw_mci_rtd129x_match);

static int dw_mci_rtd129x_probe(struct platform_device *pdev)
{
	return dw_mci_pltfm_register(pdev, &rtd129x_data);
}

static struct platform_driver dw_mci_rtd129x_driver = {
	.probe = dw_mci_rtd129x_probe,
	.remove = dw_mci_pltfm_remove,
	.driver = {
		.name = "dwmmc_rtd129x",
		.probe_type = PROBE_PREFER_ASYNCHRONOUS,
		.of_match_table = dw_mci_rtd129x_match,
	},
};
module_platform_driver(dw_mci_rtd129x_driver);

MODULE_DESCRIPTION("Realtek RTD129x eMMC (DW-MSHC) driver extension");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:dwmmc_rtd129x");
