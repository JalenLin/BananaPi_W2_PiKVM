/*
 * hdmirx_sysfs.h - RTK hdmi rx driver header file
 *
 * Copyright (C) 2017 Realtek Semiconductor Corporation
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <linux/platform_device.h>

void register_hdmirx_sysfs(struct platform_device *pdev);
void unregister_hdmirx_sysfs(struct platform_device *pdev);

