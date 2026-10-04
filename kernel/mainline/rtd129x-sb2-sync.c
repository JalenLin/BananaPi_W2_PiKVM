// SPDX-License-Identifier: GPL-2.0-only
/*
 * RTD129x: drain the SB2 bus bridge on every write barrier.
 *
 * SB2 holds CPU writes to DDR back until its sync register is written. A
 * device told to fetch a descriptor (a doorbell, a TRB, an OWN bit) can
 * otherwise read what was there before. Realtek's 4.9 kernel hides this in
 * the barriers: its wmb() is dsb(st) plus a write of 0x1234 to SB2's sync
 * register (CONFIG_RTK_RBUS_BARRIER), and on 4.9 every writel() begins with
 * a wmb(). Every driver got the drain for free before each register write.
 * Mainline's writel() begins with dma_wmb() and its wmb() is a dsb alone,
 * so none does: the eMMC DMAC read stale descriptors, eth0's transmitter
 * wedged, and the USB gadget left the host waiting until it reset the bus.
 *
 * asm/barrier.h and asm/io.h (patch 0018) call rtd_sb2_sync() from wmb()
 * and writel()'s barrier. It does nothing until rtd_sb2_sync_reg is set,
 * here, on RTD129x only. "rtd_sb2_sync=off" on the command line leaves it
 * off; debugfs/rtd_sb2_sync switches it at run time (for A/B tests).
 */

#include <linux/debugfs.h>
#include <linux/export.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/printk.h>
#include <linux/string.h>

#define RTD129X_SB2_SYNC	0x9801a020	/* SB2 base 0x9801a000 + 0x20 */

void __iomem *rtd_sb2_sync_reg __read_mostly;
EXPORT_SYMBOL(rtd_sb2_sync_reg);

static void __iomem *sb2_sync_map;
static bool sb2_sync_off __initdata;

static int __init rtd_sb2_sync_param(char *s)
{
	sb2_sync_off = s && !strcmp(s, "off");
	return 0;
}
early_param("rtd_sb2_sync", rtd_sb2_sync_param);

static int sb2_sync_get(void *data, u64 *val)
{
	*val = !!READ_ONCE(rtd_sb2_sync_reg);
	return 0;
}

static int sb2_sync_set(void *data, u64 val)
{
	WRITE_ONCE(rtd_sb2_sync_reg, val ? sb2_sync_map : NULL);
	return 0;
}
DEFINE_DEBUGFS_ATTRIBUTE(sb2_sync_fops, sb2_sync_get, sb2_sync_set, "%llu\n");

static int __init rtd_sb2_sync_init(void)
{
	if (!of_machine_is_compatible("realtek,rtd1293") &&
	    !of_machine_is_compatible("realtek,rtd1295") &&
	    !of_machine_is_compatible("realtek,rtd1296"))
		return 0;

	sb2_sync_map = ioremap(RTD129X_SB2_SYNC, 4);
	if (!sb2_sync_map)
		return -ENOMEM;

	if (!sb2_sync_off)
		WRITE_ONCE(rtd_sb2_sync_reg, sb2_sync_map);
	pr_info("rtd129x: SB2 sync on write barriers %s\n",
		sb2_sync_off ? "off (rtd_sb2_sync=off)" : "on");
	return 0;
}
early_initcall(rtd_sb2_sync_init);

static int __init rtd_sb2_sync_debugfs(void)
{
	if (sb2_sync_map)
		debugfs_create_file_unsafe("rtd_sb2_sync", 0600, NULL, NULL,
					   &sb2_sync_fops);
	return 0;
}
late_initcall(rtd_sb2_sync_debugfs);
