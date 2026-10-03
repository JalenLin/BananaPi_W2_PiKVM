// SPDX-License-Identifier: GPL-2.0-only
/*
 * Realtek RTD1295/RTD1296 thermal sensor
 *
 * One sensor next to the CPUs (0x9801d150). The BSP's DT lists a second one
 * at 0x980124b4, inside the eMMC wrapper, which reads nothing but zeroes and
 * is left alone here.
 *
 * The sensor reports a signed 19-bit value in 1/1024 degree C. It comes up
 * from the boot loader idle (both status words zero) and needs the reset
 * pulse of the BSP's sensor-rtd129x.c before it reports anything; the same
 * reset is used to recover it when it returns nonsense, as the BSP does.
 */

#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/thermal.h>

#include "thermal_hwmon.h"

#define TM_CTRL2	0x08
#define  TM_CTRL2_RESET		0x01904001
#define  TM_CTRL2_RUN		0x01924001
#define TM_STATUS1	0x18	/* temperature, signed, bits 18:0 */
#define TM_STATUS2	0x1c	/* 0 or 0x3fffff when the sensor is not running */

struct rtd129x_thermal {
	struct device *dev;
	void __iomem *base;
};

static void rtd129x_thermal_reset(struct rtd129x_thermal *tm)
{
	writel(TM_CTRL2_RESET, tm->base + TM_CTRL2);
	writel(TM_CTRL2_RUN, tm->base + TM_CTRL2);
	usleep_range(25000, 26000);
}

static int rtd129x_thermal_read(struct rtd129x_thermal *tm)
{
	u32 val = readl(tm->base + TM_STATUS1);

	return sign_extend32(val, 18) * 1000 / 1024;
}

static bool rtd129x_thermal_valid(struct rtd129x_thermal *tm, int t)
{
	u32 st = readl(tm->base + TM_STATUS2);

	return st != 0 && st != 0x3fffff && t >= -3000 && t <= 150000;
}

static int rtd129x_thermal_get_temp(struct thermal_zone_device *tz, int *temp)
{
	struct rtd129x_thermal *tm = thermal_zone_device_priv(tz);
	int t = rtd129x_thermal_read(tm);

	if (!rtd129x_thermal_valid(tm, t)) {
		dev_info(tm->dev, "resetting the sensor (status %08x %08x)\n",
			 readl(tm->base + TM_STATUS1),
			 readl(tm->base + TM_STATUS2));
		rtd129x_thermal_reset(tm);
		t = rtd129x_thermal_read(tm);
		if (!rtd129x_thermal_valid(tm, t))
			return -EIO;
	}

	*temp = t;
	return 0;
}

static const struct thermal_zone_device_ops rtd129x_thermal_ops = {
	.get_temp = rtd129x_thermal_get_temp,
};

static int rtd129x_thermal_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct thermal_zone_device *tz;
	struct rtd129x_thermal *tm;

	tm = devm_kzalloc(dev, sizeof(*tm), GFP_KERNEL);
	if (!tm)
		return -ENOMEM;
	tm->dev = dev;
	platform_set_drvdata(pdev, tm);

	tm->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(tm->base))
		return PTR_ERR(tm->base);

	rtd129x_thermal_reset(tm);

	tz = devm_thermal_of_zone_register(dev, 0, tm, &rtd129x_thermal_ops);
	if (IS_ERR(tz))
		return dev_err_probe(dev, PTR_ERR(tz),
				     "cannot register the thermal zone\n");

	devm_thermal_add_hwmon_sysfs(dev, tz);
	return 0;
}

static int rtd129x_thermal_resume(struct device *dev)
{
	rtd129x_thermal_reset(dev_get_drvdata(dev));
	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(rtd129x_thermal_pm, NULL, rtd129x_thermal_resume);

static const struct of_device_id rtd129x_thermal_of_match[] = {
	{ .compatible = "realtek,rtd1295-thermal" },
	{ }
};
MODULE_DEVICE_TABLE(of, rtd129x_thermal_of_match);

static struct platform_driver rtd129x_thermal_driver = {
	.probe = rtd129x_thermal_probe,
	.driver = {
		.name = "rtd129x-thermal",
		.of_match_table = rtd129x_thermal_of_match,
		.pm = pm_sleep_ptr(&rtd129x_thermal_pm),
	},
};
module_platform_driver(rtd129x_thermal_driver);

MODULE_DESCRIPTION("Realtek RTD129x thermal sensor");
MODULE_LICENSE("GPL");
