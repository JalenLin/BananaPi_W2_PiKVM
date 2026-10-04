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
 * connector with the one mode the firmware runs. Changing the TV system is
 * left to the boot loader. Scanout buffers come from a reserved pool below
 * 512 MiB (the "vo" region of the audio CPU node), which is what the audio
 * CPU can reach.
 */
#include <linux/auxiliary_bus.h>
#include <linux/dma-mapping.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of_reserved_mem.h>

#include <drm/clients/drm_client_setup.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_drv.h>
#include <drm/drm_fb_dma_helper.h>
#include <drm/drm_fbdev_dma.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem_atomic_helper.h>
#include <drm/drm_gem_dma_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_simple_kms_helper.h>

#include "rtd129x-acpu.h"

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

/* What the boot loader has the firmware run: CEA-861 VIC 16, 1080p60 */
static const struct drm_display_mode vo_mode = {
	DRM_MODE("1920x1080", DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED,
		 148500, 1920, 2008, 2052, 2200, 0, 1080, 1084, 1089, 1125, 0,
		 DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC),
	.picture_aspect_ratio = HDMI_PICTURE_ASPECT_16_9,
};

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

struct rtd_vo {
	struct drm_device drm;
	struct drm_simple_display_pipe pipe;
	struct drm_connector connector;

	struct device *dev;
	struct rtd_acpu *acpu;
	struct rtd_acpu_buf shared;	/* ring, header, refclock */
	u32 instance;
	u32 context;
};

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
static int vo_window(struct rtd_vo *vo)
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
		.video = { 0, 0, cpu_to_be16(1920), cpu_to_be16(1080) },
		.border = { 0, 0, cpu_to_be16(1920), cpu_to_be16(1080) },
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

	ret = vo_display(vo, false) ?: vo_window(vo);
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

static void vo_post(struct rtd_vo *vo, struct drm_plane_state *state)
{
	struct drm_framebuffer *fb = state->fb;

	if (!fb)
		return;
	vo_show(vo, drm_fb_dma_get_gem_addr(fb, state, 0), fb->width,
		fb->height, fb->pitches[0]);
}

static void vo_pipe_enable(struct drm_simple_display_pipe *pipe,
			   struct drm_crtc_state *crtc_state,
			   struct drm_plane_state *plane_state)
{
	vo_post(pipe_to_vo(pipe), plane_state);
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
	return drm_mode_match(mode, &vo_mode, DRM_MODE_MATCH_TIMINGS) ?
	       MODE_OK : MODE_BAD;
}

static const struct drm_simple_display_pipe_funcs vo_pipe_funcs = {
	.mode_valid = vo_pipe_mode_valid,
	.enable = vo_pipe_enable,
	.update = vo_pipe_update,
};

static int vo_get_modes(struct drm_connector *connector)
{
	struct drm_display_mode *mode;

	mode = drm_mode_duplicate(connector->dev, &vo_mode);
	if (!mode)
		return 0;
	drm_mode_probed_add(connector, mode);
	connector->display_info.width_mm = 0;
	connector->display_info.height_mm = 0;
	return 1;
}

static const struct drm_connector_helper_funcs vo_connector_helper_funcs = {
	.get_modes = vo_get_modes,
};

static const struct drm_connector_funcs vo_connector_funcs = {
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
	drm->mode_config.min_width = vo_mode.hdisplay;
	drm->mode_config.max_width = vo_mode.hdisplay;
	drm->mode_config.min_height = vo_mode.vdisplay;
	drm->mode_config.max_height = vo_mode.vdisplay;
	drm->mode_config.funcs = &vo_mode_config_funcs;

	drm_connector_helper_add(&vo->connector, &vo_connector_helper_funcs);
	ret = drm_connector_init(drm, &vo->connector, &vo_connector_funcs,
				 DRM_MODE_CONNECTOR_HDMIA);
	if (ret)
		return ret;

	ret = drm_simple_display_pipe_init(drm, &vo->pipe, &vo_pipe_funcs,
					   vo_formats, ARRAY_SIZE(vo_formats),
					   NULL, &vo->connector);
	if (ret)
		return ret;
	drm_mode_config_reset(drm);
	return 0;
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

	ret = rtd_acpu_alloc(vo->acpu, VO_SHARED_SIZE, &vo->shared);
	if (ret)
		goto err_rmem;
	ret = vo_start(vo);
	if (ret)
		goto err_shared;

	ret = vo_kms_init(vo);
	if (ret)
		goto err_shared;
	ret = drm_dev_register(&vo->drm, 0);
	if (ret)
		goto err_shared;
	auxiliary_set_drvdata(adev, vo);
	drm_client_setup(&vo->drm, NULL);
	return 0;

err_shared:
	/* the firmware may still hold on to it; leave it be */
	dev_err(dev, "video output not started: %d\n", ret);
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
