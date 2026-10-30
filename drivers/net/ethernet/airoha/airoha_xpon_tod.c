// SPDX-License-Identifier: GPL-2.0-only
/* AN7581/AN7583 Time of Day generator, owned by the xPON MAC. */

#include <linux/bitfield.h>
#include <linux/completion.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/ptp_clock_kernel.h>
#include <linux/slab.h>
#include <linux/timekeeping.h>

#include "airoha_xpon.h"
#include "airoha_xpon_tod.h"

#define TOD_CTRL			0x00
#define TOD_LOAD			BIT(0)
#define TOD_SOURCE_CPU			BIT(1)
#define TOD_ADJUST_OFFSET		BIT(8)
#define TOD_NEW_SEC_HI			0x04
#define TOD_NEW_SEC_LO			0x08
#define TOD_NEW_NSEC			0x0c
#define TOD_OFFSET_SEC_HI			0x10
#define TOD_OFFSET_NEGATIVE		BIT(31)
#define TOD_OFFSET_SEC_LO			0x14
#define TOD_OFFSET_NSEC			0x18
#define TOD_ADD_UNIT_INT			0x30
#define TOD_ADD_UNIT_NUM			0x34
#define TOD_ADD_UNIT_DEN			0x38
#define TOD_1PPS_WIDTH			0x50
#define TOD_CUR_SEC_HI			0x54
#define TOD_CUR_SEC_LO			0x58
#define TOD_CUR_NSEC			0x5c
#define TOD_INT_ENABLE			0x60
#define TOD_INT_STATUS			0x64
#define TOD_INT_LOAD_DONE			BIT(0)
#define TOD_INT_1PPS			BIT(1)
#define TOD_INT_FREQ_DONE			BIT(2)
#define TOD_INT_ALL			GENMASK(2, 0)
#define TOD_SEC_HI_MASK			GENMASK(15, 0)
#define TOD_MAX_SECONDS			GENMASK_ULL(47, 0)
#define TOD_RESOURCE_SIZE			0xd4
#define TOD_LOAD_TIMEOUT_MS		100
#define GPON_TOD_SUPERFRAME_MASK		GENMASK(29, 0)
#define GPON_PHY_RX_DLY			0x3e4
#define GPON_PHY_RX_DLY_MASK		GENMASK(11, 0)

struct airoha_xpon_tod {
	struct xpon_priv *priv;
	void __iomem *base;
	/* Serializes clock operations and GPON clock handover. */
	struct mutex lock;
	/* Serializes command arming with the W1C interrupt snapshot. */
	spinlock_t irq_lock;
	struct completion load_done;
	struct ptp_clock_info info;
	struct ptp_clock *clock;
	int irq;
	bool pending;
	bool gpon_source;
	bool pps_enabled;
	bool failed;
	bool dead;
};

static u32 tod_read(struct airoha_xpon_tod *tod, u32 reg)
{
	return readl(tod->base + reg);
}

static void tod_write(struct airoha_xpon_tod *tod, u32 reg, u32 val)
{
	writel(val, tod->base + reg);
}

static int tod_read_time(struct airoha_xpon_tod *tod, struct timespec64 *ts,
			 struct ptp_system_timestamp *sts)
{
	u32 hi, lo, ns, hi_after, lo_after;
	int i;

	/* There is no snapshot command: retry a read that crosses a second. */
	for (i = 0; i < 4; i++) {
		hi = tod_read(tod, TOD_CUR_SEC_HI) & TOD_SEC_HI_MASK;
		lo = tod_read(tod, TOD_CUR_SEC_LO);
		ptp_read_system_prets(sts);
		ns = tod_read(tod, TOD_CUR_NSEC);
		ptp_read_system_postts(sts);
		lo_after = tod_read(tod, TOD_CUR_SEC_LO);
		hi_after = tod_read(tod, TOD_CUR_SEC_HI) & TOD_SEC_HI_MASK;
		if (hi == hi_after && lo == lo_after && ns < NSEC_PER_SEC) {
			ts->tv_sec = (u64)hi << 32 | lo;
			ts->tv_nsec = ns;
			return 0;
		}
	}

	return -EAGAIN;
}

/* The caller holds lock; IRQ cannot consume an old completion while arming. */
static int tod_load_time(struct airoha_xpon_tod *tod,
			 const struct timespec64 *ts, bool gpon, u32 superframe)
{
	unsigned long flags;
	u32 ctrl, cfg;

	if (tod->dead)
		return -ENODEV;
	if (tod->failed)
		return -EIO;
	if (ts->tv_sec < 0 || ts->tv_sec > TOD_MAX_SECONDS ||
	    (unsigned long)ts->tv_nsec >= NSEC_PER_SEC)
		return -ERANGE;

	spin_lock_irqsave(&tod->irq_lock, flags);
	if (tod->pending) {
		spin_unlock_irqrestore(&tod->irq_lock, flags);
		return -EBUSY;
	}

	/* LOAD latches both the timestamp and its clock increment. */
	ctrl = gpon ? 0 : TOD_SOURCE_CPU;
	tod_write(tod, TOD_CTRL, ctrl);
	tod_write(tod, TOD_INT_STATUS, TOD_INT_LOAD_DONE);
	reinit_completion(&tod->load_done);
	if (gpon) {
		/* ASIC recovered PON clock: 6 + 209/486 ns (vendor SDK). */
		tod_write(tod, TOD_ADD_UNIT_INT, 6);
		tod_write(tod, TOD_ADD_UNIT_NUM, 209);
		tod_write(tod, TOD_ADD_UNIT_DEN, 486);
		cfg = readl(tod->priv->gpon_reg + GPON_TOD_CFG);
		cfg &= ~GPON_TOD_SUPERFRAME_MASK;
		writel(cfg | superframe, tod->priv->gpon_reg + GPON_TOD_CFG);
	} else {
		/* Match tod_module_init() for the CPU-selected clock. */
		tod_write(tod, TOD_ADD_UNIT_INT, 5);
		tod_write(tod, TOD_ADD_UNIT_NUM, 0);
		tod_write(tod, TOD_ADD_UNIT_DEN, 1);
	}
	tod_write(tod, TOD_NEW_SEC_HI, (u64)ts->tv_sec >> 32);
	tod_write(tod, TOD_NEW_SEC_LO, lower_32_bits(ts->tv_sec));
	tod_write(tod, TOD_NEW_NSEC, ts->tv_nsec);
	WRITE_ONCE(tod->pending, true);
	tod->gpon_source = gpon;
	tod_write(tod, TOD_CTRL, ctrl | TOD_LOAD);
	tod_read(tod, TOD_CTRL);
	spin_unlock_irqrestore(&tod->irq_lock, flags);

	/* OLT updates are asynchronous and may name a future superframe. */
	if (gpon)
		return 0;
	if (wait_for_completion_timeout(&tod->load_done,
					msecs_to_jiffies(TOD_LOAD_TIMEOUT_MS)))
		return 0;

	spin_lock_irqsave(&tod->irq_lock, flags);
	tod_write(tod, TOD_CTRL, TOD_SOURCE_CPU);
	tod_write(tod, TOD_INT_STATUS, TOD_INT_LOAD_DONE);
	WRITE_ONCE(tod->pending, false);
	/* Do not attribute a late completion to another load after a timeout. */
	tod->failed = true;
	spin_unlock_irqrestore(&tod->irq_lock, flags);
	return -ETIMEDOUT;
}

static int tod_gettimex(struct ptp_clock_info *info, struct timespec64 *ts,
			struct ptp_system_timestamp *sts)
{
	struct airoha_xpon_tod *tod = container_of(info, typeof(*tod), info);
	int ret;

	mutex_lock(&tod->lock);
	ret = tod->dead ? -ENODEV : tod_read_time(tod, ts, sts);
	mutex_unlock(&tod->lock);
	return ret;
}

static int tod_settime(struct ptp_clock_info *info, const struct timespec64 *ts)
{
	struct airoha_xpon_tod *tod = container_of(info, typeof(*tod), info);
	int ret;

	mutex_lock(&tod->lock);
	ret = tod_load_time(tod, ts, false, 0);
	mutex_unlock(&tod->lock);
	return ret;
}

static int tod_adjtime(struct ptp_clock_info *info, s64 delta)
{
	struct airoha_xpon_tod *tod = container_of(info, typeof(*tod), info);
	u64 magnitude = delta < 0 ? -(delta + 1) + 1ULL : delta;
	struct timespec64 now, offset, adjusted;
	u32 ns, ctrl;
	u64 sec;
	int ret = 0;

	sec = div_u64_rem(magnitude, NSEC_PER_SEC, &ns);
	mutex_lock(&tod->lock);
	if (tod->dead) {
		ret = -ENODEV;
		goto out;
	}
	if (tod->failed) {
		ret = -EIO;
		goto out;
	}
	if (READ_ONCE(tod->pending)) {
		ret = -EBUSY;
		goto out;
	}
	ret = tod_read_time(tod, &now, NULL);
	if (ret)
		goto out;
	offset = ns_to_timespec64(delta);
	adjusted = timespec64_add(now, offset);
	if (adjusted.tv_sec < 0 || adjusted.tv_sec > TOD_MAX_SECONDS) {
		ret = -ERANGE;
		goto out;
	}

	ctrl = tod_read(tod, TOD_CTRL) & TOD_SOURCE_CPU;
	tod_write(tod, TOD_CTRL, ctrl);
	tod_write(tod, TOD_OFFSET_SEC_HI, upper_32_bits(sec) |
		  (delta < 0 ? TOD_OFFSET_NEGATIVE : 0));
	tod_write(tod, TOD_OFFSET_SEC_LO, lower_32_bits(sec));
	tod_write(tod, TOD_OFFSET_NSEC, ns);
	tod_write(tod, TOD_CTRL, ctrl | TOD_ADJUST_OFFSET);
	ret = readl_poll_timeout(tod->base + TOD_CTRL, ctrl,
				 !(ctrl & TOD_ADJUST_OFFSET), 10, 10000);
	if (ret)
		tod->failed = true;
out:
	mutex_unlock(&tod->lock);
	return ret;
}

static int tod_adjfine(struct ptp_clock_info *info, long scaled_ppm)
{
	/* The SDK exposes raw frequency fractions, but not their ppb encoding. */
	return scaled_ppm ? -EOPNOTSUPP : 0;
}

static int tod_enable(struct ptp_clock_info *info,
		      struct ptp_clock_request *request, int on)
{
	struct airoha_xpon_tod *tod = container_of(info, typeof(*tod), info);
	unsigned long flags;
	u32 mask = TOD_INT_LOAD_DONE;
	int ret = 0;

	if (request->type != PTP_CLK_REQ_PPS)
		return -EOPNOTSUPP;
	mutex_lock(&tod->lock);
	if (tod->dead) {
		ret = -ENODEV;
		goto out;
	}
	spin_lock_irqsave(&tod->irq_lock, flags);
	if (on) {
		/* Minimum pulse width; board pinctrl owns any external output. */
		tod_write(tod, TOD_1PPS_WIDTH, 1);
		mask |= TOD_INT_1PPS;
	}
	WRITE_ONCE(tod->pps_enabled, !!on);
	tod_write(tod, TOD_INT_STATUS, TOD_INT_1PPS);
	tod_write(tod, TOD_INT_ENABLE, mask);
	spin_unlock_irqrestore(&tod->irq_lock, flags);
out:
	mutex_unlock(&tod->lock);
	return ret;
}

static irqreturn_t tod_interrupt(int irq, void *data)
{
	struct airoha_xpon_tod *tod = data;
	struct ptp_clock_event event = { .type = PTP_CLOCK_PPS };
	unsigned long flags;
	u32 status, active;

	spin_lock_irqsave(&tod->irq_lock, flags);
	status = tod_read(tod, TOD_INT_STATUS);
	if (!status) {
		spin_unlock_irqrestore(&tod->irq_lock, flags);
		return IRQ_NONE;
	}
	active = status & tod_read(tod, TOD_INT_ENABLE);
	tod_write(tod, TOD_INT_STATUS, status);
	if ((active & TOD_INT_LOAD_DONE) && tod->pending) {
		WRITE_ONCE(tod->pending, false);
		complete(&tod->load_done);
	}
	spin_unlock_irqrestore(&tod->irq_lock, flags);
	if ((active & TOD_INT_1PPS) && READ_ONCE(tod->pps_enabled) && tod->clock)
		ptp_clock_event(tod->clock, &event);
	return IRQ_HANDLED;
}

/* floor(one-way fibre delay - RX PHY delay), using the SDK optical ratios. */
static u64 tod_gpon_delay(u32 eqd, u32 response, u32 rx_delay)
{
	u64 tx = ((u64)eqd + (u64)response * 32) * 2 * 14682;
	u64 rx = (u64)rx_delay * 29359;

	if (tx <= rx)
		return 0;
	return div64_u64((tx - rx) * 100000, 248832ULL * 29359);
}

int airoha_gpon_omci_hw_set_time_of_day(void *hw_priv,
					const struct omci_time_of_day *time)
{
	struct xpon_priv *priv = hw_priv;
	struct airoha_xpon_tod *tod = priv->tod;
	struct timespec64 ts, delay;
	u32 response, rx_delay;
	int ret;

	if (!tod)
		return -EOPNOTSUPP;
	if (time->superframe > GPON_TOD_SUPERFRAME_MASK ||
	    time->seconds > TOD_MAX_SECONDS || time->nanoseconds >= NSEC_PER_SEC)
		return -EINVAL;

	mutex_lock(&tod->lock);
	if (tod->dead) {
		ret = -ENODEV;
		goto out;
	}
	if (!READ_ONCE(priv->mac_enabled) || !READ_ONCE(priv->gpon_o5)) {
		ret = -ENOLINK;
		goto out;
	}
	response = readl(priv->gpon_reg + GPON_RSP_TIME) & GENMASK(15, 0);
	rx_delay = readl(priv->gpon_reg + GPON_PHY_RX_DLY) & GPON_PHY_RX_DLY_MASK;
	delay = ns_to_timespec64(tod_gpon_delay(ploam_get_eqd(priv->ploam),
						response, rx_delay));
	ts = (struct timespec64) {
		.tv_sec = time->seconds,
		.tv_nsec = time->nanoseconds,
	};
	ts = timespec64_sub(ts, delay);
	ret = tod_load_time(tod, &ts, true, time->superframe);
out:
	mutex_unlock(&tod->lock);
	return ret;
}

void airoha_xpon_tod_stop(struct xpon_priv *priv)
{
	struct airoha_xpon_tod *tod = priv->tod;
	struct timespec64 ts;
	unsigned long flags;
	int ret;

	if (!tod)
		return;
	mutex_lock(&tod->lock);
	if (tod->dead || !tod->gpon_source)
		goto out;
	ret = tod_read_time(tod, &ts, NULL);
	spin_lock_irqsave(&tod->irq_lock, flags);
	/* Cancel a superframe load before the MAC/PHY clock disappears. */
	tod_write(tod, TOD_CTRL, TOD_SOURCE_CPU);
	tod_write(tod, TOD_INT_STATUS, TOD_INT_LOAD_DONE);
	WRITE_ONCE(tod->pending, false);
	tod->gpon_source = false;
	spin_unlock_irqrestore(&tod->irq_lock, flags);
	if (!ret)
		ret = tod_load_time(tod, &ts, false, 0);
	if (ret)
		dev_warn(priv->dev, "failed to switch ToD to the CPU clock: %d\n", ret);
out:
	mutex_unlock(&tod->lock);
}

static void tod_unregister(void *data)
{
	struct airoha_xpon_tod *tod = data;
	unsigned long flags;

	mutex_lock(&tod->lock);
	if (tod->dead) {
		mutex_unlock(&tod->lock);
		return;
	}
	tod->dead = true;
	WRITE_ONCE(tod->pps_enabled, false);
	spin_lock_irqsave(&tod->irq_lock, flags);
	tod_write(tod, TOD_INT_ENABLE, 0);
	tod_write(tod, TOD_CTRL, TOD_SOURCE_CPU);
	tod_write(tod, TOD_INT_STATUS, TOD_INT_ALL);
	WRITE_ONCE(tod->pending, false);
	tod_read(tod, TOD_INT_ENABLE);
	spin_unlock_irqrestore(&tod->irq_lock, flags);
	mutex_unlock(&tod->lock);
	synchronize_irq(tod->irq);
	if (tod->clock) {
		ptp_clock_unregister(tod->clock);
		tod->clock = NULL;
	}
}

void airoha_xpon_tod_remove(struct xpon_priv *priv)
{
	if (priv->tod)
		tod_unregister(priv->tod);
}

int airoha_xpon_tod_init(struct platform_device *pdev, struct xpon_priv *priv)
{
	struct device *dev = &pdev->dev;
	struct airoha_xpon_tod *tod;
	struct timespec64 now;
	struct resource *res;
	int ret;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "tod");
	if (!res)
		return 0;
	if (priv->mode != AIROHA_XPON_MODE_GPON ||
	    (priv->match_data->version != airoha_en7581 &&
	     priv->match_data->version != airoha_an7583))
		return dev_err_probe(dev, -EOPNOTSUPP, "unsupported ToD integration\n");
	if (resource_size(res) < TOD_RESOURCE_SIZE)
		return dev_err_probe(dev, -EINVAL, "ToD register resource is too small\n");
	tod = devm_kzalloc(dev, sizeof(*tod), GFP_KERNEL);
	if (!tod)
		return -ENOMEM;
	tod->priv = priv;
	mutex_init(&tod->lock);
	spin_lock_init(&tod->irq_lock);
	init_completion(&tod->load_done);
	tod->base = devm_ioremap_resource(dev, res);
	if (IS_ERR(tod->base))
		return PTR_ERR(tod->base);
	tod->irq = platform_get_irq_byname(pdev, "tod");
	if (tod->irq < 0)
		return tod->irq;
	tod_write(tod, TOD_INT_ENABLE, 0);
	tod_write(tod, TOD_INT_STATUS, TOD_INT_ALL);
	ret = devm_request_irq(dev, tod->irq, tod_interrupt, 0, "xpon-tod", tod);
	if (ret)
		return dev_err_probe(dev, ret, "failed to request ToD interrupt\n");
	ret = devm_add_action_or_reset(dev, tod_unregister, tod);
	if (ret)
		return ret;
	tod_write(tod, TOD_INT_ENABLE, TOD_INT_LOAD_DONE);
	ktime_get_real_ts64(&now);
	mutex_lock(&tod->lock);
	ret = tod_load_time(tod, &now, false, 0);
	mutex_unlock(&tod->lock);
	if (ret)
		return dev_err_probe(dev, ret, "failed to initialize ToD clock\n");

	tod->info = (struct ptp_clock_info) {
		.owner = THIS_MODULE,
		.name = "airoha-xpon-tod",
		.pps = 1,
		.gettimex64 = tod_gettimex,
		.settime64 = tod_settime,
		.adjtime = tod_adjtime,
		.adjfine = tod_adjfine,
		.enable = tod_enable,
	};
	tod->clock = ptp_clock_register(&tod->info, dev);
	if (IS_ERR(tod->clock)) {
		ret = PTR_ERR(tod->clock);
		tod->clock = NULL;
		return dev_err_probe(dev, ret, "failed to register ToD PHC\n");
	}
	priv->tod = tod;
	dev_info(dev, "ToD initialized, PTP clock index %d\n",
		 tod->clock ? ptp_clock_index(tod->clock) : -1);
	return 0;
}
