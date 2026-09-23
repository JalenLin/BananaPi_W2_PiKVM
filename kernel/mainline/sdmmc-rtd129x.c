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
 * Card detection, commands and responses -- including the 136-bit R2, which
 * this core returns by DMA rather than in registers -- and data transfer,
 * one 512-byte block per request through a coherent bounce buffer. Multi-block
 * transfers (the core's auto-CMD12 modes) are not used yet, which is correct
 * but slow.
 */

#include <linux/bitops.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/mmc/host.h>
#include <linux/mmc/mmc.h>
#include <linux/mmc/sd.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/reset.h>
#include <linux/scatterlist.h>
#include <linux/sizes.h>
#include <linux/unaligned.h>

/* --- the card reader core, reg[1] ------------------------------------- */

#define CR_SD_DMA_CTL1		0x0004	/* buffer address / 8 */
#define CR_SD_DMA_CTL2		0x0008	/* length, in 8-byte units */
#define CR_SD_DMA_CTL3		0x000c	/* direction and go */
#define CR_SD_ISR		0x0024
#define CR_SD_ISREN		0x0028
#define CR_SD_PAD_CTL		0x0074	/* 0 = 3.3 V signalling */
#define CR_SD_CKGEN_CTL		0x0078
#define CR_CARD_STOP		0x0103
#define CR_CARD_OE		0x0104
#define CARD_SELECT		0x010e
#define CARD_EXIST		0x011f
#define CR_SD_INT_EN		0x0120	/* card insert/remove interrupt enable */
#define CARD_INT_PEND		0x0121	/* ... and pending, write one to clear */
#define CARD_CLOCK_EN_CTL	0x0129

#define SD_CONFIGURE1		0x0180
#define SD_CONFIGURE2		0x0181
#define SD_CONFIGURE3		0x0182
#define SD_STATUS1		0x0183
#define SD_STATUS2		0x0184
#define SD_BUS_STATUS		0x0185
#define SD_CMD_MODE		0x0186
#define SD_SAMPLE_POINT_CTL	0x0187
#define SD_PUSH_POINT_CTL	0x0188
#define SD_CMD0			0x0189	/* command out, response back */
#define SD_BYTE_CNT_L		0x018f
#define SD_BYTE_CNT_H		0x0190
#define SD_BLOCK_CNT_L		0x0191
#define SD_BLOCK_CNT_H		0x0192
#define SD_TRANSFER		0x0193

/* CR_SD_DMA_CTL3 */
#define DMA_XFER		BIT(0)
#define DDR_WR			BIT(1)	/* DMA writes to DDR, i.e. card -> memory */
#define RSP17_SEL		BIT(4)	/* this transfer is an R2 response */
#define RSP64_SEL		BIT(5)	/* a short read: SCR, SD status, switch */

/*
 * CR_SD_ISR / CR_SD_ISREN. Bit 0 is not a status bit but the value to write:
 * a write sets every other bit given in the mask to bit 0's value. So
 * 0x07 enables END and ERR, 0x16 clears END, ERR and DMA_DONE -- which is
 * both "disable all" for ISREN and "acknowledge all" for ISR. Writing back
 * what was read, as a plain W1C register would want, re-asserts it instead.
 */
#define ISR_WRITE_DATA		BIT(0)
#define ISR_CARD_END		BIT(1)
#define ISR_CARD_ERR		BIT(2)
#define ISR_DMA_DONE		BIT(4)
#define ISR_ALL			(ISR_CARD_END | ISR_CARD_ERR | ISR_DMA_DONE)

/* CARD_EXIST, CR_SD_INT_EN, CARD_INT_PEND -- rtsx's XD_INT/MS_INT/SD_INT */
#define SD_EXISTENCE		BIT(2)
#define CARD_INT_SD		BIT(2)
#define CARD_INT_ALL		(BIT(4) | BIT(3) | BIT(2))

/* CARD_SELECT, CR_CARD_OE, CARD_CLOCK_EN_CTL */
#define SD_MOD_SEL		0x02
#define SD_MOD_OE		BIT(2)
#define SD_MOD_CLK_EN		BIT(2)

/* SD_CONFIGURE1 */
#define SD_CFG1_BUS_WIDTH_MASK	0x03
#define SD_CFG1_BUS_WIDTH_1		0x00
#define SD_CFG1_BUS_WIDTH_4		0x01
#define SD_CFG1_BUS_WIDTH_8		0x02
#define SD_CLOCK_DIV_MASK	(0x03 << 6)
#define SD_CLOCK_DIV_256	BIT(6)
#define SD_CLOCK_DIV_EN		BIT(7)

/*
 * SD_CONFIGURE2. Names from the BSP's rtk-sdmmc-reg.h. Note that bit 7 stops
 * the core *generating* the CRC7 of the outgoing command, which no card will
 * accept; the bit that stops it *checking* the response's CRC7 is bit 2.
 */
#define SD_CRC7_CAL_DIS		BIT(7)
#define SD_CRC16_CAL_DIS	BIT(6)
#define SD_WAIT_BUSY_EN		BIT(3)
#define SD_CRC7_CHK_DIS		BIT(2)
#define SD_RESP_TYPE_NONE	0x00
#define SD_RESP_TYPE_6B		0x01
#define SD_RESP_TYPE_17B	0x02

/* SD_CONFIGURE3 */
#define SD_CMD_RSP_TO		BIT(0)
#define SD_RESP_CHK_EN		BIT(2)

#define SD_CMD5			0x018e	/* the 17th byte of an R2 */

/* SD_TRANSFER */
#define SD_TRANSFER_START	BIT(7)
#define SD_TRANSFER_END		BIT(6)
#define SD_TRANSFER_IDLE	BIT(5)
#define SD_TRANSFER_ERR		BIT(4)
#define SD_AUTOWRITE1		0x09	/* command, data out, then CMD12 */
#define SD_AUTOWRITE2		0x0a	/* command, then data out */
#define SD_NORMALREAD		0x0c	/* command, then a short data read */
#define SD_AUTOREAD1		0x0d	/* command, data in, then CMD12 */
#define SD_AUTOREAD2		0x0e	/* command, then data in */
#define SD_SENDCMDGETRSP	0x08

/* SD_STATUS1 */
#define SD_CRC7_ERR		BIT(7)
#define SD_CRC16_ERR		BIT(6)
#define SD_CRC_WRITE_ERR	BIT(5)
#define SD_CRC_TIMEOUT		BIT(1)

/*
 * One bounce buffer serves both the R2 response DMA and block data. The core
 * DMAs a whole 512-byte unit even for short results. Data requests are capped
 * at the buffer size, so a request is always one DMA.
 */
#define RTD129X_RSP17_LEN	0x200
#define RTD129X_BUF_LEN		SZ_64K

struct rtd129x_sdmmc {
	struct device		*dev;
	struct mmc_host		*mmc;

	void __iomem		*sd;	/* the card reader core */

	int			irq;
	struct completion	done;
	u32			isr;
	u32			wait_for;	/* ISR bit that ends this command */

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

	cfg1 = readb(host->sd + SD_CONFIGURE1) & ~SD_CFG1_BUS_WIDTH_MASK;
	switch (ios->bus_width) {
	case MMC_BUS_WIDTH_8:
		cfg1 |= SD_CFG1_BUS_WIDTH_8;
		break;
	case MMC_BUS_WIDTH_4:
		cfg1 |= SD_CFG1_BUS_WIDTH_4;
		break;
	default:
		cfg1 |= SD_CFG1_BUS_WIDTH_1;
		break;
	}
	writeb(cfg1, host->sd + SD_CONFIGURE1);
	rtd129x_sync(host);

	if (ios->clock)
		rtd129x_set_clock(host, ios->clock);
}

/*
 * SD_CONFIGURE2/3 for a command. This is the rule behind the BSP's per-opcode
 * table in rtk_sdmmc_set_rspparam(): CRC16 calculation off for anything that
 * moves no data, the response length, CRC7 checking off where the response
 * has no CRC (R3), busy-waiting for R1b, and response checking in CONFIGURE3
 * only where there is a CRC to check. CMD0 and CMD8 take CONFIGURE3 = 0 there
 * too.
 */
static void rtd129x_rsp_cfg(struct mmc_command *cmd, u8 *cfg2, u8 *cfg3,
			    bool *is_r2)
{
	*is_r2 = false;

	if (!(cmd->flags & MMC_RSP_PRESENT)) {
		*cfg2 = 0x74;			/* the BSP's value for CMD0 */
		*cfg3 = 0;
		return;
	}

	if (cmd->flags & MMC_RSP_136) {
		*cfg2 = SD_CRC16_CAL_DIS | SD_RESP_TYPE_17B;
		*cfg3 = SD_RESP_CHK_EN | SD_CMD_RSP_TO;
		*is_r2 = true;
		return;
	}

	*cfg2 = SD_CRC16_CAL_DIS | SD_RESP_TYPE_6B;
	if (!(cmd->flags & MMC_RSP_CRC))
		*cfg2 |= SD_CRC7_CHK_DIS;
	if (cmd->flags & MMC_RSP_BUSY)
		*cfg2 |= SD_WAIT_BUSY_EN;

	if ((cmd->flags & MMC_RSP_CRC) && cmd->opcode != SD_SEND_IF_COND)
		*cfg3 = SD_RESP_CHK_EN | SD_CMD_RSP_TO;
	else
		*cfg3 = 0;
}

static void rtd129x_read_response(struct rtd129x_sdmmc *host,
				  struct mmc_command *cmd, bool is_r2)
{
	int i;

	if (!(cmd->flags & MMC_RSP_PRESENT))
		return;

	if (is_r2) {
		/*
		 * Sixteen of the seventeen response bytes are DMA'd: the 0x3f
		 * header, then 15 payload bytes. The last one -- CRC7 and the
		 * end bit -- stays in SD_CMD5. The MMC core wants the payload
		 * as four big-endian words. This is what the BSP's
		 * rtk_sdmmc_read_rsp() + rtk_sdmmc_swap_data() come to.
		 */
		const u8 *p = host->buf;
		u8 last[4] = { p[13], p[14], p[15], readb(host->sd + SD_CMD5) };

		for (i = 0; i < 3; i++)
			cmd->resp[i] = get_unaligned_be32(p + 1 + i * 4);
		cmd->resp[3] = get_unaligned_be32(last);
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

/*
 * Abort whatever the core is doing and return it to idle, as the BSP's
 * rtk_sdmmc_reset() does. Without this a transfer that never finished leaves
 * SD_TRANSFER started, and every later command times out.
 */
static void rtd129x_reset(struct rtd129x_sdmmc *host)
{
	writel(0, host->sd + CR_SD_DMA_CTL3);
	writel(ISR_ALL, host->sd + CR_SD_ISREN);
	writel(ISR_ALL, host->sd + CR_SD_ISR);
	writeb(0, host->sd + SD_TRANSFER);
	writeb(0xff, host->sd + CR_CARD_STOP);
	writeb(0x00, host->sd + CR_CARD_STOP);
	rtd129x_sync(host);
}

static int rtd129x_wait(struct rtd129x_sdmmc *host, unsigned int ms)
{
	unsigned long deadline;
	u8 status1, xfer;

	if (!wait_for_completion_timeout(&host->done, msecs_to_jiffies(ms)))
		return -ETIMEDOUT;

	/*
	 * The interrupt can come before the transfer state machine is done:
	 * DMA_DONE in particular fires while the core is still clocking the
	 * rest of the block. The BSP's rtk_sdmmc_int_wait() polls for END and
	 * IDLE before it touches the core again.
	 */
	deadline = jiffies + msecs_to_jiffies(300);
	for (;;) {
		rtd129x_sync(host);
		xfer = readb(host->sd + SD_TRANSFER);
		if (xfer & SD_TRANSFER_ERR)
			break;
		if ((xfer & (SD_TRANSFER_END | SD_TRANSFER_IDLE)) ==
		    (SD_TRANSFER_END | SD_TRANSFER_IDLE))
			break;
		if (time_after(jiffies, deadline))
			return -ETIMEDOUT;
		udelay(10);
	}

	if (!(host->isr & ISR_CARD_ERR) && !(xfer & SD_TRANSFER_ERR))
		return 0;

	/* No response at all is how an SD 1.x card answers CMD8. */
	status1 = readb(host->sd + SD_STATUS1);
	if (status1 & SD_CRC_TIMEOUT)
		return -ETIMEDOUT;
	return -EILSEQ;
}

static int rtd129x_send_cmd(struct rtd129x_sdmmc *host,
			    struct mmc_command *cmd)
{
	u8 cfg2, cfg3;
	bool is_r2;
	int ret;

	rtd129x_rsp_cfg(cmd, &cfg2, &cfg3, &is_r2);

	writeb(cfg2, host->sd + SD_CONFIGURE2);
	writeb(cfg3, host->sd + SD_CONFIGURE3);

	writeb(0x40 | cmd->opcode,       host->sd + SD_CMD0);
	writeb((cmd->arg >> 24) & 0xff,  host->sd + SD_CMD0 + 1);
	writeb((cmd->arg >> 16) & 0xff,  host->sd + SD_CMD0 + 2);
	writeb((cmd->arg >> 8) & 0xff,   host->sd + SD_CMD0 + 3);
	writeb(cmd->arg & 0xff,          host->sd + SD_CMD0 + 4);
	writeb(0,                        host->sd + SD_CMD0 + 5);

	/*
	 * Acknowledge anything left over, then enable only what ends this
	 * command: END and ERR normally, DMA_DONE and ERR when the result
	 * arrives by DMA -- END fires before the data has landed. This is the
	 * BSP's rtk_sdmmc_cpu_wait().
	 */
	writel(ISR_ALL, host->sd + CR_SD_ISR);
	writel(ISR_ALL, host->sd + CR_SD_ISREN);
	reinit_completion(&host->done);
	host->isr = 0;
	host->wait_for = is_r2 ? ISR_DMA_DONE : ISR_CARD_END;
	writel(ISR_WRITE_DATA | ISR_CARD_ERR | host->wait_for,
	       host->sd + CR_SD_ISREN);

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
		writel(RSP17_SEL | DDR_WR | DMA_XFER, host->sd + CR_SD_DMA_CTL3);
	}

	writeb(SD_TRANSFER_START | SD_SENDCMDGETRSP, host->sd + SD_TRANSFER);
	rtd129x_sync(host);

	ret = rtd129x_wait(host, 1000);
	if (ret) {
		dev_dbg(host->dev, "cmd%u: %d (isr 0x%02x, status1 0x%02x, xfer 0x%02x)\n",
			cmd->opcode, ret, host->isr,
			readb(host->sd + SD_STATUS1),
			readb(host->sd + SD_TRANSFER));
		rtd129x_reset(host);
		return ret;
	}

	rtd129x_read_response(host, cmd, is_r2);

	return 0;
}

/*
 * A command with data, as the BSP's rtk_sdmmc_stream_cmd() does it: the
 * command goes out, then the core moves the data by DMA. Full 512-byte blocks
 * use the "command then data" modes; the short reads the MMC core issues
 * during setup (SCR, SD status, switch, number of written blocks) use
 * NORMALREAD with RSP64_SEL and a 64-byte count. Below 64 bytes the core still
 * clocks 64, so the CRC16 cannot match and is not checked -- that is the
 * BSP's 0x41 for ACMD51.
 */
static int rtd129x_xfer(struct rtd129x_sdmmc *host, struct mmc_request *mrq)
{
	struct mmc_command *cmd = mrq->cmd;
	struct mmc_data *data = mrq->data;
	bool read = data->flags & MMC_DATA_READ;
	size_t len = data->blksz * data->blocks;
	u16 byte_cnt, blk_cnt;
	u32 dma3;
	u8 cfg2, tm;
	int ret;

	if (len > RTD129X_BUF_LEN)
		return -EINVAL;

	/*
	 * The data path does not work behind the /256 divider: a short read
	 * at 400 kHz lands as zeros and the core never flags END. The BSP
	 * never tries -- it moves to 6.2 MHz as soon as CMD7 selects the card
	 * (rtk_sdmmc_request()). Data only flows once a card is selected, so
	 * leaving the identification clock behind here is within the spec.
	 */
	if (host->mmc->ios.clock <= 400000)
		rtd129x_set_clock(host, 6200000);

	if (data->blksz == 512) {
		byte_cnt = 512;
		blk_cnt = data->blocks;
		dma3 = 0;
		/*
		 * The multi-block modes send CMD12 themselves, so mrq->stop
		 * is never issued separately.
		 */
		if (read)
			tm = data->blocks > 1 ? SD_AUTOREAD1 : SD_AUTOREAD2;
		else
			tm = data->blocks > 1 ? SD_AUTOWRITE1 : SD_AUTOWRITE2;
		cfg2 = SD_RESP_TYPE_6B;
	} else {
		if (!read)
			return -EINVAL;
		byte_cnt = 0x40;
		blk_cnt = 1;
		dma3 = RSP64_SEL;
		tm = SD_NORMALREAD;
		cfg2 = SD_RESP_TYPE_6B;
		if (data->blksz < 0x40)
			cfg2 |= SD_CRC16_CAL_DIS;
	}

	if (!read)
		sg_copy_to_buffer(data->sg, data->sg_len, host->buf, len);

	writeb(cfg2, host->sd + SD_CONFIGURE2);
	writeb(SD_RESP_CHK_EN | SD_CMD_RSP_TO, host->sd + SD_CONFIGURE3);

	writeb(0x40 | cmd->opcode,      host->sd + SD_CMD0);
	writeb((cmd->arg >> 24) & 0xff, host->sd + SD_CMD0 + 1);
	writeb((cmd->arg >> 16) & 0xff, host->sd + SD_CMD0 + 2);
	writeb((cmd->arg >> 8) & 0xff,  host->sd + SD_CMD0 + 3);
	writeb(cmd->arg & 0xff,         host->sd + SD_CMD0 + 4);
	writeb(0,                       host->sd + SD_CMD0 + 5);

	writeb(byte_cnt & 0xff, host->sd + SD_BYTE_CNT_L);
	writeb(byte_cnt >> 8,   host->sd + SD_BYTE_CNT_H);
	writeb(blk_cnt & 0xff,  host->sd + SD_BLOCK_CNT_L);
	writeb(blk_cnt >> 8,    host->sd + SD_BLOCK_CNT_H);

	writel(host->buf_phys / 8, host->sd + CR_SD_DMA_CTL1);
	writel(blk_cnt, host->sd + CR_SD_DMA_CTL2);
	writel(dma3 | (read ? DDR_WR : 0) | DMA_XFER, host->sd + CR_SD_DMA_CTL3);

	/* Reads are done when the data has landed; writes when the card acks. */
	writel(ISR_ALL, host->sd + CR_SD_ISR);
	writel(ISR_ALL, host->sd + CR_SD_ISREN);
	reinit_completion(&host->done);
	host->isr = 0;
	host->wait_for = read ? ISR_DMA_DONE : ISR_CARD_END;
	writel(ISR_WRITE_DATA | ISR_CARD_ERR | host->wait_for,
	       host->sd + CR_SD_ISREN);

	writeb(SD_TRANSFER_START | tm, host->sd + SD_TRANSFER);
	rtd129x_sync(host);

	ret = rtd129x_wait(host, 2000);
	writel(0, host->sd + CR_SD_DMA_CTL3);
	if (ret) {
		dev_dbg(host->dev, "cmd%u data %zu%s: %d (isr 0x%02x, status1 0x%02x, xfer 0x%02x)\n",
			cmd->opcode, len, read ? "r" : "w", ret, host->isr,
			readb(host->sd + SD_STATUS1),
			readb(host->sd + SD_TRANSFER));
		rtd129x_reset(host);
		return ret;
	}

	/*
	 * After the automatic CMD12 the response registers hold its R1b, not
	 * this command's; the BSP leaves the response alone in those modes.
	 */
	if (tm != SD_AUTOREAD1 && tm != SD_AUTOWRITE1)
		rtd129x_read_response(host, cmd, false);
	if (read && data->blksz != 512)
		dev_dbg(host->dev, "cmd%u short read %u: %*ph\n",
			cmd->opcode, data->blksz, 64, host->buf);
	if (read)
		sg_copy_from_buffer(data->sg, data->sg_len, host->buf, len);
	data->bytes_xfered = len;

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
		ret = rtd129x_xfer(host, mrq);
		mrq->cmd->error = ret;
		mrq->data->error = ret;
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
	u8 pend;

	/*
	 * Card insert/remove shares the line. It is disabled at init because
	 * detection is polled, but if it ever latches, acknowledge it rather
	 * than leave a level interrupt asserted.
	 */
	pend = readb(host->sd + CARD_INT_PEND) & CARD_INT_ALL;
	if (pend)
		writeb(pend, host->sd + CARD_INT_PEND);

	isr = readl(host->sd + CR_SD_ISR) & ISR_ALL;
	if (!isr)
		return pend ? IRQ_HANDLED : IRQ_NONE;

	/* Bit 0 clear: this clears the given bits, see ISR_WRITE_DATA. */
	writel(isr, host->sd + CR_SD_ISR);

	host->isr |= isr;
	if (isr & (host->wait_for | ISR_CARD_ERR))
		complete(&host->done);

	return IRQ_HANDLED;
}

static void rtd129x_hw_init(struct rtd129x_sdmmc *host)
{
	/*
	 * The BSP's rtk_sdmmc_hw_reset(). CARD_CLOCK_EN_CTL matters most: the
	 * SD_* registers (0x180 up) are clocked by the SD module, and with its
	 * clock off they read back their last value and silently drop every
	 * write -- the CARD_* registers below 0x180 and the CR_SD_* ones below
	 * 0x100 keep working, which makes it look like a mapping problem. An
	 * earlier version of this driver wrote 0x3b here, copied from the
	 * BSP's card-removal path, and switched that clock off itself.
	 */
	writeb(0xff, host->sd + CR_CARD_STOP);		/* stop, go idle */
	writeb(0x00, host->sd + CR_CARD_STOP);
	writeb(SD_MOD_SEL, host->sd + CARD_SELECT);
	writeb(SD_MOD_OE, host->sd + CR_CARD_OE);
	writeb(SD_MOD_CLK_EN, host->sd + CARD_CLOCK_EN_CTL);
	writeb(0xd0, host->sd + SD_CONFIGURE1);		/* /256, 1 bit, FIFO reset */
	writeb(0x00, host->sd + SD_STATUS2);
	writel(0, host->sd + CR_SD_PAD_CTL);		/* 3.3 V */
	writeb(0x00, host->sd + SD_SAMPLE_POINT_CTL);
	writeb(0x00, host->sd + SD_PUSH_POINT_CTL);
	rtd129x_sync(host);

	/*
	 * Everything acknowledged and disabled (see ISR_WRITE_DATA); each
	 * command enables what it waits for. u-boot has just used this core
	 * and may have left sources enabled and pending.
	 */
	writel(0, host->sd + CR_SD_DMA_CTL3);
	writel(ISR_ALL, host->sd + CR_SD_ISR);
	writel(ISR_ALL, host->sd + CR_SD_ISREN);

	/*
	 * The card insert/remove interrupt drives the same GIC line, and
	 * u-boot leaves it enabled -- with a card in the slot it is pending
	 * from the moment the handler is installed, and the kernel disables
	 * the line after 100000 unhandled interrupts:
	 *
	 *   irq 17: nobody cared ... Disabling IRQ #17
	 *
	 * (CR_SD_INT_EN = 0x04, CARD_INT_PEND = 0x04 read with devmem.)
	 * Detection is polled here, so switch it off and clear what is
	 * pending the way rtsx_usb.c does. The BSP never touches either
	 * register; its handler returns IRQ_HANDLED unconditionally, which
	 * hides a storm rather than stopping one.
	 *
	 * Whether CR_SD_INT_EN takes a plain value or the ISR-style mask with
	 * a data bit is not documented, so write zero and, if the SD bit is
	 * still set, clear it the other way.
	 */
	writeb(0, host->sd + CR_SD_INT_EN);
	if (readb(host->sd + CR_SD_INT_EN) & CARD_INT_SD)
		writeb(CARD_INT_SD, host->sd + CR_SD_INT_EN);
	writeb(CARD_INT_ALL, host->sd + CARD_INT_PEND);
	rtd129x_sync(host);
	dev_dbg(host->dev, "card int en 0x%02x pend 0x%02x\n",
		readb(host->sd + CR_SD_INT_EN), readb(host->sd + CARD_INT_PEND));
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

	host->buf = dmam_alloc_coherent(dev, RTD129X_BUF_LEN,
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
	mmc->max_blk_count = RTD129X_BUF_LEN / 512;
	mmc->max_segs = 128;		/* gathered into the bounce buffer */
	mmc->max_seg_size = RTD129X_BUF_LEN;
	mmc->max_req_size = RTD129X_BUF_LEN;

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
	writel(ISR_ALL, host->sd + CR_SD_ISREN);	/* disable all */
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
