/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * v4l2_hdmi_dev.h - RTK hdmi rx driver header file
 *
 * Copyright (C) 2017 Realtek Semiconductor Corporation
 *
 * The V4L2 side of this driver was rewritten for mainline (6.18): the 4.9
 * BSP hand-rolled its ioctl dispatch, its compat layer and parts of vb2,
 * and depended on ION and Android's switch class. What is kept from the BSP
 * is everything below the V4L2 boundary -- rx_drv/, the HDMI RX and MIPI
 * wrappers, and the clock sequencing.
 */

#ifndef V4L2_HDMI_DEV_H
#define V4L2_HDMI_DEV_H

#include <linux/delay.h>
#include <linux/device.h>
#include <linux/interrupt.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/videodev2.h>

#include <media/v4l2-device.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-event.h>
#include <media/v4l2-fh.h>
#include <media/videobuf2-v4l2.h>
#include <media/videobuf2-dma-contig.h>

#define __RTK_HDMI_RX_DEBUG__	0

#if __RTK_HDMI_RX_DEBUG__
#define HDMIRX_DEBUG(format, ...) pr_err("[HDMI RX DBG]" format "\n", ## __VA_ARGS__)
#else
#define HDMIRX_DEBUG(format, ...) do { } while (0)
#endif

#define HDMIRX_ERROR(format, ...) pr_err("[HDMI RX ERR]" format "\n", ## __VA_ARGS__)
#define HDMIRX_INFO(format, ...) pr_info("[HDMI RX]" format "\n", ## __VA_ARGS__)

#define roundup16(x)	roundup(x, 16)

/* buffer for one video frame */
struct hdmi_buffer {
	struct vb2_v4l2_buffer vb;	/* must be first */
	u32 phys;
	struct list_head list;
};

struct hdmi_dmaqueue {
	atomic_t rcnt;	/* buffers completed by the hardware */
	atomic_t qcnt;	/* buffers waiting for the hardware */
	struct list_head active;
	struct hdmi_buffer *hwbuf[2];
	unsigned char skip_frame[2];
};

struct v4l2_hdmi_dev {
	struct device *dev;		/* the platform device, for DMA */
	struct v4l2_device v4l2_dev;
	struct video_device vdev;
	struct vb2_queue queue;

	spinlock_t slock;		/* hdmidq, from the interrupt */
	struct mutex mutex;		/* the queue and the ioctls */

	struct hdmi_dmaqueue hdmidq;
	u32 sequence;

	/* the format userspace asked for */
	unsigned int width;
	unsigned int height;
	unsigned int outfmt;		/* MIPI_OUT_COLOR_SPACE_T */
	unsigned int bpp;
	bool fmt_set;

	/*
	 * Where the MIPI DMA writes when userspace has not queued a buffer.
	 * The BSP took this from an ION heap.
	 */
	void *scratch;
	dma_addr_t scratch_dma;
	size_t scratch_size;

	/* what used to be Android switch devices */
	int rx_video_state;
	int rx_audio_state;
	int rx_hdcp_state;

	struct task_struct *thread;
	int irq;

	/* ISO GPIO for HPD, and the MUXPAD for the DDC pins; see probe */
	void __iomem *iso;
	unsigned int hpd_gpio;
};

extern struct v4l2_hdmi_dev *hdmirx_dev;

void hdmirx_set_hpd_pin(int level);
void hdmirx_source_changed(struct v4l2_hdmi_dev *dev);

#endif /* V4L2_HDMI_DEV_H */
