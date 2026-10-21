// SPDX-License-Identifier: GPL-2.0
/*
 *	Airoha Watchdog Driver
 *
 *	Copyright (c) 2024, AIROHA  All rights reserved.
 *
 *	Mayur Kumar <mayur.kumar@airoha.com>
 *	Christian Marangi <ansuelsmth@gmail.com>
 *
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/types.h>
#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/io.h>
#include <linux/math.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/watchdog.h>

/* Base address of timer and watchdog registers */
#define TIMER_CTRL			0x0
#define   WDT_ENABLE			BIT(25)
#define   WDT_TIMER_INTERRUPT_STATUS	BIT(21)
#define   WDT_TIMER_STATUS_MASK		GENMASK(21, 16)
#define   WDT_TIMER_MODE			BIT(13)
/* The watchdog slot is timer3 on EN7581 and timer5 on EN751221. */
#define   WDT_TIMER_ENABLE		BIT(5)
#define WDT_TIMER_LOAD_VALUE		0x2c
#define WDT_TIMER_CUR_VALUE		0x30
#define  WDT_TIMER_VAL			GENMASK(31, 0)
#define WDT_RELOAD			0x38
#define   WDT_RLD			BIT(0)

struct airoha_wdt_soc_data {
	u32 timer_mode;
};

/* Airoha watchdog structure description */
struct airoha_wdt_desc {
	struct watchdog_device wdog_dev;
	const struct airoha_wdt_soc_data *soc_data;
	unsigned int wdt_freq;
	void __iomem *base;
	struct clk *bus_clk;
	struct notifier_block clk_nb;
	/* Serialize watchdog operations and clock-notifier register updates. */
	struct mutex lock;
	unsigned int clk_generation;
	unsigned int old_freq;
	bool rate_pending;
};

#define WDT_HEARTBEAT			24
static int heartbeat = WDT_HEARTBEAT;
module_param(heartbeat, int, 0);
MODULE_PARM_DESC(heartbeat, "Watchdog heartbeats in seconds. (default="
		 __MODULE_STRING(WDT_HEARTBEAT) ")");

static bool nowayout = WATCHDOG_NOWAYOUT;
module_param(nowayout, bool, 0);
MODULE_PARM_DESC(nowayout, "Watchdog cannot be stopped once started (default="
		 __MODULE_STRING(WATCHDOG_NOWAYOUT) ")");

static void airoha_wdt_enable(struct airoha_wdt_desc *airoha_wdt, u32 load)
{
	u32 val;

	/* Program the countdown before enabling the watchdog. */
	writel(load, airoha_wdt->base + WDT_TIMER_LOAD_VALUE);

	val = readl(airoha_wdt->base + TIMER_CTRL);
	/* Interrupt status bits are W1C; preserve pending sibling timers. */
	val &= ~WDT_TIMER_STATUS_MASK;
	val |= WDT_TIMER_ENABLE | WDT_ENABLE | WDT_TIMER_INTERRUPT_STATUS;
	val |= airoha_wdt->soc_data->timer_mode;
	writel(val, airoha_wdt->base + TIMER_CTRL);
}

static bool airoha_wdt_running(struct airoha_wdt_desc *wdt)
{
	u32 ctrl = readl(wdt->base + TIMER_CTRL);

	return (ctrl & (WDT_ENABLE | WDT_TIMER_ENABLE)) ==
		(WDT_ENABLE | WDT_TIMER_ENABLE);
}

/* Caller holds lock. Preserve the current countdown, then restore the reload. */
static int airoha_wdt_retime(struct airoha_wdt_desc *wdt, unsigned int freq)
{
	u64 remaining;

	if (!freq || wdt->wdog_dev.timeout > FIELD_MAX(WDT_TIMER_VAL) / freq)
		return -EINVAL;
	if (freq == wdt->wdt_freq)
		return 0;

	if (airoha_wdt_running(wdt)) {
		remaining = readl(wdt->base + WDT_TIMER_CUR_VALUE);
		remaining = DIV_ROUND_UP_ULL(remaining * freq, wdt->wdt_freq);
		if (remaining > FIELD_MAX(WDT_TIMER_VAL))
			return -ERANGE;
		writel(max_t(u64, remaining, 1), wdt->base + WDT_TIMER_LOAD_VALUE);
		writel(WDT_RLD, wdt->base + WDT_RELOAD);
		/* Future pings must reload the full configured timeout. */
		writel(wdt->wdog_dev.timeout * freq,
		       wdt->base + WDT_TIMER_LOAD_VALUE);
	}
	wdt->wdt_freq = freq;
	wdt->wdog_dev.max_timeout = FIELD_MAX(WDT_TIMER_VAL) / freq;
	return 0;
}

/* Get the clock before lock: CCF notifiers take the locks in that order. */
static int airoha_wdt_lock_rate(struct airoha_wdt_desc *wdt)
{
	unsigned int generation, freq;
	int ret;

	for (;;) {
		generation = READ_ONCE(wdt->clk_generation);
		freq = clk_get_rate(wdt->bus_clk) / 2;
		mutex_lock(&wdt->lock);
		if (generation == wdt->clk_generation)
			break;
		mutex_unlock(&wdt->lock);
	}

	/* Also recover the conservative PRE rate if CCF omitted a failed POST. */
	ret = airoha_wdt_retime(wdt, freq);
	if (ret)
		mutex_unlock(&wdt->lock);
	else
		wdt->rate_pending = false;
	return ret;
}

static int airoha_wdt_clk_notify(struct notifier_block *nb,
				 unsigned long event, void *arg)
{
	struct airoha_wdt_desc *wdt = container_of(nb, struct airoha_wdt_desc, clk_nb);
	struct clk_notifier_data *change = arg;
	unsigned int freq = change->new_rate / 2;
	int ret = 0;

	mutex_lock(&wdt->lock);
	switch (event) {
	case PRE_RATE_CHANGE:
		wdt->old_freq = change->old_rate / 2;
		if (!freq || !wdt->old_freq) {
			ret = -EINVAL;
			wdt->rate_pending = false;
			break;
		}
		/* Preload for the faster clock so an increase cannot expire early. */
		ret = airoha_wdt_retime(wdt, max(freq, wdt->old_freq));
		wdt->rate_pending = !ret;
		break;
	case POST_RATE_CHANGE:
	case ABORT_RATE_CHANGE:
		if (wdt->rate_pending) {
			ret = airoha_wdt_retime(wdt, event == POST_RATE_CHANGE ?
					       freq : wdt->old_freq);
			wdt->rate_pending = false;
		}
		break;
	default:
		mutex_unlock(&wdt->lock);
		return NOTIFY_DONE;
	}
	wdt->clk_generation++;
	mutex_unlock(&wdt->lock);
	return notifier_from_errno(ret);
}

static void airoha_wdt_start_locked(struct airoha_wdt_desc *wdt)
{
	u32 load = wdt->wdog_dev.timeout * wdt->wdt_freq;

	airoha_wdt_enable(wdt, load);
}

static int airoha_wdt_start(struct watchdog_device *wdog_dev)
{
	struct airoha_wdt_desc *wdt = watchdog_get_drvdata(wdog_dev);
	int ret = airoha_wdt_lock_rate(wdt);

	if (ret)
		return ret;
	airoha_wdt_start_locked(wdt);
	mutex_unlock(&wdt->lock);
	return 0;
}

static void airoha_wdt_stop_locked(struct airoha_wdt_desc *wdt)
{
	u32 val = readl(wdt->base + TIMER_CTRL);

	val &= ~WDT_TIMER_STATUS_MASK;
	val &= ~(WDT_ENABLE | WDT_TIMER_ENABLE);
	writel(val, wdt->base + TIMER_CTRL);
}

static int airoha_wdt_stop(struct watchdog_device *wdog_dev)
{
	struct airoha_wdt_desc *wdt = watchdog_get_drvdata(wdog_dev);

	mutex_lock(&wdt->lock);
	airoha_wdt_stop_locked(wdt);
	mutex_unlock(&wdt->lock);
	return 0;
}

static int airoha_wdt_ping(struct watchdog_device *wdog_dev)
{
	struct airoha_wdt_desc *wdt = watchdog_get_drvdata(wdog_dev);
	int ret = airoha_wdt_lock_rate(wdt);

	if (ret)
		return ret;
	writel(WDT_RLD, wdt->base + WDT_RELOAD);
	mutex_unlock(&wdt->lock);
	return 0;
}

static int airoha_wdt_set_timeout(struct watchdog_device *wdog_dev, unsigned int timeout)
{
	struct airoha_wdt_desc *wdt = watchdog_get_drvdata(wdog_dev);
	int ret = airoha_wdt_lock_rate(wdt);

	if (ret)
		return ret;
	if (!timeout || timeout > FIELD_MAX(WDT_TIMER_VAL) / wdt->wdt_freq) {
		mutex_unlock(&wdt->lock);
		return -EINVAL;
	}
	wdog_dev->timeout = timeout;
	if (watchdog_active(wdog_dev)) {
		airoha_wdt_stop_locked(wdt);
		airoha_wdt_start_locked(wdt);
	}
	mutex_unlock(&wdt->lock);
	return 0;
}

static unsigned int airoha_wdt_get_timeleft(struct watchdog_device *wdog_dev)
{
	struct airoha_wdt_desc *wdt = watchdog_get_drvdata(wdog_dev);
	u32 val;

	if (airoha_wdt_lock_rate(wdt))
		return 0;
	val = readl(wdt->base + WDT_TIMER_CUR_VALUE);
	val = DIV_ROUND_UP_ULL((u64)val, wdt->wdt_freq);
	mutex_unlock(&wdt->lock);
	return val;
}

static const struct watchdog_info airoha_wdt_info = {
	.options = WDIOF_SETTIMEOUT | WDIOF_MAGICCLOSE | WDIOF_KEEPALIVEPING,
	.identity = "Airoha Watchdog",
};

static const struct watchdog_ops airoha_wdt_ops = {
	.owner = THIS_MODULE,
	.start = airoha_wdt_start,
	.stop = airoha_wdt_stop,
	.ping = airoha_wdt_ping,
	.set_timeout = airoha_wdt_set_timeout,
	.get_timeleft = airoha_wdt_get_timeleft,
};

static int airoha_wdt_probe(struct platform_device *pdev)
{
	struct airoha_wdt_desc *airoha_wdt;
	struct watchdog_device *wdog_dev;
	struct device *dev = &pdev->dev;
	struct clk *bus_clk;
	int ret;

	airoha_wdt = devm_kzalloc(dev, sizeof(*airoha_wdt), GFP_KERNEL);
	if (!airoha_wdt)
		return -ENOMEM;
	mutex_init(&airoha_wdt->lock);

	airoha_wdt->soc_data = device_get_match_data(dev);
	if (!airoha_wdt->soc_data)
		return -EINVAL;

	airoha_wdt->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(airoha_wdt->base))
		return PTR_ERR(airoha_wdt->base);

	bus_clk = devm_clk_get_enabled(dev, "bus");
	if (IS_ERR(bus_clk))
		return dev_err_probe(dev, PTR_ERR(bus_clk),
				     "failed to enable bus clock\n");

	airoha_wdt->bus_clk = bus_clk;
	/* Watchdog ticks at half the bus rate */
	airoha_wdt->wdt_freq = clk_get_rate(bus_clk) / 2;
	if (!airoha_wdt->wdt_freq)
		return dev_err_probe(dev, -EINVAL,
				     "invalid clock frequency\n");

	/* Initialize struct watchdog device */
	wdog_dev = &airoha_wdt->wdog_dev;
	wdog_dev->timeout = heartbeat;
	wdog_dev->info = &airoha_wdt_info;
	wdog_dev->ops = &airoha_wdt_ops;
	/* Bus 300MHz, watchdog 150MHz, 28 seconds */
	wdog_dev->max_timeout = FIELD_MAX(WDT_TIMER_VAL) / airoha_wdt->wdt_freq;
	wdog_dev->parent = dev;

	watchdog_set_drvdata(wdog_dev, airoha_wdt);
	watchdog_set_nowayout(wdog_dev, nowayout);
	watchdog_stop_on_unregister(wdog_dev);

	airoha_wdt->clk_nb.notifier_call = airoha_wdt_clk_notify;
	ret = devm_clk_notifier_register(dev, bus_clk, &airoha_wdt->clk_nb);
	if (ret)
		return dev_err_probe(dev, ret, "failed to register clock notifier\n");

	ret = devm_watchdog_register_device(dev, wdog_dev);
	if (ret)
		return ret;

	platform_set_drvdata(pdev, airoha_wdt);
	return 0;
}

static int airoha_wdt_suspend(struct device *dev)
{
	struct airoha_wdt_desc *airoha_wdt = dev_get_drvdata(dev);

	if (watchdog_active(&airoha_wdt->wdog_dev))
		airoha_wdt_stop(&airoha_wdt->wdog_dev);

	return 0;
}

static int airoha_wdt_resume(struct device *dev)
{
	struct airoha_wdt_desc *airoha_wdt = dev_get_drvdata(dev);

	if (watchdog_active(&airoha_wdt->wdog_dev)) {
		airoha_wdt_start(&airoha_wdt->wdog_dev);
		airoha_wdt_ping(&airoha_wdt->wdog_dev);
	}
	return 0;
}

static const struct airoha_wdt_soc_data en7581_wdt_data = {
};

static const struct airoha_wdt_soc_data en751221_wdt_data = {
	.timer_mode = WDT_TIMER_MODE,
};

static const struct of_device_id airoha_wdt_of_match[] = {
	{ .compatible = "airoha,en7581-wdt", .data = &en7581_wdt_data },
	{ .compatible = "econet,en751221-wdt", .data = &en751221_wdt_data },
	{ },
};

MODULE_DEVICE_TABLE(of, airoha_wdt_of_match);

static DEFINE_SIMPLE_DEV_PM_OPS(airoha_wdt_pm_ops, airoha_wdt_suspend, airoha_wdt_resume);

static struct platform_driver airoha_wdt_driver = {
	.probe = airoha_wdt_probe,
	.driver = {
		.name = "airoha-wdt",
		.pm = pm_sleep_ptr(&airoha_wdt_pm_ops),
		.of_match_table = airoha_wdt_of_match,
	},
};

module_platform_driver(airoha_wdt_driver);

MODULE_AUTHOR("Mayur Kumar <mayur.kumar@airoha.com>");
MODULE_AUTHOR("Christian Marangi <ansuelsmth@gmail.com>");
MODULE_DESCRIPTION("Airoha/EcoNet Watchdog Driver");
MODULE_LICENSE("GPL");
