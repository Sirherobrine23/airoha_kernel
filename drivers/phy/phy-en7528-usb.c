// SPDX-License-Identifier: GPL-2.0
/*
 * EcoNet EN7528/EN7580 USB PHY driver
 *
 * Based on GPL vendor code at https://github.com/keenetic/kernel-49
 * and the Airoha AN7581 USB PHY driver by
 * Christian Marangi <ansuelsmth@gmail.com>
 */

#include <dt-bindings/phy/phy.h>
#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/math.h>
#include <linux/module.h>
#include <linux/nvmem-consumer.h>
#include <linux/phy/phy.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/regmap.h>

/* Frequency Meter registers (shared across ports) */
#define EN7528_USB_PHY_FMCR0			0x100
#define   EN7528_USB_PHY_MONCLK_SEL		GENMASK(27, 26)
#define   EN7528_USB_PHY_FREQDET_EN		BIT(24)
#define   EN7528_USB_PHY_CYCLECNT		GENMASK(23, 0)
#define EN7528_USB_PHY_FMMONR0			0x10c
#define EN7528_USB_PHY_FMMONR1			0x110
#define   EN7528_USB_PHY_FRCK_EN		BIT(8)

/* U2 PHY port bases */
#define EN7528_USB_PHY_U2_P0_BASE		0x300
#define EN7528_USB_PHY_U2_P1_BASE		0x1300
#define EN7528_USB_PHY_NUM_U2_PORTS		2

/* U2 PHY register offsets (relative to port base) */
#define EN7528_USB_PHY_ACR0			0x10
#define   EN7528_USB_PHY_HSTX_SRCAL_EN		BIT(23)
#define   EN7528_USB_PHY_HSTX_SRCTRL		GENMASK(18, 16)
#define EN7528_USB_PHY_ACR3			0x1c
#define EN7528_USB_PHY_ACR3_ENABLE		0xC0240000

/* U3 PHYA registers (base at +0xb00) */
#define EN7528_USB_PHY_U3_PHYA_REG11		0xb2c
#define   EN7528_USB_PHY_RX_IMPSEL		GENMASK(13, 12)

#define EN7528_USB_PHY_FM_DET_CYCLE_CNT		1024
#define EN7528_USB_PHY_REF_CK			20	/* MHz */
#define EN7528_USB_PHY_SR_COEF			28
#define EN7528_USB_PHY_SR_COEF_DIVISOR		1000
#define EN7528_USB_PHY_DEFAULT_SR		4

/* EN7580 has one U2/U3 pair and one frequency meter per controller. */
#define EN7580_USB_PHY_ACR1			0x304
#define   EN7580_USB_PHY_INTR_CAL		GENMASK(23, 19)
#define EN7580_USB_PHY_ACR5			0x314
#define   EN7580_USB_PHY_HSTX_SRCAL_EN		BIT(15)
#define   EN7580_USB_PHY_HSTX_SRCTRL		GENMASK(14, 12)
#define   EN7580_USB_PHY_HS_100U_U3_EN		BIT(11)
#define EN7580_USB_PHY_ACR6			0x318
#define   EN7580_USB_PHY_BC11_SW_EN		BIT(23)
#define EN7580_USB_PHY_U3_IMP_TX			0x910
#define EN7580_USB_PHY_U3_IMP_RX			0x914
#define   EN7580_USB_PHY_U3_IMP_SEL		GENMASK(28, 24)
#define EN7580_USB_PHY_U3_DEEMPH			0xa04
#define   EN7580_USB_PHY_FORCE_DEEMPH		BIT(22)
#define   EN7580_USB_PHY_DEEMPH_3P5DB		GENMASK(21, 16)
#define EN7580_USB_PHY_U3_PHYA_REG0		0xb00
#define   EN7580_USB_PHY_IEXT_INTR_CTRL		GENMASK(15, 10)
#define EN7580_USB_PHY_DEFAULT_SR		5

#define EN7528_USB_PHY_FREQDET_SLEEP		1000	/* 1ms */
#define EN7528_USB_PHY_FREQDET_TIMEOUT		(EN7528_USB_PHY_FREQDET_SLEEP * 10)

static const unsigned int en7528_u2_port_bases[EN7528_USB_PHY_NUM_U2_PORTS] = {
	EN7528_USB_PHY_U2_P0_BASE,
	EN7528_USB_PHY_U2_P1_BASE,
};

struct en7528_usb_phy_instance {
	struct phy *phy;
	u32 type;
};

struct en7528_usb_phy_soc_data {
	unsigned int num_u2_ports;
	unsigned int sr_reg;
	u32 sr_cal_en;
	u32 sr_ctrl;
	u32 default_sr;
	bool en7580;
};

struct en7580_usb_phy_trim {
	u32 u2_intr;
	u32 u3_intr;
	u32 tx_imp;
	u32 rx_imp;
};

enum en7528_usb_phy_type {
	EN7528_PHY_USB2,
	EN7528_PHY_USB3,

	EN7528_PHY_USB_MAX,
};

struct en7528_usb_phy_priv {
	struct device *dev;
	struct regmap *regmap;
	const struct en7528_usb_phy_soc_data *soc;
	struct en7580_usb_phy_trim trim;

	struct en7528_usb_phy_instance *phys[EN7528_PHY_USB_MAX];
};

static void en7528_usb_phy_slew_rate_calibration(struct en7528_usb_phy_priv *priv,
						 unsigned int port_id,
						 unsigned int port_base)
{
	const struct en7528_usb_phy_soc_data *soc = priv->soc;
	unsigned int acr0 = port_base + soc->sr_reg;
	u32 fm_out = 0;
	u32 srctrl;

	/* Enable HS TX SR calibration */
	regmap_set_bits(priv->regmap, acr0,
			soc->sr_cal_en);

	usleep_range(1000, 1500);

	/* Enable free run clock */
	regmap_set_bits(priv->regmap, EN7528_USB_PHY_FMMONR1,
			EN7528_USB_PHY_FRCK_EN);

	/* Only EN7528 shares a frequency meter between its U2 ports. */
	if (!soc->en7580)
		regmap_update_bits(priv->regmap, EN7528_USB_PHY_FMCR0,
				   EN7528_USB_PHY_MONCLK_SEL,
				   FIELD_PREP(EN7528_USB_PHY_MONCLK_SEL, port_id));

	/* Set cycle count */
	regmap_update_bits(priv->regmap, EN7528_USB_PHY_FMCR0,
			   EN7528_USB_PHY_CYCLECNT,
			   FIELD_PREP(EN7528_USB_PHY_CYCLECNT,
				      EN7528_USB_PHY_FM_DET_CYCLE_CNT));

	/* Enable frequency meter */
	regmap_set_bits(priv->regmap, EN7528_USB_PHY_FMCR0,
			EN7528_USB_PHY_FREQDET_EN);

	/* Timeout can happen and we will apply default value at the end */
	(void)regmap_read_poll_timeout(priv->regmap, EN7528_USB_PHY_FMMONR0,
				       fm_out, fm_out,
				       EN7528_USB_PHY_FREQDET_SLEEP,
				       EN7528_USB_PHY_FREQDET_TIMEOUT);

	/* Disable frequency meter */
	regmap_clear_bits(priv->regmap, EN7528_USB_PHY_FMCR0,
			  EN7528_USB_PHY_FREQDET_EN);

	/* Disable free run clock */
	regmap_clear_bits(priv->regmap, EN7528_USB_PHY_FMMONR1,
			  EN7528_USB_PHY_FRCK_EN);

	/* Disable HS TX SR calibration */
	regmap_clear_bits(priv->regmap, acr0,
			  soc->sr_cal_en);

	usleep_range(1000, 1500);

	if (!fm_out) {
		srctrl = soc->default_sr;
		dev_err(priv->dev, "port%u: frequency not detected, using default SR calibration.\n",
			port_id);
	} else {
		/* (1024 / FM_OUT) * REF_CK * SR_COEF */
		srctrl = EN7528_USB_PHY_REF_CK * EN7528_USB_PHY_SR_COEF;
		srctrl = (srctrl * EN7528_USB_PHY_FM_DET_CYCLE_CNT) / fm_out;
		srctrl = DIV_ROUND_CLOSEST(srctrl,
					   EN7528_USB_PHY_SR_COEF_DIVISOR);
		dev_dbg(priv->dev, "port%u: SR calibration applied: %x\n",
			port_id, srctrl);
	}

	regmap_update_bits(priv->regmap, acr0,
			   soc->sr_ctrl, field_prep(soc->sr_ctrl, srctrl));
}

static int en7580_usb_phy_init(struct en7528_usb_phy_priv *priv, u32 type)
{
	const struct en7580_usb_phy_trim *trim = &priv->trim;
	u32 intr;
	int ret;

	switch (type) {
	case PHY_TYPE_USB2:
		ret = regmap_clear_bits(priv->regmap, EN7580_USB_PHY_ACR6,
					EN7580_USB_PHY_BC11_SW_EN);
		if (ret)
			return ret;
		usleep_range(1000, 1500);

		ret = regmap_clear_bits(priv->regmap, EN7580_USB_PHY_ACR5,
					EN7580_USB_PHY_HS_100U_U3_EN);
		if (ret)
			return ret;
		usleep_range(1000, 1500);

		/* The SDK adds three to the programmed U2 factory trim. */
		intr = trim->u2_intr ? min(trim->u2_intr + 3, 31U) : 0x17;
		ret = regmap_update_bits(priv->regmap, EN7580_USB_PHY_ACR1,
					 EN7580_USB_PHY_INTR_CAL,
					 FIELD_PREP(EN7580_USB_PHY_INTR_CAL, intr));
		break;
	case PHY_TYPE_USB3:
		/* Force a 3.5 dB de-emphasis value of 18, as in the SDK. */
		ret = regmap_update_bits(priv->regmap, EN7580_USB_PHY_U3_DEEMPH,
					 EN7580_USB_PHY_FORCE_DEEMPH |
					 EN7580_USB_PHY_DEEMPH_3P5DB,
					 EN7580_USB_PHY_FORCE_DEEMPH |
					 FIELD_PREP(EN7580_USB_PHY_DEEMPH_3P5DB, 18));
		if (ret)
			return ret;
		usleep_range(1000, 1500);

		if (trim->u3_intr > 23) {
			ret = regmap_update_bits(priv->regmap,
						 EN7580_USB_PHY_U3_PHYA_REG0,
						 EN7580_USB_PHY_IEXT_INTR_CTRL,
						 FIELD_PREP(EN7580_USB_PHY_IEXT_INTR_CTRL,
							    trim->u3_intr));
			if (ret)
				return ret;
			usleep_range(1000, 1500);
		}

		if (trim->tx_imp) {
			ret = regmap_update_bits(priv->regmap, EN7580_USB_PHY_U3_IMP_TX,
						 EN7580_USB_PHY_U3_IMP_SEL,
						 FIELD_PREP(EN7580_USB_PHY_U3_IMP_SEL,
							    trim->tx_imp));
			if (ret)
				return ret;
			usleep_range(1000, 1500);
		}

		if (trim->rx_imp) {
			ret = regmap_update_bits(priv->regmap, EN7580_USB_PHY_U3_IMP_RX,
						 EN7580_USB_PHY_U3_IMP_SEL,
						 FIELD_PREP(EN7580_USB_PHY_U3_IMP_SEL,
							    trim->rx_imp));
			if (ret)
				return ret;
			usleep_range(1000, 1500);
		}
		return 0;
	default:
		return -EINVAL;
	}

	if (!ret)
		usleep_range(1000, 1500);

	return ret;
}

static int en7528_usb_phy_init(struct phy *phy)
{
	struct en7528_usb_phy_instance *instance = phy_get_drvdata(phy);
	struct en7528_usb_phy_priv *priv = dev_get_drvdata(phy->dev.parent);
	unsigned int i;

	if (priv->soc->en7580)
		return en7580_usb_phy_init(priv, instance->type);

	switch (instance->type) {
	case PHY_TYPE_USB2:
		/* Enable both U2 PHY ports before calibration */
		for (i = 0; i < EN7528_USB_PHY_NUM_U2_PORTS; i++)
			regmap_write(priv->regmap,
				     en7528_u2_port_bases[i] + EN7528_USB_PHY_ACR3,
				     EN7528_USB_PHY_ACR3_ENABLE);
		break;
	case PHY_TYPE_USB3:
		/* Combo PHY Rx R mean value too high, tune -5 Ohm */
		regmap_update_bits(priv->regmap,
				   EN7528_USB_PHY_U3_PHYA_REG11,
				   EN7528_USB_PHY_RX_IMPSEL,
				   FIELD_PREP(EN7528_USB_PHY_RX_IMPSEL, 0x1));
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int en7528_usb_phy_power_on(struct phy *phy)
{
	struct en7528_usb_phy_instance *instance = phy_get_drvdata(phy);
	struct en7528_usb_phy_priv *priv = dev_get_drvdata(phy->dev.parent);
	unsigned int i;

	if (instance->type != PHY_TYPE_USB2)
		return 0;

	/* Calibrate each U2 PHY controlled by this provider. */
	for (i = 0; i < priv->soc->num_u2_ports; i++)
		en7528_usb_phy_slew_rate_calibration(priv, i,
						     en7528_u2_port_bases[i]);

	return 0;
}

static const struct phy_ops en7528_usb_phy_ops = {
	.init		= en7528_usb_phy_init,
	.power_on	= en7528_usb_phy_power_on,
	.owner		= THIS_MODULE,
};

static struct phy *en7528_usb_phy_xlate(struct device *dev,
					const struct of_phandle_args *args)
{
	struct en7528_usb_phy_priv *priv = dev_get_drvdata(dev);
	struct en7528_usb_phy_instance *instance = NULL;
	unsigned int index, phy_type;

	if (args->args_count != 1) {
		dev_err(dev, "invalid number of cells in 'phy' property\n");
		return ERR_PTR(-EINVAL);
	}

	phy_type = args->args[0];
	if (!(phy_type == PHY_TYPE_USB2 || phy_type == PHY_TYPE_USB3)) {
		dev_err(dev, "unsupported device type: %d\n", phy_type);
		return ERR_PTR(-EINVAL);
	}

	for (index = 0; index < EN7528_PHY_USB_MAX; index++)
		if (priv->phys[index] &&
		    phy_type == priv->phys[index]->type) {
			instance = priv->phys[index];
			break;
		}

	if (!instance) {
		dev_err(dev, "failed to find appropriate phy\n");
		return ERR_PTR(-EINVAL);
	}

	return instance->phy;
}

static const struct regmap_config en7528_usb_phy_regmap_config = {
	.reg_bits = 32,
	.val_bits = 32,
	.reg_stride = 4,
	.max_register = EN7528_USB_PHY_U2_P1_BASE + EN7528_USB_PHY_ACR3,
};

static const struct regmap_config en7580_usb_phy_regmap_config = {
	.reg_bits = 32,
	.val_bits = 32,
	.reg_stride = 4,
	.max_register = EN7580_USB_PHY_U3_PHYA_REG0,
};

static int en7580_usb_phy_get_trim(struct en7528_usb_phy_priv *priv)
{
	struct device *dev = priv->dev;
	struct en7580_usb_phy_trim *trim = &priv->trim;
	int ret;

	if (!priv->soc->en7580 || !device_property_present(dev, "nvmem-cells"))
		return 0;

	ret = nvmem_cell_read_variable_le_u32(dev, "u2-intr", &trim->u2_intr);
	if (ret)
		return ret;
	ret = nvmem_cell_read_variable_le_u32(dev, "u3-intr", &trim->u3_intr);
	if (ret)
		return ret;
	ret = nvmem_cell_read_variable_le_u32(dev, "tx-imp", &trim->tx_imp);
	if (ret)
		return ret;

	return nvmem_cell_read_variable_le_u32(dev, "rx-imp", &trim->rx_imp);
}

static int en7528_usb_phy_probe(struct platform_device *pdev)
{
	struct phy_provider *phy_provider;
	const struct regmap_config *config;
	struct en7528_usb_phy_priv *priv;
	struct device *dev = &pdev->dev;
	unsigned int index;
	void __iomem *base;
	int ret;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->dev = dev;
	priv->soc = device_get_match_data(dev);
	if (!priv->soc)
		return -EINVAL;

	ret = en7580_usb_phy_get_trim(priv);
	if (ret)
		return dev_err_probe(dev, ret, "failed to read PHY calibration data\n");

	base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(base))
		return PTR_ERR(base);

	config = priv->soc->en7580 ? &en7580_usb_phy_regmap_config :
				   &en7528_usb_phy_regmap_config;

	priv->regmap = devm_regmap_init_mmio(dev, base, config);
	if (IS_ERR(priv->regmap))
		return PTR_ERR(priv->regmap);

	platform_set_drvdata(pdev, priv);

	for (index = 0; index < EN7528_PHY_USB_MAX; index++) {
		struct en7528_usb_phy_instance *instance;
		enum en7528_usb_phy_type phy_type;

		switch (index) {
		case EN7528_PHY_USB2:
			phy_type = PHY_TYPE_USB2;
			break;
		case EN7528_PHY_USB3:
			phy_type = PHY_TYPE_USB3;
			break;
		default:
			continue;
		}

		instance = devm_kzalloc(dev, sizeof(*instance), GFP_KERNEL);
		if (!instance)
			return -ENOMEM;

		instance->type = phy_type;
		priv->phys[index] = instance;

		instance->phy = devm_phy_create(dev, NULL, &en7528_usb_phy_ops);
		if (IS_ERR(instance->phy))
			return dev_err_probe(dev, PTR_ERR(instance->phy),
					     "failed to create phy\n");

		phy_set_drvdata(instance->phy, instance);
	}

	phy_provider = devm_of_phy_provider_register(dev,
						     en7528_usb_phy_xlate);

	return PTR_ERR_OR_ZERO(phy_provider);
}

static const struct en7528_usb_phy_soc_data en7528_usb_phy_data = {
	.num_u2_ports = EN7528_USB_PHY_NUM_U2_PORTS,
	.sr_reg = EN7528_USB_PHY_ACR0,
	.sr_cal_en = EN7528_USB_PHY_HSTX_SRCAL_EN,
	.sr_ctrl = EN7528_USB_PHY_HSTX_SRCTRL,
	.default_sr = EN7528_USB_PHY_DEFAULT_SR,
};

static const struct en7528_usb_phy_soc_data en7580_usb_phy_data = {
	.num_u2_ports = 1,
	.sr_reg = EN7580_USB_PHY_ACR5 - EN7528_USB_PHY_U2_P0_BASE,
	.sr_cal_en = EN7580_USB_PHY_HSTX_SRCAL_EN,
	.sr_ctrl = EN7580_USB_PHY_HSTX_SRCTRL,
	.default_sr = EN7580_USB_PHY_DEFAULT_SR,
	.en7580 = true,
};

static const struct of_device_id en7528_usb_phy_match[] = {
	{ .compatible = "econet,en751627-usb-phy", .data = &en7528_usb_phy_data },
	{ .compatible = "econet,en7528-usb-phy", .data = &en7528_usb_phy_data },
	{ .compatible = "econet,en7580-usb-phy", .data = &en7580_usb_phy_data },
	{ },
};
MODULE_DEVICE_TABLE(of, en7528_usb_phy_match);

static struct platform_driver en7528_usb_phy_driver = {
	.probe		= en7528_usb_phy_probe,
	.driver		= {
		.name	= "en7528-usb-phy",
		.of_match_table = en7528_usb_phy_match,
	},
};

module_platform_driver(en7528_usb_phy_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("EcoNet EN7528/EN7580 USB PHY driver");
