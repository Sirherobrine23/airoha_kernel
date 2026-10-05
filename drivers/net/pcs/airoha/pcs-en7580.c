// SPDX-License-Identifier: GPL-2.0
/* Airoha EN7580 XSI PCS and PMA. */

#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/pcs/pcs-provider.h>
#include <linux/phylink.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/rtnetlink.h>

#include "pcs-airoha.h"

#define EN7580_SCU_RESET			0x830
#define EN7580_SCU_XSI_PHY_RESET		BIT(8)
#define EN7580_SCU_XSI_CLK		0x860
#define EN7580_SCU_XSI_CLK_PD		BIT(10)
#define EN7580_SCU_XSI_CTRL		0x92c
#define EN7580_SCU_XSI_DISABLE		BIT(2)
#define EN7580_CHIP_SCU_XSI_CTRL		0x104
#define EN7580_CHIP_SCU_XSI_ENABLE	(BIT(15) | BIT(0))

/* Offsets relative to the PCS resource at 0x1faf7900. */
#define EN7580_XFI_CTRL			0x0
#define EN7580_XFI_STATUS		0x30
#define EN7580_XFI_READY			(BIT(12) | BIT(0))
#define EN7580_HSGMII_CTRL		0x808
#define EN7580_HSGMII_STATUS		0x91c
#define EN7580_HSGMII_STATE		GENMASK(3, 0)
#define EN7580_HSGMII_READY		0xa

#define EN7580_MAC_STOP			GENMASK(3, 0)
#define EN7580_MAC_TX_MASK		BIT(9)
#define EN7580_MAC_INT_STATUS		0x4
#define EN7580_SEQ_DELAY			0xffff
#define EN7580_SEQ_SCU			BIT(15)

struct en7580_pcs_step {
	u16 reg;
	u32 mask;
	u32 value[4];
};

#include "pcs-en7580-seq.h"

struct en7580_pcs {
	struct phylink_pcs pcs;
	struct regmap *mac;
	struct regmap *pma;
	struct regmap *digital;
	struct regmap *scu;
	struct regmap *chip_scu;
	phy_interface_t interface;
	bool configured;
};

static struct en7580_pcs *to_en7580_pcs(struct phylink_pcs *pcs)
{
	return container_of(pcs, struct en7580_pcs, pcs);
}

static int en7580_pcs_mode(phy_interface_t interface)
{
	switch (interface) {
	case PHY_INTERFACE_MODE_10GBASER:
		return 0;
	case PHY_INTERFACE_MODE_5GBASER:
		return 1;
	case PHY_INTERFACE_MODE_2500BASEX:
		return 2;
	case PHY_INTERFACE_MODE_1000BASEX:
		return 3;
	default:
		return -EINVAL;
	}
}

static int en7580_pcs_sequence(struct en7580_pcs *priv,
			       const struct en7580_pcs_step *seq,
			       unsigned int count, unsigned int mode)
{
	unsigned int i;
	int ret;

	for (i = 0; i < count; i++) {
		struct regmap *map = priv->pma;
		u32 value = seq[i].value[mode];
		u16 reg = seq[i].reg;

		if (reg == EN7580_SEQ_DELAY) {
			if (value < 10)
				udelay(value);
			else
				usleep_range(value, value + max(1U, value / 10));
			continue;
		}

		if (reg & EN7580_SEQ_SCU) {
			map = priv->scu;
			reg &= ~EN7580_SEQ_SCU;
		}

		/* Calibration and reset strobes must always reach hardware. */
		ret = regmap_write_bits(map, reg, seq[i].mask, value);
		if (ret)
			return ret;
	}

	return 0;
}

static int en7580_pcs_digital_reset(struct en7580_pcs *priv, int mode,
				    bool release)
{
	u32 ctrl = mode < 2 ? EN7580_XFI_CTRL : EN7580_HSGMII_CTRL;
	u32 first = mode < 2 ? BIT(15) : BIT(27);
	u32 second = mode < 2 ? BIT(16) : BIT(31);
	int ret;

	/* The vendor changes RX and TX reset separately, in this order. */
	ret = regmap_write_bits(priv->digital, ctrl, first, release ? first : 0);
	if (ret)
		return ret;
	return regmap_write_bits(priv->digital, ctrl, second, release ? second : 0);
}

static unsigned int en7580_pcs_inband_caps(struct phylink_pcs *pcs,
					 phy_interface_t interface)
{
	/* The recovered path uses the external PHY for negotiation. */
	return LINK_INBAND_DISABLE;
}

static void en7580_pcs_link_down(struct phylink_pcs *pcs)
{
	struct en7580_pcs *priv = to_en7580_pcs(pcs);

	regmap_set_bits(priv->mac, AIROHA_PCS_XFI_MAC_XFI_GIB_CFG,
			EN7580_MAC_STOP | EN7580_MAC_TX_MASK);
}

static int en7580_pcs_config(struct phylink_pcs *pcs, unsigned int neg_mode,
			     phy_interface_t interface,
			     const unsigned long *advertising,
			     bool permit_pause_to_mac)
{
	struct en7580_pcs *priv = to_en7580_pcs(pcs);
	int mode, ret;

	mode = en7580_pcs_mode(interface);
	if (mode < 0 || neg_mode == PHYLINK_PCS_NEG_INBAND_ENABLED)
		return -EINVAL;

	if (priv->configured && priv->interface == interface)
		return 0;

	priv->configured = false;
	en7580_pcs_link_down(pcs);

	ret = regmap_set_bits(priv->scu, EN7580_SCU_RESET,
			      EN7580_SCU_XSI_PHY_RESET);
	if (ret)
		return ret;
	udelay(1);
	ret = regmap_clear_bits(priv->scu, EN7580_SCU_RESET,
				EN7580_SCU_XSI_PHY_RESET);
	if (ret)
		return ret;

	ret = en7580_pcs_sequence(priv, en7580_pcs_init_seq,
				 ARRAY_SIZE(en7580_pcs_init_seq), mode);
	if (ret)
		return ret;

	ret = en7580_pcs_digital_reset(priv, mode, false);
	if (ret)
		return ret;

	ret = en7580_pcs_sequence(priv, en7580_pcs_start_seq,
				 ARRAY_SIZE(en7580_pcs_start_seq), mode);
	if (ret)
		return ret;

	ret = en7580_pcs_digital_reset(priv, mode, true);
	if (ret)
		return ret;

	/* Reproduce xsi_mac_itf_reset while keeping traffic stopped. */
	ret = regmap_set_bits(priv->mac, AIROHA_PCS_XFI_MAC_XFI_CNT_CLR,
			      AIROHA_PCS_XFI_GLB_CNT_CLR);
	if (ret)
		return ret;
	ret = regmap_write(priv->mac, EN7580_MAC_INT_STATUS, U32_MAX);
	if (ret)
		return ret;

	priv->interface = interface;
	priv->configured = true;
	return 0;
}

static void en7580_pcs_get_state(struct phylink_pcs *pcs, unsigned int neg_mode,
				 struct phylink_link_state *state)
{
	struct en7580_pcs *priv = to_en7580_pcs(pcs);
	u32 val;
	int mode = en7580_pcs_mode(priv->interface);
	static const int speeds[] = { SPEED_10000, SPEED_5000,
				      SPEED_2500, SPEED_1000 };

	state->link = false;
	if (!priv->configured || mode < 0)
		return;

	if (mode < 2) {
		if (regmap_read(priv->digital, EN7580_XFI_STATUS, &val))
			return;
		state->link = (val & EN7580_XFI_READY) == EN7580_XFI_READY;
	} else {
		if (regmap_read(priv->digital, EN7580_HSGMII_STATUS, &val))
			return;
		state->link = FIELD_GET(EN7580_HSGMII_STATE, val) ==
			      EN7580_HSGMII_READY;
	}
	state->speed = speeds[mode];
	state->duplex = DUPLEX_FULL;
}

static void en7580_pcs_link_up(struct phylink_pcs *pcs, unsigned int neg_mode,
			      phy_interface_t interface, int speed, int duplex)
{
	struct en7580_pcs *priv = to_en7580_pcs(pcs);
	u32 pause = AIROHA_PCS_XFI_TX_FC_EN | AIROHA_PCS_XFI_RX_FC_EN;

	if (!priv->configured)
		return;

	regmap_update_bits(priv->mac, AIROHA_PCS_XFI_MAC_XFI_GIB_CFG,
			   AIROHA_PCS_XFI_TX_FC_EN | AIROHA_PCS_XFI_RX_FC_EN,
			   pause);
	regmap_clear_bits(priv->mac, AIROHA_PCS_XFI_MAC_XFI_GIB_CFG,
			  EN7580_MAC_STOP | EN7580_MAC_TX_MASK);
}

static const struct phylink_pcs_ops en7580_pcs_ops = {
	.pcs_inband_caps = en7580_pcs_inband_caps,
	.pcs_config = en7580_pcs_config,
	.pcs_get_state = en7580_pcs_get_state,
	.pcs_link_up = en7580_pcs_link_up,
	.pcs_link_down = en7580_pcs_link_down,
	.pcs_disable = en7580_pcs_link_down,
};

static struct phylink_pcs *en7580_pcs_get(struct fwnode_reference_args *args,
					void *data)
{
	struct en7580_pcs *priv = data;

	if (args->nargs)
		return ERR_PTR(-EINVAL);
	return &priv->pcs;
}

static struct regmap *en7580_pcs_map(struct platform_device *pdev,
				   const char *name)
{
	struct resource *res = platform_get_resource_byname(pdev, IORESOURCE_MEM,
							 name);
	const struct regmap_config config = {
		.name = name,
		.reg_bits = 32,
		.val_bits = 32,
		.reg_stride = 4,
		.max_register = res ? resource_size(res) - 4 : 0,
	};
	void __iomem *base;

	if (!res)
		return ERR_PTR(-EINVAL);
	base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(base))
		return ERR_CAST(base);
	return devm_regmap_init_mmio(&pdev->dev, base, &config);
}

static int en7580_pcs_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct en7580_pcs *priv;
	int ret;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->mac = en7580_pcs_map(pdev, "mac");
	if (IS_ERR(priv->mac))
		return dev_err_probe(dev, PTR_ERR(priv->mac), "failed to map MAC\n");
	priv->pma = en7580_pcs_map(pdev, "pma");
	if (IS_ERR(priv->pma))
		return dev_err_probe(dev, PTR_ERR(priv->pma), "failed to map PMA\n");
	priv->digital = en7580_pcs_map(pdev, "pcs");
	if (IS_ERR(priv->digital))
		return dev_err_probe(dev, PTR_ERR(priv->digital), "failed to map PCS\n");
	priv->scu = syscon_regmap_lookup_by_phandle(dev->of_node, "airoha,scu");
	if (IS_ERR(priv->scu))
		return dev_err_probe(dev, PTR_ERR(priv->scu), "failed to get NP SCU\n");
	priv->chip_scu = syscon_regmap_lookup_by_phandle(dev->of_node,
						       "airoha,chip-scu");
	if (IS_ERR(priv->chip_scu))
		return dev_err_probe(dev, PTR_ERR(priv->chip_scu), "failed to get chip SCU\n");

	/* en7580_xfi_phy_dev_init: enable XSI and its clock. */
	ret = regmap_set_bits(priv->chip_scu, EN7580_CHIP_SCU_XSI_CTRL,
			      EN7580_CHIP_SCU_XSI_ENABLE);
	if (ret)
		return ret;
	ret = regmap_clear_bits(priv->scu, EN7580_SCU_XSI_CTRL,
				EN7580_SCU_XSI_DISABLE);
	if (ret)
		return ret;
	ret = regmap_clear_bits(priv->scu, EN7580_SCU_XSI_CLK,
				EN7580_SCU_XSI_CLK_PD);
	if (ret)
		return ret;

	priv->pcs.ops = &en7580_pcs_ops;
	priv->pcs.poll = true;
	__set_bit(PHY_INTERFACE_MODE_10GBASER, priv->pcs.supported_interfaces);
	__set_bit(PHY_INTERFACE_MODE_5GBASER, priv->pcs.supported_interfaces);
	__set_bit(PHY_INTERFACE_MODE_2500BASEX, priv->pcs.supported_interfaces);
	__set_bit(PHY_INTERFACE_MODE_1000BASEX, priv->pcs.supported_interfaces);
	platform_set_drvdata(pdev, priv);

	return fwnode_pcs_add_provider(dev_fwnode(dev), en7580_pcs_get, priv);
}

static void en7580_pcs_remove(struct platform_device *pdev)
{
	struct en7580_pcs *priv = platform_get_drvdata(pdev);

	fwnode_pcs_del_provider(dev_fwnode(&pdev->dev));
	rtnl_lock();
	phylink_release_pcs(&priv->pcs);
	rtnl_unlock();
}

static const struct of_device_id en7580_pcs_of_match[] = {
	{ .compatible = "airoha,en7580-pcs" },
	{ }
};
MODULE_DEVICE_TABLE(of, en7580_pcs_of_match);

static struct platform_driver en7580_pcs_driver = {
	.probe = en7580_pcs_probe,
	.remove = en7580_pcs_remove,
	.driver = {
		.name = "en7580-pcs",
		.of_match_table = en7580_pcs_of_match,
	},
};
module_platform_driver(en7580_pcs_driver);

MODULE_DESCRIPTION("Airoha EN7580 XSI PCS driver");
MODULE_LICENSE("GPL");
