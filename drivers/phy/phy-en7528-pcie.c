// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2026 Ahmed Naseef <naseefkm@gmail.com>
 *
 * EcoNet EN7528 PCIe PHY Driver
 */

#include <linux/bitops.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/phy/phy.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>

struct en7528_pcie_phy_op {
	u32 reg;
	u32 mask;
	u32 val;
};

struct en7528_pcie_phy {
	struct regmap *regmap;
	const struct en7528_pcie_phy_op *data;
};

/* Port 0 PHY: set LCDDS_CLK_PH_INV for PLL operation */
static const struct en7528_pcie_phy_op en7528_phy_port0[] = {
	{
		.reg = 0x4a0,
		.mask = BIT(5),
		.val = BIT(5),
	},
	{ /* sentinel */ }
};

/* Port 1 PHY: Rx impedance tuning, target R -5 Ohm */
static const struct en7528_pcie_phy_op en7528_phy_port1[] = {
	{
		.reg = 0xb2c,
		.mask = GENMASK(13, 12),
		.val = BIT(12),
	},
	{ /* sentinel */ }
};

/* EN751221 Port 1 PHY */
static const struct en7528_pcie_phy_op en751221_phy_port1[] = {
	/* Rx Detection Timing for 7512 E1, 16*8 clock cycles */
	{
		.reg = 0xa28,
		.mask = GENMASK(17, 9),
		.val = 16 << 9,
	},
	/* Same for different power mode */
	{
		.reg = 0xa2c,
		.mask = GENMASK(8, 0),
		.val = 16,
	},
	{ /* sentinel */ }
};

static int en7528_pcie_phy_init(struct phy *phy)
{
	struct en7528_pcie_phy *ephy = phy_get_drvdata(phy);
	const struct en7528_pcie_phy_op *data = ephy->data;
	int i, ret;

	for (i = 0; data[i].mask || data[i].val; i++) {
		if (i)
			usleep_range(1000, 2000);

		ret = regmap_update_bits(ephy->regmap, data[i].reg,
					 data[i].mask, data[i].val);
		if (ret)
			return ret;
	}

	return 0;
}

static const struct phy_ops en7528_pcie_phy_ops = {
	.init	= en7528_pcie_phy_init,
	.owner	= THIS_MODULE,
};

static int en7528_pcie_phy_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	const struct en7528_pcie_phy_op *data;
	struct regmap_config regmap_config = {
		.reg_bits = 32,
		.val_bits = 32,
		.reg_stride = 4,
	};
	struct phy_provider *provider;
	struct en7528_pcie_phy *ephy;
	void __iomem *base;
	struct phy *phy;
	int i;

	data = of_device_get_match_data(dev);
	if (!data)
		return -EINVAL;

	ephy = devm_kzalloc(dev, sizeof(*ephy), GFP_KERNEL);
	if (!ephy)
		return -ENOMEM;

	ephy->data = data;

	base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(base))
		return PTR_ERR(base);

	for (i = 0; data[i].mask || data[i].val; i++)
		if (data[i].reg > regmap_config.max_register)
			regmap_config.max_register = data[i].reg;

	ephy->regmap = devm_regmap_init_mmio(dev, base, &regmap_config);
	if (IS_ERR(ephy->regmap))
		return PTR_ERR(ephy->regmap);

	phy = devm_phy_create(dev, dev->of_node, &en7528_pcie_phy_ops);
	if (IS_ERR(phy))
		return PTR_ERR(phy);

	phy_set_drvdata(phy, ephy);

	provider = devm_of_phy_provider_register(dev, of_phy_simple_xlate);

	return PTR_ERR_OR_ZERO(provider);
}

static const struct of_device_id en7528_pcie_phy_ids[] = {
	{ .compatible = "econet,en7528-pcie-phy0", .data = en7528_phy_port0 },
	{ .compatible = "econet,en7528-pcie-phy1", .data = en7528_phy_port1 },
	{ .compatible = "econet,en751221-pcie-phy0", .data = en7528_phy_port0 },
	{ .compatible = "econet,en751221-pcie-phy1", .data = en751221_phy_port1 },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, en7528_pcie_phy_ids);

static struct platform_driver en7528_pcie_phy_driver = {
	.probe = en7528_pcie_phy_probe,
	.driver = {
		.name = "en7528-pcie-phy",
		.of_match_table = en7528_pcie_phy_ids,
	},
};
module_platform_driver(en7528_pcie_phy_driver);

MODULE_AUTHOR("Ahmed Naseef <naseefkm@gmail.com>");
MODULE_DESCRIPTION("EcoNet EN7528 PCIe PHY driver");
MODULE_LICENSE("GPL v2");
