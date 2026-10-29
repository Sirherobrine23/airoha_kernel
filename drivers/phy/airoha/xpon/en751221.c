// SPDX-License-Identifier: GPL-2.0-only
/* EN751221 xPON PHY backend. */

#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regmap.h>
#include <linux/sizes.h>

#include "xpon.h"

static int econet_en751221_xpon_phy_configure(struct airoha_xpon_phy *priv)
{
	u32 val;
	int ret;

	/*
	 * PON pad routing belongs to pinctrl.  In particular GPIO16 is the
	 * board-level TX_DISABLE line on the EN751221 reference design while
	 * GPIO17..20 remain owned by the PON hardware.  Writing the global
	 * PON_MODE bit here races the GPIO consumer and cannot describe that
	 * split ownership.
	 *
	 * Clear FW_READY before reconfiguring, matching xpon_phy_stop().
	 */
	airoha_xpon_phy_rmw(priv, XPON_PHYFWREADY, XPON_PHYFWREADY_READY, 0);

	/* Preserve the vendor phy_dev_init() ordering around PHYSET3[2]. */
	airoha_xpon_phy_rmw(priv, XPON_PHYSET3, BIT(2), 0);

	ret = regmap_clear_bits(priv->scu, ECONET_SCU_PHY_CTRL1,
				ECONET_SCU_PHY_CTRL1_DIS);
	if (ret)
		return dev_err_probe(priv->dev, ret,
				     "failed to enable EN751221 xPON PHY control 1\n");

	ret = regmap_clear_bits(priv->scu, ECONET_SCU_PHY_CTRL0,
				ECONET_SCU_PHY_CTRL0_DIS);
	if (ret)
		return dev_err_probe(priv->dev, ret,
				     "failed to enable EN751221 xPON PHY control 0\n");

	/*
	 * Complete the counter side of vendor phy_dev_init().  This was missing
	 * from the initial EN751221 port and left 0x0230 disabled.
	 */
	econet_xpon_phy_counter_init(priv);
	airoha_xpon_phy_write(priv, XPON_GPON_DELIMITER_GUARD,
			      XPON_GPON_DELIMITER_DEFAULT);
	econet_xpon_phy_counter_clear(priv,
				      ECONET_XPON_COUNTER_CLEAR_ALL);

	/*
	 * TCSUPPORT_CPU_EN7521 overrides the vendor TDC default with 0x2d.
	 * This is part of the upstream burst timing path and must be applied
	 * before the mode reset is released.
	 */
	airoha_xpon_phy_write(priv, XPON_TDCSET2, ECONET_XPON_TDCSET2);

	/*
	 * EN751221 phy_mode_config(): bit 5 is cleared while switching mode,
	 * PHYSET10[31] selects GPON, and the PLL/counter reset is pulsed after
	 * the mode change.  EPON sets PHYSET3[5] again after the reset pulse.
	 */
	airoha_xpon_phy_rmw(priv, XPON_PHYSET3, BIT(5), 0);
	airoha_xpon_phy_rmw(priv, XPON_PHYSET10, XPON_PHYSET10_GPON,
			    priv->submode == AIROHA_XPON_PHY_SUBMODE_GPON ?
			     XPON_PHYSET10_GPON : 0);

	/*
	 * EN7570 and EN7571 both program XPON_SETTING to 0x10f in the vendor
	 * transceiver-model setup.  Program the complete base value instead of
	 * relying on bootloader/reset residue, then overlay the board-specific
	 * polarity bits from DT.
	 */
	val = ECONET_XPON_SETTING_EN757X;
	val &= ~XPON_SETTING_INV_MASK;
	val |= priv->trans_invert;
	airoha_xpon_phy_write(priv, XPON_SETTING, val);

	val = airoha_xpon_phy_read(priv, XPON_PHYSET3);
	airoha_xpon_phy_write(priv, XPON_PHYSET3,
			      val | XPON_PHYSET3_PLL_RST |
			       XPON_PHYSET3_COUNTER_RST);
	mdelay(1);
	airoha_xpon_phy_write(priv, XPON_PHYSET3, val);

	if (priv->submode == AIROHA_XPON_PHY_SUBMODE_EPON)
		airoha_xpon_phy_rmw(priv, XPON_PHYSET3, BIT(5), BIT(5));

	/*
	 * The vendor enables PHY interrupts here, but this driver monitors LOS
	 * and PHY_READY by delayed work.  Keep interrupts masked until an IRQ
	 * consumer is added instead of enabling an unhandled source.
	 *
	 * Only the pin-convention bits of XPON_SETTING are written on EN751221;
	 * the vendor derives the rest from the optical transceiver model.
	 */
	val = airoha_xpon_phy_read(priv, XPON_INT_STATUS);
	airoha_xpon_phy_write(priv, XPON_INT_STATUS_CLR, val);
	airoha_xpon_phy_write(priv, XPON_INT_ENABLE, 0);

	/*
	 * The vendor exposes phy_fw_ready(1) as a distinct xPON PHY command.
	 * Linux owns the whole bring-up sequence, so assert the software-ready
	 * bit only after the mode, reset and counter programming is complete.
	 */
	airoha_xpon_phy_rmw(priv, XPON_PHYFWREADY, XPON_PHYFWREADY_READY,
			    XPON_PHYFWREADY_READY);

	return 0;
}

static const struct airoha_xpon_phy_soc_data econet_en751221_xpon_phy_data = {
	.name = "EN751221",
	.has_gpon_bip = true,
	.min_size = ECONET_XPON_PHY_MIN_SIZE,
	.gpon_bit_delay_reg = XPON_PHYSET5,
	.gpon_bit_delay_mask = XPON_PHYSET5_BIT_DELAY_MASK,
	.gpon_bit_delay_enable = XPON_PHYSET5_BIT_DELAY_EN,
	.manages_fw_ready = true,
	.configure = econet_en751221_xpon_phy_configure,
};

static const struct of_device_id en751221_xpon_phy_of_match[] = {
	{ .compatible = "econet,en751221-xpon-phy", .data = &econet_en751221_xpon_phy_data },
	{ }
};
MODULE_DEVICE_TABLE(of, en751221_xpon_phy_of_match);

static struct platform_driver en751221_xpon_phy_driver = {
	.probe = airoha_xpon_phy_probe,
	.shutdown = airoha_xpon_phy_shutdown,
	.driver = {
		.name = "airoha-en751221-xpon-phy",
		.of_match_table = en751221_xpon_phy_of_match,
	},
};
module_platform_driver(en751221_xpon_phy_driver);

MODULE_DESCRIPTION("Airoha EN751221 xPON PHY driver");
MODULE_AUTHOR("Matheus Sampaio Queiroga <srherobrine20@gmail.com>");
MODULE_LICENSE("GPL");
