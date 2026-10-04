// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Realtek RTD129x DisplayPort output (the BPI-W2's mini DP)
 *
 * The audio firmware's video output feeds the DP transmitter as well, but
 * the transmitter is the ARM side's to set up: its PLLs, the timing it
 * expects from VO, the main stream attributes, and link training over
 * AUX. Then the firmware is told, with the TV system it already runs, to
 * send the same picture to DP too.
 *
 * Everything here follows Realtek's BSP driver (rtk_dptx: dptx_hwapi.c,
 * dptx_core.c, dptx_rpc.c), for the one mode the HDMI side runs: 1080p60,
 * two lanes at 2.7 Gb/s. Link training uses the DRM DP helpers instead of
 * the BSP's own state machine, with the BSP's limits.
 *
 * Hot plug: the HPD line is ISO GPIO 7, which is polled.
 */
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/iopoll.h>
#include <linux/mfd/syscon.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/regmap.h>
#include <linux/reset.h>
#include <linux/workqueue.h>

#include <drm/display/drm_dp_helper.h>

#include "acpu-vo.h"

/* DPTX registers: page B, C and D of Realtek's map */
#define PBB(x)			((x) * 4)
#define PBC(x)			(0x400 + (x) * 4)
#define PBD(x)			(0x800 + (x) * 4)

#define DP_PHY_CTRL		PBB(0x00)
#define DPTX_ML_PAT_SEL		PBB(0x01)
#define DPTX_PHY_CTRL		PBB(0x0d)
#define HDCP_ECF_BYTE0		PBB(0x64)
#define HPD_IRQ_EN		PBB(0x72)
#define DP_MAC_CTRL		PBB(0xa0)
#define DP_RESET_CTRL		PBB(0xa1)
#define DPTX_IRQ_CTRL		PBB(0xa3)
#define PG_FIFO_CTRL		PBB(0xa4)
#define MN_VID_AUTO_EN_1	PBB(0xa8)
#define MN_M_VID_H		PBB(0xa9)	/* M, N: high, middle, low */
#define MN_N_VID_H		PBB(0xac)
#define MVID_AUTO_H		PBB(0xaf)
#define MSA_CTRL		PBB(0xb4)
#define MSA_MISC0		PBB(0xb5)
#define MN_STRM_ATTR_HTT_M	PBB(0xb7)	/* ... through VSW_L, 0xc6 */
#define VBID			PBB(0xc7)
#define VBID_FW_CTL		PBB(0xc8)
#define ARBITER_CTRL		PBB(0xc9)
#define V_DATA_PER_LINE0	PBB(0xca)
#define V_DATA_PER_LINE1	PBB(0xcb)
#define TU_DATA_SIZE0		PBB(0xcd)
#define TU_DATA_SIZE1		PBB(0xce)
#define LFIFO_WL_SET		PBB(0xd3)
#define ARBITER_SEC_END_CNT_HB	PBB(0xd4)
#define DPTX_SFIFO_CTRL0	PBC(0xa7)
#define AUX_TX_CTRL		PBD(0xa0)
#define AUXTX_TRAN_CTRL		PBD(0xa3)
#define AUXTX_REQ_CMD		PBD(0xa4)
#define AUXTX_REQ_ADDR_M	PBD(0xa5)
#define AUXTX_REQ_ADDR_L	PBD(0xa6)
#define AUXTX_REQ_LEN		PBD(0xa7)
#define AUXTX_REQ_DATA		PBD(0xa8)
#define AUX_REPLY_CMD		PBD(0xa9)
#define AUX_REPLY_DATA		PBD(0xaa)
#define AUX_FIFO_CTRL		PBD(0xab)
#define AUX_IRQ_EVENT		PBD(0xb1)
#define  AUX_IRQ_SENT		BIT(0)	/* the request is out; not the end */
#define  AUX_IRQ_ERROR		BIT(1)
#define  AUX_IRQ_REPLY		BIT(5)
#define AUX_IRQ_EN		PBD(0xb2)
#define AUX_DIG_PHY8		PBD(0xf6)

/* The LVDS/AIF block: the timing generator in front of the transmitter */
#define AIF_MISC		0x000
#define AIF_EDP1		0x040
#define AIF_EDP2		0x044	/* drive: emphasis per lane */
#define AIF_EDP3		0x048	/* drive: swing per lane */
#define CT_CTRL			0x100
#define DH_WIDTH		0x404
#define DH_TOTAL		0x408
#define DH_DEN_START_END	0x40c
#define DV_DEN_START_END_F1	0x410
#define DV_TOTAL		0x418
#define DV_VS_START_END_F1	0x41c
#define DV_SYNC_INT		0x42c

/* CRT */
#define DISP_PLL_DIV2		0x024
#define PLL_EDP1		0x248
#define PLL_EDP2		0x24c
#define PLL_PIXEL1		0x250
#define PLL_PIXEL2		0x254
#define PLL_SSC_DIG_EDP0	0x5e0
#define PLL_SSC_DIG_EDP1	0x5e4
#define PLL_SSC_DIG_EDP2	0x5e8
#define PLL_SSC_DIG_EDP3	0x5ec
#define PLL_SSC_DIG_EDP4	0x5f0
#define PLL_SSC_DIG_PIXEL0	0x600
#define PLL_SSC_DIG_PIXEL1	0x604
#define PLL_SSC_DIG_PIXEL2	0x608

/* VO: its DisplayPort output */
#define VO_DP_OUT		0xe78

/* ISO: GPIO data in, bit 7 is the DP connector's HPD */
#define ISO_GPDATI		0x108
#define DP_HPD			BIT(7)

#define DP_LANES		2
#define DP_PIXEL_MHZ		148

/*
 * The PHY's drive per (swing, pre-emphasis), three values each, from the
 * BSP's tDPTX_DRV_TABLE.
 */
static const u8 dp_drive[4][4][3] = {
	{ { 0x00, 0x00, 0x03 }, { 0x00, 0x07, 0x05 }, { 0x00, 0x0c, 0x05 }, { 0x01, 0x09, 0x06 } },
	{ { 0x00, 0x00, 0x05 }, { 0x00, 0x07, 0x05 }, { 0x00, 0x0c, 0x05 }, { 0x00, 0x0c, 0x05 } },
	{ { 0x00, 0x00, 0x09 }, { 0x00, 0x0c, 0x09 }, { 0x00, 0x0c, 0x09 }, { 0x00, 0x0c, 0x09 } },
	{ { 0x00, 0x00, 0x0e }, { 0x00, 0x00, 0x0e }, { 0x00, 0x00, 0x0e }, { 0x00, 0x00, 0x0e } },
};

struct vo_dp {
	struct rtd_vo *vo;
	struct device_node *np;
	void __iomem *base;
	void __iomem *aif;
	void __iomem *vo_regs;
	struct regmap *crt;
	struct regmap *iso;
	struct clk *clk_tve, *clk_lvds;
	struct reset_control *rst_tve, *rst_lvds;
	struct drm_dp_aux aux;
	struct delayed_work hpd_work;
	bool connected;
	bool on;
	unsigned int tries, polls;
};

static void dp_update(void __iomem *reg, u32 clear, u32 set)
{
	writel((readl(reg) & ~clear) | set, reg);
}

static void crt_write(struct vo_dp *dp, u32 reg, u32 val)
{
	regmap_write(dp->crt, reg, val);
	usleep_range(1000, 1200);
}

static void crt_update(struct vo_dp *dp, u32 reg, u32 clear, u32 set)
{
	regmap_update_bits(dp->crt, reg, clear | set, set);
	usleep_range(1000, 1200);
}

/* AUX: one request, up to 16 bytes, polled */
static ssize_t dp_aux_transfer(struct drm_dp_aux *aux, struct drm_dp_aux_msg *msg)
{
	struct vo_dp *dp = container_of(aux, struct vo_dp, aux);
	void __iomem *b = dp->base;
	u8 *buf = msg->buffer;
	u32 ev, reply;
	int i, ret;

	if (msg->size > 16)
		return -E2BIG;
	/* the BSP never sends an address-only request; neither does this */
	if (!msg->size)
		return -EOPNOTSUPP;

	writel(0x3f, b + AUX_IRQ_EVENT);
	dp_update(b + AUX_FIFO_CTRL, 0, BIT(1) | BIT(0));
	writel((msg->request & 0xf) << 4 | ((msg->address >> 16) & 0xf),
	       b + AUXTX_REQ_CMD);
	writel((msg->address >> 8) & 0xff, b + AUXTX_REQ_ADDR_M);
	writel(msg->address & 0xff, b + AUXTX_REQ_ADDR_L);
	writel(msg->size - 1, b + AUXTX_REQ_LEN);
	if (!(msg->request & DP_AUX_I2C_READ))	/* both writes have bit 0 clear */
		for (i = 0; i < msg->size; i++)
			writel(buf[i], b + AUXTX_REQ_DATA);
	dp_update(b + AUXTX_TRAN_CTRL, 0, BIT(0));

	/* as u-boot: done on a reply or an error, not on "sent" */
	ret = readl_poll_timeout(b + AUX_IRQ_EVENT, ev,
				 ev & (AUX_IRQ_REPLY | AUX_IRQ_ERROR), 50, 20000);
	reply = readl(b + AUX_REPLY_CMD);
	writel(0x3f, b + AUX_IRQ_EVENT);
	dev_dbg(dp->vo->dev, "aux req %#x addr %#x len %zu: ret %d event %#x reply %#x\n",
		msg->request, msg->address, msg->size, ret, ev, reply);
	if (ret || !(ev & AUX_IRQ_REPLY)) {
		dev_dbg(dp->vo->dev, "aux %#x@%#x: event %#x\n", msg->request,
			msg->address, ev);
		writel(0x20, b + DP_RESET_CTRL);	/* AUX reset */
		writel(0, b + DP_RESET_CTRL);
		return -ETIMEDOUT;
	}
	/* the reply's command nibble, as on the wire */
	msg->reply = reply & 0xf;
	if (msg->request & DP_AUX_I2C_READ)
		for (i = 0; i < msg->size; i++)
			buf[i] = readl(b + AUX_REPLY_DATA);
	return msg->size;
}

static void dp_phy_drive(struct vo_dp *dp, const u8 *set)
{
	int lane;

	for (lane = 0; lane < DP_LANES; lane++) {
		u8 sw = set[lane] & DP_TRAIN_VOLTAGE_SWING_MASK;
		u8 pe = (set[lane] & DP_TRAIN_PRE_EMPHASIS_MASK) >>
			DP_TRAIN_PRE_EMPHASIS_SHIFT;
		const u8 *d = dp_drive[sw][min_t(u8, pe, 3 - sw)];

		dp_update(dp->aif + AIF_EDP2, BIT(20) | 0xf << (lane * 4),
			  d[0] << 16 | d[1] << (lane * 4) | 0x00f00000);
		dp_update(dp->aif + AIF_EDP3, 0xff000000 | 0xf << (lane * 4),
			  d[2] << (lane * 4) | 0x1f000000);
	}
}

/* The next drive from the sink's adjust request, within the BSP's limits */
static void dp_adjust(const u8 *status, u8 *set)
{
	int lane;

	for (lane = 0; lane < DP_LANES; lane++) {
		u8 sw = drm_dp_get_adjust_request_voltage(status, lane);
		u8 pe = drm_dp_get_adjust_request_pre_emphasis(status, lane) >>
			DP_TRAIN_PRE_EMPHASIS_SHIFT;

		if (sw + pe > 3)
			pe = 3 - sw;
		set[lane] = sw | pe << DP_TRAIN_PRE_EMPHASIS_SHIFT;
		if (sw == 3)
			set[lane] |= DP_TRAIN_MAX_SWING_REACHED;
		if (pe == 3)
			set[lane] |= DP_TRAIN_MAX_PRE_EMPHASIS_REACHED;
	}
}

static int dp_train_phase(struct vo_dp *dp, u8 pattern, u8 *set)
{
	u8 status[DP_LINK_STATUS_SIZE], buf[1 + DP_LANES];
	int tries;

	writel(pattern << 4, dp->base + DPTX_ML_PAT_SEL);
	writel(pattern << 4 | 1, dp->base + DPTX_ML_PAT_SEL);
	buf[0] = pattern | DP_LINK_SCRAMBLING_DISABLE;
	memcpy(buf + 1, set, DP_LANES);
	if (drm_dp_dpcd_write(&dp->aux, DP_TRAINING_PATTERN_SET, buf,
			      sizeof(buf)) != sizeof(buf))
		return -EIO;

	for (tries = 0; tries < 6; tries++) {
		usleep_range(5000, 6000);
		if (drm_dp_dpcd_read_link_status(&dp->aux, status) < 0)
			return -EIO;
		dev_dbg(dp->vo->dev, "TP%u: %*ph\n", pattern, 6, status);
		if (pattern == DP_TRAINING_PATTERN_1 ?
		    drm_dp_clock_recovery_ok(status, DP_LANES) :
		    drm_dp_channel_eq_ok(status, DP_LANES))
			return 0;
		if (pattern == DP_TRAINING_PATTERN_2 &&
		    !drm_dp_clock_recovery_ok(status, DP_LANES))
			break;
		dp_adjust(status, set);
		dp_phy_drive(dp, set);
		if (drm_dp_dpcd_write(&dp->aux, DP_TRAINING_LANE0_SET, set,
				      DP_LANES) != DP_LANES)
			return -EIO;
	}
	return -ETIMEDOUT;
}

static int dp_link_train(struct vo_dp *dp)
{
	u8 dpcd[16], set[DP_LANES] = { }, lanes;
	int ret;

	drm_dp_dpcd_writeb(&dp->aux, DP_SET_POWER, DP_SET_POWER_D0);
	/* the source OUI, Realtek's, as the BSP writes it */
	drm_dp_dpcd_write(&dp->aux, DP_SOURCE_OUI, (u8[]){ 0x4c, 0xe0, 0x00 }, 3);
	if (drm_dp_dpcd_read(&dp->aux, DP_DPCD_REV, dpcd, sizeof(dpcd)) !=
	    sizeof(dpcd))
		return -EIO;
	dev_info(dp->vo->dev, "DP sink: DPCD %x.%x, %u lanes, link rate %#x\n",
		 dpcd[0] >> 4, dpcd[0] & 0xf,
		 dpcd[DP_MAX_LANE_COUNT] & DP_MAX_LANE_COUNT_MASK,
		 dpcd[DP_MAX_LINK_RATE]);
	if ((dpcd[DP_MAX_LANE_COUNT] & DP_MAX_LANE_COUNT_MASK) < DP_LANES)
		return -EINVAL;
	/*
	 * The transmitter's link PLL runs at 2.7 Gb/s and the stream's M/N
	 * are worked out for it, so that is the rate, as with the BSP.
	 */
	if (dpcd[DP_MAX_LINK_RATE] < DP_LINK_BW_2_7)
		dev_warn(dp->vo->dev, "sink below 2.7 Gb/s; trying anyway\n");

	dp_phy_drive(dp, set);
	lanes = DP_LANES;
	if (dpcd[DP_MAX_LANE_COUNT] & DP_ENHANCED_FRAME_CAP)
		lanes |= DP_LANE_COUNT_ENHANCED_FRAME_EN;
	drm_dp_dpcd_writeb(&dp->aux, DP_LINK_BW_SET, DP_LINK_BW_2_7);
	drm_dp_dpcd_writeb(&dp->aux, DP_LANE_COUNT_SET, lanes);

	ret = dp_train_phase(dp, DP_TRAINING_PATTERN_1, set);
	if (!ret)
		ret = dp_train_phase(dp, DP_TRAINING_PATTERN_2, set);
	drm_dp_dpcd_writeb(&dp->aux, DP_TRAINING_PATTERN_SET,
			   DP_TRAINING_PATTERN_DISABLE);
	if (ret)
		return ret;
	/* the main link to video */
	writel(0x40, dp->base + DPTX_ML_PAT_SEL);
	writel(0x41, dp->base + DPTX_ML_PAT_SEL);
	return 0;
}

/* dptx_close_phy(), dptx_close_pll() */
static void dp_off(struct vo_dp *dp)
{
	writel(1, dp->base + DP_PHY_CTRL);
	writel(0, dp->aif + AIF_EDP1);
	crt_write(dp, PLL_EDP2, 0);
	crt_update(dp, PLL_EDP1, BIT(31), 0);
	crt_write(dp, PLL_SSC_DIG_PIXEL0, 0x8);
	crt_update(dp, PLL_PIXEL2, BIT(0), 0);
	crt_update(dp, PLL_PIXEL1, BIT(31), 0);
}

/* rtk_dptx_hw_init(): PLLs, then the AUX side */
static void dp_hw_init(struct vo_dp *dp)
{
	void __iomem *b = dp->base;
	u32 v;

	dp_off(dp);

	/* the pixel PLL, for 1080p60, with the timing generator in reset */
	reset_control_assert(dp->rst_lvds);
	crt_write(dp, PLL_PIXEL1, 0x93470192);
	crt_write(dp, PLL_PIXEL1, 0x93474192);
	crt_write(dp, PLL_SSC_DIG_PIXEL1, 0xf000);
	crt_write(dp, PLL_PIXEL2, 0x1e01);
	crt_write(dp, PLL_SSC_DIG_PIXEL2, 0x501008);
	crt_write(dp, PLL_SSC_DIG_PIXEL0, 0xd);
	reset_control_deassert(dp->rst_lvds);

	/*
	 * "Disable iso control", then the link PLL. The BSP writes
	 * DISP_PLL_DIV2 whole; the bits below 16 are left as they are here,
	 * since the HDMI side runs off this register too.
	 */
	regmap_read(dp->crt, DISP_PLL_DIV2, &v);
	crt_write(dp, DISP_PLL_DIV2, (v & 0xffff) | 0x440000);
	writel(0x1003301, dp->aif + AIF_MISC);
	usleep_range(1000, 1200);
	crt_write(dp, DISP_PLL_DIV2, (v & 0xffff) | 0x420000);
	crt_write(dp, PLL_EDP1, 0x9a356007);
	crt_write(dp, PLL_EDP2, 0x3);
	crt_write(dp, PLL_SSC_DIG_EDP0, 0xc);
	crt_write(dp, PLL_SSC_DIG_EDP1, 0x30800);
	crt_write(dp, PLL_SSC_DIG_EDP3, 0x30480);
	crt_write(dp, PLL_SSC_DIG_EDP4, 0x1100);
	crt_write(dp, PLL_SSC_DIG_EDP2, 0x501068);
	crt_write(dp, PLL_SSC_DIG_EDP0, 0xd);

	/* dptx_initial() */
	dp_update(b + HDCP_ECF_BYTE0, BIT(7), 0);	/* ECF by firmware */
	dp_update(b + AUX_TX_CTRL, 0, BIT(0));		/* AUX channel on */
	dp_update(b + AUX_DIG_PHY8, 0, BIT(1));
	dp_update(b + HPD_IRQ_EN, BIT(6) | BIT(5), BIT(6));
	writel(0x80, b + DPTX_IRQ_CTRL);		/* TX clock divider */
	dp_update(b + ARBITER_CTRL, 0, BIT(0));
	writel(0xff, b + AUX_IRQ_EN);			/* events latch; polled */
	dp_update(b + VBID, 0, BIT(1));
}

/* dptx_sst_setting(): M/N, measured as the BSP measures it */
static void dp_mn(struct vo_dp *dp)
{
	void __iomem *b = dp->base;
	u32 n = 0x8000, m = DP_PIXEL_MHZ * n / 270, sum = 0, cnt = 0;
	u32 lo = U32_MAX, hi = 0, got;
	int i;

	for (i = 0; i < 10; i++) {
		dp_update(b + MN_VID_AUTO_EN_1, 0, BIT(7));
		/* one measurement period: N link symbols at 270 MHz, twice */
		usleep_range(2 * n / 27, 2 * n / 27 + 100);
		dp_update(b + MN_VID_AUTO_EN_1, BIT(7), 0);
		got = readl(b + MVID_AUTO_H) << 16 |
		      readl(b + MVID_AUTO_H + 4) << 8 | readl(b + MVID_AUTO_H + 8);
		if (!got) {
			sum = 0;
			cnt = 0;
			break;
		}
		if (abs((int)(m - got)) > m >> 2)
			continue;
		if (cnt && abs((int)(sum / cnt - got)) >= 0x50)
			continue;
		lo = min(lo, got);
		hi = max(hi, got);
		sum += got;
		cnt++;
	}
	if (cnt > 2) {
		got = (sum - lo - hi + (cnt - 2) / 2) / (cnt - 2);
		if (abs((int)(m - got)) <= m >> 1)
			m = got;
	}
	dev_dbg(dp->vo->dev, "M %u (%u measured), N %u\n", m, cnt, n);
	writel((m >> 16) & 0xff, b + MN_M_VID_H);
	writel((m >> 8) & 0xff, b + MN_M_VID_H + 4);
	writel(m & 0xff, b + MN_M_VID_H + 8);
	writel((n >> 16) & 0xff, b + MN_N_VID_H);
	writel((n >> 8) & 0xff, b + MN_N_VID_H + 4);
	writel(n & 0xff, b + MN_N_VID_H + 8);
	dp_update(b + MSA_CTRL, BIT(6), 0);
	dp_update(b + MN_VID_AUTO_EN_1, BIT(7) | BIT(6), BIT(6));
}

/* dptx_set_1080p_2lane(), with the firmware setting up VO itself */
static void dp_set_1080p(struct vo_dp *dp)
{
	/* the main stream attributes, HTT_M through VSW_L (no VST_M) */
	static const u8 msa[16] = {
		0x08, 0x98, 0x00, 0xc1, 0x07, 0x80, 0x00, 0x2c,
		0x04, 0x65, 0x00, 0x2a, 0x04, 0x38, 0x00, 0x05,
	};
	void __iomem *b = dp->base, *a = dp->aif;
	int i;

	writel(1, b + VBID_FW_CTL);
	writel(0, a + CT_CTRL);
	writel(0x2c, a + DH_WIDTH);
	writel(0x8980898, a + DH_TOTAL);
	writel(0xb70837, a + DH_DEN_START_END);
	writel(0x2a0462, a + DV_DEN_START_END_F1);
	writel(0x465, a + DV_TOTAL);
	writel(0x10006, a + DV_VS_START_END_F1);
	writel(0x463, a + DV_SYNC_INT);

	dp_mn(dp);

	/* dptx_sstmsa_setting() */
	dp_update(b + DP_RESET_CTRL, 0, BIT(6));
	dp_update(b + DP_RESET_CTRL, BIT(6), 0);
	for (i = 0; i < ARRAY_SIZE(msa); i++)
		if (i != 10)	/* 0xc1, VST_M, is not written */
			writel(msa[i], b + MN_STRM_ATTR_HTT_M + 4 * i);
	writel(0x20, b + MSA_MISC0);
	dp_update(b + MSA_CTRL, 0, BIT(7));

	/* dptx_sst_displayformat_setting() */
	dp_update(b + LFIFO_WL_SET, BIT(7), 0);
	dp_update(b + DP_RESET_CTRL, 0, BIT(7));
	dp_update(b + DP_RESET_CTRL, BIT(7), 0);
	writel(0x35, b + TU_DATA_SIZE0);
	writel(0x00, b + TU_DATA_SIZE1);
	writel(0x0b, b + V_DATA_PER_LINE0);
	writel(0x40, b + V_DATA_PER_LINE1);
	writel(0xc0, b + LFIFO_WL_SET);

	writel(0xff, b + PG_FIFO_CTRL);
	writel(0x4, b + VBID);			/* no audio */
	writel(0, b + ARBITER_SEC_END_CNT_HB);

	writel(0x38, b + DP_PHY_CTRL);
	writel(0x6, b + DP_MAC_CTRL);
	writel(0x3, a + AIF_EDP1);
	writel(0x80, b + DPTX_SFIFO_CTRL0);
	writel(0x15, b + DPTX_PHY_CTRL);
	writel(0x3, dp->vo_regs + VO_DP_OUT);
}

static int dp_enable(struct vo_dp *dp)
{
	struct rtd_vo *vo = dp->vo;
	u8 tv[VO_TV_SYSTEM_SIZE];
	int ret, tries;

	dp_hw_init(dp);
	dp_set_1080p(dp);
	for (tries = 0; tries < 3; tries++) {
		ret = dp_link_train(dp);
		if (!ret)
			break;
		msleep(100);
	}
	if (ret) {
		dev_err(vo->dev, "DP link training failed: %d\n", ret);
		return ret;
	}

	/* the TV system the firmware runs, with DP added to it */
	ret = vo_query_tv_system(vo, tv);
	if (ret)
		return ret;
	dev_dbg(vo->dev, "TV system %*ph\n", VO_TV_SYSTEM_SIZE, tv);
	*(__be32 *)(tv + VO_TV_INTERFACE_TYPE) =
		cpu_to_be32(VO_INTERFACE_HDMI_AND_DP_SAME_SOURCE);
	*(__be32 *)(tv + VO_TV_PED_TYPE) =
		cpu_to_be32(VO_STANDARD_DP_FORMAT_1920_1080P_60);
	ret = vo_config_tv_system(vo, tv);
	if (ret)
		return ret;
	dev_info(vo->dev, "DisplayPort on, 1920x1080, %u lanes\n", DP_LANES);
	return 0;
}

static bool dp_hpd(struct vo_dp *dp)
{
	u32 v = 0;

	regmap_read(dp->iso, ISO_GPDATI, &v);
	return v & DP_HPD;
}

static void dp_hpd_work(struct work_struct *work)
{
	struct vo_dp *dp = container_of(work, struct vo_dp, hpd_work.work);
	bool hpd = dp_hpd(dp);

	if (hpd != dp->connected) {
		dp->connected = hpd;
		dp->tries = 0;
		dp->polls = 0;		/* the first try right away */
		dev_info(dp->vo->dev, "DP %s\n", hpd ? "connected" : "disconnected");
		if (!hpd && dp->on) {
			dp_off(dp);
			dp->on = false;
		}
		if (hpd)
			msleep(200);	/* let the sink come up */
	}
	/*
	 * A sink may take its time: three tries, 10 s apart, then nothing
	 * until it is plugged in again. A passive DP++ (HDMI) adapter never
	 * answers on AUX: this board cannot drive one.
	 */
	if (hpd && !dp->on && dp->tries < 3 && !(dp->polls++ % 10)) {
		dp->tries++;
		dp->on = !dp_enable(dp);
	}
	schedule_delayed_work(&dp->hpd_work, HZ);
}

int vo_dp_init(struct rtd_vo *vo)
{
	struct device_node *np;
	struct vo_dp *dp;
	int ret;

	np = of_find_compatible_node(NULL, NULL, "realtek,rtd1295-dptx");
	if (!np || !of_device_is_available(np)) {
		of_node_put(np);
		return 0;
	}
	dp = devm_kzalloc(vo->dev, sizeof(*dp), GFP_KERNEL);
	if (!dp) {
		ret = -ENOMEM;
		goto err_np;
	}
	dp->vo = vo;
	dp->np = np;

	dp->base = of_iomap(np, 0);
	dp->aif = of_iomap(np, 1);
	dp->vo_regs = of_iomap(np, 2);
	dp->crt = syscon_regmap_lookup_by_phandle(np, "realtek,crt");
	dp->iso = syscon_regmap_lookup_by_phandle(np, "realtek,iso");
	if (!dp->base || !dp->aif || !dp->vo_regs || IS_ERR(dp->crt) ||
	    IS_ERR(dp->iso)) {
		ret = -ENODEV;
		goto err_map;
	}

	dp->clk_tve = of_clk_get_by_name(np, "tve");
	dp->clk_lvds = of_clk_get_by_name(np, "lvds");
	dp->rst_tve = of_reset_control_get_exclusive(np, "tve");
	dp->rst_lvds = of_reset_control_get_exclusive(np, "lvds");
	if (IS_ERR(dp->clk_tve) || IS_ERR(dp->clk_lvds) ||
	    IS_ERR(dp->rst_tve) || IS_ERR(dp->rst_lvds)) {
		ret = -ENODEV;
		goto err_map;
	}
	ret = clk_prepare_enable(dp->clk_tve);
	if (ret)
		goto err_map;
	ret = clk_prepare_enable(dp->clk_lvds);
	if (ret)
		goto err_tve;
	reset_control_deassert(dp->rst_tve);
	reset_control_deassert(dp->rst_lvds);

	dp->aux.name = "rtd129x-dp";
	dp->aux.dev = vo->dev;
	dp->aux.drm_dev = &vo->drm;
	dp->aux.transfer = dp_aux_transfer;
	drm_dp_aux_init(&dp->aux);

	vo->dp = dp;
	INIT_DELAYED_WORK(&dp->hpd_work, dp_hpd_work);
	schedule_delayed_work(&dp->hpd_work, 0);
	return 0;

err_tve:
	clk_disable_unprepare(dp->clk_tve);
err_map:
	if (dp->base)
		iounmap(dp->base);
	if (dp->aif)
		iounmap(dp->aif);
	if (dp->vo_regs)
		iounmap(dp->vo_regs);
err_np:
	of_node_put(np);
	return ret;
}

void vo_dp_fini(struct rtd_vo *vo)
{
	if (vo->dp)
		cancel_delayed_work_sync(&vo->dp->hpd_work);
}
