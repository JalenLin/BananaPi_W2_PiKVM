# SPDX-License-Identifier: GPL-2.0-or-later
#
# Realtek RTD129x HDMI receiver, from Realtek's 4.9 BSP
# (drivers/media/platform/rtk_hdmirx/rtd129x). Copied into the kernel tree
# by scripts/build-kernel-mainline.sh.
#
# Left out of the BSP's set: HDCP 2.2 and the TEE HDCP 1.4 path, which need
# Realtek's closed TEE, and the two EDID helpers the BSP never built either.

ccflags-y += -I$(src) -I$(src)/reg_include -I$(src)/rx_drv

obj-$(CONFIG_VIDEO_RTD129X_HDMIRX) += rtd129x-hdmirx.o
rtd129x-hdmirx-y := v4l2_hdmi_dev.o hdmirx_video_dev.o hdmirx_clk_ctrl.o \
	mipi_wrapper.o hdmirx_wrapper.o hdmirx_reg.o hdmirx_hdcp.o \
	hdmirx_repeater.o hdmirx_sysfs.o \
	rx_drv/hdmiHdmi.o rx_drv/hdmiEDID.o rx_drv/hdmiPhy.o rx_drv/hdmiMS.o
