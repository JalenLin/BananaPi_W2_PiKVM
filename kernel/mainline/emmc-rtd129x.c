// SPDX-License-Identifier: GPL-2.0
/*
 * Realtek RTD1295/RTD1296 eMMC host
 *
 * The controller is a Synopsys DesignWare MSHC (VERID 2.70a, internal DMAC,
 * 32-bit descriptors) with a Realtek wrapper at +0x400. Commands work as
 * dw_mmc expects; its DMAC and what is around it do not:
 *
 *  - The bus bridge (SB2) holds CPU writes back from DDR until it is told
 *    to sync. Descriptors the DMAC fetches without that are the previous
 *    ones.
 *  - After the last descriptor the DMAC still follows the next pointer and
 *    takes whatever descriptor it finds there that says OWN -- and it never
 *    hands a descriptor back, so old ones keep saying it. It stays parked
 *    there, and DBADDR does not move it.
 *  - Data-transfer-over comes when the card is done; on a read the DMAC is
 *    still emptying the FIFO into DDR then.
 *  - The data-busy status does not clear after an R1b command.
 *
 * Each of these turned into DMA to stale buffers, freed memory or random
 * addresses, i.e. memory corruption and resets of the SoC. So this driver
 * does it the way Realtek's BSP and u-boot do (rtkemmc.c), plus what they
 * get away without: fresh descriptors for every transfer behind a sync,
 * ended on one the DMAC does not own; the DMAC restarted from DBADDR for
 * every transfer; reads complete only once the DMAC has moved every byte
 * and the wrapper says DMA_DONE; and busy left to the MMC core's CMD13
 * polling.
 *
 * The boot loader -- the SD and the eMMC builds of the BSP u-boot alike --
 * has set up the pins, pad drive and the 1.8 V I/O LDO, and this relies on
 * that. It also leaves the EMMC PLL at 200 MHz with the phases it tuned for
 * HS200; the driver sets it to 100 MHz with the phases at 0, as the BSP
 * does before it starts on the card. The core divides that: High Speed at
 * 50 MHz, 8 bits, SDR. HS200 would need the BSP's phase tuning.
 */
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/mmc/host.h>
#include <linux/mmc/mmc.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/mfd/syscon.h>
#include <linux/regmap.h>
#include <linux/workqueue.h>

/* DesignWare core */
#define EM_CTRL			0x000
#define  CTRL_RESET		BIT(0)
#define  CTRL_FIFO_RESET	BIT(1)
#define  CTRL_DMA_RESET		BIT(2)
#define  CTRL_INT_ENABLE	BIT(4)
#define  CTRL_USE_IDMAC		BIT(25)
#define  CTRL_RESETS		(CTRL_RESET | CTRL_FIFO_RESET | CTRL_DMA_RESET)
#define EM_PWREN		0x004
#define EM_CLKDIV		0x008
#define EM_CLKSRC		0x00c
#define EM_CLKENA		0x010
#define EM_TMOUT		0x014
#define EM_CTYPE		0x018
#define  CTYPE_4BIT		BIT(0)
#define  CTYPE_8BIT		BIT(16)
#define EM_BLKSIZ		0x01c
#define EM_BYTCNT		0x020
#define EM_INTMASK		0x024
#define EM_CMDARG		0x028
#define EM_CMD			0x02c
#define  CMD_START		BIT(31)
#define  CMD_USE_HOLD_REG	BIT(29)
#define  CMD_UPD_CLK		BIT(21)
#define  CMD_INIT		BIT(15)
#define  CMD_STOP		BIT(14)
#define  CMD_AUTO_STOP		BIT(12)
#define  CMD_PRV_DAT_WAIT	BIT(13)
#define  CMD_DAT_WR		BIT(10)
#define  CMD_DAT_EXP		BIT(9)
#define  CMD_RESP_CRC		BIT(8)
#define  CMD_RESP_LONG		BIT(7)
#define  CMD_RESP_EXP		BIT(6)
#define EM_RESP0		0x030
#define EM_RESP1		0x034	/* the auto-stop's response */
#define EM_MINTSTS		0x040
#define EM_RINTSTS		0x044
#define  INT_RE			BIT(1)
#define  INT_CD			BIT(2)
#define  INT_DTO		BIT(3)
#define  INT_RCRC		BIT(6)
#define  INT_DCRC		BIT(7)
#define  INT_RTO		BIT(8)
#define  INT_DRTO		BIT(9)
#define  INT_HTO		BIT(10)
#define  INT_FRUN		BIT(11)
#define  INT_HLE		BIT(12)
#define  INT_SBE		BIT(13)
#define  INT_ACD		BIT(14)	/* auto-stop done */
#define  INT_EBE		BIT(15)
#define  INT_CMD_ERR		(INT_RE | INT_RCRC | INT_RTO | INT_HLE)
#define  INT_DATA_ERR		(INT_DCRC | INT_DRTO | INT_HTO | INT_FRUN | \
				 INT_SBE | INT_EBE)
#define EM_STATUS		0x048
#define EM_TBBCNT		0x060	/* bytes between memory and the FIFO */
#define EM_FIFOTH		0x04c
#define EM_UHS_REG		0x074
#define  UHS_18V		BIT(0)
#define EM_BMOD			0x080
#define  BMOD_SWR		BIT(0)
#define  BMOD_DE		BIT(7)
#define EM_DBADDR		0x088
#define EM_IDSTS		0x08c
#define EM_IDINTEN		0x090
#define EM_CARDTHRCTL		0x100

/* Realtek wrapper */
#define EM_ISR			0x424
#define  ISR_WRITE_DATA		BIT(0)	/* 1: set the bits given, 0: clear */
#define  ISR_DMA_DONE		BIT(1)
#define  ISR_DMA_INT_MASK	BIT(2)
#define  ISR_DESC_INT_MASK	BIT(3)
#define  ISR_IP_INT_MASK	BIT(4)
#define EM_CP			0x41c
#define EM_SWC_SEL		0x4d4

/* 32-bit IDMAC descriptors, chained */
struct em_desc {
	__le32 des0;
#define DES0_OWN		BIT(31)
#define DES0_CH			BIT(4)
#define DES0_FS			BIT(3)
#define DES0_LD			BIT(2)
#define DES0_DIC		BIT(1)
	__le32 des1;		/* buffer size */
	__le32 des2;		/* buffer address */
	__le32 des3;		/* next descriptor */
};

#define EM_DESC_LEN		4096		/* per descriptor, as the BSP */
#define EM_NR_DESC		256
#define EM_TIMEOUT_MS		2000

/* SB2, the bus bridge: writing SYNC drains the CPU's posted writes to DDR */
#define SB2_SYNC		0x020

/* CRT: the EMMC PLL */
#define CRT_PLL_EMMC1		0x1f0	/* bits 7:3 TX phase, 12:8 RX phase */
#define CRT_PLL_EMMC3		0x1f8	/* bits 31:16 frequency code */
#define CRT_PLL_EMMC4		0x1fc	/* bit 0 output enable */
#define EM_PLL_100MHZ		0x57	/* the BSP's codes: 0x46 80, 0xa6 200 MHz */
#define EM_DUMMY_SYS		0x42c
#define EM_DDR_REG		0x10c
#define EM_DQS_CTRL1		0x498

enum em_state { ST_IDLE, ST_CMD, ST_DATA, ST_STOP };

struct em_host {
	struct device *dev;
	struct mmc_host *mmc;
	void __iomem *base;
	unsigned long bus_hz;
	u32 clock;			/* the card clock set, Hz */

	struct regmap *sb2;
	struct regmap *crt;
	struct clk *clk_ip;
	struct em_desc *desc;
	dma_addr_t desc_dma;

	spinlock_t lock;
	struct mmc_request *mrq;
	struct mmc_command *cmd;	/* the command in flight */
	enum em_state state;
	struct mmc_request *done;	/* to report once the lock is dropped */
	u32 rint;			/* status collected by the hard irq */
	u32 got, need;			/* data phase: seen, and needed to end */
	u32 last_rint;
	int sg_count;
	int nr_desc;
	struct delayed_work timeout;
};

static u32 em_read(struct em_host *h, u32 reg)
{
	return readl(h->base + reg);
}

static void em_write(struct em_host *h, u32 reg, u32 val)
{
	writel(val, h->base + reg);
}

static int em_reset(struct em_host *h, u32 bits)
{
	u32 val;

	em_write(h, EM_CTRL, em_read(h, EM_CTRL) | bits);
	return readl_poll_timeout_atomic(h->base + EM_CTRL, val,
					 !(val & bits), 1, 500000);
}

/* Send a command that only updates the card clock */
static int em_update_clock(struct em_host *h)
{
	u32 val;

	em_write(h, EM_CMD, CMD_START | CMD_USE_HOLD_REG | CMD_UPD_CLK);
	return readl_poll_timeout_atomic(h->base + EM_CMD, val,
					 !(val & CMD_START), 1, 500000);
}

static void em_set_clock(struct em_host *h, unsigned int clock)
{
	u32 div;

	if (clock == h->clock)
		return;
	/* the core divides by 2 * div; 0 passes the clock through */
	div = clock ? DIV_ROUND_UP(h->bus_hz, 2 * clock) : 0;
	if (clock && clock >= h->bus_hz)
		div = 0;

	em_write(h, EM_CLKENA, 0);
	em_update_clock(h);
	if (!clock) {
		h->clock = 0;
		return;
	}
	em_write(h, EM_CLKDIV, div);
	em_update_clock(h);
	/* enable; bit 16 would gate the clock when idle */
	em_write(h, EM_CLKENA, BIT(0));
	em_update_clock(h);
	h->clock = clock;
	dev_dbg(h->dev, "card clock %u Hz (div %u)\n",
		div ? (u32)(h->bus_hz / (2 * div)) : (u32)h->bus_hz, div);
}

/*
 * The boot loader leaves the PLL at 200 MHz with the TX and RX phases it
 * tuned for HS200. As the BSP does before it starts on the card: phases to
 * 0, and the PLL to a rate the core's divider makes the slower modes from
 * -- with the core's clock stopped meanwhile, as it does it.
 */
static void em_pll_init(struct em_host *h)
{
	u32 val;

	em_write(h, EM_DDR_REG, 0);
	writel(0, h->base + EM_DQS_CTRL1);

	clk_disable_unprepare(h->clk_ip);
	regmap_write(h->crt, CRT_PLL_EMMC1, 3);
	regmap_read(h->crt, CRT_PLL_EMMC4, &val);
	regmap_write(h->crt, CRT_PLL_EMMC4, val & 0x6);
	regmap_read(h->crt, CRT_PLL_EMMC3, &val);
	regmap_write(h->crt, CRT_PLL_EMMC3, (val & 0xffff) | EM_PLL_100MHZ << 16);
	regmap_read(h->crt, CRT_PLL_EMMC4, &val);
	regmap_write(h->crt, CRT_PLL_EMMC4, val | 1);
	writel(readl(h->base + EM_DUMMY_SYS) ^ BIT(30), h->base + EM_DUMMY_SYS);
	udelay(400);
	clk_prepare_enable(h->clk_ip);
	h->bus_hz = 100000000;
}

static void em_hw_init(struct em_host *h)
{
	/* the wrapper: pass only the core's interrupt */
	writel(ISR_WRITE_DATA | ISR_DMA_INT_MASK | ISR_DESC_INT_MASK,
	       h->base + EM_ISR);
	writel(ISR_DMA_DONE, h->base + EM_ISR);
	writel(ISR_IP_INT_MASK, h->base + EM_ISR);
	writel(0, h->base + EM_SWC_SEL);
	writel(0, h->base + EM_CP);

	/* As the BSP's mmc_host_reset(), step for step */
	em_write(h, EM_BMOD, BMOD_SWR | BMOD_DE);
	em_write(h, EM_CTRL, CTRL_USE_IDMAC);
	em_write(h, EM_PWREN, 1);
	em_write(h, EM_INTMASK, 0);
	em_write(h, EM_RINTSTS, 0xffffffff);
	em_write(h, EM_CLKSRC, 0);
	em_write(h, EM_TMOUT, 0xffffff40);
	em_write(h, EM_CTYPE, 0);
	em_write(h, EM_FIFOTH, 0x0001007f);	/* single transfers */
	em_write(h, EM_CTRL, CTRL_USE_IDMAC | CTRL_INT_ENABLE);
	em_write(h, EM_BMOD, BMOD_DE);
	em_write(h, EM_DBADDR, h->desc_dma);
	em_write(h, EM_IDINTEN, 0);
	em_write(h, EM_IDSTS, 0xffffffff);
	em_write(h, EM_UHS_REG, UHS_18V);
	em_write(h, EM_CARDTHRCTL, 0x02000001);	/* read threshold, 512 B */
	h->clock = ~0;
	em_set_clock(h, 400000);
}

static void em_prepare_data(struct em_host *h, struct mmc_data *data)
{
	struct scatterlist *sg;
	struct em_desc *d = h->desc;
	int i, n = 0;
	u32 val;

	h->sg_count = dma_map_sg(h->dev, data->sg, data->sg_len,
				 mmc_get_dma_dir(data));
	for_each_sg(data->sg, sg, h->sg_count, i) {
		u32 addr = sg_dma_address(sg), len = sg_dma_len(sg);

		while (len) {
			u32 n_len = min_t(u32, len, EM_DESC_LEN);

			d[n].des0 = cpu_to_le32(DES0_OWN | DES0_CH | DES0_DIC);
			d[n].des1 = cpu_to_le32(n_len);
			d[n].des2 = cpu_to_le32(addr);
			d[n].des3 = cpu_to_le32(h->desc_dma +
						(n + 1) * sizeof(*d));
			addr += n_len;
			len -= n_len;
			n++;
		}
	}
	dev_dbg(h->dev, "xfer %s %u x %u, sg %d, desc %d, first %08x last %08x+%x\n",
		data->flags & MMC_DATA_WRITE ? "W" : "R", data->blocks, data->blksz,
		h->sg_count, n, le32_to_cpu(d[0].des2), le32_to_cpu(d[n - 1].des2),
		le32_to_cpu(d[n - 1].des1));
	d[0].des0 |= cpu_to_le32(DES0_FS);
	d[n - 1].des0 |= cpu_to_le32(DES0_LD);
	/*
	 * After the last descriptor the DMAC still follows its next pointer
	 * and takes what it finds there if it says OWN: a stale descriptor
	 * from an earlier, longer transfer, or garbage at 0 without CH -- a
	 * DMA to anywhere. End the chain on one it does not own.
	 */
	d[n].des0 = 0;
	d[n].des3 = cpu_to_le32(h->desc_dma + n * sizeof(*d));
	h->nr_desc = n;

	/*
	 * Descriptors, and the data of a write, have to be in DDR before the
	 * DMAC reads them. A barrier is not enough on this SoC: the bus
	 * bridge holds CPU writes back until told to sync. The BSP syncs
	 * the same way.
	 */
	wmb();
	regmap_write(h->sb2, SB2_SYNC, 0);
	dmb(sy);

	/*
	 * The DMAC is parked on that last descriptor from the previous
	 * transfer and would go on from there: start it over from DBADDR.
	 */
	em_reset(h, CTRL_DMA_RESET);
	em_write(h, EM_BMOD, BMOD_SWR);
	readl_poll_timeout_atomic(h->base + EM_BMOD, val, !(val & BMOD_SWR),
				  1, 1000);
	em_write(h, EM_DBADDR, h->desc_dma);
	em_write(h, EM_BMOD, BMOD_DE);

	writel(0, h->base + EM_SWC_SEL);
	writel(0, h->base + EM_CP);
	em_write(h, EM_BLKSIZ, data->blksz);
	em_write(h, EM_BYTCNT, data->blksz * data->blocks);
}

static void em_start_cmd(struct em_host *h, struct mmc_command *cmd,
			 enum em_state state)
{
	struct mmc_data *data = state == ST_CMD ? cmd->data : NULL;
	u32 flags = CMD_START | CMD_USE_HOLD_REG | cmd->opcode;

	if (cmd->opcode == MMC_GO_IDLE_STATE)
		flags |= CMD_INIT;
	if (state == ST_STOP)
		flags |= CMD_STOP;
	if (cmd->flags & MMC_RSP_PRESENT) {
		flags |= CMD_RESP_EXP;
		if (cmd->flags & MMC_RSP_136)
			flags |= CMD_RESP_LONG;
	}
	if (cmd->flags & MMC_RSP_CRC)
		flags |= CMD_RESP_CRC;
	if (data) {
		flags |= CMD_DAT_EXP;
		/*
		 * As the BSP: only block reads wait for the previous data to
		 * be over. A command that waits while the core still believes
		 * DAT0 busy -- it does after an R1b -- never starts.
		 */
		if (data->flags & MMC_DATA_WRITE)
			flags |= CMD_DAT_WR;
		else if (cmd->opcode != MMC_SEND_EXT_CSD)
			flags |= CMD_PRV_DAT_WAIT;
		/* without CMD23, the core sends CMD12 by itself */
		h->need = INT_DTO;
		if (data->stop && !h->mrq->sbc) {
			flags |= CMD_AUTO_STOP;
			h->need |= INT_ACD;
		}
		h->got = 0;
		em_prepare_data(h, data);
	}

	h->cmd = cmd;
	h->state = state;
	h->rint = 0;
	em_write(h, EM_RINTSTS, 0xffffffff);
	writel(ISR_DMA_DONE, h->base + EM_ISR);
	em_write(h, EM_INTMASK, INT_CD | INT_CMD_ERR |
		 (data ? h->need | INT_DATA_ERR : 0));
	em_write(h, EM_CMDARG, cmd->arg);
	wmb();
	em_write(h, EM_CMD, flags);
	schedule_delayed_work(&h->timeout, msecs_to_jiffies(EM_TIMEOUT_MS));
}

static void em_read_resp(struct em_host *h, struct mmc_command *cmd)
{
	if (!(cmd->flags & MMC_RSP_PRESENT))
		return;
	if (cmd->flags & MMC_RSP_136) {
		cmd->resp[3] = em_read(h, EM_RESP0);
		cmd->resp[2] = em_read(h, EM_RESP0 + 4);
		cmd->resp[1] = em_read(h, EM_RESP0 + 8);
		cmd->resp[0] = em_read(h, EM_RESP0 + 12);
	} else {
		cmd->resp[0] = em_read(h, EM_RESP0);
	}
}

static void em_finish(struct em_host *h)
{
	struct mmc_request *mrq = h->mrq;

	cancel_delayed_work(&h->timeout);
	em_write(h, EM_INTMASK, 0);
	dev_dbg(h->dev, "CMD%u arg %08x: %d resp %08x%s data %d rint %08x status %08x\n",
		mrq->cmd->opcode, mrq->cmd->arg, mrq->cmd->error, mrq->cmd->resp[0],
		mrq->data ? "" : " (none)", mrq->data ? mrq->data->error : 0,
		h->last_rint, em_read(h, EM_STATUS));
	h->mrq = NULL;
	h->cmd = NULL;
	h->state = ST_IDLE;
	h->done = mrq;
}

/* With h->lock held on entry; drops it */
static void em_unlock_and_report(struct em_host *h, unsigned long flags)
{
	struct mmc_request *mrq = h->done;

	h->done = NULL;
	spin_unlock_irqrestore(&h->lock, flags);
	if (mrq)
		mmc_request_done(h->mmc, mrq);
}

static void em_end_data(struct em_host *h, struct mmc_data *data, int err)
{
	int i;

	/* the DMAC never clears OWN itself */
	for (i = 0; i < h->nr_desc; i++)
		h->desc[i].des0 = 0;
	dma_unmap_sg(h->dev, data->sg, data->sg_len, mmc_get_dma_dir(data));
	data->error = err;
	data->bytes_xfered = err ? 0 : data->blksz * data->blocks;
	if (err)	/* the FIFO may be half full */
		em_reset(h, CTRL_FIFO_RESET | CTRL_DMA_RESET);
	if (data->stop && err)
		em_start_cmd(h, data->stop, ST_STOP);
	else
		em_finish(h);
}

/*
 * Data-transfer-over is the card's side. On a read the DMAC may still be
 * emptying the FIFO into DDR then, and the data is not there to read, nor
 * may the next transfer start. It is done once it has moved every byte and
 * the wrapper has raised DMA_DONE -- which the BSP and u-boot wait for, but
 * which comes for every descriptor. A write's data has all been read by
 * the time the card has it.
 */
static int em_wait_dma(struct em_host *h, struct mmc_data *data)
{
	u32 total = data->blksz * data->blocks, isr;
	int ret;

	if (data->flags & MMC_DATA_WRITE)
		return 0;
	ret = read_poll_timeout_atomic(readl, isr, (isr & ISR_DMA_DONE) &&
				       em_read(h, EM_TBBCNT) == total,
				       1, 5000, false, h->base + EM_ISR);
	writel(ISR_DMA_DONE, h->base + EM_ISR);
	if (ret) {
		dev_err(h->dev, "read DMA not done: %u of %u bytes, ISR %08x\n",
			em_read(h, EM_TBBCNT), total, isr);
		return -EIO;
	}
	return 0;
}

/* Runs the request on from what the hard irq or the timeout found */
static void em_advance(struct em_host *h, u32 rint, bool timeout)
{
	struct mmc_command *cmd = h->cmd;
	struct mmc_data *data = cmd ? cmd->data : NULL;

	switch (h->state) {
	case ST_CMD:
		if (timeout || (rint & INT_CMD_ERR)) {
			cmd->error = timeout ? -ETIMEDOUT :
				     rint & INT_RTO ? -ETIMEDOUT : -EILSEQ;
			if (data) {
				em_end_data(h, data, cmd->error);
				return;
			}
			em_finish(h);
			return;
		}
		if (!(rint & INT_CD))
			return;
		em_read_resp(h, cmd);
		if (cmd == h->mrq->sbc) {
			em_start_cmd(h, h->mrq->cmd, ST_CMD);
			return;
		}
		if (!data) {
			em_finish(h);
			return;
		}
		h->state = ST_DATA;
		fallthrough;
	case ST_DATA:
		if (timeout || (rint & INT_DATA_ERR)) {
			em_end_data(h, data, timeout ? -ETIMEDOUT :
				    rint & (INT_DRTO | INT_HTO) ? -ETIMEDOUT :
				    -EILSEQ);
			return;
		}
		h->got |= rint;
		if ((h->got & h->need) != h->need)
			return;
		if (data->stop && !h->mrq->sbc)
			data->stop->resp[0] = em_read(h, EM_RESP1);
		em_end_data(h, data, em_wait_dma(h, data));
		return;
	case ST_STOP:
		if (timeout || (rint & INT_CMD_ERR))
			cmd->error = -ETIMEDOUT;
		else if (!(rint & INT_CD))
			return;
		else
			em_read_resp(h, cmd);
		em_finish(h);
		return;
	case ST_IDLE:
		return;
	}
}

static irqreturn_t em_irq(int irq, void *dev_id)
{
	struct em_host *h = dev_id;
	u32 rint = em_read(h, EM_RINTSTS) & em_read(h, EM_INTMASK);

	if (!rint)
		return IRQ_NONE;
	em_write(h, EM_RINTSTS, rint);
	spin_lock(&h->lock);
	h->rint |= rint;
	spin_unlock(&h->lock);
	return IRQ_WAKE_THREAD;
}

static irqreturn_t em_irq_thread(int irq, void *dev_id)
{
	struct em_host *h = dev_id;
	unsigned long flags;
	u32 rint;

	spin_lock_irqsave(&h->lock, flags);
	rint = h->rint;
	h->rint = 0;
	h->last_rint = rint;
	if (h->mrq)
		em_advance(h, rint, false);
	em_unlock_and_report(h, flags);
	return IRQ_HANDLED;
}

static void em_timeout(struct work_struct *work)
{
	struct em_host *h = container_of(work, struct em_host, timeout.work);
	unsigned long flags;

	spin_lock_irqsave(&h->lock, flags);
	if (h->mrq) {
		dev_err(h->dev, "CMD%u timed out: RINTSTS %08x STATUS %08x IDSTS %08x\n",
			h->cmd->opcode, em_read(h, EM_RINTSTS),
			em_read(h, EM_STATUS), em_read(h, EM_IDSTS));
		em_advance(h, 0, true);
	}
	em_unlock_and_report(h, flags);
}

static void em_request(struct mmc_host *mmc, struct mmc_request *mrq)
{
	struct em_host *h = mmc_priv(mmc);
	unsigned long flags;

	spin_lock_irqsave(&h->lock, flags);
	h->mrq = mrq;
	em_start_cmd(h, mrq->sbc ? mrq->sbc : mrq->cmd, ST_CMD);
	em_unlock_and_report(h, flags);
}

static void em_set_ios(struct mmc_host *mmc, struct mmc_ios *ios)
{
	struct em_host *h = mmc_priv(mmc);

	switch (ios->bus_width) {
	case MMC_BUS_WIDTH_8:
		em_write(h, EM_CTYPE, CTYPE_8BIT);
		break;
	case MMC_BUS_WIDTH_4:
		em_write(h, EM_CTYPE, CTYPE_4BIT);
		break;
	default:
		em_write(h, EM_CTYPE, 0);
	}
	if (ios->power_mode != MMC_POWER_OFF)
		em_set_clock(h, ios->clock);
}

/* The pads are wired for 1.8 V; the core asks for 3.3 V first */
static int em_switch_voltage(struct mmc_host *mmc, struct mmc_ios *ios)
{
	return ios->signal_voltage == MMC_SIGNAL_VOLTAGE_180 ? 0 : -EINVAL;
}

static const struct mmc_host_ops em_ops = {
	.request			= em_request,
	.set_ios			= em_set_ios,
	.start_signal_voltage_switch	= em_switch_voltage,
	/* no card_busy: the core then polls with CMD13, as the BSP does */
};

static int em_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mmc_host *mmc;
	struct em_host *h;
	struct clk *clk;
	int irq, ret;

	mmc = devm_mmc_alloc_host(dev, sizeof(*h));
	if (!mmc)
		return -ENOMEM;
	h = mmc_priv(mmc);
	h->dev = dev;
	h->mmc = mmc;
	spin_lock_init(&h->lock);
	INIT_DELAYED_WORK(&h->timeout, em_timeout);

	h->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(h->base))
		return PTR_ERR(h->base);
	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;

	/* CLK_EN1: EMMC (bus) and EMMC_IP (the core) */
	clk = devm_clk_get_optional_enabled(dev, "biu");
	if (IS_ERR(clk))
		return PTR_ERR(clk);
	h->clk_ip = devm_clk_get_optional_enabled(dev, "ip");
	if (IS_ERR(h->clk_ip))
		return PTR_ERR(h->clk_ip);

	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		return ret;
	h->sb2 = syscon_regmap_lookup_by_phandle(dev->of_node, "realtek,sb2");
	if (IS_ERR(h->sb2))
		return dev_err_probe(dev, PTR_ERR(h->sb2), "no realtek,sb2\n");
	h->crt = syscon_regmap_lookup_by_phandle(dev->of_node, "realtek,crt");
	if (IS_ERR(h->crt))
		return dev_err_probe(dev, PTR_ERR(h->crt), "no realtek,crt\n");

	h->desc = dmam_alloc_coherent(dev, EM_NR_DESC * sizeof(*h->desc),
				      &h->desc_dma, GFP_KERNEL);
	if (!h->desc)
		return -ENOMEM;

	ret = mmc_of_parse(mmc);
	if (ret)
		return ret;
	mmc->ops = &em_ops;
	mmc->f_min = 400000;
	if (!mmc->f_max || mmc->f_max > 52000000)
		mmc->f_max = 52000000;
	mmc->ocr_avail = MMC_VDD_32_33 | MMC_VDD_33_34 | MMC_VDD_165_195;
	mmc->max_segs = EM_NR_DESC - 1;	/* and one to end the chain */
	mmc->max_seg_size = EM_DESC_LEN;
	mmc->max_blk_size = 512;
	mmc->max_req_size = (EM_NR_DESC - 1) * EM_DESC_LEN;
	mmc->max_blk_count = mmc->max_req_size / 512;

	em_pll_init(h);
	em_hw_init(h);

	ret = devm_request_threaded_irq(dev, irq, em_irq, em_irq_thread, 0,
					dev_name(dev), h);
	if (ret)
		return ret;

	platform_set_drvdata(pdev, h);
	ret = mmc_add_host(mmc);
	if (ret)
		return ret;
	dev_info(dev, "RTD129x eMMC (DW MSHC %04x), %lu MHz PLL\n",
		 em_read(h, 0x06c) & 0xffff, h->bus_hz / 1000000);
	return 0;
}

static void em_remove(struct platform_device *pdev)
{
	struct em_host *h = platform_get_drvdata(pdev);

	mmc_remove_host(h->mmc);
	cancel_delayed_work_sync(&h->timeout);
	em_write(h, EM_INTMASK, 0);
}

static const struct of_device_id em_of_match[] = {
	{ .compatible = "realtek,rtd1295-emmc" },
	{ }
};
MODULE_DEVICE_TABLE(of, em_of_match);

static struct platform_driver em_driver = {
	.probe = em_probe,
	.remove = em_remove,
	.driver = {
		.name = "rtd129x-emmc",
		.of_match_table = em_of_match,
		.probe_type = PROBE_PREFER_ASYNCHRONOUS,
	},
};
module_platform_driver(em_driver);

MODULE_DESCRIPTION("Realtek RTD1295/RTD1296 eMMC host");
MODULE_LICENSE("GPL");
