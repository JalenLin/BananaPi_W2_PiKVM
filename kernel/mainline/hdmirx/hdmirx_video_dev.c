// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * hdmirx_video_dev.c - the V4L2 capture device of the RTD129x HDMI receiver
 *
 * Copyright (C) 2017 Realtek Semiconductor Corporation
 *
 * Rewritten on v4l2_ioctl_ops for mainline. The BSP's version dispatched
 * every ioctl through its own switch statement, with its own 32-bit compat
 * layer, and only grew the ioctls a generic capture client needs (G_FMT,
 * ENUMINPUT, ...) as the main branch of this project hit each one. The
 * behaviour those fixes settled on is kept:
 *
 *  - G_FMT reports the detected input until userspace sets a format, so a
 *    client that asks first gets a size it can use;
 *  - S_FMT writes back bytesperline/sizeimage, with bytesperline the
 *    stride of the Y plane for the semi-planar formats;
 *  - the output may be scaled down from the input, never up.
 *
 * New here: the DV timings ioctls and V4L2_EVENT_SOURCE_CHANGE, so a client
 * such as ustreamer can follow the source's resolution by itself.
 */

#include <linux/v4l2-dv-timings.h>
#include <media/v4l2-dv-timings.h>

#include "hdmirx_video_dev.h"
#include "hdmirx_wrapper.h"
#include "mipi_wrapper.h"
#include "hdmirx_reg.h"

#include "rx_drv/hdmiInternal.h"

#define MAX_WIDTH	4096
#define MAX_HEIGHT	2160

extern MIPI_TOP_INFO mipi_top;
extern HDMI_INFO_T hdmi;
extern HDMIRX_IOCTL_STRUCT_T hdmi_ioctl_struct;

extern unsigned int rx_pitch_measurement(unsigned int output_h,
					 MIPI_OUT_COLOR_SPACE_T output_color);

struct hdmirx_format_desc {
	u32 fourcc;
	MIPI_OUT_COLOR_SPACE_T color;
	MIPI_YUV420_UV_SEQ uv_seq;
};

/* What the MIPI wrapper can write. NV16 first: it is the default. */
static const struct hdmirx_format_desc hdmirx_formats[] = {
	{ V4L2_PIX_FMT_NV16,  OUT_8BIT_YUV422, UV_NV12 },
	{ V4L2_PIX_FMT_NV12,  OUT_8BIT_YUV420, UV_NV12 },
	{ V4L2_PIX_FMT_NV21,  OUT_8BIT_YUV420, UV_NV21 },
	/* byte order B, G, R, A */
	{ V4L2_PIX_FMT_BGR32, OUT_ARGB,        UV_NV12 },
};

static const struct hdmirx_format_desc *find_format(u32 fourcc)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(hdmirx_formats); i++)
		if (hdmirx_formats[i].fourcc == fourcc)
			return &hdmirx_formats[i];
	return NULL;
}

int out_color_to_bpp(unsigned int output_color)
{
	switch (output_color) {
	case OUT_8BIT_YUV420:
		return 12;	/* NV12, NV21 */
	case OUT_8BIT_YUV422:
		return 16;	/* NV16: a W*H Y plane and a W*H CbCr plane */
	default:
		return 32;	/* BGR32, and the 10-bit YUV422 layout */
	}
}

static u32 out_color_to_fourcc(unsigned int output_color)
{
	switch (output_color) {
	case OUT_8BIT_YUV420:
		return mipi_top.uv_seq == UV_NV21 ?
			V4L2_PIX_FMT_NV21 : V4L2_PIX_FMT_NV12;
	case OUT_ARGB:
		return V4L2_PIX_FMT_BGR32;
	default:
		return V4L2_PIX_FMT_NV16;
	}
}

static bool signal_present(void)
{
	return hdmi_ioctl_struct.measure_ready && mipi_top.h_input_len &&
	       mipi_top.v_input_len;
}

/* The input frame size; an interlaced input is reported as whole frames. */
static void input_size(unsigned int *w, unsigned int *h)
{
	*w = mipi_top.h_input_len;
	*h = mipi_top.v_input_len;
	if (!hdmi.tx_timing.progressive)
		*h <<= 1;
}

/* Must agree with hdmi_queue_setup() so userspace never sees two sizes. */
static void fill_pix_format(struct v4l2_pix_format *pix, unsigned int w,
			    unsigned int h, unsigned int color, u32 fourcc)
{
	int bpp = out_color_to_bpp(color);

	memset(pix, 0, sizeof(*pix));
	pix->width = w;
	pix->height = h;
	pix->pixelformat = fourcc;
	pix->field = hdmi.tx_timing.progressive ?
		V4L2_FIELD_NONE : V4L2_FIELD_INTERLACED;
	pix->colorspace = V4L2_COLORSPACE_SRGB;

	if (color >= OUT_ARGB) {
		pix->bytesperline = w * bpp / 8;
		pix->sizeimage = w * h * bpp / 8;
	} else {
		/* semi-planar: bytesperline is the Y plane's stride */
		pix->bytesperline = roundup16(w);
		pix->sizeimage = roundup16(w) * roundup16(h) * bpp / 8;
	}
}

static int hdmirx_querycap(struct file *file, void *priv,
			   struct v4l2_capability *cap)
{
	strscpy(cap->driver, "rtd129x-hdmirx", sizeof(cap->driver));
	strscpy(cap->card, "RTD129x HDMI RX", sizeof(cap->card));
	strscpy(cap->bus_info, "platform:rtd129x-hdmirx", sizeof(cap->bus_info));
	return 0;
}

static int hdmirx_enum_fmt(struct file *file, void *priv,
			   struct v4l2_fmtdesc *f)
{
	if (f->index >= ARRAY_SIZE(hdmirx_formats))
		return -EINVAL;
	f->pixelformat = hdmirx_formats[f->index].fourcc;
	return 0;
}

static int hdmirx_g_fmt(struct file *file, void *priv, struct v4l2_format *f)
{
	struct v4l2_hdmi_dev *dev = video_drvdata(file);
	unsigned int w, h;

	if (dev->fmt_set) {
		fill_pix_format(&f->fmt.pix, dev->width, dev->height, dev->outfmt,
				out_color_to_fourcc(dev->outfmt));
		return 0;
	}

	/*
	 * Nothing set yet: report the detected input rather than probe's
	 * defaults, so a client that asks first gets a size it can capture.
	 */
	input_size(&w, &h);
	if (!w || !h) {
		w = dev->width;
		h = dev->height;
	}
	fill_pix_format(&f->fmt.pix, w, h, dev->outfmt,
			out_color_to_fourcc(dev->outfmt));
	return 0;
}

static int hdmirx_try_fmt(struct file *file, void *priv, struct v4l2_format *f)
{
	struct v4l2_pix_format *pix = &f->fmt.pix;
	const struct hdmirx_format_desc *fmt;
	unsigned int w, h, max_w, max_h;

	fmt = find_format(pix->pixelformat);
	if (!fmt)
		fmt = &hdmirx_formats[0];

	/* The hardware can only scale down. */
	input_size(&max_w, &max_h);
	if (!max_w || !max_h) {
		max_w = MAX_WIDTH;
		max_h = MAX_HEIGHT;
	}
	w = clamp(pix->width, 48U, max_w);
	h = clamp(pix->height, 32U, max_h);
	if (!pix->width)
		w = max_w;
	if (!pix->height)
		h = max_h;

	fill_pix_format(pix, w, h, fmt->color, fmt->fourcc);
	return 0;
}

static int hdmirx_s_fmt(struct file *file, void *priv, struct v4l2_format *f)
{
	struct v4l2_hdmi_dev *dev = video_drvdata(file);
	const struct hdmirx_format_desc *fmt;
	int ret;

	if (vb2_is_busy(&dev->queue))
		return -EBUSY;

	ret = hdmirx_try_fmt(file, priv, f);
	if (ret)
		return ret;

	fmt = find_format(f->fmt.pix.pixelformat);
	dev->width = f->fmt.pix.width;
	dev->height = f->fmt.pix.height;
	dev->outfmt = fmt->color;
	dev->bpp = out_color_to_bpp(fmt->color);
	dev->fmt_set = true;
	if (fmt->color == OUT_8BIT_YUV420)
		mipi_top.uv_seq = fmt->uv_seq;

	HDMIRX_INFO("S_FMT %ux%u %p4cc", dev->width, dev->height,
		    &f->fmt.pix.pixelformat);
	return 0;
}

static int hdmirx_enum_input(struct file *file, void *priv,
			     struct v4l2_input *inp)
{
	if (inp->index != 0)
		return -EINVAL;
	strscpy(inp->name, "HDMI", sizeof(inp->name));
	inp->type = V4L2_INPUT_TYPE_CAMERA;
	inp->capabilities = V4L2_IN_CAP_DV_TIMINGS;
	if (!signal_present())
		inp->status = V4L2_IN_ST_NO_SIGNAL;
	return 0;
}

static int hdmirx_g_input(struct file *file, void *priv, unsigned int *i)
{
	*i = 0;
	return 0;
}

static int hdmirx_s_input(struct file *file, void *priv, unsigned int i)
{
	return i ? -EINVAL : 0;
}

static unsigned int input_fps(void)
{
	unsigned int vic = hdmi_ioctl_struct.measure_ready ?
		drvif_Hdmi_AVI_VIC() : 0;
	unsigned int fps = hdmi_vic_table[vic].fps;

	if (hdmi_vic_table[vic].interlace)
		fps /= 2;
	return fps ? fps : 60;
}

static int hdmirx_enum_framesizes(struct file *file, void *priv,
				  struct v4l2_frmsizeenum *fsize)
{
	unsigned int w, h;

	if (fsize->index != 0 || !find_format(fsize->pixel_format))
		return -EINVAL;
	if (!signal_present())
		return -EINVAL;

	input_size(&w, &h);
	fsize->type = V4L2_FRMSIZE_TYPE_CONTINUOUS;
	fsize->stepwise.min_width = w / 32;
	fsize->stepwise.max_width = w;
	fsize->stepwise.step_width = 1;
	fsize->stepwise.min_height = h / 32;
	fsize->stepwise.max_height = h;
	fsize->stepwise.step_height = 1;
	return 0;
}

static int hdmirx_enum_frameintervals(struct file *file, void *priv,
				      struct v4l2_frmivalenum *fival)
{
	if (fival->index != 0 || !find_format(fival->pixel_format))
		return -EINVAL;
	fival->type = V4L2_FRMIVAL_TYPE_DISCRETE;
	fival->discrete.numerator = 1;
	fival->discrete.denominator = input_fps();
	return 0;
}

/* The rate is the source's; there is nothing to set. */
static int hdmirx_g_parm(struct file *file, void *priv,
			 struct v4l2_streamparm *a)
{
	if (a->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;
	a->parm.capture.capability = V4L2_CAP_TIMEPERFRAME;
	a->parm.capture.readbuffers = 2;
	a->parm.capture.timeperframe.numerator = 1;
	a->parm.capture.timeperframe.denominator = input_fps();
	return 0;
}

static const struct v4l2_dv_timings_cap hdmirx_timings_cap = {
	.type = V4L2_DV_BT_656_1120,
	.reserved = { 0 },
	V4L2_INIT_BT_TIMINGS(640, MAX_WIDTH, 480, MAX_HEIGHT,
			     25000000, 600000000,
			     V4L2_DV_BT_STD_CEA861 | V4L2_DV_BT_STD_DMT,
			     V4L2_DV_BT_CAP_PROGRESSIVE |
			     V4L2_DV_BT_CAP_INTERLACED)
};

static int hdmirx_query_dv_timings(struct file *file, void *priv,
				   struct v4l2_dv_timings *timings)
{
	struct v4l2_bt_timings *bt = &timings->bt;
	unsigned int vic, w, h;

	if (!signal_present())
		return -ENOLINK;

	/*
	 * A CEA VIC gives the full timings, blanking and pixel clock included.
	 * Without one (DVI, or a mode outside CEA-861) only the active size and
	 * the rate are known; fill in what there is.
	 */
	vic = drvif_Hdmi_AVI_VIC();
	memset(timings, 0, sizeof(*timings));
	if (vic && v4l2_find_dv_timings_cea861_vic(timings, vic))
		return 0;

	input_size(&w, &h);
	timings->type = V4L2_DV_BT_656_1120;
	bt->width = w;
	bt->height = h;
	bt->interlaced = !hdmi.tx_timing.progressive;
	bt->pixelclock = (u64)w * h * input_fps();
	return 0;
}

static int hdmirx_g_dv_timings(struct file *file, void *priv,
			       struct v4l2_dv_timings *timings)
{
	return hdmirx_query_dv_timings(file, priv, timings);
}

/* The source decides; accept whatever matches it. */
static int hdmirx_s_dv_timings(struct file *file, void *priv,
			       struct v4l2_dv_timings *timings)
{
	struct v4l2_hdmi_dev *dev = video_drvdata(file);

	if (vb2_is_busy(&dev->queue))
		return -EBUSY;
	return 0;
}

static int hdmirx_dv_timings_cap(struct file *file, void *priv,
				 struct v4l2_dv_timings_cap *cap)
{
	*cap = hdmirx_timings_cap;
	return 0;
}

static int hdmirx_enum_dv_timings(struct file *file, void *priv,
				  struct v4l2_enum_dv_timings *timings)
{
	return v4l2_enum_dv_timings_cap(timings, &hdmirx_timings_cap, NULL, NULL);
}

static int hdmirx_subscribe_event(struct v4l2_fh *fh,
				  const struct v4l2_event_subscription *sub)
{
	switch (sub->type) {
	case V4L2_EVENT_SOURCE_CHANGE:
		return v4l2_src_change_event_subscribe(fh, sub);
	default:
		return -EINVAL;
	}
}

void hdmirx_source_changed(struct v4l2_hdmi_dev *dev)
{
	static const struct v4l2_event ev = {
		.type = V4L2_EVENT_SOURCE_CHANGE,
		.u.src_change.changes = V4L2_EVENT_SRC_CH_RESOLUTION,
	};

	v4l2_event_queue(&dev->vdev, &ev);
}

static const struct v4l2_ioctl_ops hdmirx_ioctl_ops = {
	.vidioc_querycap		= hdmirx_querycap,
	.vidioc_enum_fmt_vid_cap	= hdmirx_enum_fmt,
	.vidioc_g_fmt_vid_cap		= hdmirx_g_fmt,
	.vidioc_try_fmt_vid_cap		= hdmirx_try_fmt,
	.vidioc_s_fmt_vid_cap		= hdmirx_s_fmt,
	.vidioc_enum_framesizes		= hdmirx_enum_framesizes,
	.vidioc_enum_frameintervals	= hdmirx_enum_frameintervals,
	.vidioc_g_parm			= hdmirx_g_parm,
	.vidioc_s_parm			= hdmirx_g_parm,

	.vidioc_enum_input		= hdmirx_enum_input,
	.vidioc_g_input			= hdmirx_g_input,
	.vidioc_s_input			= hdmirx_s_input,

	.vidioc_query_dv_timings	= hdmirx_query_dv_timings,
	.vidioc_g_dv_timings		= hdmirx_g_dv_timings,
	.vidioc_s_dv_timings		= hdmirx_s_dv_timings,
	.vidioc_dv_timings_cap		= hdmirx_dv_timings_cap,
	.vidioc_enum_dv_timings		= hdmirx_enum_dv_timings,

	.vidioc_reqbufs			= vb2_ioctl_reqbufs,
	.vidioc_create_bufs		= vb2_ioctl_create_bufs,
	.vidioc_prepare_buf		= vb2_ioctl_prepare_buf,
	.vidioc_querybuf		= vb2_ioctl_querybuf,
	.vidioc_qbuf			= vb2_ioctl_qbuf,
	.vidioc_dqbuf			= vb2_ioctl_dqbuf,
	.vidioc_expbuf			= vb2_ioctl_expbuf,
	.vidioc_streamon		= vb2_ioctl_streamon,
	.vidioc_streamoff		= vb2_ioctl_streamoff,

	.vidioc_subscribe_event		= hdmirx_subscribe_event,
	.vidioc_unsubscribe_event	= v4l2_event_unsubscribe,
};

static const struct v4l2_file_operations hdmirx_fops = {
	.owner		= THIS_MODULE,
	.open		= v4l2_fh_open,
	.release	= vb2_fop_release,
	.poll		= vb2_fop_poll,
	.mmap		= vb2_fop_mmap,
	.unlocked_ioctl	= video_ioctl2,
};

/*
 * sysfs, next to the video node. kvmd's udev rule and this project's
 * hdmirx-info / hdmirx-capture find the device by hdmirx_video_info; the
 * *_state files replace the BSP's Android switch devices
 * (/sys/class/switch/rx_video/state and friends).
 */
static const char * const type_str[] = {"MIPI", "HDMIRx"};
static const char * const status_str[] = {"NotReady", "Ready"};
static const char * const color_str[] = {"RGB", "YUV444", "YUV422", "?"};
static const char * const scan_str[] = {"Interlaced", "Progressive"};

static ssize_t hdmirx_video_info_show(struct device *d,
				      struct device_attribute *attr, char *buf)
{
	unsigned int w, h;

	input_size(&w, &h);
	return sysfs_emit(buf,
		"Type:%s\nStatus:%s\nWidth:%u\nHeight:%u\nScanMode:%s\nColor:%s\nFps:%u\n",
		type_str[mipi_top.src_sel & 1],
		status_str[hdmi_ioctl_struct.measure_ready & 1], w, h,
		scan_str[hdmi.tx_timing.progressive & 1],
		color_str[mipi_top.input_color & 3],
		hdmi_ioctl_struct.measure_ready ? input_fps() : 0);
}
static DEVICE_ATTR_RO(hdmirx_video_info);

static const char * const lpcm_str[] = {"LPCM", "Non-LPCM", "N/A"};

static ssize_t hdmirx_audio_info_show(struct device *d,
				      struct device_attribute *attr, char *buf)
{
	unsigned int ready = hdmi_ioctl_struct.audio_detect_done;

	return sysfs_emit(buf, "Ready:%u\nFreq:%u\nSPDIF Type:%s\n", ready,
			  ready ? hdmi.audio_freq : 0,
			  lpcm_str[ready ? (hdmi.spdif_type & 1) : 2]);
}
static DEVICE_ATTR_RO(hdmirx_audio_info);

static ssize_t hdmirx_bufcnt_show(struct device *d,
				  struct device_attribute *attr, char *buf)
{
	struct v4l2_hdmi_dev *dev = dev_get_drvdata(d);

	return sysfs_emit(buf, "qcnt:%d rcnt:%d\n",
			  atomic_read(&dev->hdmidq.qcnt),
			  atomic_read(&dev->hdmidq.rcnt));
}
static DEVICE_ATTR_RO(hdmirx_bufcnt);

#define HDMIRX_STATE_ATTR(_name)					\
static ssize_t _name##_show(struct device *d,				\
			    struct device_attribute *attr, char *buf)	\
{									\
	struct v4l2_hdmi_dev *dev = dev_get_drvdata(d);			\
									\
	return sysfs_emit(buf, "%d\n", dev->_name);			\
}									\
static DEVICE_ATTR_RO(_name)

HDMIRX_STATE_ATTR(rx_video_state);
HDMIRX_STATE_ATTR(rx_audio_state);
HDMIRX_STATE_ATTR(rx_hdcp_state);

static struct attribute *hdmirx_attrs[] = {
	&dev_attr_hdmirx_video_info.attr,
	&dev_attr_hdmirx_audio_info.attr,
	&dev_attr_hdmirx_bufcnt.attr,
	&dev_attr_rx_video_state.attr,
	&dev_attr_rx_audio_state.attr,
	&dev_attr_rx_hdcp_state.attr,
	NULL
};

static const struct attribute_group hdmirx_attr_group = {
	.attrs = hdmirx_attrs,
};

int register_video_device(struct v4l2_hdmi_dev *hdmi_dev)
{
	struct video_device *vdev = &hdmi_dev->vdev;
	int ret;

	strscpy(vdev->name, "rtd129x-hdmirx", sizeof(vdev->name));
	vdev->fops = &hdmirx_fops;
	vdev->ioctl_ops = &hdmirx_ioctl_ops;
	vdev->release = video_device_release_empty;
	vdev->v4l2_dev = &hdmi_dev->v4l2_dev;
	vdev->queue = &hdmi_dev->queue;
	vdev->lock = &hdmi_dev->mutex;
	vdev->vfl_dir = VFL_DIR_RX;
	vdev->device_caps = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING;
	video_set_drvdata(vdev, hdmi_dev);

	ret = video_register_device(vdev, VFL_TYPE_VIDEO, -1);
	if (ret)
		return ret;

	ret = sysfs_create_group(&vdev->dev.kobj, &hdmirx_attr_group);
	if (ret) {
		video_unregister_device(vdev);
		return ret;
	}

	HDMIRX_INFO("registered %s", video_device_node_name(vdev));
	return 0;
}

void unregister_video_device(struct v4l2_hdmi_dev *hdmi_dev)
{
	sysfs_remove_group(&hdmi_dev->vdev.dev.kobj, &hdmirx_attr_group);
	video_unregister_device(&hdmi_dev->vdev);
}
