// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * v4l2_hdmi_dev.c - Realtek RTD129x HDMI receiver: probe, vb2, the RX thread
 *
 * Copyright (C) 2017 Realtek Semiconductor Corporation
 *
 * The hardware sequencing (the RX thread, 5V detection, what happens on
 * plug and unplug) is Realtek's, unchanged. Around it, for mainline:
 *
 *  - vb2 is set up once at probe with dma-contig, the way every mainline
 *    capture driver does it. The BSP created the queue inside its REQBUFS
 *    handler, reached into vb2-vmalloc's private structures for USERPTR,
 *    and oopsed on a STREAMOFF before REQBUFS.
 *  - The frame the DMA writes while no buffer is queued goes into a
 *    dma_alloc_coherent() scratch buffer instead of an ION allocation.
 *  - The Android switch devices become sysfs attributes and a
 *    V4L2_EVENT_SOURCE_CHANGE (hdmirx_video_dev.c).
 *  - HPD and the DDC pin mux are driven directly; see hdmirx_setup_iso().
 *  - remove() actually tears the device down, so the module can be
 *    reloaded.
 */

#include <linux/dma-mapping.h>
#include <linux/io.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>

#include "v4l2_hdmi_dev.h"
#include "hdmirx_video_dev.h"
#include "hdmirx_wrapper.h"
#include "mipi_wrapper.h"
#include "hdmirx_reg.h"
#include "hdmirx_sysfs.h"
#include "hdmirx_clk_ctrl.h"

#include "rx_drv/hdmiInternal.h"
#include "rx_drv/hdmiEDID.h"

extern void __iomem *hdmi_rx_base[HDMI_RX_REG_BLOCK_NUM];
extern MIPI_TOP_INFO mipi_top;
extern HDMI_INFO_T hdmi;
extern HDMIRX_IOCTL_STRUCT_T hdmi_ioctl_struct;
extern HDMIRX_DTS_EDID_TBL_T hdmirx_edid;
extern int hdmi_stream_on;

extern int Cbus_GetRx5v(void);
extern void Hdmi_SetHPD(char high);
extern void drvif_Hdmi_Init(void);
extern void drvif_Hdmi_InitSrc(unsigned char channel);
extern void drvif_Hdmi_Release(void);
extern HDMI_bool drvif_Hdmi_DetectMode(void);
extern HDMI_bool drvif_Hdmi_CheckMode(void);
extern void rtd_hdmiPhy_ISR(void);
extern unsigned int rx_pitch_measurement(unsigned int output_h,
					 MIPI_OUT_COLOR_SPACE_T output_color);

/* The rx_drv/ layer is built around globals, so there is one instance. */
struct v4l2_hdmi_dev *hdmirx_dev;

int hdmi_stream_on;

/* ---- HPD and the DDC pins ------------------------------------------------ */

/*
 * Offsets in the ISO block (0x98007000).
 *
 * HPD is ISO GPIO 22. Mainline's gpio-rtd would drive it, but it claims the
 * ISO interrupt-status window (0x000-0x0e7) as a second resource, which
 * overlaps iso_reset@88 and the ISO clock gates at 0x8c, so it cannot be
 * instantiated on this SoC as rtd129x.dtsi stands. The two registers are
 * written here instead.
 *
 * The DDC pins are ISO pads 20 and 26, which reset to GPIO. Their mux is
 * bits 3:0 of MUXPAD 0x314 and must be 0b0101 (I2C6), or every EDID read
 * the source makes is NAKed and it falls back to 1024x768 DVI -- see
 * docs/04-hdmi-rx-bringup.md, blocker 3. Mainline has no RTD129x pinctrl.
 */
#define ISO_GPIO_DIR		0x100
#define ISO_GPIO_DATO		0x104
#define ISO_MUXPAD5		0x314
#define ISO_MUXPAD5_I2C6	0x5
#define ISO_MUXPAD5_I2C6_MASK	0xf

void hdmirx_set_hpd_pin(int level)
{
	struct v4l2_hdmi_dev *dev = hdmirx_dev;
	u32 bit = BIT(dev->hpd_gpio);
	u32 val;

	val = readl(dev->iso + ISO_GPIO_DATO);
	val = level ? (val | bit) : (val & ~bit);
	writel(val, dev->iso + ISO_GPIO_DATO);
	writel(readl(dev->iso + ISO_GPIO_DIR) | bit, dev->iso + ISO_GPIO_DIR);
}

static int hdmirx_setup_iso(struct platform_device *pdev,
			    struct v4l2_hdmi_dev *dev)
{
	struct device_node *np = pdev->dev.of_node;
	int idx = of_property_match_string(np, "reg-names", "iso");
	u32 val;

	if (idx < 0)
		return dev_err_probe(&pdev->dev, idx, "no \"iso\" reg\n");
	/*
	 * Not "...-gpio": fw_devlink reads any property with that suffix as
	 * a GPIO phandle ("could not find phandle 22").
	 */
	if (of_property_read_u32(np, "realtek,hpd-iso-pin", &dev->hpd_gpio) ||
	    dev->hpd_gpio > 31)
		return dev_err_probe(&pdev->dev, -EINVAL,
				     "realtek,hpd-iso-pin missing or out of range\n");

	/* of_iomap: the ISO block is already claimed by the iso syscon. */
	dev->iso = of_iomap(np, idx);	/* not requested, see above */
	if (!dev->iso)
		return -ENOMEM;

	val = readl(dev->iso + ISO_MUXPAD5);
	val = (val & ~ISO_MUXPAD5_I2C6_MASK) | ISO_MUXPAD5_I2C6;
	writel(val, dev->iso + ISO_MUXPAD5);
	return 0;
}

/* ---- vb2 --------------------------------------------------------------- */

static unsigned long frame_size(struct v4l2_hdmi_dev *dev)
{
	if (dev->outfmt >= OUT_ARGB)
		return dev->width * dev->height * dev->bpp / 8;
	return roundup16(dev->width) * roundup16(dev->height) * dev->bpp / 8;
}

static int hdmi_queue_setup(struct vb2_queue *vq, unsigned int *nbuffers,
			    unsigned int *nplanes, unsigned int sizes[],
			    struct device *alloc_devs[])
{
	struct v4l2_hdmi_dev *dev = vb2_get_drv_priv(vq);
	unsigned long size = frame_size(dev);

	if (!size)
		return -EINVAL;
	if (*nplanes)
		return sizes[0] < size ? -EINVAL : 0;

	*nplanes = 1;
	sizes[0] = size;
	return 0;
}

/*
 * The semi-planar formats are laid out at a 16-line height (1080 -> 1088),
 * and the DMA never writes the padding lines. Left at zero, their chroma is
 * U = V = 0, and VE1's H.264 encoder (CODA980) then garbles the whole last
 * macroblock row, visible lines included. Make the padding black once, when
 * the buffer is allocated.
 */
static int hdmi_buffer_init(struct vb2_buffer *vb)
{
	struct v4l2_hdmi_dev *dev = vb2_get_drv_priv(vb->vb2_queue);
	unsigned int stride = roundup16(dev->width);
	unsigned int h = dev->height, ah = roundup16(dev->height);
	unsigned int c_h, c_ah;
	u8 *p;

	if (vb->memory != VB2_MEMORY_MMAP || dev->outfmt >= OUT_ARGB || h == ah)
		return 0;

	p = vb2_plane_vaddr(vb, 0);
	if (!p || vb2_plane_size(vb, 0) < frame_size(dev))
		return 0;

	/* 4:2:0 has half as many chroma lines, 4:2:2 as many as luma */
	c_h = dev->outfmt == OUT_8BIT_YUV420 ? h / 2 : h;
	c_ah = dev->outfmt == OUT_8BIT_YUV420 ? ah / 2 : ah;

	memset(p + stride * h, 16, stride * (ah - h));
	memset(p + stride * ah + stride * c_h, 128, stride * (c_ah - c_h));
	return 0;
}

static int hdmi_buffer_prepare(struct vb2_buffer *vb)
{
	struct v4l2_hdmi_dev *dev = vb2_get_drv_priv(vb->vb2_queue);
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);
	struct hdmi_buffer *buf = container_of(vbuf, struct hdmi_buffer, vb);
	unsigned long size = frame_size(dev);

	if (vb2_plane_size(vb, 0) < size) {
		HDMIRX_ERROR("buffer too small (%lu < %lu)",
			     vb2_plane_size(vb, 0), size);
		return -EINVAL;
	}

	vb2_set_plane_payload(vb, 0, size);
	buf->phys = vb2_dma_contig_plane_dma_addr(vb, 0);
	return 0;
}

static void hdmi_buffer_queue(struct vb2_buffer *vb)
{
	struct v4l2_hdmi_dev *dev = vb2_get_drv_priv(vb->vb2_queue);
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);
	struct hdmi_buffer *buf = container_of(vbuf, struct hdmi_buffer, vb);
	unsigned long flags;

	spin_lock_irqsave(&dev->slock, flags);
	list_add_tail(&buf->list, &dev->hdmidq.active);
	atomic_inc(&dev->hdmidq.qcnt);
	spin_unlock_irqrestore(&dev->slock, flags);
}

/* Hand every buffer the driver holds back to vb2 in @state. */
static void hdmi_return_buffers(struct v4l2_hdmi_dev *dev,
				enum vb2_buffer_state state)
{
	struct hdmi_dmaqueue *dq = &dev->hdmidq;
	struct hdmi_buffer *buf, *tmp;
	unsigned long flags;
	int i;

	spin_lock_irqsave(&dev->slock, flags);
	list_for_each_entry_safe(buf, tmp, &dq->active, list) {
		list_del(&buf->list);
		vb2_buffer_done(&buf->vb.vb2_buf, state);
	}
	for (i = 0; i < 2; i++) {
		if (dq->hwbuf[i]) {
			vb2_buffer_done(&dq->hwbuf[i]->vb.vb2_buf, state);
			dq->hwbuf[i] = NULL;
		}
	}
	atomic_set(&dq->qcnt, 0);
	spin_unlock_irqrestore(&dev->slock, flags);
}

static int hdmi_start_streaming(struct vb2_queue *vq, unsigned int count)
{
	struct v4l2_hdmi_dev *dev = vb2_get_drv_priv(vq);
	unsigned int v_in;

	if (!hdmi_ioctl_struct.measure_ready || !mipi_top.h_input_len) {
		HDMIRX_INFO("start streaming: no signal");
		hdmi_return_buffers(dev, VB2_BUF_STATE_QUEUED);
		return -ENOLINK;
	}

	v_in = mipi_top.v_input_len;
	if (!hdmi.tx_timing.progressive)
		v_in <<= 1;
	if (dev->width > mipi_top.h_input_len || dev->height > v_in) {
		HDMIRX_ERROR("%ux%u is larger than the %ux%u input",
			     dev->width, dev->height, mipi_top.h_input_len, v_in);
		hdmi_return_buffers(dev, VB2_BUF_STATE_QUEUED);
		return -EINVAL;
	}

	/* What the BSP's S_FMT used to program into the MIPI wrapper */
	mipi_top.h_output_len = dev->width;
	mipi_top.v_output_len = dev->height;
	mipi_top.output_color = dev->outfmt;
	mipi_top.pitch = rx_pitch_measurement(dev->width, dev->outfmt);

	/* One frame and an eighth, as the BSP sized its ION buffer. */
	dev->scratch_size = PAGE_ALIGN(frame_size(dev) * 9 / 8);
	dev->scratch = dma_alloc_coherent(dev->dev, dev->scratch_size,
					  &dev->scratch_dma, GFP_KERNEL);
	if (!dev->scratch) {
		hdmi_return_buffers(dev, VB2_BUF_STATE_QUEUED);
		return -ENOMEM;
	}

	dev->sequence = 0;
	atomic_set(&dev->hdmidq.rcnt, 0);

	HDMIRX_INFO("start streaming %ux%u color %u pitch %u",
		    mipi_top.h_output_len, mipi_top.v_output_len,
		    mipi_top.output_color, mipi_top.pitch);

	set_video_DDR_start_addr(dev);
	setup_mipi();
	hdmi_stream_on = 1;
	return 0;
}

static void hdmi_stop_streaming(struct vb2_queue *vq)
{
	struct v4l2_hdmi_dev *dev = vb2_get_drv_priv(vq);

	if (is_clock_enabled(CLK_MIPI) && hdmi.rx_5v_state)
		stop_mipi_process();
	hdmi_stream_on = 0;

	/* The DMA is stopped; nothing points at the scratch buffer now. */
	hdmi_return_buffers(dev, VB2_BUF_STATE_ERROR);

	if (dev->scratch) {
		dma_free_coherent(dev->dev, dev->scratch_size, dev->scratch,
				  dev->scratch_dma);
		dev->scratch = NULL;
	}
}

static const struct vb2_ops hdmi_qops = {
	.queue_setup		= hdmi_queue_setup,
	.buf_init		= hdmi_buffer_init,
	.buf_prepare		= hdmi_buffer_prepare,
	.buf_queue		= hdmi_buffer_queue,
	.start_streaming	= hdmi_start_streaming,
	.stop_streaming		= hdmi_stop_streaming,
};

static int hdmirx_queue_init(struct v4l2_hdmi_dev *dev)
{
	struct vb2_queue *q = &dev->queue;

	q->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	q->io_modes = VB2_MMAP | VB2_DMABUF;
	q->drv_priv = dev;
	q->buf_struct_size = sizeof(struct hdmi_buffer);
	q->ops = &hdmi_qops;
	q->mem_ops = &vb2_dma_contig_memops;
	q->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
	q->lock = &dev->mutex;
	q->dev = dev->dev;
	q->allow_cache_hints = 1;	/* see hdmirx_reqbufs() */
	/*
	 * The MIPI DMA takes 32-bit addresses; the DMA mask set in probe is
	 * what keeps buffers below 4 GiB. No GFP_DMA32 here: the non-coherent
	 * allocator refuses zone flags.
	 */

	return vb2_queue_init(q);
}

/* ---- the RX thread (Realtek's) ------------------------------------------ */

static void update_hdmirx_switch_state(struct v4l2_hdmi_dev *dev)
{
	static unsigned char audio_count;
	int hdcp_state = 0;

	if (hdmi_ioctl_struct.measure_ready != dev->rx_video_state) {
		dev->rx_video_state = hdmi_ioctl_struct.measure_ready;
		HDMIRX_INFO("video state %d", dev->rx_video_state);
		hdmirx_source_changed(dev);
	}

	if (!hdmi_ioctl_struct.audio_detect_done)
		audio_count = 0;
	if (hdmi_ioctl_struct.audio_detect_done != dev->rx_audio_state) {
		if (hdmi_ioctl_struct.audio_detect_done)
			audio_count++;
		if (!hdmi_ioctl_struct.audio_detect_done || audio_count >= 10) {
			dev->rx_audio_state = hdmi_ioctl_struct.audio_detect_done;
			HDMIRX_INFO("audio state %d", dev->rx_audio_state);
		}
	}

	if (hdmi_ioctl_struct.hdcp_state == HDCPRX_STATE_1PX_ON ||
	    hdmi_ioctl_struct.hdcp_state == HDCPRX_STATE_2P2_ON)
		hdcp_state = 1;
	dev->rx_hdcp_state = hdcp_state;
}

static void rx5v_do_task(struct v4l2_hdmi_dev *dev)
{
	int state = Cbus_GetRx5v();

	if (!state && hdmi.rx_5v_state) {
		HDMIRX_INFO("Cable Unplugged");

		stop_mipi_process();
		/* Stop DMA, disable the wrapper interrupt */
		set_hdmirx_wrapper_control_0(-1, 0, -1, -1, -1, -1);
		set_hdmirx_wrapper_interrupt_en(0, 0, 0);
		drvif_Hdmi_Release();
		memset(&hdmi_ioctl_struct, 0, sizeof(hdmi_ioctl_struct));
		SET_HDMI_VIDEO_FSM(MAIN_FSM_HDMI_SETUP_VEDIO_PLL);
		SET_HDMI_AUDIO_FSM(AUDIO_FSM_AUDIO_START);
		mipi_top.hdmi_rx_init = 0;

		hdmi.rx_5v_state = 0;
		Hdmi_SetHPD(0);

		update_hdmirx_switch_state(dev);
		usleep_range(280000, 300000);

		/* Stop hdmirx, gate its clocks */
		set_hdmirx_wrapper_control_0(-1, -1, -1, -1, -1, 0);
		hdmirx_clock_control(CLK_HDMIRX | CLK_RXWRAP | CLK_MIPI, CTL_DISABLE);
	} else if (state && !hdmi.rx_5v_state) {
		HDMIRX_INFO("Cable Plugged");

		hdmirx_clock_control(CLK_HDMIRX | CLK_RXWRAP | CLK_MIPI, CTL_ENABLE);
		set_hdmirx_wrapper_control_0(-1, -1, -1, -1, -1, 1);
		hdmi.rx_5v_state = 1;
	}
}

static void hdmi_rx_process(void)
{
	static int state;
	unsigned char ret;

	if (mipi_top.hdmi_rx_init == 0) {
		hdmi_related_wrapper_init();
		HdmiRx_EnableEDID();
		drvif_Hdmi_Init();
		drvif_Hdmi_InitSrc(HDMI_CHANNEL0);
		mipi_top.hdmi_rx_init = 1;
		state = 0;
		Hdmi_SetHPD(1);
	}

	switch (state) {
	case 0:
		if (hdmi_ioctl_struct.DEF_ready &&
		    drvif_Hdmi_DetectMode() == _MODE_SUCCESS)
			state++;
		break;
	case 1:
		ret = drvif_Hdmi_CheckMode();
		if (ret == FALSE && hdmi_ioctl_struct.detect_done) {
			HDMIRX_INFO("mode check failed, restarting detection");
			state = 0;
			restartHdmiRxWrapperDetection();
		} else if (hdmi_ioctl_struct.detect_done == 0) {
			state = 0;
		}
		break;
	default:
		state = 0;
		break;
	}
}

static int mipi_top_thread(void *arg)
{
	struct v4l2_hdmi_dev *dev = arg;
	unsigned int i;

	while (!kthread_should_stop()) {
		rx5v_do_task(dev);

		if (!hdmi.rx_5v_state) {
			msleep(250);
			continue;
		}

		hdmi_rx_process();
		for (i = 0; i < 3 && !kthread_should_stop(); i++) {
			update_hdmirx_switch_state(dev);
			usleep_range(70000, 75000);
			if (mipi_top.hdmi_rx_init)
				rtd_hdmiPhy_ISR();
		}
	}
	return 0;
}

/* ---- probe / remove ------------------------------------------------------- */

/*
 * The EDID tables, as 256 bytes each. The BSP's property held one byte per
 * 32-bit cell; this one is a plain byte string ([..] in the DTS).
 */
static void hdmirx_load_edid(struct device_node *np)
{
	HDMIRX_DTS_EDID_TBL_T tbl;

	memset(&tbl, 0, sizeof(tbl));
	if (!of_property_read_u8_array(np, "realtek,edid", tbl.EDID, 256))
		HdmiRx_Save_DTS_EDID_Table(&tbl);
	else
		HDMIRX_ERROR("no realtek,edid (256 bytes) in the DT");

	memset(&tbl, 0, sizeof(tbl));
	if (!of_property_read_u8_array(np, "realtek,edid-hdmi20", tbl.EDID, 256))
		HdmiRx_Save_DTS_EDID2p0_Table(&tbl);
}

static int hdmirx_rtk_drv_probe(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	struct v4l2_hdmi_dev *dev;
	int ret, i;

	if (hdmirx_dev)
		return -EBUSY;

	dev = devm_kzalloc(&pdev->dev, sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;
	dev->dev = &pdev->dev;
	hdmirx_dev = dev;

	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
	if (ret)
		goto err_clear;

	ret = hdmirx_clock_init(pdev);
	if (ret)
		goto err_clear;

	/* Release the resets and open the clocks, then gate RX until plug-in */
	hdmirx_clock_control(CLK_ALL, CTL_ENABLE);
	hdmirx_clock_control(CLK_HDMIRX | CLK_RXWRAP | CLK_MIPI, CTL_DISABLE);

	memset(&mipi_top, 0, sizeof(mipi_top));
	memset(&hdmi, 0, sizeof(hdmi));
	memset(&hdmi_ioctl_struct, 0, sizeof(hdmi_ioctl_struct));
	memset(&hdmirx_edid, 0, sizeof(hdmirx_edid));

	for (i = 0; i < HDMI_RX_REG_BLOCK_NUM; i++) {
		hdmi_rx_base[i] = devm_platform_ioremap_resource(pdev, i);
		if (IS_ERR(hdmi_rx_base[i])) {
			ret = PTR_ERR(hdmi_rx_base[i]);
			goto err_clock;
		}
	}

	ret = hdmirx_setup_iso(pdev, dev);
	if (ret)
		goto err_clock;

	hdmirx_load_edid(np);

	/* No 5V-detect GPIO: 5V comes from CBUS (see the DT notes). */
	hdmi.gpio_hpd_ctrl = -1;
	hdmi.gpio_5v_det = -1;
	Hdmi_SetHPD(0);

	/* Defaults until userspace sets a format: 1080p NV16 */
	dev->width = 1920;
	dev->height = 1080;
	dev->outfmt = OUT_8BIT_YUV422;
	dev->bpp = 16;
	mipi_top.src_sel = 1;

	spin_lock_init(&dev->slock);
	mutex_init(&dev->mutex);
	INIT_LIST_HEAD(&dev->hdmidq.active);
	INIT_WORK(&mipi_top.mipi_reset_work, mipi_reset_work_func);

	ret = v4l2_device_register(&pdev->dev, &dev->v4l2_dev);
	if (ret)
		goto err_iso;

	ret = hdmirx_queue_init(dev);
	if (ret)
		goto err_v4l2;

	ret = register_video_device(dev);
	if (ret)
		goto err_v4l2;

	dev->irq = platform_get_irq(pdev, 0);
	if (dev->irq < 0) {
		ret = dev->irq;
		goto err_vdev;
	}
	ret = request_irq(dev->irq, hdmirx_mipi_isr, IRQF_SHARED, "hdmirx", dev);
	if (ret)
		goto err_vdev;

	register_hdmirx_sysfs(pdev);
	platform_set_drvdata(pdev, dev);

	dev->thread = kthread_run(mipi_top_thread, dev, "hdmirx");
	if (IS_ERR(dev->thread)) {
		ret = PTR_ERR(dev->thread);
		goto err_sysfs;
	}

	return 0;

err_sysfs:
	unregister_hdmirx_sysfs(pdev);
	free_irq(dev->irq, dev);
err_vdev:
	unregister_video_device(dev);
err_v4l2:
	v4l2_device_unregister(&dev->v4l2_dev);
err_iso:
	iounmap(dev->iso);
err_clock:
	hdmirx_clock_control(CLK_ALL, CTL_DISABLE);
err_clear:
	hdmirx_dev = NULL;
	return ret;
}

static void hdmirx_rtk_drv_remove(struct platform_device *pdev)
{
	struct v4l2_hdmi_dev *dev = platform_get_drvdata(pdev);

	kthread_stop(dev->thread);
	unregister_video_device(dev);	/* stops streaming if it is */
	unregister_hdmirx_sysfs(pdev);

	if (hdmi.rx_5v_state) {
		stop_mipi_process();
		set_hdmirx_wrapper_control_0(-1, 0, -1, -1, -1, -1);
		set_hdmirx_wrapper_interrupt_en(0, 0, 0);
		drvif_Hdmi_Release();
		set_hdmirx_wrapper_control_0(-1, -1, -1, -1, -1, 0);
	}
	Hdmi_SetHPD(0);

	free_irq(dev->irq, dev);
	cancel_work_sync(&mipi_top.mipi_reset_work);
	v4l2_device_unregister(&dev->v4l2_dev);
	hdmirx_clock_control(CLK_ALL, CTL_DISABLE);
	iounmap(dev->iso);
	hdmirx_dev = NULL;
}

static const struct of_device_id hdmirx_rtk_dt_ids[] = {
	{ .compatible = "realtek,rtd1295-hdmirx" },
	{ }
};
MODULE_DEVICE_TABLE(of, hdmirx_rtk_dt_ids);

static struct platform_driver hdmirx_rtk_driver = {
	.probe = hdmirx_rtk_drv_probe,
	.remove = hdmirx_rtk_drv_remove,
	.driver = {
		.name = "rtd129x-hdmirx",
		.of_match_table = hdmirx_rtk_dt_ids,
	},
};
module_platform_driver(hdmirx_rtk_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Realtek RTD129x HDMI receiver");
