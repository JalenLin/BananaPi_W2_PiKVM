// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Realtek RTD129x video output, through the audio firmware
 *
 * On this SoC the audio CPU's firmware (bluecore.audio) also runs the video
 * output: the VO mixer and its planes, and the HDMI transmitter behind it.
 * The boot loader leaves it a TV system (VO_RESOLUTION, "one step"), which
 * the firmware applies when it starts -- 1080p60 HDMI on the BPI-W2 -- but
 * nothing is shown on any plane.
 *
 * This puts one plane, OSD1, on screen the way Realtek's own kernel
 * framebuffer does it (rtk_fb_RPC.c, RTK_FB_RPC_OSD_init(), in Realtek's
 * 4.9 kernel for this SoC): create a video-out instance, display it on
 * OSD1, size the window, give it a reference clock and a ring buffer, and
 * run it. Pictures are then handed over as in-band commands in that ring
 * (VIDEO_GRAPHIC_PICTURE_OBJECT, as dc2vo.c does it).
 *
 * That kernel used RPC numbers this firmware does not have (its kernel RPC
 * table stops at 39), so the calls go as the firmware's AUDIO_SYSTEM
 * procedures instead, the way Realtek's user-space libraries make them; the
 * numbers are from AudioRPC_System.h in Realtek's Android SDK and match the
 * firmware's dispatcher.
 *
 * On top of that sits a minimal DRM driver: one CRTC, one plane, one HDMI
 * connector. The modes are the monitor's (its EDID, read over the HDMI
 * output's DDC) that the firmware has a TV system for: the CEA-861 ones
 * Realtek's own hdmitx driver offers, 480p to 1080p. A new mode is a new TV
 * system for the firmware (kernel RPC 27, as hdmitx_config.c's
 * set_hdmitx_format() makes it), and the plane's window follows. Scanout
 * buffers come from a reserved pool below 512 MiB (the "vo" region of the
 * audio CPU node), which is what the audio CPU can reach.
 */
#include <linux/auxiliary_bus.h>
#include <linux/dma-mapping.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of_reserved_mem.h>
#include <linux/unaligned.h>
#include <linux/mfd/syscon.h>
#include <linux/of.h>

#include <drm/clients/drm_client_setup.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_drv.h>
#include <drm/drm_edid.h>
#include <drm/drm_fb_dma_helper.h>
#include <drm/drm_fbdev_dma.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem_atomic_helper.h>
#include <drm/drm_gem_dma_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_simple_kms_helper.h>

#include "acpu-vo.h"

/* AUDIO_SYSTEM procedures (AudioRPC_System.h) */
#define VIDEO_RPC_ToAgent_Create			1010
#define VIDEO_RPC_ToAgent_InitRingBuffer		1030
#define VIDEO_RPC_ToAgent_Run				1040
#define VIDEO_RPC_ToAgent_SetRefClock			1090
#define VIDEO_RPC_VOUT_ToAgent_ConfigureDisplayWindow	2080
#define VIDEO_RPC_VO_FILTER_ToAgent_Display		2260

#define VF_TYPE_VIDEO_OUT	10
#define VO_VIDEO_PLANE_OSD1	3

/* In-band commands (dc2vo.h, InbandAPI.h) */
#define INBAND_CMD_TYPE_PICTURE_OBJECT	32
#define GRAPHIC_FORMAT_ARGB8888_LITTLE	39

/*
 * The TV systems (enum VO_STANDARD) for the CEA-861 modes Realtek's hdmitx
 * driver offers (support_vic[] and vic_to_vo_standard() in hdmitx_config.c),
 * RGB, 8 bits, no 3D. "shift" is the 1000/1001 rate (59.94 Hz and so on),
 * where the firmware has one. Left out: 480i and 576i (pixel repetition),
 * and 2160p. A 2160p framebuffer is 35 MB, and the scanout pool -- 50 MiB,
 * below 512 MiB where the firmware can read, handed out in power-of-two
 * blocks -- cannot hold one; nothing larger is free down there.
 */
struct vo_standard {
	u8 vic;
	u8 standard;
	u8 shift;		/* 0: none */
	bool pal;		/* videoInfo.dataInt0: the CVBS side, PAL or NTSC */
};

static const struct vo_standard vo_standards[] = {
	{   2,  1,  0, false },	/* 720x480p59.94 4:3: NTSC_J */
	{   3,  1,  0, false },	/* 720x480p59.94 16:9 */
	{  17,  7,  0, true },	/* 720x576p50 4:3: PAL_I */
	{  18,  7,  0, true },	/* 720x576p50 16:9 */
	{   4, 13, 28, false },	/* 1280x720p60 */
	{  19, 14,  0, true },	/* 1280x720p50 */
	{   5, 18, 27, false },	/* 1920x1080i60 */
	{  20, 19,  0, true },	/* 1920x1080i50 */
	{  16, 25, 30, false },	/* 1920x1080p60 */
	{  31, 26,  0, true },	/* 1920x1080p50 */
	{  34, 20,  0, false },	/* 1920x1080p30 */
	{  33, 21,  0, true },	/* 1920x1080p25 */
	{  32, 22, 29, false },	/* 1920x1080p24 */
};

/* What the boot loader has the firmware run: CEA-861 VIC 16, 1080p60 */
#define VO_BOOT_VIC		16
#define VO_MAX_WIDTH		1920
#define VO_MAX_HEIGHT		1080

/* ISO: GPIO data in, bit 6 is the HDMI connector's HPD */
#define ISO_GPDATI		0x108
#define HDMI_HPD		BIT(6)

/* The shared block: ring, its header, the reference clock */
#define VO_RING_SIZE		SZ_64K
#define VO_RING_HDR		VO_RING_SIZE
#define VO_REFCLOCK		(VO_RING_SIZE + SZ_1K)
#define VO_SHARED_SIZE		(VO_RING_SIZE + SZ_2K)

/* RINGBUFFER_HEADER, all big-endian */
struct vo_ring_hdr {
	__be32 magic;
	__be32 begin;
	__be32 size;
	__be32 buffer_id;
	__be32 write;
	__be32 num_read;
	__be32 reserved[2];
	__be32 read[4];
	__be32 file_offset;
	__be32 requested_file_offset;
	__be32 file_size;
	__be32 seekable;
};

/* REFCLOCK (dc_rpc.h), big-endian, laid out as the MIPS side has it */
struct vo_refclock {
	__be64 rcd;
	__be32 rcd_ext;
	__be64 gpts_timeout;
	__be64 video_system_pts;
	__be64 audio_system_pts;
	__be64 video_rpts;
	__be64 audio_rpts;
	__be32 video_context;
	__be32 audio_context;
	u8 system_mode, video_mode, audio_mode, master_state;
	__be32 video_free_run_threshold;
	__be32 audio_free_run_threshold;
	__be64 master_gpts;
	__be32 audio_fullness;
	__be32 audio_pause_flag;
	__be32 vo_underflow;
	__be32 ao_underflow;
	__be32 video_end_of_segment;
	__be32 audio_end_of_segment;
	u8 reserved[16];
};

#define AVSYNC_FORCED_SLAVE	0
#define AVSYNC_FORCED_MASTER	1


/*
 * The arguments go as the firmware's C structures, big-endian, laid out as
 * on the MIPS side: shorts and chars are not widened to words, as real XDR
 * would have them. (A window sent as four words came out as 0x0.)
 */
static int vo_rpc_raw(struct rtd_vo *vo, u32 procedure, const void *args,
		      u32 len, u32 *res, u32 nres)
{
	__be32 r[8];
	int i, got;

	got = rtd_acpu_rpc(vo->acpu, procedure, args, DIV_ROUND_UP(len, 4), r,
			   ARRAY_SIZE(r));
	if (got < 0)
		return got;
	dev_dbg(vo->dev, "procedure %u: %d words, %*ph\n", procedure, got,
		got * 4, r);
	for (i = 0; i < min_t(int, got, nres); i++)
		res[i] = be32_to_cpu(r[i]);
	return got;
}

/* A call whose arguments are all words */
static int vo_rpc(struct rtd_vo *vo, u32 procedure, const u32 *args, u32 n,
		  u32 *res, u32 nres)
{
	__be32 a[16];
	int i;

	for (i = 0; i < n; i++)
		a[i] = cpu_to_be32(args[i]);
	return vo_rpc_raw(vo, procedure, a, n * 4, res, nres);
}

static int vo_check(struct rtd_vo *vo, u32 procedure, int got, u32 res)
{
	if (got < 0)
		return got;
	if (got < 1 || res != RTD_ACPU_S_OK) {
		dev_err(vo->dev, "procedure %u: result %#x\n", procedure,
			got ? res : 0);
		return -EIO;
	}
	return 0;
}

/* Calls whose result is just an HRESULT */
static int vo_call_raw(struct rtd_vo *vo, u32 procedure, const void *args,
		       u32 len)
{
	u32 res;

	return vo_check(vo, procedure,
			vo_rpc_raw(vo, procedure, args, len, &res, 1), res);
}

static int vo_call(struct rtd_vo *vo, u32 procedure, const u32 *args, u32 n)
{
	u32 res;
	int got = vo_rpc(vo, procedure, args, n, &res, 1);

	return vo_check(vo, procedure, got, res);
}

/* A kernel RPC with a TV system as its argument and as its result */
static int vo_tv_system_call(struct rtd_vo *vo, u32 cmd, u8 *tv)
{
	u32 param = rtd_acpu_addr(vo->param.phys), ret;
	int err;

	memcpy(vo->param.vaddr, tv, VO_TV_SYSTEM_SIZE);
	wmb();
	err = rtd_acpu_call(vo->acpu, cmd, param, param + VO_TV_SYSTEM_SIZE,
			    &ret);
	if (err)
		return err;
	if (ret != RTD_ACPU_S_OK) {
		dev_err(vo->dev, "kernel RPC %u: %#x\n", cmd, ret);
		return -EIO;
	}
	memcpy(tv, vo->param.vaddr + VO_TV_SYSTEM_SIZE, VO_TV_SYSTEM_SIZE);
	return 0;
}

/* The TV system the firmware runs, VO_TV_SYSTEM_SIZE bytes */
int vo_query_tv_system(struct rtd_vo *vo, u8 *tv)
{
	memset(tv, 0, VO_TV_SYSTEM_SIZE);
	return vo_tv_system_call(vo, VO_KRPC_QUERY_TV_SYSTEM, tv);
}

int vo_config_tv_system(struct rtd_vo *vo, const u8 *tv)
{
	u8 buf[VO_TV_SYSTEM_SIZE];

	memcpy(buf, tv, sizeof(buf));
	return vo_tv_system_call(vo, VO_KRPC_CONFIG_TV_SYSTEM, buf);
}

/* VIDEO_RPC_VO_FILTER_DISPLAY */
static int vo_display(struct rtd_vo *vo, bool zero_buffer)
{
	struct {
		__be32 instance;
		__be32 plane;
		u8 zero_buffer;
		u8 real_time_src;
		u8 pad[2];
	} arg = {
		.instance = cpu_to_be32(vo->instance),
		.plane = cpu_to_be32(VO_VIDEO_PLANE_OSD1),
		.zero_buffer = zero_buffer,
	};

	return vo_call_raw(vo, VIDEO_RPC_VO_FILTER_ToAgent_Display, &arg,
			   sizeof(arg));
}

struct vo_rect {
	__be16 x, y, w, h;
};

/* VIDEO_RPC_VOUT_CONFIG_DISP_WIN: the plane's window, no border */
static int vo_window(struct rtd_vo *vo, u16 width, u16 height)
{
	struct {
		__be32 plane;
		struct vo_rect video;
		struct vo_rect border;
		u8 c1, c2, c3, is_rgb;
		u8 en_border;
		u8 pad[3];
	} arg = {
		.plane = cpu_to_be32(VO_VIDEO_PLANE_OSD1),
		.video = { 0, 0, cpu_to_be16(width), cpu_to_be16(height) },
		.border = { 0, 0, cpu_to_be16(width), cpu_to_be16(height) },
		.c3 = 255, .is_rgb = 1,
	};

	return vo_call_raw(vo, VIDEO_RPC_VOUT_ToAgent_ConfigureDisplayWindow,
			   &arg, sizeof(arg));
}

static void vo_write_cmd(struct rtd_vo *vo, const u32 *cmd, u32 len)
{
	struct vo_ring_hdr __iomem *hdr = vo->shared.vaddr + VO_RING_HDR;
	u32 begin = be32_to_cpu(readl(&hdr->begin));
	u32 size = be32_to_cpu(readl(&hdr->size));
	u32 write = be32_to_cpu(readl(&hdr->write));
	u32 read = be32_to_cpu(readl(&hdr->read[0]));
	u32 i, off;

	if ((read + (read > write ? 0 : size) - write) <= len) {
		dev_warn(vo->dev, "ring full\n");
		return;
	}
	for (i = 0; i < len / 4; i++) {
		off = (write - begin + i * 4) % size;
		writel(cpu_to_be32(cmd[i]), vo->shared.vaddr + off);
	}
	write = begin + (write - begin + len) % size;
	wmb();
	writel(cpu_to_be32(write), &hdr->write);
	rtd_acpu_vo_kick(vo->acpu);
}

/*
 * VIDEO_GRAPHIC_PICTURE_OBJECT: the plane shows this buffer from now on, and
 * keeps reading it. The alpha byte of XRGB8888 is not to be trusted (fbcon
 * leaves it 0), so the plane gets a constant alpha of 0xff instead.
 */
static void vo_show(struct rtd_vo *vo, u32 addr, u32 width, u32 height,
		    u32 pitch)
{
	u32 obj[21] = {
		INBAND_CMD_TYPE_PICTURE_OBJECT, sizeof(obj),
		GRAPHIC_FORMAT_ARGB8888_LITTLE,
		0, 0,			/* PTS */
		vo->context++,
		0xffffffff,		/* no colour key */
		0xff,			/* constant alpha */
		0, 0, width, height,
		addr, pitch,
		0, 0,			/* right eye */
		0,			/* 2D */
		0, 0, 0,		/* no AFBC */
	};

	vo_write_cmd(vo, obj, sizeof(obj));
}

static int vo_start(struct rtd_vo *vo)
{
	struct vo_ring_hdr __iomem *hdr = vo->shared.vaddr + VO_RING_HDR;
	struct vo_refclock *clk = vo->shared.vaddr + VO_REFCLOCK;
	u32 ring = vo->shared.phys, res[2];
	int ret;

	ret = vo_rpc(vo, VIDEO_RPC_ToAgent_Create,
		     (u32[]){ VF_TYPE_VIDEO_OUT }, 1, res, 2);
	if (ret < 0)
		return ret;
	if (ret < 2 || res[0] != RTD_ACPU_S_OK) {
		dev_err(vo->dev, "no video-out instance (%d, %#x)\n", ret, res[0]);
		return -EIO;
	}
	vo->instance = res[1];
	dev_info(vo->dev, "video-out instance %#x\n", vo->instance);

	ret = vo_display(vo, false) ?: vo_window(vo, 1920, 1080);
	if (ret)
		return ret;

	memset(clk, 0, sizeof(*clk));
	clk->rcd = cpu_to_be64(-1LL);
	clk->rcd_ext = cpu_to_be32(-1);
	clk->master_gpts = cpu_to_be64(-1LL);
	clk->video_system_pts = cpu_to_be64(-1LL);
	clk->audio_system_pts = cpu_to_be64(-1LL);
	clk->video_rpts = cpu_to_be64(-1LL);
	clk->audio_rpts = cpu_to_be64(-1LL);
	clk->video_context = cpu_to_be32(-1);
	clk->audio_context = cpu_to_be32(-1);
	clk->video_end_of_segment = cpu_to_be32(-1);
	clk->video_free_run_threshold = cpu_to_be32(0x7fffffff);
	clk->audio_free_run_threshold = cpu_to_be32(0x7fffffff);
	clk->system_mode = AVSYNC_FORCED_SLAVE;
	clk->video_mode = AVSYNC_FORCED_MASTER;
	clk->audio_mode = AVSYNC_FORCED_MASTER;
	ret = vo_call(vo, VIDEO_RPC_ToAgent_SetRefClock,
		      (u32[]){ vo->instance, ring + VO_REFCLOCK }, 2);
	if (ret)
		return ret;

	ret = vo_call(vo, VIDEO_RPC_ToAgent_Run, (u32[]){ vo->instance }, 1);
	if (ret)
		return ret;

	writel(cpu_to_be32(ring), &hdr->begin);
	writel(cpu_to_be32(VO_RING_SIZE), &hdr->size);
	writel(cpu_to_be32(ring), &hdr->write);
	writel(cpu_to_be32(ring), &hdr->read[0]);
	writel(cpu_to_be32(1), &hdr->buffer_id);
	ret = vo_call(vo, VIDEO_RPC_ToAgent_InitRingBuffer,
		      (u32[]){ vo->instance, 0, 0, ring + VO_RING_HDR }, 4);
	if (ret)
		return ret;

	return vo_display(vo, true);
}

static struct rtd_vo *pipe_to_vo(struct drm_simple_display_pipe *pipe)
{
	return container_of(pipe, struct rtd_vo, pipe);
}

static struct rtd_vo *connector_to_vo(struct drm_connector *connector)
{
	return container_of(connector, struct rtd_vo, connector);
}

/*
 * The TV system for a mode, NULL if the firmware has none. *shift says
 * whether it is the 1000/1001 rate of it.
 */
static const struct vo_standard *vo_mode_standard(struct rtd_vo *vo,
						  const struct drm_display_mode *mode,
						  bool *shift)
{
	const struct vo_standard *std = NULL;
	struct drm_display_mode *cea;
	u8 vic = drm_match_cea_mode(mode);
	int i, clock;

	for (i = 0; vic && i < ARRAY_SIZE(vo_standards); i++)
		if (vo_standards[i].vic == vic)
			std = &vo_standards[i];
	if (!std)
		return NULL;

	cea = drm_display_mode_from_cea_vic(&vo->drm, vic);
	if (!cea)
		return NULL;
	clock = cea->clock;
	drm_mode_destroy(&vo->drm, cea);

	*shift = mode->clock != clock;
	if (*shift && !std->shift)
		return NULL;
	return std;
}

/*
 * A new TV system, the way hdmitx_config.c's set_hdmitx_format() makes one:
 * RGB, 8 bits, the AVI infoframe's colorimetry, aspect ratio and VIC. What
 * the DisplayPort side set (the interface type and its own standard) is
 * kept.
 */
static int vo_set_standard(struct rtd_vo *vo, const struct vo_standard *std,
			   bool shift, bool dvi, bool interlaced)
{
	u8 tv[VO_TV_SYSTEM_SIZE];
	int ret;

	ret = vo_query_tv_system(vo, tv);
	if (ret)
		return ret;

	put_unaligned_be32(shift ? std->shift : std->standard, tv + VO_TV_STANDARD);
	tv[VO_TV_EN_PROG] = !interlaced;
	tv[VO_TV_EN_PROG + 1] = 1;	/* enDIF */
	tv[VO_TV_EN_PROG + 2] = 0;	/* enCompRGB */
	put_unaligned_be32(std->pal ? 0x2 : 0x4, tv + VO_TV_VIDEO_DATA0);

	put_unaligned_be32(dvi ? VO_DVI_ON : VO_HDMI_ON, tv + VO_TV_HDMI_MODE);
	put_unaligned_be32(VO_HDMI_AUDIO_48K, tv + VO_TV_AUDIO_FREQ);
	tv[VO_TV_AUDIO_CHANNELS] = 1;
	/* AVI infoframe: RGB; BT.709 from 720p up, else SMPTE 170M; 4:3 or 16:9 */
	tv[VO_TV_AUDIO_CHANNELS + 1] = 0;
	tv[VO_TV_AUDIO_CHANNELS + 2] = (std->standard >= 13 ? 0x80 : 0x40) |
		(std->vic == 2 || std->vic == 17 ? 0x18 : 0x28);
	tv[VO_TV_AUDIO_CHANNELS + 3] = 0;
	tv[VO_TV_AUDIO_CHANNELS + 4] = std->vic;
	tv[VO_TV_AUDIO_CHANNELS + 5] = 0;
	put_unaligned_be32(0, tv + VO_TV_HDMI_DATA0);
	put_unaligned_be32(0, tv + VO_TV_HDMI2);
	put_unaligned_be32(0, tv + VO_TV_HDMI_OFF_MODE);
	put_unaligned_be32(0, tv + VO_TV_HDR_MODE);

	ret = vo_config_tv_system(vo, tv);
	if (ret)
		return ret;
	vo->std = std;
	vo->std_shift = shift;
	vo->std_dvi = dvi;
	dev_info(vo->dev, "TV system %u (VIC %u%s), %s\n",
		 shift ? std->shift : std->standard, std->vic,
		 shift ? " at 1000/1001" : "", dvi ? "DVI" : "HDMI");
	return 0;
}

/* What the DisplayPort side can follow */
bool vo_hdmi_1080p60(struct rtd_vo *vo)
{
	return vo->std && vo->std->vic == VO_BOOT_VIC && !vo->std_shift;
}

static void vo_post(struct rtd_vo *vo, struct drm_plane_state *state)
{
	struct drm_framebuffer *fb = state->fb;

	if (!fb)
		return;
	vo_show(vo, drm_fb_dma_get_gem_addr(fb, state, 0), state->src_w >> 16,
		state->src_h >> 16, fb->pitches[0]);
}

static void vo_pipe_enable(struct drm_simple_display_pipe *pipe,
			   struct drm_crtc_state *crtc_state,
			   struct drm_plane_state *plane_state)
{
	struct rtd_vo *vo = pipe_to_vo(pipe);
	const struct drm_display_mode *mode = &crtc_state->adjusted_mode;
	const struct vo_standard *std;
	bool shift = false, dvi = !vo->connector.display_info.is_hdmi;

	/* without an EDID, HDMI as the boot loader has it */
	if (!vo->connector.edid_blob_ptr)
		dvi = false;

	std = vo_mode_standard(vo, mode, &shift);
	if (std && (std != vo->std || shift != vo->std_shift || dvi != vo->std_dvi)) {
		if (vo_set_standard(vo, std, shift, dvi,
				    mode->flags & DRM_MODE_FLAG_INTERLACE))
			dev_err(vo->dev, "%s: no TV system\n", mode->name);
	}
	vo_window(vo, mode->hdisplay, mode->vdisplay);
	vo_post(vo, plane_state);
}

static void vo_pipe_update(struct drm_simple_display_pipe *pipe,
			   struct drm_plane_state *old_state)
{
	struct drm_plane_state *state = pipe->plane.state;

	if (state->fb && state->fb != old_state->fb)
		vo_post(pipe_to_vo(pipe), state);
}

static enum drm_mode_status vo_pipe_mode_valid(struct drm_simple_display_pipe *pipe,
					       const struct drm_display_mode *mode)
{
	struct rtd_vo *vo = pipe_to_vo(pipe);
	const struct vo_standard *std;
	bool shift;

	std = vo_mode_standard(vo, mode, &shift);
	if (!std)
		return MODE_BAD;
	/* DP, when it is on, runs 1080p60 only, and from the same source */
	if (vo_dp_on(vo) && (std->vic != VO_BOOT_VIC || shift))
		return MODE_BAD;
	return MODE_OK;
}

static const struct drm_simple_display_pipe_funcs vo_pipe_funcs = {
	.mode_valid = vo_pipe_mode_valid,
	.enable = vo_pipe_enable,
	.update = vo_pipe_update,
};

static enum drm_connector_status vo_detect(struct drm_connector *connector,
					   bool force)
{
	struct rtd_vo *vo = connector_to_vo(connector);
	u32 v = 0;

	if (!vo->iso)
		return connector_status_connected;
	regmap_read(vo->iso, ISO_GPDATI, &v);
	return v & HDMI_HPD ? connector_status_connected :
			      connector_status_disconnected;
}

/*
 * The monitor's modes. Without an EDID (no DDC, or nothing answering on it),
 * all of vo_standards[], with 1080p60 -- what the boot loader set --
 * preferred.
 */
static int vo_get_modes(struct drm_connector *connector)
{
	struct rtd_vo *vo = connector_to_vo(connector);
	const struct drm_edid *edid = NULL;
	struct drm_display_mode *mode;
	int i, n = 0;

	if (vo->ddc)
		edid = drm_edid_read_ddc(connector, vo->ddc);
	drm_edid_connector_update(connector, edid);
	if (edid) {
		n = drm_edid_connector_add_modes(connector);
		drm_edid_free(edid);
		if (n)
			return n;
	}

	for (i = 0; i < ARRAY_SIZE(vo_standards); i++) {
		mode = drm_display_mode_from_cea_vic(connector->dev,
						     vo_standards[i].vic);
		if (!mode)
			continue;
		if (vo_standards[i].vic == VO_BOOT_VIC)
			mode->type |= DRM_MODE_TYPE_PREFERRED;
		drm_mode_probed_add(connector, mode);
		n++;
	}
	return n;
}

static const struct drm_connector_helper_funcs vo_connector_helper_funcs = {
	.get_modes = vo_get_modes,
};

static const struct drm_connector_funcs vo_connector_funcs = {
	.detect = vo_detect,
	.fill_modes = drm_helper_probe_single_connector_modes,
	.destroy = drm_connector_cleanup,
	.reset = drm_atomic_helper_connector_reset,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};

static const struct drm_mode_config_funcs vo_mode_config_funcs = {
	.fb_create = drm_gem_fb_create,
	.atomic_check = drm_atomic_helper_check,
	.atomic_commit = drm_atomic_helper_commit,
};

static const u32 vo_formats[] = {
	DRM_FORMAT_XRGB8888,
};

DEFINE_DRM_GEM_DMA_FOPS(vo_fops);

static const struct drm_driver vo_drm_driver = {
	.driver_features = DRIVER_GEM | DRIVER_MODESET | DRIVER_ATOMIC,
	.fops = &vo_fops,
	DRM_GEM_DMA_DRIVER_OPS,
	DRM_FBDEV_DMA_DRIVER_OPS,
	.name = "rtd129x-vo",
	.desc = "Realtek RTD129x video output",
	.major = 1,
	.minor = 0,
};

static int vo_kms_init(struct rtd_vo *vo)
{
	struct drm_device *drm = &vo->drm;
	int ret;

	ret = drmm_mode_config_init(drm);
	if (ret)
		return ret;
	drm->mode_config.min_width = 640;
	drm->mode_config.max_width = VO_MAX_WIDTH;
	drm->mode_config.min_height = 480;
	drm->mode_config.max_height = VO_MAX_HEIGHT;
	drm->mode_config.funcs = &vo_mode_config_funcs;

	drm_connector_helper_add(&vo->connector, &vo_connector_helper_funcs);
	ret = drm_connector_init(drm, &vo->connector, &vo_connector_funcs,
				 DRM_MODE_CONNECTOR_HDMIA);
	if (ret)
		return ret;
	vo->connector.interlace_allowed = true;
	if (vo->iso)
		vo->connector.polled = DRM_CONNECTOR_POLL_CONNECT |
				       DRM_CONNECTOR_POLL_DISCONNECT;

	ret = drm_simple_display_pipe_init(drm, &vo->pipe, &vo_pipe_funcs,
					   vo_formats, ARRAY_SIZE(vo_formats),
					   NULL, &vo->connector);
	if (ret)
		return ret;
	drm_mode_config_reset(drm);
	drmm_kms_helper_poll_init(drm);
	return 0;
}

/*
 * The monitor's side, from the hdmitx node: its DDC and its HPD line. Either
 * may be missing; the driver then offers all the modes it has and takes the
 * monitor as connected.
 */
static void vo_hdmi_init(struct rtd_vo *vo)
{
	struct device_node *np, *ddc;
	u8 tv[VO_TV_SYSTEM_SIZE];
	u32 standard;
	int i;

	np = of_find_compatible_node(NULL, NULL, "realtek,rtd1295-hdmitx");
	if (np && of_device_is_available(np)) {
		ddc = of_parse_phandle(np, "ddc-i2c-bus", 0);
		if (ddc) {
			vo->ddc = of_get_i2c_adapter_by_node(ddc);
			of_node_put(ddc);
		}
		vo->iso = syscon_regmap_lookup_by_phandle(np, "realtek,iso");
		if (IS_ERR(vo->iso))
			vo->iso = NULL;
	}
	of_node_put(np);
	if (!vo->ddc)
		dev_warn(vo->dev, "no DDC: no EDID, all modes offered\n");

	/* the TV system the boot loader left */
	if (vo_query_tv_system(vo, tv))
		return;
	standard = get_unaligned_be32(tv + VO_TV_STANDARD);
	for (i = 0; i < ARRAY_SIZE(vo_standards); i++) {
		if (vo_standards[i].standard == standard ||
		    (vo_standards[i].shift && vo_standards[i].shift == standard)) {
			vo->std = &vo_standards[i];
			vo->std_shift = vo_standards[i].standard != standard;
			break;
		}
	}
	vo->std_dvi = get_unaligned_be32(tv + VO_TV_HDMI_MODE) == VO_DVI_ON;
	dev_info(vo->dev, "TV system %u at boot\n", standard);
}

static int vo_probe(struct auxiliary_device *adev,
		    const struct auxiliary_device_id *id)
{
	struct rtd_acpu_adev *aadev = container_of(adev, struct rtd_acpu_adev, adev);
	struct device *dev = &adev->dev;
	struct rtd_vo *vo;
	int ret;

	ret = dma_coerce_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		return ret;
	ret = of_reserved_mem_device_init_by_name(dev, dev->parent->of_node, "vo");
	if (ret)
		return dev_err_probe(dev, ret, "no vo memory region\n");

	vo = devm_drm_dev_alloc(dev, &vo_drm_driver, struct rtd_vo, drm);
	if (IS_ERR(vo)) {
		ret = PTR_ERR(vo);
		goto err_rmem;
	}
	vo->dev = dev;
	vo->acpu = aadev->acpu;

	ret = rtd_acpu_alloc(vo->acpu, VO_SHARED_SIZE, &vo->shared) ?:
	      rtd_acpu_alloc(vo->acpu, 2 * VO_TV_SYSTEM_SIZE, &vo->param);
	if (ret)
		goto err_rmem;
	ret = vo_start(vo);
	if (ret)
		goto err_shared;
	vo_hdmi_init(vo);

	ret = vo_kms_init(vo);
	if (ret)
		goto err_shared;
	ret = drm_dev_register(&vo->drm, 0);
	if (ret)
		goto err_shared;
	auxiliary_set_drvdata(adev, vo);
	drm_client_setup(&vo->drm, NULL);

	/* the DisplayPort output shows the same picture, if there is one */
	ret = vo_dp_init(vo);
	if (ret)
		dev_warn(dev, "no DisplayPort output: %d\n", ret);
	return 0;

err_shared:
	/* the firmware may still hold on to it; leave it be */
	dev_err(dev, "video output not started: %d\n", ret);
	if (vo->ddc)
		i2c_put_adapter(vo->ddc);
err_rmem:
	of_reserved_mem_device_release(dev);
	return ret;
}

static const struct auxiliary_device_id vo_id_table[] = {
	{ .name = "rtd129x_acpu." RTD_ACPU_VO_NAME },
	{ }
};
MODULE_DEVICE_TABLE(auxiliary, vo_id_table);

static struct auxiliary_driver vo_driver = {
	.probe = vo_probe,
	.id_table = vo_id_table,
	.driver = {
		/* the firmware keeps what it was given */
		.suppress_bind_attrs = true,
	},
};
module_auxiliary_driver(vo_driver);

MODULE_DESCRIPTION("Realtek RTD129x video output");
MODULE_LICENSE("GPL");
