// SPDX-License-Identifier: GPL-2.0
/*
 * MID7021 (Onn 100135924, MT6765) arm64 bring-up helper.
 *
 * PURPOSE: this device's preloader/LK hands the boot mode up to the kernel via
 * the device-tree /chosen/atag,boot node (struct tag_bootmode). On a USB/charger
 * power-on, LK sets bootmode = 8 (KERNEL_POWER_OFF_CHARGING_BOOT), so userspace
 * init runs kpoc_charger.rc (the battery/charging screen) instead of Android --
 * which means we can NEVER get adb with the cable attached.
 *
 * Every consumer (charger driver, mtkfb, usb gadget, AND userspace init via
 * /proc/device-tree/chosen/atag,boot) reads this ONE property. A previous
 * attempt that flipped the derived g_boot_mode did nothing, because init reads
 * the raw DT property, not that mirror. So we patch the SOURCE: rewrite the
 * /chosen/atag,boot bootmode field 8/9 -> 0 (NORMAL_BOOT) in an early_initcall,
 * after the DT is unflattened but before any driver or userspace reads it.
 *
 * This is a deliberate, single-purpose bring-up tool to make USB-tethered boots
 * reach Android (adbd). It does NOT mask any boot failure; it only routes a
 * charger power-on to a normal boot. Loudly logged on purpose.
 */
#include <linux/init.h>
#include <linux/of.h>
#include <linux/printk.h>
#include <linux/types.h>

struct mid7021_tag_bootmode {
	u32 size;
	u32 tag;
	u32 bootmode;
	u32 boottype;
};

static int __init mid7021_force_normal_boot(void)
{
	struct device_node *np;
	struct mid7021_tag_bootmode *tag;
	int len = 0;

	np = of_find_node_by_path("/chosen");
	if (!np)
		np = of_find_node_by_path("/chosen@0");
	if (!np) {
		pr_notice("MID7021-FNB: no /chosen node\n");
		return 0;
	}

	tag = (struct mid7021_tag_bootmode *)of_get_property(np, "atag,boot",
							     &len);
	if (!tag || len < (int)sizeof(*tag)) {
		pr_notice("MID7021-FNB: no/short atag,boot (len=%d)\n", len);
		of_node_put(np);
		return 0;
	}

	pr_notice("MID7021-FNB: atag,boot bootmode=0x%x boottype=0x%x\n",
		  tag->bootmode, tag->boottype);

	/* 8 = KERNEL_POWER_OFF_CHARGING_BOOT, 9 = LOW_POWER_OFF_CHARGING_BOOT */
	if (tag->bootmode == 8 || tag->bootmode == 9) {
		tag->bootmode = 0; /* NORMAL_BOOT */
		pr_notice("MID7021-FNB: forced bootmode -> NORMAL_BOOT (0)\n");
	} else {
		pr_notice("MID7021-FNB: bootmode not charger; left as-is\n");
	}

	of_node_put(np);
	return 0;
}
early_initcall(mid7021_force_normal_boot);
