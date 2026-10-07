/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * hdmirx_video_dev.h - RTK hdmi rx driver header file
 *
 * Copyright (C) 2017 Realtek Semiconductor Corporation
 */

#ifndef _HDMIRX_VIDEO_DEV_H_
#define _HDMIRX_VIDEO_DEV_H_

#include "v4l2_hdmi_dev.h"

int out_color_to_bpp(unsigned int output_color);
void hdmirx_measure_rate(bool present);
int register_video_device(struct v4l2_hdmi_dev *hdmi_dev);
void unregister_video_device(struct v4l2_hdmi_dev *hdmi_dev);

#endif /* _HDMIRX_VIDEO_DEV_H_ */
