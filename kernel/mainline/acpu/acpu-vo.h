/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Realtek RTD129x video output: what acpu-vo.c (the plane, DRM) and
 * acpu-dp.c (the DisplayPort transmitter) share.
 */
#ifndef _RTD129X_ACPU_VO_H
#define _RTD129X_ACPU_VO_H

#include <drm/drm_connector.h>
#include <drm/drm_device.h>
#include <drm/drm_simple_kms_helper.h>
#include <linux/i2c.h>
#include <linux/regmap.h>

#include "rtd129x-acpu.h"

struct vo_dp;
struct vo_standard;

struct rtd_vo {
	struct drm_device drm;
	struct drm_simple_display_pipe pipe;
	struct drm_connector connector;

	struct device *dev;
	struct rtd_acpu *acpu;
	struct rtd_acpu_buf shared;	/* ring, header, refclock */
	struct rtd_acpu_buf param;	/* kernel RPC arguments and results */
	u32 instance;
	u32 context;

	/* the HDMI side: the monitor's DDC and HPD, and what the firmware runs */
	struct i2c_adapter *ddc;
	struct regmap *iso;
	const struct vo_standard *std;
	bool std_shift;
	bool std_dvi;

	struct vo_dp *dp;
};

/* Kernel RPCs (ENUM_VIDEO_KERNEL_RPC_* in the BSP) this firmware has */
#define VO_KRPC_CONFIG_TV_SYSTEM	27
#define VO_KRPC_QUERY_TV_SYSTEM		35

/* VIDEO_RPC_VOUT_CONFIG_TV_SYSTEM, big-endian, as the MIPS side lays it out */
#define VO_TV_SYSTEM_SIZE		60
#define VO_TV_INTERFACE_TYPE		0	/* enum VO_INTERFACE_TYPE */
#define VO_TV_STANDARD			4	/* videoInfo.standard */
#define VO_TV_EN_PROG			8	/* videoInfo.enProg, enDIF, enCompRGB */
#define VO_TV_PED_TYPE			12	/* videoInfo.pedType: the DP standard */
#define VO_TV_VIDEO_DATA0		16	/* videoInfo.dataInt0 */
#define VO_TV_HDMI_MODE			24	/* hdmiInfo.hdmiMode */
#define VO_TV_AUDIO_FREQ		28	/* hdmiInfo.audioSampleFreq */
#define VO_TV_AUDIO_CHANNELS		32	/* then the AVI infoframe's bytes 1-5 */
#define VO_TV_HDMI_DATA0		40	/* hdmiInfo.dataInt0: colour depth */
#define VO_TV_HDMI2			44	/* hdmiInfo.hdmi2p0_feature */
#define VO_TV_HDMI_OFF_MODE		48
#define VO_TV_HDR_MODE			52

#define VO_INTERFACE_HDMI_AND_DP_SAME_SOURCE	4
#define VO_DVI_ON				0	/* enum VO_HDMI_MODE */
#define VO_HDMI_ON				1
#define VO_HDMI_AUDIO_48K			3
#define VO_STANDARD_DP_FORMAT_1920_1080P_60	60

int vo_query_tv_system(struct rtd_vo *vo, u8 *tv);
int vo_config_tv_system(struct rtd_vo *vo, const u8 *tv);

bool vo_hdmi_1080p60(struct rtd_vo *vo);

int vo_dp_init(struct rtd_vo *vo);
bool vo_dp_on(struct rtd_vo *vo);
void vo_dp_fini(struct rtd_vo *vo);

#endif
