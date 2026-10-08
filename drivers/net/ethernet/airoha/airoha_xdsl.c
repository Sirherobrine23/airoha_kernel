// SPDX-License-Identifier: GPL-2.0-only
/*
 * EcoNet EN7512/EN7516 DMT/PTM frontend.
 *
 * This is the resource and packet-transport boundary, not a replacement for
 * the proprietary host-side G.hs/ADSL/VDSL PHY. Opening the netdev without a
 * registered line engine is an error; it must not imply successful training.
 */

#include <linux/atomic.h>
#include <linux/bitfield.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/dmaengine.h>
#include <linux/ethtool.h>
#include <linux/interrupt.h>
#include <linux/iopoll.h>
#include <linux/math64.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/of_net.h>
#include <linux/platform_device.h>
#include <linux/pm.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/reset.h>
#include <linux/rtnetlink.h>
#include <linux/seq_file.h>
#include <linux/workqueue.h>

#include "airoha_eth.h"
#include "airoha_xdsl.h"
#include "airoha_xdsl_internal.h"

/* 0xbf900000 is the KSEG1 alias; 0x1faa0000 is an eFuse bank. */
#define AIROHA_XDSL_DMT_PHYS_BASE	0x1f900000
#define AIROHA_XDSL_PTM_PHYS_BASE	0x1fb62000
#define AIROHA_XDSL_DMT_MIN_SIZE	0x2000
#define AIROHA_XDSL_PTM_MIN_SIZE	0x200

#define SCU_WAN_CONF		0x0070
#define SCU_WAN_MODE_MASK		GENMASK(1, 0)
#define SCU_WAN_MODE_PTM		2

#define DMT_VERSION		0x0000
#define DMT_INT_STATUS0		0x0004 /* read-to-clear */
#define DMT_INT_MASK3		0x000c
#define DMT_INT_MASK2		0x000d
#define DMT_INT_MASK1		0x000e
#define DMT_INT_MASK0		0x000f
#define DMT_INT_MASK4		0x0013
#define DMT_INT_MASKED		1

#define PTM_RESET		0x0000
#define PTM_RESET_TX_IDLE		BIT(2)
#define PTM_RESET_RX_IDLE		BIT(3)
#define PTM_CTRL			0x0004
#define PTM_CTRL_TX_EN		BIT(0)
#define PTM_CTRL_RX_EN		BIT(8)
#define PTM_CTRL_U_CELL_BASE_MODE	BIT(17)

#define AIROHA_XDSL_NUM_DMT_IRQS	6

struct airoha_xdsl_soc_data {
	const char *name;
	enum airoha_xdsl_afe afe;
};

struct airoha_xdsl {
	struct device *dev;
	const struct airoha_xdsl_soc_data *soc;
	struct airoha_xdsl_hw hw;
	void __iomem *ptm_base;
	struct regmap *scu;
	struct reset_control *dmt_reset;
	struct reset_control *ptm_reset;
	struct net_device *gdm_dev;
	struct dentry *debugfs;
	const struct airoha_xdsl_engine_ops *engine_ops;
	void *engine_priv;
	/* RTNL -> GDM xdsl_lock -> state_lock -> event_lock. */
	struct mutex state_lock;
	/* Protects pending events and the reporting endpoint in IRQ context. */
	spinlock_t event_lock;
	struct work_struct event_work;
	struct airoha_xdsl_line_state pending;
	struct airoha_xdsl_line_state line;
	atomic64_t irq_count[AIROHA_XDSL_NUM_DMT_IRQS];
	u32 dmt_version;
	u32 last_irq;
	u32 saved_wan_mode;
	int last_error;
	int dmt_irq;
	bool mux_owned;
	bool dmt_powered;
	bool ptm_powered;
	bool irq_enabled;
	bool running;
	bool registered;
	bool removing;
};

static struct platform_driver airoha_xdsl_driver;
/* Protects frontend lookup against devm resource release during removal. */
static DEFINE_MUTEX(airoha_xdsl_registry_lock);

static void airoha_xdsl_dmt_mask_all(struct airoha_xdsl *priv)
{
	writeb(DMT_INT_MASKED, priv->hw.dmt + DMT_INT_MASK0);
	writeb(DMT_INT_MASKED, priv->hw.dmt + DMT_INT_MASK1);
	writeb(DMT_INT_MASKED, priv->hw.dmt + DMT_INT_MASK2);
	writeb(DMT_INT_MASKED, priv->hw.dmt + DMT_INT_MASK3);
	writeb(DMT_INT_MASKED, priv->hw.dmt + DMT_INT_MASK4);
}

static void airoha_xdsl_publish_link(struct airoha_xdsl *priv, bool link,
				     u64 downstream, u64 upstream)
{
	struct airoha_xdsl_link_state state = {
		.valid = true,
		.link = link,
		.speed = link ? max_t(u64, 1, div_u64(downstream, 1000000)) :
				SPEED_UNKNOWN,
		.duplex = link ? DUPLEX_FULL : DUPLEX_UNKNOWN,
		.autoneg = AUTONEG_DISABLE,
		.port = PORT_OTHER,
		.rx_line_rate_bps = link ? downstream : 0,
		.tx_line_rate_bps = link ? upstream : 0,
	};

	airoha_eth_xdsl_update_link(priv->gdm_dev, &state);
}

/* RX precedes TX on enable; TX precedes RX on disable (vendor ptm_open/close). */
static void airoha_xdsl_ptm_enable(struct airoha_xdsl *priv, bool enable)
{
	u32 val = readl(priv->ptm_base + PTM_CTRL);

	if (enable) {
		val |= PTM_CTRL_RX_EN;
		writel(val, priv->ptm_base + PTM_CTRL);
		val |= PTM_CTRL_TX_EN;
	} else {
		val &= ~PTM_CTRL_TX_EN;
		writel(val, priv->ptm_base + PTM_CTRL);
		val &= ~PTM_CTRL_RX_EN;
	}
	writel(val, priv->ptm_base + PTM_CTRL);
	readl(priv->ptm_base + PTM_CTRL);
}

static int airoha_xdsl_ptm_idle(struct airoha_xdsl *priv)
{
	u32 val, mask = PTM_RESET_TX_IDLE | PTM_RESET_RX_IDLE;

	return readl_poll_timeout(priv->ptm_base + PTM_RESET, val,
				  (val & mask) == mask, 100, 20000);
}

static void airoha_xdsl_restore_mux(struct airoha_xdsl *priv)
{
	int ret;

	if (!priv->mux_owned)
		return;
	ret = regmap_update_bits(priv->scu, SCU_WAN_CONF, SCU_WAN_MODE_MASK,
				 priv->saved_wan_mode);
	if (ret) {
		dev_warn(priv->dev, "failed to restore WAN mux: %d\n", ret);
		return;
	}
	priv->mux_owned = false;
}

/* Refuse an unsafe MAC reset rather than provoking a PTM bus timeout. */
static int airoha_xdsl_power_down(struct airoha_xdsl *priv)
{
	int ret;

	if (priv->dmt_powered)
		airoha_xdsl_dmt_mask_all(priv);
	if (priv->ptm_powered) {
		airoha_xdsl_ptm_enable(priv, false);
		ret = airoha_xdsl_ptm_idle(priv);
		if (ret)
			return ret;
		ret = reset_control_assert(priv->ptm_reset);
		if (ret)
			return ret;
		priv->ptm_powered = false;
	}
	if (priv->dmt_powered) {
		ret = reset_control_assert(priv->dmt_reset);
		if (ret)
			return ret;
		priv->dmt_powered = false;
	}
	airoha_xdsl_restore_mux(priv);
	return 0;
}

static int airoha_xdsl_power_up(struct airoha_xdsl *priv)
{
	u32 val;
	int ret;

	/* Retry quiescence if a previous close could not safely assert reset. */
	ret = airoha_xdsl_power_down(priv);
	if (ret)
		return ret;
	ret = regmap_read(priv->scu, SCU_WAN_CONF, &val);
	if (ret)
		return ret;
	priv->saved_wan_mode = val & SCU_WAN_MODE_MASK;
	ret = regmap_update_bits(priv->scu, SCU_WAN_CONF,
				 SCU_WAN_MODE_MASK, SCU_WAN_MODE_PTM);
	if (ret)
		return ret;
	priv->mux_owned = true;

	/* A bootloader may have left PTM active. Make it accessible and idle
	 * before pulsing either the MAC or its DMT/TPSTC supplier.
	 */
	ret = reset_control_deassert(priv->ptm_reset);
	if (ret)
		return ret;
	priv->ptm_powered = true;
	airoha_xdsl_ptm_enable(priv, false);
	ret = airoha_xdsl_ptm_idle(priv);
	if (ret)
		return ret;
	ret = reset_control_assert(priv->ptm_reset);
	if (ret)
		return ret;
	priv->ptm_powered = false;
	ret = reset_control_assert(priv->dmt_reset);
	if (ret)
		return ret;
	msleep(20);
	ret = reset_control_deassert(priv->dmt_reset);
	if (ret)
		return ret;
	priv->dmt_powered = true;
	msleep(20);
	airoha_xdsl_dmt_mask_all(priv);
	readl(priv->hw.dmt + DMT_INT_STATUS0);
	priv->dmt_version = readl(priv->hw.dmt + DMT_VERSION);
	ret = reset_control_deassert(priv->ptm_reset);
	if (ret)
		return ret;
	priv->ptm_powered = true;
	msleep(20);
	val = readl(priv->ptm_base + PTM_CTRL);
	val &= ~(PTM_CTRL_U_CELL_BASE_MODE | PTM_CTRL_TX_EN | PTM_CTRL_RX_EN);
	writel(val, priv->ptm_base + PTM_CTRL);
	return 0;
}

static void airoha_xdsl_set_running(struct airoha_xdsl *priv, bool running)
{
	unsigned long flags;

	spin_lock_irqsave(&priv->event_lock, flags);
	priv->running = running;
	memset(&priv->pending, 0, sizeof(priv->pending));
	spin_unlock_irqrestore(&priv->event_lock, flags);
}

static int airoha_xdsl_netdev_start(void *data)
{
	struct airoha_xdsl *priv = data;
	int ret;

	mutex_lock(&priv->state_lock);
	if (!priv->engine_ops) {
		dev_err_ratelimited(priv->dev,
				    "no ADSL/VDSL line engine registered\n");
		ret = -EOPNOTSUPP;
		goto out;
	}
	if (priv->removing || !try_module_get(priv->engine_ops->owner)) {
		ret = -ENODEV;
		goto out;
	}
	ret = airoha_eth_xdsl_set_datapath(priv->gdm_dev, 0, 0);
	if (ret)
		goto put_engine;
	ret = airoha_xdsl_power_up(priv);
	if (ret)
		goto power_down;
	priv->line = (struct airoha_xdsl_line_state) {
		.status = AIROHA_XDSL_LINE_TRAINING,
	};
	airoha_xdsl_publish_link(priv, false, 0, 0);
	airoha_xdsl_set_running(priv, true);
	ret = priv->engine_ops->start(priv->engine_priv, &priv->hw);
	if (ret) {
		airoha_xdsl_set_running(priv, false);
		airoha_xdsl_dmt_mask_all(priv);
		priv->engine_ops->stop(priv->engine_priv);
		goto power_down;
	}
	enable_irq(priv->dmt_irq);
	priv->irq_enabled = true;
	priv->last_error = 0;
	mutex_unlock(&priv->state_lock);
	return 0;

power_down:
	if (priv->hw.qam_dma)
		dmaengine_terminate_sync(priv->hw.qam_dma);
	if (airoha_xdsl_power_down(priv))
		dev_warn(priv->dev, "PTM could not be quiesced after start failure\n");
put_engine:
	module_put(priv->engine_ops->owner);
out:
	priv->last_error = ret;
	priv->line.status = AIROHA_XDSL_LINE_FAILED;
	mutex_unlock(&priv->state_lock);
	return ret;
}

static void airoha_xdsl_netdev_stop(void *data)
{
	struct airoha_xdsl *priv = data;
	int ret;

	mutex_lock(&priv->state_lock);
	if (!priv->running)
		goto out;
	airoha_xdsl_set_running(priv, false);
	airoha_xdsl_publish_link(priv, false, 0, 0);
	airoha_xdsl_dmt_mask_all(priv);
	if (priv->irq_enabled) {
		disable_irq(priv->dmt_irq);
		priv->irq_enabled = false;
	}
	ret = airoha_eth_xdsl_set_datapath(priv->gdm_dev, 0, 0);
	if (ret)
		dev_warn(priv->dev, "PTM channel retirement failed: %d\n", ret);
	airoha_xdsl_ptm_enable(priv, false);
	priv->engine_ops->stop(priv->engine_priv);
	if (priv->hw.qam_dma)
		dmaengine_terminate_sync(priv->hw.qam_dma);
	ret = airoha_xdsl_power_down(priv);
	if (ret)
		dev_warn(priv->dev, "PTM still busy; reset not asserted: %d\n", ret);
	module_put(priv->engine_ops->owner);
	priv->last_error = ret;
	priv->line = (struct airoha_xdsl_line_state) {};
out:
	mutex_unlock(&priv->state_lock);
	/* Do not join this worker while holding the mutex it needs. */
	cancel_work_sync(&priv->event_work);
}

static const struct airoha_xdsl_link_ops airoha_xdsl_link_ops = {
	.start = airoha_xdsl_netdev_start,
	.stop = airoha_xdsl_netdev_stop,
};

static void airoha_xdsl_event_work(struct work_struct *work)
{
	struct airoha_xdsl *priv = container_of(work, struct airoha_xdsl,
					      event_work);
	struct airoha_xdsl_line_state state;
	unsigned long flags;
	u8 path_mask = 0;
	int ret;

	mutex_lock(&priv->state_lock);
	if (!priv->running)
		goto out;
	spin_lock_irqsave(&priv->event_lock, flags);
	state = priv->pending;
	spin_unlock_irqrestore(&priv->event_lock, flags);
	if (airoha_xdsl_same_line(&state, &priv->line))
		goto out;

	airoha_xdsl_publish_link(priv, false, 0, 0);
	airoha_xdsl_ptm_enable(priv, false);
	if (state.status == AIROHA_XDSL_LINE_SHOWTIME)
		path_mask = airoha_xdsl_path_mask(state.bearer_mask);
	ret = airoha_eth_xdsl_set_datapath(priv->gdm_dev, path_mask,
					   state.tx_bearer * 2);
	if (ret) {
		state.status = AIROHA_XDSL_LINE_FAILED;
		state.error = ret;
	} else if (path_mask) {
		airoha_xdsl_ptm_enable(priv, true);
		spin_lock_irqsave(&priv->event_lock, flags);
		/* A newer line-down notification must win over this snapshot. */
		if (airoha_xdsl_same_line(&priv->pending, &state))
			airoha_xdsl_publish_link(priv, true, state.downstream_bps,
						 state.upstream_bps);
		spin_unlock_irqrestore(&priv->event_lock, flags);
	}
	priv->line = state;
	priv->last_error = state.error;
out:
	mutex_unlock(&priv->state_lock);
}

static struct airoha_xdsl *airoha_xdsl_from_dev(struct device *dev)
{
	if (!dev || dev->driver != &airoha_xdsl_driver.driver)
		return NULL;
	return READ_ONCE(dev->driver_data);
}

int airoha_xdsl_register_engine(struct device *frontend, struct device *engine,
				const struct airoha_xdsl_engine_ops *ops,
				void *engine_priv)
{
	struct airoha_xdsl *priv;
	unsigned long flags;
	int ret = 0;

	if (!engine || !engine_priv || !ops || !ops->start || !ops->stop || !ops->irq)
		return -EINVAL;
	mutex_lock(&airoha_xdsl_registry_lock);
	priv = airoha_xdsl_from_dev(frontend);
	if (!priv) {
		ret = -EPROBE_DEFER;
		goto out_registry;
	}
	rtnl_lock();
	mutex_lock(&priv->state_lock);
	if (priv->removing || netif_running(priv->gdm_dev) || priv->engine_ops) {
		ret = -EBUSY;
		goto out;
	}
	if (!device_link_add(engine, frontend, DL_FLAG_AUTOREMOVE_CONSUMER)) {
		ret = -EINVAL;
		goto out;
	}
	spin_lock_irqsave(&priv->event_lock, flags);
	priv->engine_ops = ops;
	priv->engine_priv = engine_priv;
	spin_unlock_irqrestore(&priv->event_lock, flags);
	priv->line = (struct airoha_xdsl_line_state) {};
	priv->last_error = 0;
out:
	mutex_unlock(&priv->state_lock);
	rtnl_unlock();
out_registry:
	mutex_unlock(&airoha_xdsl_registry_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(airoha_xdsl_register_engine);

void airoha_xdsl_unregister_engine(struct device *frontend, void *engine_priv)
{
	struct airoha_xdsl *priv;
	unsigned long flags;

	if (!engine_priv)
		return;
	mutex_lock(&airoha_xdsl_registry_lock);
	priv = airoha_xdsl_from_dev(frontend);
	if (!priv)
		goto out_registry;
	rtnl_lock();
	if (priv->engine_priv != engine_priv)
		goto out;
	/* Closing via ndo_stop also updates the GDM provider's started state. */
	dev_close(priv->gdm_dev);
	airoha_xdsl_netdev_stop(priv);
	mutex_lock(&priv->state_lock);
	spin_lock_irqsave(&priv->event_lock, flags);
	priv->engine_ops = NULL;
	priv->engine_priv = NULL;
	spin_unlock_irqrestore(&priv->event_lock, flags);
	mutex_unlock(&priv->state_lock);
out:
	rtnl_unlock();
	/* The managed link is released by driver core on engine unbind. */
out_registry:
	mutex_unlock(&airoha_xdsl_registry_lock);
}
EXPORT_SYMBOL_GPL(airoha_xdsl_unregister_engine);

int airoha_xdsl_report_line(struct device *frontend, void *engine_priv,
			    const struct airoha_xdsl_line_state *state)
{
	struct airoha_xdsl *priv = airoha_xdsl_from_dev(frontend);
	struct airoha_xdsl_line_state new_state;
	unsigned long flags;
	int ret;

	if (!priv || !state)
		return -ENODEV;
	ret = airoha_xdsl_validate_line(state);
	new_state = ret ? (struct airoha_xdsl_line_state) {
		.status = AIROHA_XDSL_LINE_FAILED, .error = ret,
	} : *state;
	spin_lock_irqsave(&priv->event_lock, flags);
	if (!priv->running || priv->engine_priv != engine_priv) {
		ret = -ENETDOWN;
		goto out;
	}
	priv->pending = new_state;
	if (new_state.status != AIROHA_XDSL_LINE_SHOWTIME)
		airoha_xdsl_publish_link(priv, false, 0, 0);
	/* Queue before releasing the lock so stop cannot miss an in-flight report. */
	schedule_work(&priv->event_work);
out:
	spin_unlock_irqrestore(&priv->event_lock, flags);
	return ret;
}
EXPORT_SYMBOL_GPL(airoha_xdsl_report_line);

static irqreturn_t airoha_xdsl_irq_thread(int irq, void *data)
{
	struct airoha_xdsl *priv = data;
	u32 status;
	int i;

	status = readl(priv->hw.dmt + DMT_INT_STATUS0);
	if (!status)
		return IRQ_NONE;
	WRITE_ONCE(priv->last_irq, status);
	for (i = 0; i < AIROHA_XDSL_NUM_DMT_IRQS; i++)
		if (status & BIT(i))
			atomic64_inc(&priv->irq_count[i]);
	/* Ops remain pinned until disable_irq() has joined this thread. */
	priv->engine_ops->irq(priv->engine_priv, status);
	return IRQ_HANDLED;
}

static int airoha_xdsl_status_show(struct seq_file *seq, void *unused)
{
	struct airoha_xdsl *priv = seq->private;
	static const char * const states[] = { "down", "training", "showtime", "failed" };
	int i;

	mutex_lock(&priv->state_lock);
	seq_printf(seq, "soc: %s\nafe: %s\nengine: %s\nstate: %s\n",
		   priv->soc->name,
		   priv->soc->afe == AIROHA_XDSL_AFE_A60928 ? "A60928" : "A10627",
		   priv->engine_ops ? "registered" : "unavailable",
		   states[priv->line.status]);
	seq_printf(seq, "running: %u\ndmt_version: %#x\nlast_error: %d\n",
		   priv->running, priv->dmt_version, priv->last_error);
	seq_printf(seq, "downstream_bps: %llu\nupstream_bps: %llu\n",
		   priv->line.downstream_bps, priv->line.upstream_bps);
	seq_printf(seq, "bearer_mask: %#x\ntx_bearer: %u\nqam_dma: %s\n",
		   priv->line.bearer_mask, priv->line.tx_bearer,
		   priv->hw.qam_dma ? "available" : "absent");
	seq_printf(seq, "last_irq: %#x\n", READ_ONCE(priv->last_irq));
	for (i = 0; i < AIROHA_XDSL_NUM_DMT_IRQS; i++)
		seq_printf(seq, "irq_bit_%u: %lld\n", i,
			   atomic64_read(&priv->irq_count[i]));
	mutex_unlock(&priv->state_lock);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(airoha_xdsl_status);

static int airoha_xdsl_map_resource(struct platform_device *pdev,
				    const char *name, resource_size_t start,
				    resource_size_t min_size, void __iomem **base)
{
	struct resource *res;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, name);
	if (!res || res->start != start || resource_size(res) < min_size)
		return dev_err_probe(&pdev->dev, -EINVAL,
				     "invalid %s register resource\n", name);
	*base = devm_ioremap_resource(&pdev->dev, res);
	return PTR_ERR_OR_ZERO(*base);
}

static void airoha_xdsl_release_dma(void *data)
{
	dma_release_channel(data);
}

static int airoha_xdsl_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *eth_node;
	struct airoha_xdsl *priv;
	struct resource *res;
	int ret;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;
	priv->dev = dev;
	priv->soc = device_get_match_data(dev);
	if (!priv->soc)
		return -EINVAL;
	mutex_init(&priv->state_lock);
	spin_lock_init(&priv->event_lock);
	INIT_WORK(&priv->event_work, airoha_xdsl_event_work);

	ret = airoha_xdsl_map_resource(pdev, "dmt", AIROHA_XDSL_DMT_PHYS_BASE,
				       AIROHA_XDSL_DMT_MIN_SIZE, &priv->hw.dmt);
	if (ret)
		return ret;
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "dmt");
	priv->hw.dmt_phys = res->start;
	priv->hw.dmt_size = resource_size(res);
	priv->hw.afe = priv->soc->afe;
	ret = airoha_xdsl_map_resource(pdev, "ptm", AIROHA_XDSL_PTM_PHYS_BASE,
				       AIROHA_XDSL_PTM_MIN_SIZE, &priv->ptm_base);
	if (ret)
		return ret;
	priv->scu = syscon_regmap_lookup_by_phandle(dev->of_node, "airoha,scu");
	if (IS_ERR(priv->scu))
		return dev_err_probe(dev, PTR_ERR(priv->scu), "SCU unavailable\n");
	priv->dmt_irq = platform_get_irq_byname(pdev, "dmt");
	if (priv->dmt_irq < 0)
		return priv->dmt_irq;
	priv->dmt_reset = devm_reset_control_get_exclusive(dev, "dmt");
	if (IS_ERR(priv->dmt_reset))
		return dev_err_probe(dev, PTR_ERR(priv->dmt_reset), "DMT reset unavailable\n");
	priv->ptm_reset = devm_reset_control_get_exclusive(dev, "ptm");
	if (IS_ERR(priv->ptm_reset))
		return dev_err_probe(dev, PTR_ERR(priv->ptm_reset), "PTM reset unavailable\n");

	if (device_property_present(dev, "dmas")) {
		priv->hw.qam_dma = dma_request_chan(dev, "qam");
		if (IS_ERR(priv->hw.qam_dma))
			return dev_err_probe(dev, PTR_ERR(priv->hw.qam_dma),
					     "QAM GDMA channel unavailable\n");
		ret = devm_add_action_or_reset(dev, airoha_xdsl_release_dma,
					       priv->hw.qam_dma);
		if (ret)
			return ret;
	}
	ret = devm_request_threaded_irq(dev, priv->dmt_irq, NULL,
					airoha_xdsl_irq_thread,
					IRQF_ONESHOT | IRQF_NO_AUTOEN,
					dev_name(dev), priv);
	if (ret)
		return dev_err_probe(dev, ret, "DMT interrupt unavailable\n");

	eth_node = of_parse_phandle(dev->of_node, "ethernet", 0);
	if (!eth_node)
		return -EINVAL;
	priv->gdm_dev = of_find_net_device_by_node(eth_node);
	of_node_put(eth_node);
	if (!priv->gdm_dev)
		return -EPROBE_DEFER;
	/* Prevent Ethernet removal while the frontend still uses FE registers. */
	if (!device_link_add(dev, priv->gdm_dev->dev.parent,
			     DL_FLAG_AUTOREMOVE_CONSUMER)) {
		ret = -EINVAL;
		goto put_netdev;
	}
	ret = airoha_eth_register_xdsl(priv->gdm_dev, &airoha_xdsl_link_ops, priv);
	if (ret)
		goto put_netdev;
	priv->registered = true;
	mutex_lock(&airoha_xdsl_registry_lock);
	platform_set_drvdata(pdev, priv);
	mutex_unlock(&airoha_xdsl_registry_lock);
	airoha_xdsl_publish_link(priv, false, 0, 0);
	priv->debugfs = debugfs_create_dir(dev_name(dev), NULL);
	debugfs_create_file("status", 0400, priv->debugfs, priv,
			    &airoha_xdsl_status_fops);
	dev_info(dev, "%s DMT/PTM frontend; line engine required for operation\n",
		 priv->soc->name);
	return 0;

put_netdev:
	put_device(&priv->gdm_dev->dev);
	return dev_err_probe(dev, ret, "failed to attach GDM2 xDSL frontend\n");
}

static void airoha_xdsl_remove(struct platform_device *pdev)
{
	struct airoha_xdsl *priv = platform_get_drvdata(pdev);

	mutex_lock(&airoha_xdsl_registry_lock);
	platform_set_drvdata(pdev, NULL);
	mutex_unlock(&airoha_xdsl_registry_lock);
	debugfs_remove_recursive(priv->debugfs);
	rtnl_lock();
	mutex_lock(&priv->state_lock);
	priv->removing = true;
	mutex_unlock(&priv->state_lock);
	dev_close(priv->gdm_dev);
	rtnl_unlock();
	if (priv->registered)
		airoha_eth_unregister_xdsl(priv->gdm_dev, &airoha_xdsl_link_ops, priv);
	airoha_xdsl_netdev_stop(priv);
	mutex_lock(&priv->state_lock);
	if (airoha_xdsl_power_down(priv))
		dev_warn(priv->dev, "leaving busy PTM block out of reset\n");
	mutex_unlock(&priv->state_lock);
	put_device(&priv->gdm_dev->dev);
}

static void airoha_xdsl_shutdown(struct platform_device *pdev)
{
	struct airoha_xdsl *priv = platform_get_drvdata(pdev);

	rtnl_lock();
	dev_close(priv->gdm_dev);
	airoha_xdsl_netdev_stop(priv);
	mutex_lock(&priv->state_lock);
	if (airoha_xdsl_power_down(priv))
		dev_warn(priv->dev, "PTM still busy during shutdown\n");
	mutex_unlock(&priv->state_lock);
	rtnl_unlock();
}

/* No line-engine suspend/resume protocol exists yet. Never suspend a live
 * PHY and leave its training workers accessing powered-down hardware.
 */
static int airoha_xdsl_suspend(struct device *dev)
{
	struct airoha_xdsl *priv = dev_get_drvdata(dev);
	int ret;

	mutex_lock(&priv->state_lock);
	ret = priv->running || priv->dmt_powered || priv->ptm_powered ? -EBUSY : 0;
	mutex_unlock(&priv->state_lock);
	return ret;
}

static DEFINE_SIMPLE_DEV_PM_OPS(airoha_xdsl_pm_ops, airoha_xdsl_suspend, NULL);

static const struct airoha_xdsl_soc_data en751221_xdsl_data = {
	.name = "EN7512",
	.afe = AIROHA_XDSL_AFE_A60928,
};

static const struct airoha_xdsl_soc_data en751627_xdsl_data = {
	.name = "EN7516",
	.afe = AIROHA_XDSL_AFE_A10627,
};

static const struct of_device_id airoha_xdsl_of_match[] = {
	{ .compatible = "econet,en751221-xdsl", .data = &en751221_xdsl_data },
	{ .compatible = "econet,en751627-xdsl", .data = &en751627_xdsl_data },
	{}
};
MODULE_DEVICE_TABLE(of, airoha_xdsl_of_match);

static struct platform_driver airoha_xdsl_driver = {
	.probe = airoha_xdsl_probe,
	.remove = airoha_xdsl_remove,
	.shutdown = airoha_xdsl_shutdown,
	.driver = {
		.name = "airoha-xdsl",
		.of_match_table = airoha_xdsl_of_match,
		.pm = pm_sleep_ptr(&airoha_xdsl_pm_ops),
	},
};
module_platform_driver(airoha_xdsl_driver);

MODULE_DESCRIPTION("EcoNet EN7512/EN7516 DMT/PTM resource and line-engine frontend");
MODULE_LICENSE("GPL");
