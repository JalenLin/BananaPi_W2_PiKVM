// SPDX-License-Identifier: GPL-2.0-only
/*
 * Realtek RTD129x SD/MMC host
 *
 * The RTD129x embeds a Realtek RTS-family card reader core -- the same IP
 * mainline already drives behind PCIe (drivers/mmc/host/rtsx_pci_sdmmc.c) and
 * USB (rtsx_usb_sdmmc.c) -- but memory-mapped into the SoC rather than sitting
 * behind a transport. The register names below are the rtsx ones; the offsets
 * differ from include/linux/rtsx_pci.h by a constant per block, 0xfc20 for the
 * SD_* group and 0xfc50 for the CARD_* group. rtsx_pci_sdmmc.c is therefore a
 * usable reference for anything here that looks unexplained.
 *
 * What cannot come from rtsx is SoC-specific: the DMA engine, the clock
 * generator and the pad drive settings. Those follow the BSP's
 * drivers/mmc/host/rtk-sdmmc.c.
 *
 * Only the core's own register window is mapped. The BSP also maps the
 * CRT/PLL block, SB2 and the eMMC controller's DMA engine, but those overlap
 * nodes the .dtsi already owns; anything needed from them later should come
 * through a syscon phandle.
 *
 * Stage 1: card detection, command submission and responses, including the
 * 136-bit R2 that this core returns by DMA rather than in registers. Block
 * data transfer is not implemented yet, so the card will be identified and
 * then fail when the core tries to read from it.
 */

#include <linux/bitops.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/mmc/host.h>
#include <linux/mmc/mmc.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/reset.h>
#include <linux/unaligned.h>

/* --- the card reader core, reg[1] ------------------------------------- */

#define CR_SD_DMA_CTL1		0x0004	/* buffer address / 8 */
#define CR_SD_DMA_CTL2		0x0008	/* length, in 8-byte units */
#define CR_SD_DMA_CTL3		0x000c	/* direction and go */
#define CR_SD_ISR		0x0024
#define CR_SD_ISREN		0x0028
#define CR_SD_CKGEN_CTL		0x0078
#define CR_CARD_STOP		0x0103
#define CR_CARD_OE		0x0104
#define CARD_SELECT		0x010e
#define CARD_EXIST		0x011f
#define CARD_CLOCK_EN_CTL	0x0129

#define SD_CONFIGURE1		0x0180
#define SD_CONFIGURE2		0x0181
#define SD_CONFIGURE3		0x0182
#define SD_STATUS1		0x0183
#define SD_BUS_STATUS		0x0185
#define SD_CMD_MODE		0x0186
#define SD_CMD0			0x0189	/* command out, response back */
#define SD_BYTE_CNT_L		0x018f
#define SD_BYTE_CNT_H		0x0190
#define SD_BLOCK_CNT_L		0x0191
#define SD_BLOCK_CNT_H		0x0192
#define SD_TRANSFER		0x0193

/* CR_SD_DMA_CTL3 */
#define DMA_XFER		BIT(0)
#define DDR_WR			BIT(1)	/* clear = card to memory */
#define RSP17_SEL		BIT(4)	/* this transfer is an R2 response */

/* CR_SD_ISR / CR_SD_ISREN */
#define ISR_CARD_END		BIT(1)
#define ISR_CARD_ERR		BIT(2)
#define ISR_DMA_DONE		BIT(4)

/* CARD_EXIST */
#define SD_EXISTENCE		BIT(2)

/* CARD_SELECT */
#define SD_MOD_SEL		0x02

/* SD_CONFIGURE1 */
#define SD_BUS_WIDTH_MASK	0x03
#define SD_BUS_WIDTH_1		0x00
#define SD_BUS_WIDTH_4		0x01
#define SD_BUS_WIDTH_8		0x02
#define SD_CLOCK_DIV_MASK	(0x03 << 6)
#define SD_CLOCK_DIV_256	BIT(6)
#define SD_CLOCK_DIV_EN		BIT(7)

/* SD_CONFIGURE2 -- response length the core should clock in */
#define SD_RESP_TYPE_NONE	0x00
#define SD_RESP_TYPE_6B		0x01
#define SD_RESP_TYPE_17B	0x02
#define SD_NO_CHECK_CRC7	BIT(7)
#define SD_NO_CHECK_WAIT_BUSY	BIT(6)

/* SD_CONFIGURE3 */
#define SD_CMD_RSP_TO		BIT(0)
#define SD_RESP_CHK_EN		BIT(2)

/* SD_TRANSFER */
#define SD_TRANSFER_START	BIT(7)
#define SD_TRANSFER_END		BIT(6)
#define SD_TRANSFER_ERR		BIT(4)
#define SD_SENDCMDGETRSP	0x08

/* SD_STATUS1 */
#define SD_CRC7_ERR		BIT(7)
#define SD_CRC16_ERR		BIT(6)
#define SD_CRC_WRITE_ERR	BIT(5)
#define SD_CRC_TIMEOUT		BIT(1)

/* An R2 comes back as 16 bytes plus a CRC byte, DMA'd into memory. */
#define RTD129X_RSP17_LEN	0x200

struct rtd129x_sdmmc {
	struct device		*dev;
	struct mmc_host		*mmc;

	void __iomem		*sd;	/* the card reader core */

	int			irq;
	struct completion	done;
	u32			isr;

	void			*buf;
	dma_addr_t		buf_phys;

	struct clk		*clk_cr;
	struct clk		*clk_ip;
	struct reset_control	*rstc;
};

/*
 * Writes to the core are posted; the BSP reads a register back before it
 * relies on a write having landed.
 */
static void rtd129x_sync(struct rtd129x_sdmmc *host)
{
	readb(host->sd + SD_CONFIGURE1);
}

static int rtd129x_get_cd(struct mmc_host *mmc)
{
	struct rtd129x_sdmmc *host = mmc_priv(mmc);

	return !!(readb(host->sd + CARD_EXIST) & SD_EXISTENCE);
}

static void rtd129x_set_clock(struct rtd129x_sdmmc *host, unsigned int hz)
{
	u8 cfg1 = readb(host->sd + SD_CONFIGURE1) & ~SD_CLOCK_DIV_MASK;
	u32 ckgen;

	/*
	 * Two controls in series: a divider in SD_CONFIGURE1 and the clock
	 * generator in CR_SD_CKGEN_CTL. The BSP only ever uses the handful of
	 * combinations below, so this does the same rather than computing one.
	 */
	if (hz <= 400000) {
		writeb(cfg1 | SD_CLOCK_DIV_256 | SD_CLOCK_DIV_EN,
		       host->sd + SD_CONFIGURE1);
		ckgen = 0x00002100;
	} else if (hz <= 25000000) {
		writeb(cfg1, host->sd + SD_CONFIGURE1);
		ckgen = 0x00002103;
	} else {
		writeb(cfg1, host->sd + SD_CONFIGURE1);
		ckgen = 0x00002101;
	}

	rtd129x_sync(host);
	writel(ckgen, host->sd + CR_SD_CKGEN_CTL);
	rtd129x_sync(host);
}

static void rtd129x_set_ios(struct mmc_host *mmc, struct mmc_ios *ios)
{
	struct rtd129x_sdmmc *host = mmc_priv(mmc);
	u8 cfg1;

	cfg1 = readb(host->sd + SD_CONFIGURE1) & ~SD_BUS_WIDTH_MASK;
	switch (ios->bus_width) {
	case MMC_BUS_WIDTH_8:
		cfg1 |= SD_BUS_WIDTH_8;
		break;
	case MMC_BUS_WIDTH_4:
		cfg1 |= SD_BUS_WIDTH_4;
		break;
	default:
		cfg1 |= SD_BUS_WIDTH_1;
		break;
	}
	writeb(cfg1, host->sd + SD_CONFIGURE1);
	rtd129x_sync(host);

	if (ios->clock)
		rtd129x_set_clock(host, ios->clock);
}

/*
 * SD_CONFIGURE2 tells the core how many bits to clock in after the command,
 * and whether to check the CRC and the busy line. The mapping is the same one
 * rtsx_pci_sdmmc.c makes from MMC_RSP_*.
 */
static int rtd129x_rsp_cfg(struct mmc_command *cmd, u8 *cfg2, bool *is_r2)
{
	*is_r2 = false;

	if (!(cmd->flags & MMC_RSP_PRESENT)) {
		*cfg2 = SD_RESP_TYPE_NONE | SD_NO_CHECK_CRC7 |
			SD_NO_CHECK_WAIT_BUSY;
		return 0;
	}

	if (cmd->flags & MMC_RSP_136) {
		*cfg2 = SD_RESP_TYPE_17B;
		*is_r2 = true;
		return 0;
	}

	*cfg2 = SD_RESP_TYPE_6B;
	if (!(cmd->flags & MMC_RSP_CRC))
		*cfg2 |= SD_NO_CHECK_CRC7;
	if (!(cmd->flags & MMC_RSP_BUSY))
		*cfg2 |= SD_NO_CHECK_WAIT_BUSY;

	return 0;
}

static void rtd129x_read_response(struct rtd129x_sdmmc *host,
				  struct mmc_command *cmd, bool is_r2)
{
	int i;

	if (!(cmd->flags & MMC_RSP_PRESENT))
		return;

	if (is_r2) {
		/*
		 * The DMA'd buffer holds the 17 response bytes starting at
		 * offset 0: one byte of 0x3f, then the 16 payload bytes. The
		 * MMC core wants the payload as four big-endian words with the
		 * leading byte dropped.
		 */
		const u8 *p = host->buf + 1;

		for (i = 0; i < 4; i++)
			cmd->resp[i] = get_unaligned_be32(p + i * 4);
		return;
	}

	/*
	 * A short response lands back in the command registers: one status
	 * byte then the four payload bytes.
	 */
	cmd->resp[0] = (readb(host->sd + SD_CMD0 + 1) << 24) |
		       (readb(host->sd + SD_CMD0 + 2) << 16) |
		       (readb(host->sd + SD_CMD0 + 3) << 8) |
		        readb(host->sd + SD_CMD0 + 4);
}

static int rtd129x_wait(struct rtd129x_sdmmc *host, unsigned int ms)
{
	if (!wait_for_completion_timeout(&host->done, msecs_to_jiffies(ms)))
		return -ETIMEDOUT;

	if (host->isr & ISR_CARD_ERR)
		return -EILSEQ;

	return 0;
}

static int rtd129x_send_cmd(struct rtd129x_sdmmc *host,
			    struct mmc_command *cmd)
{
	u8 cfg2;
	bool is_r2;
	int ret;

	rtd129x_rsp_cfg(cmd, &cfg2, &is_r2);

	writeb(readb(host->sd + SD_CONFIGURE1), host->sd + SD_CONFIGURE1);
	writeb(cfg2, host->sd + SD_CONFIGURE2);
	writeb(SD_CMD_RSP_TO | SD_RESP_CHK_EN, host->sd + SD_CONFIGURE3);

	writeb(0x40 | cmd->opcode,       host->sd + SD_CMD0);
	writeb((cmd->arg >> 24) & 0xff,  host->sd + SD_CMD0 + 1);
	writeb((cmd->arg >> 16) & 0xff,  host->sd + SD_CMD0 + 2);
	writeb((cmd->arg >> 8) & 0xff,   host->sd + SD_CMD0 + 3);
	writeb(cmd->arg & 0xff,          host->sd + SD_CMD0 + 4);
	writeb(0,                        host->sd + SD_CMD0 + 5);

	reinit_completion(&host->done);
	host->isr = 0;

	if (is_r2) {
		/*
		 * The core returns a 136-bit response by DMA, not in the
		 * command registers.
		 */
		memset(host->buf, 0, RTD129X_RSP17_LEN);

		writeb(RTD129X_RSP17_LEN & 0xff, host->sd + SD_BYTE_CNT_L);
		writeb(RTD129X_RSP17_LEN >> 8,   host->sd + SD_BYTE_CNT_H);
		writeb(1, host->sd + SD_BLOCK_CNT_L);
		writeb(0, host->sd + SD_BLOCK_CNT_H);

		writel(host->buf_phys / 8, host->sd + CR_SD_DMA_CTL1);
		writel(1, host->sd + CR_SD_DMA_CTL2);
		writel(RSP17_SEL | DMA_XFER, host->sd + CR_SD_DMA_CTL3);
	}

	writeb(SD_TRANSFER_START | SD_SENDCMDGETRSP, host->sd + SD_TRANSFER);
	rtd129x_sync(host);

	ret = rtd129x_wait(host, 1000);
	if (ret) {
		dev_dbg(host->dev, "cmd%u: %d (isr 0x%02x, status1 0x%02x)\n",
			cmd->opcode, ret, host->isr,
			readb(host->sd + SD_STATUS1));
		return ret;
	}

	rtd129x_read_response(host, cmd, is_r2);

	return 0;
}

static void rtd129x_request(struct mmc_host *mmc, struct mmc_request *mrq)
{
	struct rtd129x_sdmmc *host = mmc_priv(mmc);
	int ret;

	if (!rtd129x_get_cd(mmc)) {
		mrq->cmd->error = -ENOMEDIUM;
		mmc_request_done(mmc, mrq);
		return;
	}

	if (mrq->data) {
		/* Stage 2. */
		dev_dbg(host->dev, "cmd%u: data transfer not implemented\n",
			mrq->cmd->opcode);
		mrq->cmd->error = -ENOTSUPP;
		mmc_request_done(mmc, mrq);
		return;
	}

	ret = rtd129x_send_cmd(host, mrq->cmd);
	mrq->cmd->error = ret;

	mmc_request_done(mmc, mrq);
}

static const struct mmc_host_ops rtd129x_ops = {
	.request	= rtd129x_request,
	.set_ios	= rtd129x_set_ios,
	.get_cd		= rtd129x_get_cd,
};

static irqreturn_t rtd129x_irq(int irq, void *dev_id)
{
	struct rtd129x_sdmmc *host = dev_id;
	u32 isr;

	isr = readl(host->sd + CR_SD_ISR);
	if (!(isr & (ISR_CARD_END | ISR_CARD_ERR | ISR_DMA_DONE)))
		return IRQ_NONE;

	writel(isr, host->sd + CR_SD_ISR);	/* write one to clear */

	host->isr |= isr;
	if (isr & (ISR_CARD_END | ISR_CARD_ERR))
		complete(&host->done);

	return IRQ_HANDLED;
}

static void rtd129x_hw_init(struct rtd129x_sdmmc *host)
{
	/* Select the SD card slot and take the core out of its idle state. */
	writeb(SD_MOD_SEL, host->sd + CARD_SELECT);
	writeb(0xff, host->sd + CR_CARD_STOP);
	writeb(0x3b, host->sd + CARD_CLOCK_EN_CTL);
	rtd129x_sync(host);

	writel(0, host->sd + CR_SD_DMA_CTL3);
	writel(ISR_CARD_END | ISR_CARD_ERR | ISR_DMA_DONE, host->sd + CR_SD_ISR);
	writel(ISR_CARD_END | ISR_CARD_ERR | ISR_DMA_DONE,
	       host->sd + CR_SD_ISREN);
	rtd129x_sync(host);
}

static int rtd129x_sdmmc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct rtd129x_sdmmc *host;
	struct mmc_host *mmc;
	int ret;

	mmc = mmc_alloc_host(sizeof(*host), dev);
	if (!mmc)
		return -ENOMEM;

	host = mmc_priv(mmc);
	host->mmc = mmc;
	host->dev = dev;
	init_completion(&host->done);

	host->sd = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(host->sd)) {
		ret = PTR_ERR(host->sd);
		goto err_free;
	}

	/*
	 * Optional on purpose. There is no clock or reset driver for this SoC
	 * in mainline, and the bootloader has just used this controller to
	 * read the kernel, so it is already running.
	 */
	host->clk_cr = devm_clk_get_optional_enabled(dev, "cr");
	if (IS_ERR(host->clk_cr)) {
		ret = PTR_ERR(host->clk_cr);
		goto err_free;
	}
	host->clk_ip = devm_clk_get_optional_enabled(dev, "sd_ip");
	if (IS_ERR(host->clk_ip)) {
		ret = PTR_ERR(host->clk_ip);
		goto err_free;
	}
	host->rstc = devm_reset_control_get_optional_exclusive(dev, NULL);
	if (IS_ERR(host->rstc)) {
		ret = PTR_ERR(host->rstc);
		goto err_free;
	}
	reset_control_deassert(host->rstc);

	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		goto err_free;

	host->buf = dmam_alloc_coherent(dev, RTD129X_RSP17_LEN,
					&host->buf_phys, GFP_KERNEL);
	if (!host->buf) {
		ret = -ENOMEM;
		goto err_free;
	}

	host->irq = platform_get_irq(pdev, 0);
	if (host->irq < 0) {
		ret = host->irq;
		goto err_free;
	}

	rtd129x_hw_init(host);

	ret = devm_request_irq(dev, host->irq, rtd129x_irq, 0,
			       dev_name(dev), host);
	if (ret)
		goto err_free;

	mmc->ops = &rtd129x_ops;
	mmc->f_min = 400000;
	mmc->f_max = 25000000;		/* stage 1 stays slow on purpose */
	mmc->ocr_avail = MMC_VDD_32_33 | MMC_VDD_33_34;
	/*
	 * The card detect line is not wired to an interrupt here, so the core
	 * has to poll for insertion -- without this a card inserted after boot
	 * is never noticed.
	 */
	mmc->caps = MMC_CAP_4_BIT_DATA | MMC_CAP_NEEDS_POLL;
	mmc->max_blk_size = 512;
	mmc->max_blk_count = 1;
	mmc->max_segs = 1;
	mmc->max_seg_size = 512;
	mmc->max_req_size = 512;

	platform_set_drvdata(pdev, host);

	ret = mmc_add_host(mmc);
	if (ret)
		goto err_free;

	dev_info(dev, "RTD129x SD host, card %s\n",
		 rtd129x_get_cd(mmc) ? "present" : "absent");

	return 0;

err_free:
	mmc_free_host(mmc);
	return ret;
}

static void rtd129x_sdmmc_remove(struct platform_device *pdev)
{
	struct rtd129x_sdmmc *host = platform_get_drvdata(pdev);

	mmc_remove_host(host->mmc);
	writel(0, host->sd + CR_SD_ISREN);
	mmc_free_host(host->mmc);
}

static const struct of_device_id rtd129x_sdmmc_of_match[] = {
	{ .compatible = "realtek,rtd1295-sdmmc" },
	{ }
};
MODULE_DEVICE_TABLE(of, rtd129x_sdmmc_of_match);

static struct platform_driver rtd129x_sdmmc_driver = {
	.probe	= rtd129x_sdmmc_probe,
	.remove	= rtd129x_sdmmc_remove,
	.driver	= {
		.name		= "rtd129x-sdmmc",
		.of_match_table	= rtd129x_sdmmc_of_match,
	},
};
module_platform_driver(rtd129x_sdmmc_driver);

MODULE_DESCRIPTION("Realtek RTD129x SD/MMC host");
MODULE_LICENSE("GPL");
