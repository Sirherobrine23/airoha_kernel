// SPDX-License-Identifier: GPL-2.0-only
/*
 * EcoNet EN7512/EN7516 xDSL/PTM frontend
 *
 * This driver owns the Linux-visible DMT and PTM resources and connects
 * their lifecycle to the Airoha GDM2 netdev. The legacy tc3162_dmt module also
 * embeds the complete G.hs/ADSL/VDSL training and DSP implementation. That
 * line engine is deliberately not copied here; until a clean-room line engine
 * is available the driver keeps the DMT interrupts masked and reports carrier
 * down, while still providing a complete and safe PTM integration boundary.
 */

#include <linux/atomic.h>
#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/ethtool.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/math64.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/of_net.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/reset.h>

#include "airoha_eth.h"

/* Legacy 0xbf900000 is the KSEG1 alias; 0x1faa0000 is an eFuse bank. */
#define AIROHA_XDSL_DMT_PHYS_BASE	0x1f900000
#define AIROHA_XDSL_PTM_PHYS_BASE	0x1fb62000

#define AIROHA_XDSL_DMT_MIN_SIZE		0x2000
#define AIROHA_XDSL_PTM_MIN_SIZE		0x200

/* Shared WAN mux in the system-control block. */
#define SCU_WAN_CONF			0x0070
#define SCU_WAN_MODE_MASK		GENMASK(1, 0)
#define SCU_WAN_MODE_PTM		2

/* DMT system block, relative to the common DMT window. */
#define DMT_VERSION			0x0000
#define DMT_INT_STATUS0			0x0004 /* read-to-clear */
#define DMT_INT_MASK3			0x000c
#define DMT_INT_MASK2			0x000d
#define DMT_INT_MASK1			0x000e
#define DMT_INT_MASK0			0x000f
#define DMT_INT_MASK4			0x0013
#define DMT_INT_MASKED			1

/* TPSTC lives inside the DMT window. */
#define DMT_TPSTC_TX_CFG		0x0f00
#define DMT_TPSTC_RX_CFG		0x1080
#define DMT_TPSTC_MODE_MASK		GENMASK(3, 0)
#define DMT_TPSTC_PTM_MODE		2

/* PTM-TC MAC block. */
#define PTM_CTRL			0x0004
#define PTM_CTRL_TX_EN			BIT(0)
#define PTM_CTRL_RX_EN			BIT(8)
#define PTM_CTRL_U_CELL_BASE_MODE	BIT(17)

#define AIROHA_XDSL_NUM_DMT_IRQS	6

enum airoha_xdsl_afe {
	AIROHA_XDSL_AFE_A60928,
	AIROHA_XDSL_AFE_A10627,
};

struct airoha_xdsl_soc_data {
	const char *name;
	enum airoha_xdsl_afe afe;
};

struct airoha_xdsl {
	struct device *dev;
	const struct airoha_xdsl_soc_data *soc;
	void __iomem *dmt_base;
	void __iomem *ptm_base;
	struct regmap *scu;
	struct reset_control *dmt_reset;
	struct reset_control *ptm_reset;
	struct net_device *gdm_dev;
	/* Serializes hardware state changes from netdev open/close and remove. */
	struct mutex state_lock;
	atomic64_t irq_count[AIROHA_XDSL_NUM_DMT_IRQS];
	u32 dmt_version;
	int dmt_irq;
	bool irq_enabled;
	bool running;
	bool registered;
};

static void airoha_xdsl_dmt_mask_all(struct airoha_xdsl *priv)
{
	writeb(DMT_INT_MASKED, priv->dmt_base + DMT_INT_MASK0);
	writeb(DMT_INT_MASKED, priv->dmt_base + DMT_INT_MASK1);
	writeb(DMT_INT_MASKED, priv->dmt_base + DMT_INT_MASK2);
	writeb(DMT_INT_MASKED, priv->dmt_base + DMT_INT_MASK3);
	writeb(DMT_INT_MASKED, priv->dmt_base + DMT_INT_MASK4);
}

static void airoha_xdsl_publish_link(struct airoha_xdsl *priv, bool link,
				     u64 downstream, u64 upstream)
{
	struct airoha_xdsl_link_state state = {
		.valid = true,
		.link = link,
		.speed = link ? div_u64(downstream, 1000000) : SPEED_UNKNOWN,
		.duplex = link ? DUPLEX_FULL : DUPLEX_UNKNOWN,
		.autoneg = AUTONEG_DISABLE,
		.port = PORT_OTHER,
		.rx_line_rate_bps = downstream,
		.tx_line_rate_bps = upstream,
	};

	airoha_eth_xdsl_update_link(priv->gdm_dev, &state);
}

static void airoha_xdsl_ptm_enable(struct airoha_xdsl *priv, bool enable)
{
	u32 val;

	val = readl(priv->ptm_base + PTM_CTRL);
	val &= ~(PTM_CTRL_TX_EN | PTM_CTRL_RX_EN);
	if (enable)
		val |= PTM_CTRL_TX_EN | PTM_CTRL_RX_EN;
	writel(val, priv->ptm_base + PTM_CTRL);
}

static void airoha_xdsl_tpstc_set_ptm(struct airoha_xdsl *priv)
{
	u32 val;

	val = readl(priv->dmt_base + DMT_TPSTC_TX_CFG);
	val &= ~DMT_TPSTC_MODE_MASK;
	val |= FIELD_PREP(DMT_TPSTC_MODE_MASK, DMT_TPSTC_PTM_MODE);
	writel(val, priv->dmt_base + DMT_TPSTC_TX_CFG);

	val = readl(priv->dmt_base + DMT_TPSTC_RX_CFG);
	val &= ~DMT_TPSTC_MODE_MASK;
	val |= FIELD_PREP(DMT_TPSTC_MODE_MASK, DMT_TPSTC_PTM_MODE);
	writel(val, priv->dmt_base + DMT_TPSTC_RX_CFG);
}

static int airoha_xdsl_reset_deassert(struct reset_control *reset)
{
	int ret;

	ret = reset_control_assert(reset);
	if (ret)
		return ret;

	msleep(20);
	ret = reset_control_deassert(reset);
	if (ret)
		return ret;

	msleep(20);
	return 0;
}

static int airoha_xdsl_hw_start(struct airoha_xdsl *priv)
{
	u32 val;
	int ret;

	mutex_lock(&priv->state_lock);
	if (priv->running) {
		ret = 0;
		goto out_unlock;
	}

	ret = regmap_update_bits(priv->scu, SCU_WAN_CONF,
				 SCU_WAN_MODE_MASK, SCU_WAN_MODE_PTM);
	if (ret)
		goto out_unlock;

	ret = airoha_xdsl_reset_deassert(priv->dmt_reset);
	if (ret)
		goto out_unlock;

	airoha_xdsl_dmt_mask_all(priv);
	/* DMT_INT_STATUS0 is read-to-clear. */
	readl(priv->dmt_base + DMT_INT_STATUS0);
	priv->dmt_version = readl(priv->dmt_base + DMT_VERSION);

	ret = airoha_xdsl_reset_deassert(priv->ptm_reset);
	if (ret)
		goto err_assert_dmt;

	/* Match the vendor PTM fast path: disable dummy U-cell accesses. */
	val = readl(priv->ptm_base + PTM_CTRL);
	val &= ~(PTM_CTRL_U_CELL_BASE_MODE | PTM_CTRL_TX_EN |
		 PTM_CTRL_RX_EN);
	writel(val, priv->ptm_base + PTM_CTRL);
	airoha_xdsl_tpstc_set_ptm(priv);

	/* The line engine owns unmasking; keep the periodic DMT IRQ quiescent. */
	enable_irq(priv->dmt_irq);
	priv->irq_enabled = true;
	priv->running = true;
	airoha_xdsl_publish_link(priv, false, 0, 0);

	dev_info(priv->dev,
		 "%s DMT %#08x and PTM frontend started; waiting for a line engine\n",
		 priv->soc->name, priv->dmt_version);
	mutex_unlock(&priv->state_lock);
	return 0;

err_assert_dmt:
	reset_control_assert(priv->dmt_reset);
out_unlock:
	mutex_unlock(&priv->state_lock);
	return ret;
}

static void airoha_xdsl_hw_stop(struct airoha_xdsl *priv)
{
	mutex_lock(&priv->state_lock);
	if (!priv->running)
		goto out_unlock;

	airoha_xdsl_publish_link(priv, false, 0, 0);
	airoha_xdsl_ptm_enable(priv, false);
	airoha_xdsl_dmt_mask_all(priv);
	if (priv->irq_enabled) {
		disable_irq(priv->dmt_irq);
		priv->irq_enabled = false;
	}
	reset_control_assert(priv->ptm_reset);
	reset_control_assert(priv->dmt_reset);
	priv->running = false;

out_unlock:
	mutex_unlock(&priv->state_lock);
}

static int airoha_xdsl_netdev_start(void *data)
{
	return airoha_xdsl_hw_start(data);
}

static void airoha_xdsl_netdev_stop(void *data)
{
	airoha_xdsl_hw_stop(data);
}

static const struct airoha_xdsl_link_ops airoha_xdsl_link_ops = {
	.start = airoha_xdsl_netdev_start,
	.stop = airoha_xdsl_netdev_stop,
};

static irqreturn_t airoha_xdsl_irq(int irq, void *data)
{
	struct airoha_xdsl *priv = data;
	u32 status;
	int i;

	status = readl(priv->dmt_base + DMT_INT_STATUS0);
	if (!status)
		return IRQ_NONE;

	for (i = 0; i < AIROHA_XDSL_NUM_DMT_IRQS; i++)
		if (status & BIT(i))
			atomic64_inc(&priv->irq_count[i]);

	/* A future line engine consumes the decoded events before unmasking. */
	airoha_xdsl_dmt_mask_all(priv);
	dev_dbg_ratelimited(priv->dev, "masked unsupported DMT IRQ status %#x\n",
			    status);

	return IRQ_HANDLED;
}

static int airoha_xdsl_resolve_datapath(struct airoha_xdsl *priv)
{
	struct device_node *eth_node;

	eth_node = of_parse_phandle(priv->dev->of_node, "ethernet", 0);
	if (!eth_node)
		return dev_err_probe(priv->dev, -EINVAL,
				     "missing GDM2 ethernet phandle\n");

	priv->gdm_dev = of_find_net_device_by_node(eth_node);
	of_node_put(eth_node);
	if (!priv->gdm_dev)
		return dev_err_probe(priv->dev, -EPROBE_DEFER,
				     "GDM2 netdev is not registered yet\n");

	return 0;
}

static int airoha_xdsl_map_resource(struct platform_device *pdev,
				    const char *name, resource_size_t start,
				    resource_size_t min_size, void __iomem **base)
{
	struct device *dev = &pdev->dev;
	struct resource *res;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, name);
	if (!res)
		return dev_err_probe(dev, -EINVAL, "missing %s resource\n", name);
	if (res->start != start)
		return dev_err_probe(dev, -EINVAL,
				     "unexpected %s base %pa\n", name, &res->start);
	if (resource_size(res) < min_size)
		return dev_err_probe(dev, -EINVAL,
				     "%s resource is too small\n", name);

	*base = devm_ioremap_resource(dev, res);
	return PTR_ERR_OR_ZERO(*base);
}

static int airoha_xdsl_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct airoha_xdsl *priv;
	int ret;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->dev = dev;
	priv->soc = device_get_match_data(dev);
	if (!priv->soc)
		return -EINVAL;

	mutex_init(&priv->state_lock);

	ret = airoha_xdsl_map_resource(pdev, "dmt",
				       AIROHA_XDSL_DMT_PHYS_BASE,
				       AIROHA_XDSL_DMT_MIN_SIZE,
				       &priv->dmt_base);
	if (ret)
		return ret;

	ret = airoha_xdsl_map_resource(pdev, "ptm",
				       AIROHA_XDSL_PTM_PHYS_BASE,
				       AIROHA_XDSL_PTM_MIN_SIZE,
				       &priv->ptm_base);
	if (ret)
		return ret;

	priv->scu = syscon_regmap_lookup_by_phandle(dev->of_node,
						    "airoha,scu");
	if (IS_ERR(priv->scu))
		return dev_err_probe(dev, PTR_ERR(priv->scu),
				     "failed to get SCU regmap\n");

	priv->dmt_irq = platform_get_irq_byname(pdev, "dmt");
	if (priv->dmt_irq < 0)
		return dev_err_probe(dev, priv->dmt_irq,
				     "failed to get DMT interrupt\n");

	priv->dmt_reset = devm_reset_control_get_exclusive(dev, "dmt");
	if (IS_ERR(priv->dmt_reset))
		return dev_err_probe(dev, PTR_ERR(priv->dmt_reset),
				     "failed to get DMT reset\n");

	priv->ptm_reset = devm_reset_control_get_exclusive(dev, "ptm");
	if (IS_ERR(priv->ptm_reset))
		return dev_err_probe(dev, PTR_ERR(priv->ptm_reset),
				     "failed to get PTM reset\n");

	ret = devm_request_irq(dev, priv->dmt_irq, airoha_xdsl_irq,
			       IRQF_NO_AUTOEN, dev_name(dev), priv);
	if (ret)
		return dev_err_probe(dev, ret, "failed to request DMT interrupt\n");

	ret = airoha_xdsl_resolve_datapath(priv);
	if (ret)
		return ret;

	ret = airoha_eth_register_xdsl(priv->gdm_dev, &airoha_xdsl_link_ops,
				       priv);
	if (ret) {
		put_device(&priv->gdm_dev->dev);
		priv->gdm_dev = NULL;
		return dev_err_probe(dev, ret,
				     "failed to register GDM2 xDSL frontend\n");
	}
	priv->registered = true;

	platform_set_drvdata(pdev, priv);
	airoha_xdsl_publish_link(priv, false, 0, 0);

	dev_info(dev, "%s xDSL/PTM frontend, AFE %s, IRQ %d\n",
		 priv->soc->name,
		 priv->soc->afe == AIROHA_XDSL_AFE_A60928 ? "A60928" : "A10627",
		 priv->dmt_irq);

	return 0;
}

static void airoha_xdsl_remove(struct platform_device *pdev)
{
	struct airoha_xdsl *priv = platform_get_drvdata(pdev);

	if (!priv)
		return;

	if (priv->registered)
		airoha_eth_unregister_xdsl(priv->gdm_dev,
					   &airoha_xdsl_link_ops, priv);
	airoha_xdsl_hw_stop(priv);
	if (priv->gdm_dev)
		put_device(&priv->gdm_dev->dev);
}

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
	{ }
};
MODULE_DEVICE_TABLE(of, airoha_xdsl_of_match);

static struct platform_driver airoha_xdsl_driver = {
	.probe = airoha_xdsl_probe,
	.remove = airoha_xdsl_remove,
	.driver = {
		.name = "airoha-xdsl",
		.of_match_table = airoha_xdsl_of_match,
	},
};
module_platform_driver(airoha_xdsl_driver);

MODULE_DESCRIPTION("EcoNet EN7512/EN7516 xDSL/PTM frontend");
MODULE_LICENSE("GPL");
