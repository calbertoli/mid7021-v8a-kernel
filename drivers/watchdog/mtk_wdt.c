// SPDX-License-Identifier: GPL-2.0+
/*
 * Mediatek Watchdog Driver
 *
 * Copyright (C) 2014 Matthias Brugger
 *
 * Matthias Brugger <matthias.bgg@gmail.com>
 *
 * Based on sunxi_wdt.c
 */

#include <dt-bindings/reset-controller/mt2712-resets.h>
#include <dt-bindings/reset-controller/mt8183-resets.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/reset-controller.h>
#include <linux/types.h>
#include <linux/watchdog.h>
#include <linux/reboot.h>
#include <linux/sched.h>
#include <linux/timer.h>
#include <linux/sched/debug.h>
#include <mt-plat/aee.h>
#include <asm/system_misc.h>

#define WDT_MAX_TIMEOUT		31
#define WDT_MIN_TIMEOUT		1
#define WDT_LENGTH_TIMEOUT(n)	((n) << 5)

#define WDT_LENGTH		0x04
#define WDT_LENGTH_KEY		0x8

#define WDT_RST			0x08
#define WDT_RST_RELOAD		0x1971

#define WDT_MODE		0x00
#define WDT_MODE_EN		(1 << 0)
#define WDT_MODE_EXT_POL_LOW	(0 << 1)
#define WDT_MODE_EXT_POL_HIGH	(1 << 1)
#define WDT_MODE_EXRST_EN	(1 << 2)
#define WDT_MODE_IRQ_EN		(1 << 3)
#define WDT_MODE_AUTO_START	(1 << 4)
#define WDT_MODE_DUAL_EN	(1 << 6)
#define WDT_MODE_KEY		0x22000000

#define WDT_SWRST		0x14
#define WDT_SWRST_KEY		0x1209

#define WDT_SWSYSRST		0x18U
#define WDT_SWSYS_RST_KEY	0x88000000
#define WDT_LATCH_CTL2		0x48
#define WDT_DFD_EN		(1 << 17)
#define WDT_DFD_THERMAL1_DIS	(1 << 18)
#define WDT_DFD_THERMAL2_DIS	(1 << 19)
#define WDT_DFD_TIMEOUT_MASK	0x1FFFF
#define WDT_LATCH_CTL2_KEY	0x95000000

#define DRV_NAME		"mtk-wdt"
#define DRV_VERSION		"1.0"

static bool nowayout = WATCHDOG_NOWAYOUT;
/* MID7021 arm64 bring-up: neuter LK-armed TOPRGU WDT for boot diagnosis (git-reversible) */
static bool mtk_wdt_bringup_disarm = true;
static unsigned int timeout;

static int mtk_wdt_set_timeout(struct watchdog_device *wdt_dev,
			       unsigned int timeout);
static int mtk_wdt_start(struct watchdog_device *wdt_dev);
static int mtk_wdt_stop(struct watchdog_device *wdt_dev);

struct mtk_wdt_dev {
	struct watchdog_device wdt_dev;
	void __iomem *wdt_base;
	spinlock_t lock; /* protects WDT_SWSYSRST reg */
	struct reset_controller_dev rcdev;
};

struct mtk_wdt_data {
	int toprgu_sw_rst_num;
};

static const struct mtk_wdt_data mt2712_data = {
	.toprgu_sw_rst_num = MT2712_TOPRGU_SW_RST_NUM,
};

static const struct mtk_wdt_data mt8183_data = {
	.toprgu_sw_rst_num = MT8183_TOPRGU_SW_RST_NUM,
};

static int toprgu_reset_update(struct reset_controller_dev *rcdev,
			       unsigned long id, bool assert)
{
	unsigned int tmp;
	unsigned long flags;
	struct mtk_wdt_dev *data =
		 container_of(rcdev, struct mtk_wdt_dev, rcdev);

	spin_lock_irqsave(&data->lock, flags);

	tmp = readl(data->wdt_base + WDT_SWSYSRST);
	if (assert)
		tmp |= BIT(id);
	else
		tmp &= ~BIT(id);
	tmp |= WDT_SWSYS_RST_KEY;
	writel(tmp, data->wdt_base + WDT_SWSYSRST);

	spin_unlock_irqrestore(&data->lock, flags);

	return 0;
}

static int toprgu_reset_assert(struct reset_controller_dev *rcdev,
			       unsigned long id)
{
	return toprgu_reset_update(rcdev, id, true);
}

static int toprgu_reset_deassert(struct reset_controller_dev *rcdev,
				 unsigned long id)
{
	return toprgu_reset_update(rcdev, id, false);
}

static int toprgu_reset(struct reset_controller_dev *rcdev,
			unsigned long id)
{
	int ret;

	ret = toprgu_reset_assert(rcdev, id);
	if (ret)
		return ret;

	return toprgu_reset_deassert(rcdev, id);
}

static const struct reset_control_ops toprgu_reset_ops = {
	.assert = toprgu_reset_assert,
	.deassert = toprgu_reset_deassert,
	.reset = toprgu_reset,
};

static int toprgu_register_reset_controller(struct platform_device *pdev,
					    int rst_num)
{
	int ret;
	struct mtk_wdt_dev *mtk_wdt = platform_get_drvdata(pdev);

	spin_lock_init(&mtk_wdt->lock);

	mtk_wdt->rcdev.owner = THIS_MODULE;
	mtk_wdt->rcdev.nr_resets = rst_num;
	mtk_wdt->rcdev.ops = &toprgu_reset_ops;
	mtk_wdt->rcdev.of_node = pdev->dev.of_node;
	ret = devm_reset_controller_register(&pdev->dev, &mtk_wdt->rcdev);
	if (ret != 0)
		pr_info("couldn't register wdt reset controller: %d\n", ret);
	return ret;
}

static void mtk_wdt_parse_dt(struct device_node *np,
				struct watchdog_device *wdt_dev)
{
	struct mtk_wdt_dev *mtk_wdt = watchdog_get_drvdata(wdt_dev);
	void __iomem *wdt_base;
	int ret = 0;
	unsigned int reg = 0, tmp = 0, dfd_timeout = 0;

	if (!np || !mtk_wdt)
		return;

	ret = of_property_read_u32(np, "mediatek,rg_dfd_timeout",
					&dfd_timeout);

	wdt_base = mtk_wdt->wdt_base;
	if (wdt_base && !ret) {
		tmp = dfd_timeout & WDT_DFD_TIMEOUT_MASK;

		/* enable dfd_en and setup timeout */
		reg = readl(wdt_base + WDT_LATCH_CTL2);
		reg &= ~(WDT_DFD_THERMAL2_DIS | WDT_DFD_TIMEOUT_MASK);
		reg |= (WDT_DFD_EN | WDT_DFD_THERMAL1_DIS |
			WDT_LATCH_CTL2_KEY | tmp);
		writel(reg, wdt_base + WDT_LATCH_CTL2);
	}
}

static void mtk_wdt_init(struct device_node *np,
			struct watchdog_device *wdt_dev)
{
	struct mtk_wdt_dev *mtk_wdt = watchdog_get_drvdata(wdt_dev);
	void __iomem *wdt_base;

	wdt_base = mtk_wdt->wdt_base;

	if (np)
		mtk_wdt_parse_dt(np, wdt_dev);

	if (readl(wdt_base + WDT_MODE) & WDT_MODE_EN) {
		set_bit(WDOG_HW_RUNNING, &wdt_dev->status);
		mtk_wdt_set_timeout(wdt_dev, wdt_dev->timeout);
	}
}

/* MID7021 reboot-capture diagnostic (2026-06): the ~8.18s reset is hypothesized
 * to be a DELIBERATE software reboot (2nd-stage init suicide ~1.1s after sepolicy
 * load), not a HW watchdog timeout. This tree's ONLY restart path is
 * mtk_wdt_restart()->WDT_SWRST, so a clean reboot wears a watchdog costume
 * (wdt_status 0x2 = SW/bypass-pwk per MT6765 LK). Capture WHO requested it +
 * the reason string in the culprit's context, then panic so mrdump flushes the
 * ring (incl. init's last words) to expdb. */
static int mid7021_reboot_notify(struct notifier_block *nb,
				 unsigned long action, void *data)
{
	pr_emerg("[wdtk-reboot] REBOOT action=%lu reason=\"%s\" comm=%s pid=%d\n",
		action, data ? (char *)data : "(null)",
		current->comm, task_pid_nr(current));
	/* MID7021 poweroff-trap (Fable 2026-06-15): the v8a wall now POWERS OFF, not
	 * reboots. A cold power-off wipes the SRAM ring so expdb never gets a reason.
	 * Convert SYS_POWER_OFF -> WARM reboot: SRAM survives the warm reset, LK flushes
	 * the caller logged above into expdb on the next boot, AND it tests whether the
	 * boot proceeds without the poweroff (cause) or loops at the same spot (symptom).
	 * NO panic() in this path -- a panic here created the fake "30x bootloop" before. */
	if (action == SYS_POWER_OFF) {
		pr_emerg("[wdtk-pofftrap] intercept POWER_OFF -> warm reboot; caller comm=%s pid=%d\n",
			current->comm, task_pid_nr(current));
		emergency_restart();
	}
	return NOTIFY_DONE;
}

static struct notifier_block mid7021_reboot_nb = {
	.notifier_call = mid7021_reboot_notify,
	.priority = 255,
};

static int mid7021_panic_notify(struct notifier_block *nb, unsigned long ev, void *buf)
{
	pr_emerg("[wdtk-panic] PANIC comm=%s pid=%d msg=\"%s\"\n",
		current->comm, task_pid_nr(current), buf ? (char *)buf : "");
	return NOTIFY_DONE;
}
static struct notifier_block mid7021_panic_nb = {
	.notifier_call = mid7021_panic_notify, .priority = INT_MAX,
};

/* MID7021 deadline-panic: a HANG (S-state/binder wait, e.g. vold->keymaster) writes
 * nothing to expdb (nothing panics). This timer fires from softirq (independent of the
 * stuck task), dumps ALL task stacks, then panics so mrdump flushes the ring to expdb. */
static struct timer_list mid7021_deadline_timer;
static void mid7021_deadline_fire(struct timer_list *unused)
{
	pr_emerg("[wdtk-deadline] 900s: boot stalled; dumping ALL tasks then panicking\n");
	show_state();
	panic("[wdtk-deadline] 900s hang capture (all-task dump above)");
}

static int mtk_wdt_restart(struct watchdog_device *wdt_dev,
			   unsigned long action, void *data)
{
	struct mtk_wdt_dev *mtk_wdt = watchdog_get_drvdata(wdt_dev);
	void __iomem *wdt_base;

	wdt_base = mtk_wdt->wdt_base;

	pr_emerg("[wdtk-restart] mtk_wdt_restart comm=%s pid=%d\n",
		current->comm, task_pid_nr(current));

	while (1) {
		writel(WDT_SWRST_KEY, wdt_base + WDT_SWRST);
		mdelay(5);
	}

	return 0;
}

static int mtk_wdt_ping(struct watchdog_device *wdt_dev)
{
	struct mtk_wdt_dev *mtk_wdt = watchdog_get_drvdata(wdt_dev);
	void __iomem *wdt_base = mtk_wdt->wdt_base;

	iowrite32(WDT_RST_RELOAD, wdt_base + WDT_RST);
	pr_info("[wdtk] kick watchdog\n");

	return 0;
}

static int mtk_wdt_set_timeout(struct watchdog_device *wdt_dev,
				unsigned int timeout)
{
	struct mtk_wdt_dev *mtk_wdt = watchdog_get_drvdata(wdt_dev);
	void __iomem *wdt_base = mtk_wdt->wdt_base;
	u32 reg;

	wdt_dev->timeout = timeout;

	/*
	 * One bit is the value of 512 ticks
	 * The clock has 32 KHz
	 */
	reg = WDT_LENGTH_TIMEOUT(timeout << 6) | WDT_LENGTH_KEY;
	iowrite32(reg, wdt_base + WDT_LENGTH);

	mtk_wdt_ping(wdt_dev);

	return 0;
}

static int mtk_wdt_stop(struct watchdog_device *wdt_dev)
{
	struct mtk_wdt_dev *mtk_wdt = watchdog_get_drvdata(wdt_dev);
	void __iomem *wdt_base = mtk_wdt->wdt_base;
	u32 reg;

	reg = readl(wdt_base + WDT_MODE);
	reg &= ~WDT_MODE_EN;
	reg |= WDT_MODE_KEY;
	iowrite32(reg, wdt_base + WDT_MODE);

	clear_bit(WDOG_HW_RUNNING, &wdt_dev->status);

	return 0;
}

static int mtk_wdt_start(struct watchdog_device *wdt_dev)
{
	u32 reg;
	struct mtk_wdt_dev *mtk_wdt = watchdog_get_drvdata(wdt_dev);
	void __iomem *wdt_base = mtk_wdt->wdt_base;
	int ret;

	if (mtk_wdt_bringup_disarm) {
		pr_emerg("[wdtk] bringup: refuse WDT enable\n");
		return 0;
	}

	ret = mtk_wdt_set_timeout(wdt_dev, wdt_dev->timeout);
	if (ret < 0)
		return ret;

	reg = ioread32(wdt_base + WDT_MODE);
	reg |= (WDT_MODE_EN | WDT_MODE_KEY);
	iowrite32(reg, wdt_base + WDT_MODE);

	set_bit(WDOG_HW_RUNNING, &wdt_dev->status);

	return 0;
}

static const struct watchdog_info mtk_wdt_info = {
	.identity	= DRV_NAME,
	.options	= WDIOF_SETTIMEOUT |
			  WDIOF_KEEPALIVEPING |
			  WDIOF_MAGICCLOSE,
};

static const struct watchdog_ops mtk_wdt_ops = {
	.owner		= THIS_MODULE,
	.start		= mtk_wdt_start,
	.stop		= mtk_wdt_stop,
	.ping		= mtk_wdt_ping,
	.set_timeout	= mtk_wdt_set_timeout,
	.restart	= mtk_wdt_restart,
};

static int mtk_wdt_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mtk_wdt_dev *mtk_wdt;
	struct resource *res;
	const struct mtk_wdt_data *wdt_data;
	int err;

	mtk_wdt = devm_kzalloc(dev, sizeof(*mtk_wdt), GFP_KERNEL);
	if (!mtk_wdt)
		return -ENOMEM;

	platform_set_drvdata(pdev, mtk_wdt);
	register_reboot_notifier(&mid7021_reboot_nb);
	atomic_notifier_chain_register(&panic_notifier_list, &mid7021_panic_nb);
	pr_emerg("[wdtk-canary] mid7021 reboot-capture+SELdev+deadline ACTIVE (mtk_wdt probe)\n");
	aee_sram_printk("[wdtk] arm_pm_restart=%ps\n", arm_pm_restart);
	timer_setup(&mid7021_deadline_timer, mid7021_deadline_fire, 0);
	mod_timer(&mid7021_deadline_timer, jiffies + 900 * HZ);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	mtk_wdt->wdt_base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(mtk_wdt->wdt_base))
		return PTR_ERR(mtk_wdt->wdt_base);

	mtk_wdt->wdt_dev.info = &mtk_wdt_info;
	mtk_wdt->wdt_dev.ops = &mtk_wdt_ops;
	mtk_wdt->wdt_dev.timeout = WDT_MAX_TIMEOUT;
	mtk_wdt->wdt_dev.max_hw_heartbeat_ms = WDT_MAX_TIMEOUT * 1000;
	mtk_wdt->wdt_dev.min_timeout = WDT_MIN_TIMEOUT;
	mtk_wdt->wdt_dev.parent = dev;

	watchdog_init_timeout(&mtk_wdt->wdt_dev, timeout, dev);
	watchdog_set_nowayout(&mtk_wdt->wdt_dev, nowayout);
	watchdog_set_restart_priority(&mtk_wdt->wdt_dev, 128);

	watchdog_set_drvdata(&mtk_wdt->wdt_dev, mtk_wdt);

	mtk_wdt_init(pdev->dev.of_node, &mtk_wdt->wdt_dev);

	watchdog_stop_on_reboot(&mtk_wdt->wdt_dev);
	err = devm_watchdog_register_device(dev, &mtk_wdt->wdt_dev);
	if (unlikely(err))
		return err;

	dev_info(dev, "Watchdog enabled (timeout=%d sec, nowayout=%d)\n",
		 mtk_wdt->wdt_dev.timeout, nowayout);

	wdt_data = of_device_get_match_data(dev);
	if (wdt_data) {
		err = toprgu_register_reset_controller(pdev,
						       wdt_data->toprgu_sw_rst_num);
		if (err)
			return err;
	}
	return 0;
}

#if defined(CONFIG_PM_SLEEP) && defined(CONFIG_MEDIATEK_WATCHDOG_PM)
static int mtk_wdt_suspend(struct device *dev)
{
	struct mtk_wdt_dev *mtk_wdt = dev_get_drvdata(dev);
	if (watchdog_hw_running(&mtk_wdt->wdt_dev))
		mtk_wdt_stop(&mtk_wdt->wdt_dev);

	return 0;
}

static int mtk_wdt_resume(struct device *dev)
{
	struct mtk_wdt_dev *mtk_wdt = dev_get_drvdata(dev);

	if (watchdog_hw_running(&mtk_wdt->wdt_dev)) {
		mtk_wdt_start(&mtk_wdt->wdt_dev);
		mtk_wdt_ping(&mtk_wdt->wdt_dev);
	}

	return 0;
}

static const struct dev_pm_ops mtk_wdt_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(mtk_wdt_suspend,
				mtk_wdt_resume)
};
#endif

static const struct of_device_id mtk_wdt_dt_ids[] = {
	{ .compatible = "mediatek,mt2712-wdt", .data = &mt2712_data },
	{ .compatible = "mediatek,mt6589-wdt" },
	{ .compatible = "mediatek,mt8183-wdt", .data = &mt8183_data },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, mtk_wdt_dt_ids);

static struct platform_driver mtk_wdt_driver = {
	.probe		= mtk_wdt_probe,
	.driver		= {
		.name		= DRV_NAME,
#if defined(CONFIG_PM_SLEEP) && defined(CONFIG_MEDIATEK_WATCHDOG_PM)
		.pm		= &mtk_wdt_pm_ops,
#endif
		.of_match_table	= mtk_wdt_dt_ids,
	},
};

/* MID7021 arm64 bring-up: disarm the LK-armed TOPRGU watchdog before any
 * device probe so a healthy-but-slow boot is not guillotined on the LK
 * countdown (expdb signature: wdt_status 0x2, exp_type 0x0). Readback is
 * pr_emerg + the /metadata heartbeat (should now exceed ~26s if WDT was the
 * killer). Remove for production.
 */
#define MID7021_TOPRGU_PHYS	0x10007000UL
static int __init mid7021_wdt_disarm(void)
{
	void __iomem *base;
	u32 before, after;

	if (!mtk_wdt_bringup_disarm)
		return 0;

	base = ioremap(MID7021_TOPRGU_PHYS, 0x1000);
	if (!base) {
		pr_emerg("[wdtk] bringup: TOPRGU ioremap failed\n");
		return 0;
	}
	before = readl(base + WDT_MODE);
	writel((before & ~WDT_MODE_EN) | WDT_MODE_KEY, base + WDT_MODE);
	after = readl(base + WDT_MODE);
	pr_emerg("[wdtk] bringup disarm: WDT_MODE %08x -> %08x\n", before, after);
	iounmap(base);
	return 0;
}
early_initcall(mid7021_wdt_disarm);

module_platform_driver(mtk_wdt_driver);

module_param(timeout, uint, 0);
MODULE_PARM_DESC(timeout, "Watchdog heartbeat in seconds");

module_param(nowayout, bool, 0);
MODULE_PARM_DESC(nowayout, "Watchdog cannot be stopped once started (default="
			__MODULE_STRING(WATCHDOG_NOWAYOUT) ")");

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Matthias Brugger <matthias.bgg@gmail.com>");
MODULE_DESCRIPTION("Mediatek WatchDog Timer Driver");
MODULE_VERSION(DRV_VERSION);
