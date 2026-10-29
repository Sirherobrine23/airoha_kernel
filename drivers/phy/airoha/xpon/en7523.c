// SPDX-License-Identifier: GPL-2.0-only
/* EN7523 xPON PHY backend. */

#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regmap.h>
#include <linux/sizes.h>

#include "xpon.h"

#define EN7523_SCU_IOMUX_CTRL_3		0x218
#define EN7523_SCU_IOMUX_PON_EN		BIT(0)

static int
airoha_xpon_phy_configure_integrated_pma(struct airoha_xpon_phy *priv)
{
	u32 val;

	airoha_xpon_phy_write(priv, XPON_RX_CTRL0, 0x18001722);
	airoha_xpon_phy_rmw(priv, XPON_PHYSET10, XPON_PHYSET10_GPON,
			    priv->submode == AIROHA_XPON_PHY_SUBMODE_GPON ?
			     XPON_PHYSET10_GPON : 0);
	udelay(1);

	airoha_xpon_phy_write(priv, XPON_FREQ_CTRL, 0x54101801);
	airoha_xpon_phy_write(priv, XPON_CDR_CTRL, 0x00380013);
	airoha_xpon_phy_rmw(priv, XPON_SERDES_CTRL0, BIT(0), BIT(0));
	airoha_xpon_phy_rmw(priv, XPON_SERDES_CTRL19, GENMASK(15, 0),
			    priv->submode == AIROHA_XPON_PHY_SUBMODE_GPON ?
			     0x0b02 : 0x0502);
	airoha_xpon_phy_rmw(priv, XPON_SERDES_CTRL5, GENMASK(25, 16),
			    0x03f00000);
	airoha_xpon_phy_rmw(priv, XPON_PMA_CTRL13, BIT(0), BIT(0));
	airoha_xpon_phy_rmw(priv, XPON_PMA_CTRL12, BIT(16), BIT(16));
	airoha_xpon_phy_rmw(priv, XPON_PMA_CTRL0, BIT(28), 0);
	airoha_xpon_phy_write(priv, XPON_SERDES_CTRL18, 0x00000002);
	airoha_xpon_phy_rmw(priv, XPON_PMA_CTRL8, GENMASK(11, 0), 0x101);
	airoha_xpon_phy_rmw(priv, XPON_PMA_CTRL0, BIT(30), BIT(30));
	airoha_xpon_phy_rmw(priv, XPON_RX_MODE_CTRL, GENMASK(2, 0), 0x1);

	val = airoha_xpon_phy_read(priv, XPON_SERDES_RESET);
	airoha_xpon_phy_write(priv, XPON_SERDES_RESET,
			      val | XPON_SERDES_RESET_RX |
			       XPON_SERDES_RESET_CDR);
	udelay(1);
	airoha_xpon_phy_write(priv, XPON_SERDES_RESET,
			      (val | XPON_SERDES_RESET_CDR) &
			       ~XPON_SERDES_RESET_RX);
	mdelay(1);

	/*
	 * Program the integrated-PMA EN7571 values before resetting the xPON
	 * PLL and counters. The vendor EN7523/EN7528 sequence relies on these
	 * values being present when the reset is released. Resetting first can
	 * make the initial power-on miss PHY_READY.
	 */
	airoha_xpon_phy_write(priv, XPON_TDCSET2, XPON_TDCSET2_EN7571);
	airoha_xpon_phy_write(priv, XPON_GPON_DELIMITER_GUARD,
			      XPON_GPON_DELIMITER_DEFAULT);
	airoha_xpon_phy_write(priv, XPON_SETTING, XPON_SETTING_EN7571);

	/*
	 * The XX230v EN7571 path updates phy_xpon_trans_val to 0x14f after
	 * transceiver detection. Its bit 6 and bit 7 values are mirrored in
	 * the PMA RX-SD and SerDes burst-enable polarity controls below.
	 */
	airoha_xpon_phy_rmw(priv, XPON_PMA_CTRL0, BIT(29), BIT(29));
	airoha_xpon_phy_rmw(priv, XPON_SERDES_CTRL0, BIT(24), 0);

	/* Reset the PLL and counters only after all mode-dependent values have
	 * been programmed, matching the vendor bring-up order.
	 */
	val = airoha_xpon_phy_read(priv, XPON_PHYSET3);
	airoha_xpon_phy_write(priv, XPON_PHYSET3,
			      val | XPON_PHYSET3_PLL_RST |
			       XPON_PHYSET3_COUNTER_RST);
	mdelay(1);
	airoha_xpon_phy_write(priv, XPON_PHYSET3, val & ~BIT(2));
	mdelay(1);

	val = airoha_xpon_phy_read(priv, XPON_INT_STATUS);
	airoha_xpon_phy_write(priv, XPON_INT_STATUS_CLR, val);
	val = airoha_xpon_phy_read(priv, XPON_PMA_INT_STATUS);
	airoha_xpon_phy_write(priv, XPON_PMA_INT_STATUS_CLR, val);
	airoha_xpon_phy_write(priv, XPON_INT_ENABLE, 0);
	airoha_xpon_phy_write(priv, XPON_PMA_INT_ENABLE, 0);

	return 0;
}

static int airoha_en7523_xpon_phy_configure(struct airoha_xpon_phy *priv)
{
	u32 mode;
	int ret;

	mode = priv->submode == AIROHA_XPON_PHY_SUBMODE_GPON ?
		EN7523_SCU_WAN_MODE_GPON : EN7523_SCU_WAN_MODE_EPON;

	ret = regmap_update_bits(priv->scu, EN7523_SCU_WAN_CONF,
				 EN7523_SCU_WAN_MODE_MASK, mode);
	if (ret)
		return dev_err_probe(priv->dev, ret,
				     "failed to select xPON WAN mode\n");

	ret = regmap_update_bits(priv->scu, EN7523_SCU_IOMUX_CTRL_3,
				 EN7523_SCU_IOMUX_PON_EN,
				 EN7523_SCU_IOMUX_PON_EN);
	if (ret)
		return dev_err_probe(priv->dev, ret,
				     "failed to enable xPON I/O mux\n");

	return airoha_xpon_phy_configure_integrated_pma(priv);
}

static void en7523_xpon_phy_complete_ready(struct airoha_xpon_phy *priv)
{
	/* EN7523 phy_ready_handler() retriggers PMA_CTRL8 bit 0. */
	airoha_xpon_phy_rmw(priv, XPON_PMA_CTRL8, BIT(0), 0);
	udelay(1);
	airoha_xpon_phy_rmw(priv, XPON_PMA_CTRL8, BIT(0), BIT(0));
	mdelay(1);
}

static const struct airoha_xpon_phy_soc_data airoha_en7523_xpon_phy_data = {
	.name = "EN7523",
	.min_size = 0x480c,
	.gpon_bit_delay_reg = XPON_GPON_TX_BIT_DELAY,
	.gpon_bit_delay_mask = XPON_GPON_TX_BIT_DELAY_MASK,
	.has_integrated_pma = true,
	.complete_ready = en7523_xpon_phy_complete_ready,
	.configure = airoha_en7523_xpon_phy_configure,
};

static const struct of_device_id en7523_xpon_phy_of_match[] = {
	{ .compatible = "airoha,en7523-xpon-phy", .data = &airoha_en7523_xpon_phy_data },
	{ }
};
MODULE_DEVICE_TABLE(of, en7523_xpon_phy_of_match);

static struct platform_driver en7523_xpon_phy_driver = {
	.probe = airoha_xpon_phy_probe,
	.shutdown = airoha_xpon_phy_shutdown,
	.driver = {
		.name = "airoha-en7523-xpon-phy",
		.of_match_table = en7523_xpon_phy_of_match,
	},
};
module_platform_driver(en7523_xpon_phy_driver);

MODULE_DESCRIPTION("Airoha EN7523 xPON PHY driver");
MODULE_AUTHOR("Matheus Sampaio Queiroga <srherobrine20@gmail.com>");
MODULE_LICENSE("GPL");
