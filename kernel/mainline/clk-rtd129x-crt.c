// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Realtek RTD129x CRT clock gates
 *
 * The two clock-enable registers in the CRT block, CLK_EN1 (0x0c) and
 * CLK_EN2 (0x10), as plain gates. Clock specifier n is bit n of CLK_EN1 for
 * n < 32 and bit n - 32 of CLK_EN2 above that, which is also how Realtek's
 * BSP numbers them (include/dt-bindings/clock/rtk,clock-rtd129x.h).
 *
 * Mainline has no clock driver for this SoC at all, so without this the
 * USB controllers stay gated whenever the bootloader did not open them: the
 * BSP u-boot only does that on `usb start`, and on an SD boot dwc3 reads
 * garbage from GSNPSID ("this is not a DesignWare USB3 DRD Core").
 *
 * There are no rates and no parents here -- only the gates. Every gate is
 * CLK_IGNORE_UNUSED: the bootloader leaves several open (UART, SD, ETN...)
 * whose drivers do not ask for a clock, and clk_disable_unused() must not
 * shut them.
 */

#include <linux/clk-provider.h>
#include <linux/io.h>
#include <linux/mod_devicetable.h>
#include <linux/platform_device.h>
#include <linux/spinlock.h>

#define RTD129X_CRT_GATES	64

struct rtd129x_crt_clk {
	void __iomem *base;
	spinlock_t lock;
	struct clk_hw *hws[RTD129X_CRT_GATES];
};

static struct clk_hw *rtd129x_crt_clk_get(struct of_phandle_args *spec,
					  void *data)
{
	struct rtd129x_crt_clk *crt = data;
	unsigned int idx = spec->args[0];

	if (idx >= RTD129X_CRT_GATES)
		return ERR_PTR(-EINVAL);

	return crt->hws[idx];
}

static int rtd129x_crt_clk_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct rtd129x_crt_clk *crt;
	unsigned int i;

	crt = devm_kzalloc(dev, sizeof(*crt), GFP_KERNEL);
	if (!crt)
		return -ENOMEM;

	crt->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(crt->base))
		return PTR_ERR(crt->base);

	spin_lock_init(&crt->lock);

	for (i = 0; i < RTD129X_CRT_GATES; i++) {
		const char *name = devm_kasprintf(dev, GFP_KERNEL, "crt_clk_en%u_%u",
						  i / 32 + 1, i % 32);
		if (!name)
			return -ENOMEM;

		crt->hws[i] = devm_clk_hw_register_gate(dev, name, NULL,
							CLK_IGNORE_UNUSED,
							crt->base + (i / 32) * 4,
							i % 32, 0, &crt->lock);
		if (IS_ERR(crt->hws[i]))
			return PTR_ERR(crt->hws[i]);
	}

	return devm_of_clk_add_hw_provider(dev, rtd129x_crt_clk_get, crt);
}

static const struct of_device_id rtd129x_crt_clk_match[] = {
	{ .compatible = "realtek,rtd1295-crt-clk" },
	{ }
};

static struct platform_driver rtd129x_crt_clk_driver = {
	.probe = rtd129x_crt_clk_probe,
	.driver = {
		.name = "rtd129x-crt-clk",
		.of_match_table = rtd129x_crt_clk_match,
	},
};
builtin_platform_driver(rtd129x_crt_clk_driver);
