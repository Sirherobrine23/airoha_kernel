// SPDX-License-Identifier: GPL-2.0-only
/* EN751627/EN7528 xPON PHY backend. */

#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regmap.h>
#include <linux/sizes.h>

#include "xpon.h"

#define EN7528_SCU_REG_284		0x284
#define EN7528_SCU_REG_284_KEEP_REG580	BIT(9)

static int airoha_en7528_xpon_phy_configure(struct airoha_xpon_phy *priv)
{
	u32 mode, val;
	int ret;

	mode = priv->submode == AIROHA_XPON_PHY_SUBMODE_GPON ?
		EN7523_SCU_WAN_MODE_GPON : EN7523_SCU_WAN_MODE_EPON;

	ret = regmap_update_bits(priv->scu, EN7523_SCU_WAN_CONF,
				 EN7528_SCU_WAN_MODE_MASK, mode);
	if (ret)
		return dev_err_probe(priv->dev, ret,
				     "failed to select EN7528 xPON WAN mode\n");

	/*
	 * The XC220 EN7528 vendor PHY is a 0x600-byte digital GPON/EPON
	 * block.  Unlike EN7523, it does not program the PMA/SerDes window at
	 * 0x3000-0x4808.  Its bring-up matches the older EcoNet digital PHY:
	 * release the SCU disables, initialize counters, select PHYSET10 and
	 * pulse the PHYSET3 PLL/counter reset.
	 *
	 * The PON pads themselves are owned by the EN7528 pinctrl default
	 * state, so do not write the EN7523 NP-SCU IOMUX register here.
	 */
	airoha_xpon_phy_rmw(priv, XPON_PHYFWREADY, XPON_PHYFWREADY_READY, 0);
	airoha_xpon_phy_rmw(priv, XPON_PHYSET3, BIT(2), 0);

	ret = regmap_clear_bits(priv->scu, ECONET_SCU_PHY_CTRL1,
				ECONET_SCU_PHY_CTRL1_DIS);
	if (ret)
		return dev_err_probe(priv->dev, ret,
				     "failed to enable EN7528 xPON PHY control 1\n");

	ret = regmap_clear_bits(priv->scu, ECONET_SCU_PHY_CTRL0,
				ECONET_SCU_PHY_CTRL0_DIS);
	if (ret)
		return dev_err_probe(priv->dev, ret,
				     "failed to enable EN7528 xPON PHY control 0\n");

	econet_xpon_phy_counter_init(priv);
	airoha_xpon_phy_write(priv, XPON_GPON_DELIMITER_GUARD,
			      XPON_GPON_DELIMITER_DEFAULT);
	econet_xpon_phy_counter_clear(priv, ECONET_XPON_COUNTER_CLEAR_ALL);
	airoha_xpon_phy_write(priv, XPON_TDCSET2, ECONET_XPON_TDCSET2);

	/*
	 * The XC220 vendor phy_dev_init() checks NP-SCU + 0x284 bit 9 and,
	 * when it is clear, writes zero to the otherwise-undocumented xPON
	 * register at 0x580.  The tested XC220-G3v reads 0x01038500 from
	 * SCU + 0x284, so it takes this branch in the stock firmware.
	 */
	ret = regmap_read(priv->scu, EN7528_SCU_REG_284, &val);
	if (ret)
		return dev_err_probe(priv->dev, ret,
				     "failed to read EN7528 SCU register 0x284\n");

	if (!(val & EN7528_SCU_REG_284_KEEP_REG580)) {
		airoha_xpon_phy_write(priv, EN7528_XPON_REG_580, 0);
		dev_info(priv->dev,
			 "EN7528 vendor reg580 init: scu284=%#010x reg580=%#010x\n",
			 val,
			 airoha_xpon_phy_read(priv, EN7528_XPON_REG_580));
	}

	/*
	 * The XC220 uses an EN7571.  Its vendor transceiver model programs
	 * XPON_SETTING to 0x10f.  Preserve board-described polarity bits on
	 * top of that value so another EN7528 board can override them in DT.
	 */
	val = ECONET_XPON_SETTING_EN757X;
	val &= ~XPON_SETTING_INV_MASK;
	val |= priv->trans_invert;
	airoha_xpon_phy_write(priv, XPON_SETTING, val);

	/*
	 * phy_mode_config(): quiesce EPON, select GPON/EPON first, then pulse
	 * the PLL/counter reset.  The XC220 stock PHY uses this exact ordering;
	 * selecting PHYSET10 while reset is already asserted is not equivalent.
	 */
	airoha_xpon_phy_rmw(priv, XPON_PHYSET3, BIT(5), 0);
	airoha_xpon_phy_rmw(priv, XPON_PHYSET10, XPON_PHYSET10_GPON,
			    priv->submode == AIROHA_XPON_PHY_SUBMODE_GPON ?
			     XPON_PHYSET10_GPON : 0);

	val = airoha_xpon_phy_read(priv, XPON_PHYSET3);
	airoha_xpon_phy_write(priv, XPON_PHYSET3,
			      val | XPON_PHYSET3_PLL_RST |
			       XPON_PHYSET3_COUNTER_RST);
	mdelay(1);
	airoha_xpon_phy_write(priv, XPON_PHYSET3, val);
	mdelay(1);

	if (priv->submode == AIROHA_XPON_PHY_SUBMODE_EPON)
		airoha_xpon_phy_rmw(priv, XPON_PHYSET3, BIT(5), BIT(5));

	val = airoha_xpon_phy_read(priv, XPON_INT_STATUS);
	airoha_xpon_phy_write(priv, XPON_INT_STATUS_CLR, val);
	airoha_xpon_phy_write(priv, XPON_INT_ENABLE, 0);

	airoha_xpon_phy_rmw(priv, XPON_PHYFWREADY, XPON_PHYFWREADY_READY,
			    XPON_PHYFWREADY_READY);

	return 0;
}
EXPORT_SYMBOL_GPL(airoha_en7528_xpon_phy_configure);

static const struct airoha_xpon_phy_soc_data airoha_en7528_xpon_phy_data = {
	.name = "EN7528",
	.min_size = ECONET_XPON_PHY_MIN_SIZE,
	.gpon_bit_delay_reg = XPON_PHYSET5,
	.gpon_bit_delay_mask = XPON_PHYSET5_BIT_DELAY_MASK,
	.gpon_bit_delay_enable = XPON_PHYSET5_BIT_DELAY_EN,
	.manages_fw_ready = true,
	.configure = airoha_en7528_xpon_phy_configure,
};

static const struct of_device_id en7528_xpon_phy_of_match[] = {
	{ .compatible = "airoha,en751627-xpon-phy", .data = &airoha_en7528_xpon_phy_data },
	{ .compatible = "airoha,en7528-xpon-phy", .data = &airoha_en7528_xpon_phy_data },
	{ }
};
MODULE_DEVICE_TABLE(of, en7528_xpon_phy_of_match);

static struct platform_driver en7528_xpon_phy_driver = {
	.probe = airoha_xpon_phy_probe,
	.shutdown = airoha_xpon_phy_shutdown,
	.driver = {
		.name = "airoha-en7528-xpon-phy",
		.of_match_table = en7528_xpon_phy_of_match,
	},
};
module_platform_driver(en7528_xpon_phy_driver);

MODULE_DESCRIPTION("Airoha EN7528 xPON PHY driver");
MODULE_AUTHOR("Matheus Sampaio Queiroga <srherobrine20@gmail.com>");
MODULE_LICENSE("GPL");
