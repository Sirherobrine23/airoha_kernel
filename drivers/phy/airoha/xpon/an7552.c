// SPDX-License-Identifier: GPL-2.0-only
/* AN7552 xPON PHY backend. */

#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regmap.h>
#include <linux/sizes.h>
#include <linux/slab.h>

#include "xpon.h"

#define AN7552_XPON_PHY_MIN_SIZE	0x480c
#define AN7552_SCU_SSTR			0x09c
#define AN7552_SCU_PON_SEL		GENMASK(10, 9)
#define AN7552_CHIP_SCU_XTAL		0x258
#define AN7552_CHIP_SCU_XTAL_ALT	BIT(19)
#define AN7552_LCPLL_PW_CTRL0		0x4100
#define AN7552_SERDES_CTRL0		0x4200
#define AN7552_SERDES_CTRL1		0x4204
#define AN7552_SERDES_CTRL16		0x4248
#define AN7552_SERDES_CTRL17		0x424c
#define AN7552_TX_CTRL2			0x4344
#define AN7552_LCPLL_TDC_PW		0x4110
#define AN7552_LCPLL_TDC_STATUS		0x4150
#define AN7552_LCPLL_NCPO		0x415c
#define AN7552_PLL_RESET_GATE		0x417c
#define AN7552_PLL_RESET_GATE_EN	BIT(16)
#define AN7552_PON_SYS_CTRL1		0x4304
#define AN7552_PON_TX_RESET_N		BIT(8)

#define AN7552_PHY_DELAY 0xffff

struct an7552_xpon_phy {
	bool needs_kband;
	bool ever_ready;
	u8 recovery_attempts;
};

struct an7552_phy_step {
	u16 reg;
	u32 keep;
	u32 gpon;
	u32 epon;
};

static const struct an7552_phy_step an7552_phy_pll_seq[] = {
	{ 0x0124, 0x7fffffff, 0x80000000, 0x00000000 },
	{ AN7552_PHY_DELAY, 0, 1, 1 },
	{ 0x3008, 0x0000ffff, 0x02400000, 0x02400000 },
	{ 0x4530, 0x0000ffff, 0x00380000, 0x00380000 },
	{ 0x4500, 0xffffefff, 0x00000000, 0x00000000 },
	{ 0x4600, 0xff0fffff, 0x07500000, 0x07500000 },
	{ 0x464c, 0xfffffff0, 0x00000004, 0x00000004 },
	{ 0x4604, 0xffffffcf, 0x00000020, 0x00000020 },
	{ 0x3010, 0xfff00fff, 0x00010000, 0x00010000 },
	{ 0x4610, 0xfffffe00, 0x00000031, 0x00000032 },
	{ 0x4600, 0xffffcfff, 0x00000000, 0x00000000 },
	{ 0x3014, 0xffffffff, 0x01000000, 0x01000000 },
	{ 0x4130, 0x80000000, 0x63886595, 0x64000000 },
	{ 0x4148, 0x80000000, 0x63886595, 0x63886595 },
	{ 0x414c, 0x80000000, 0x64000000, 0x64000000 },
	{ 0x413c, 0xfeffffff, 0x00000000, 0x00000000 },
	{ 0x4134, 0xffffffff, 0x00000100, 0x00000100 },
	{ AN7552_PHY_DELAY, 0, 1, 1 },
	{ 0x413c, 0xffffffff, 0x01000000, 0x01000000 },
	{ 0x412c, 0xfffffffe, 0x00000001, 0x00000000 },
	{ 0x4320, 0xfffffffc, 0x00000000, 0x00000000 },
	{ 0x4208, 0xfffffeff, 0x00000000, 0x00000100 },
	{ 0x3018, 0xffff0000, 0x0000110c, 0x00001100 },
	{ 0x4128, 0xfffffff8, 0x00000004, 0x00000004 },
	{ 0x4324, 0xffffe0e0, 0x00ff100c, 0x00ff100c },
	{ 0x4328, 0xffffe0e0, 0x00001e1e, 0x00001e1e },
	{ 0x432c, 0xfff0e0e0, 0x000c0808, 0x00080810 },
	{ 0x4330, 0xffffe0e0, 0x00001e1e, 0x00001e1e },
	{ 0x4334, 0xfff0e0e0, 0x00030204, 0x00020204 },
	{ 0x4338, 0xffffe0e0, 0x00001e1e, 0x00001e1e },
	{ 0x4608, 0xffffffff, 0x02000000, 0x02000000 },
	{ 0x3024, 0xf7ffffff, 0x00000000, 0x00000000 },
};

static const struct an7552_phy_step an7552_phy_rx_seq[] = {
	{ 0x4200, 0xffffffff, 0x00000001, 0x00000001 },
	{ AN7552_PHY_DELAY, 0, 2000, 2000 },
	{ 0x4364, 0xfffff7ff, 0x00000000, 0x00000000 },
	{ 0x436c, 0xf000ffff, 0x06180000, 0x06180000 },
	{ 0x4214, 0x0000ffff, 0x03f00000, 0x03f00000 },
};

static void an7552_xpon_phy_apply(struct airoha_xpon_phy *priv,
				  const struct an7552_phy_step *seq, size_t n)
{
	bool gpon = priv->submode == AIROHA_XPON_PHY_SUBMODE_GPON;
	size_t i;

	for (i = 0; i < n; i++) {
		u32 val = gpon ? seq[i].gpon : seq[i].epon;

		if (seq[i].reg == AN7552_PHY_DELAY) {
			if (val < 1000)
				udelay(val);
			else
				usleep_range(val, val + 100);
			continue;
		}
		val |= airoha_xpon_phy_read(priv, seq[i].reg) & seq[i].keep;
		airoha_xpon_phy_write(priv, seq[i].reg, val);
	}
}

static int airoha_an7552_xpon_phy_configure(struct airoha_xpon_phy *priv)
{
	struct an7552_xpon_phy *soc = priv->soc_priv;
	u32 val, mode;
	int ret;

	airoha_xpon_phy_rmw(priv, XPON_PHYFWREADY, XPON_PHYFWREADY_READY, 0);
	soc->ever_ready = false;
	soc->recovery_attempts = 0;
	mode = priv->submode == AIROHA_XPON_PHY_SUBMODE_GPON ?
		EN7523_SCU_WAN_MODE_GPON : EN7523_SCU_WAN_MODE_EPON;
	ret = regmap_update_bits(priv->scu, EN7523_SCU_WAN_CONF,
				 EN7523_SCU_WAN_MODE_MASK, mode);
	if (ret)
		return ret;
	udelay(1);
	/* AN7552 PON pads use chip SCU 0x21c, owned by default pinctrl. */

	/* Native Mode_Config_7552(), not the EN7523 PMA sequence. */
	an7552_xpon_phy_apply(priv, an7552_phy_pll_seq,
			      ARRAY_SIZE(an7552_phy_pll_seq));
	if (priv->submode == AIROHA_XPON_PHY_SUBMODE_GPON)
		airoha_xpon_phy_rmw(priv, 0x4618, BIT(23), BIT(23));
	an7552_xpon_phy_apply(priv, an7552_phy_rx_seq,
			      ARRAY_SIZE(an7552_phy_rx_seq));
	airoha_xpon_phy_rmw(priv, AN7552_LCPLL_PW_CTRL0, BIT(29),
			    priv->trans_invert & XPON_SETTING_RX_SD_INV ?
			    BIT(29) : 0);
	airoha_xpon_phy_rmw(priv, AN7552_SERDES_CTRL1, GENMASK(9, 8),
			    GENMASK(9, 8));
	udelay(1);
	airoha_xpon_phy_rmw(priv, AN7552_SERDES_CTRL1, BIT(9), 0);
	udelay(1);
	airoha_xpon_phy_rmw(priv, 0x4364, BIT(10), 0);
	airoha_xpon_phy_rmw(priv, 0x436c, GENMASK(11, 0), 0x618);
	airoha_xpon_phy_rmw(priv, 0x4228, BIT(8), 0);
	/* CTRL17[31:16] is reserved; retain it instead of copying CTRL10. */
	airoha_xpon_phy_rmw(priv, AN7552_SERDES_CTRL17, GENMASK(15, 0), 0x0b02);
	airoha_xpon_phy_write(priv, AN7552_SERDES_CTRL16, 2);
	airoha_xpon_phy_rmw(priv, AN7552_LCPLL_PW_CTRL0, BIT(30), BIT(30));
	airoha_xpon_phy_rmw(priv, AN7552_TX_CTRL2, GENMASK(2, 0), 1);
	airoha_xpon_phy_rmw(priv, AN7552_SERDES_CTRL0, BIT(24),
			    priv->trans_invert & XPON_SETTING_BURST_EN_INV ?
			    BIT(24) : 0);
	airoha_xpon_phy_rmw(priv, 0x4134, BIT(8), 0);
	airoha_xpon_phy_rmw(priv, AN7552_LCPLL_TDC_PW, BIT(0), BIT(0));
	airoha_xpon_phy_write(priv, 0x4754, 0xff);
	airoha_xpon_phy_write(priv, 0x4740, 0x7001);
	airoha_xpon_phy_rmw(priv, 0x4740, BIT(8), BIT(8));
	airoha_xpon_phy_write(priv, 0x473c, 1);
	airoha_xpon_phy_rmw(priv, AN7552_PLL_RESET_GATE,
			    AN7552_PLL_RESET_GATE_EN, AN7552_PLL_RESET_GATE_EN);

	/* phy_dev_init(): release digital controls and initialize counters. */
	airoha_xpon_phy_rmw(priv, XPON_PHYSET3, BIT(2), 0);
	ret = regmap_clear_bits(priv->scu, ECONET_SCU_PHY_CTRL1,
				ECONET_SCU_PHY_CTRL1_DIS);
	if (ret)
		return ret;
	ret = regmap_clear_bits(priv->scu, ECONET_SCU_PHY_CTRL0,
				ECONET_SCU_PHY_CTRL0_DIS);
	if (ret)
		return ret;
	econet_xpon_phy_counter_init(priv);
	airoha_xpon_phy_write(priv, XPON_GPON_DELIMITER_GUARD,
			      XPON_GPON_DELIMITER_DEFAULT);
	econet_xpon_phy_counter_clear(priv, ECONET_XPON_COUNTER_CLEAR_ALL);
	val = ECONET_XPON_SETTING_EN757X & ~XPON_SETTING_INV_MASK;
	airoha_xpon_phy_write(priv, XPON_SETTING, val | priv->trans_invert);

	/* Poll PHY_READY/LOS; do not enable interrupt sources without a handler. */
	airoha_xpon_phy_write(priv, XPON_INT_ENABLE, 0);
	airoha_xpon_phy_write(priv, XPON_PMA_INT_ENABLE, 0);
	val = airoha_xpon_phy_read(priv, XPON_INT_STATUS);
	airoha_xpon_phy_write(priv, XPON_INT_STATUS_CLR, val);
	val = airoha_xpon_phy_read(priv, XPON_PMA_INT_STATUS);
	airoha_xpon_phy_write(priv, XPON_PMA_INT_STATUS_CLR, val);
	airoha_xpon_phy_rmw(priv, XPON_PHYFWREADY, XPON_PHYFWREADY_READY,
			    XPON_PHYFWREADY_READY);
	return 0;
}

static void an7552_xpon_phy_complete_ready(struct airoha_xpon_phy *priv)
{
	struct an7552_xpon_phy *soc = priv->soc_priv;
	u32 val = 0;
	int i;

	/* Native phy_ready_handler(): settle TDC, then release the TX reset. */
	for (i = 0; i < 7; i++) {
		airoha_xpon_phy_rmw(priv, AN7552_LCPLL_TDC_PW, BIT(0), 0);
		udelay(1);
		airoha_xpon_phy_rmw(priv, AN7552_LCPLL_TDC_PW, BIT(0), BIT(0));
		usleep_range(1000, 1100);
		val = airoha_xpon_phy_read(priv, AN7552_LCPLL_TDC_STATUS);
		if ((val & GENMASK(3, 0)) > 10)
			break;
	}
	if (i == 7)
		dev_warn(priv->dev, "AN7552 TDC did not settle: %#x\n", val);
	usleep_range(3000, 3100);
	airoha_xpon_phy_rmw(priv, AN7552_PON_SYS_CTRL1,
			    AN7552_PON_TX_RESET_N, 0);
	udelay(1);
	airoha_xpon_phy_rmw(priv, AN7552_PON_SYS_CTRL1,
			    AN7552_PON_TX_RESET_N, AN7552_PON_TX_RESET_N);
	usleep_range(1000, 1100);
	soc->ever_ready = true;
	soc->recovery_attempts = 0;
}

static void an7552_xpon_phy_kband(struct airoha_xpon_phy *priv)
{
	u32 ncpo, target;
	int i;

	target = priv->submode == AIROHA_XPON_PHY_SUBMODE_GPON ?
		0x63886595 : 0x64000000;
	for (i = 0; i < 9; i++) {
		usleep_range(1000, 1100);
		ncpo = airoha_xpon_phy_read(priv, AN7552_LCPLL_NCPO);
		if (ncpo > target - 0x10000 && ncpo < target + 0x10000)
			break;
	}
	airoha_xpon_phy_rmw(priv, 0x4614, BIT(27), BIT(27));
	udelay(1);
	airoha_xpon_phy_rmw(priv, 0x4620, BIT(21), 0);
	udelay(1);
	airoha_xpon_phy_rmw(priv, 0x4620, BIT(21), BIT(21));
	usleep_range(9000, 9100);
	airoha_xpon_phy_rmw(priv, 0x4620, BIT(21), 0);
	udelay(1);
	airoha_xpon_phy_rmw(priv, 0x4614, BIT(27), 0);
	usleep_range(5000, 5100);
	airoha_xpon_phy_rmw(priv, 0x4614, BIT(5), BIT(5));
	udelay(1);
	airoha_xpon_phy_rmw(priv, 0x4614, BIT(6), BIT(6));
	udelay(1);
	airoha_xpon_phy_rmw(priv, 0x461c, BIT(26), BIT(26));
	udelay(1);
	airoha_xpon_phy_rmw(priv, 0x461c, BIT(27), BIT(27));
	usleep_range(5000, 5100);
	airoha_xpon_phy_rmw(priv, 0x461c, BIT(27), 0);
	usleep_range(1000, 1100);
	airoha_xpon_phy_rmw(priv, 0x461c, BIT(26), 0);
	airoha_xpon_phy_rmw(priv, AN7552_PON_SYS_CTRL1, GENMASK(27, 26), 0);
	udelay(1);
	airoha_xpon_phy_rmw(priv, AN7552_PON_SYS_CTRL1, GENMASK(27, 26),
			    GENMASK(27, 26));
}

static void an7552_xpon_phy_reset_rx(struct airoha_xpon_phy *priv)
{
	struct an7552_xpon_phy *soc = priv->soc_priv;
	u32 val;

	/* xPON_Pll_Reset() gates the AN7552 PLL reset with 0x417c[16]. */
	airoha_xpon_phy_rmw(priv, AN7552_PLL_RESET_GATE,
			    AN7552_PLL_RESET_GATE_EN, 0);
	val = airoha_xpon_phy_read(priv, XPON_PHYSET3);
	airoha_xpon_phy_write(priv, XPON_PHYSET3,
			      val | XPON_PHYSET3_PLL_RST | XPON_PHYSET3_COUNTER_RST);
	usleep_range(1000, 1100);
	airoha_xpon_phy_write(priv, XPON_PHYSET3, val);
	airoha_xpon_phy_rmw(priv, AN7552_PLL_RESET_GATE,
			    AN7552_PLL_RESET_GATE_EN, AN7552_PLL_RESET_GATE_EN);

	/* The stock recovery recalibrates immediately before the first READY,
	 * then after ten unsuccessful polls. Alternate XTAL needs no K-band.
	 */
	if (soc->needs_kband &&
	    (!soc->ever_ready || ++soc->recovery_attempts >= 10)) {
		soc->recovery_attempts = 0;
		an7552_xpon_phy_kband(priv);
	}
}

static int an7552_xpon_phy_pre_init(struct airoha_xpon_phy *priv)
{
	/* Select optical PON before releasing reset on the shared SerDes. */
	return regmap_clear_bits(priv->scu, AN7552_SCU_SSTR, AN7552_SCU_PON_SEL);
}

static int an7552_xpon_phy_probe(struct airoha_xpon_phy *priv,
				 struct platform_device *pdev)
{
	struct regmap *chip_scu;
	u32 val;
	struct an7552_xpon_phy *soc;
	int ret;

	soc = devm_kzalloc(priv->dev, sizeof(*soc), GFP_KERNEL);
	if (!soc)
		return -ENOMEM;
	priv->soc_priv = soc;

	chip_scu = syscon_regmap_lookup_by_phandle(priv->dev->of_node,
						   "airoha,chip-scu");
	if (IS_ERR(chip_scu))
		return dev_err_probe(priv->dev, PTR_ERR(chip_scu),
				     "failed to get AN7552 chip SCU\n");
	ret = regmap_read(chip_scu, AN7552_CHIP_SCU_XTAL, &val);
	if (ret)
		return ret;
	soc->needs_kband = !(val & AN7552_CHIP_SCU_XTAL_ALT);
	return 0;
}

static const struct airoha_xpon_phy_soc_data airoha_an7552_xpon_phy_data = {
	.name = "AN7552",
	.min_size = AN7552_XPON_PHY_MIN_SIZE,
	.gpon_bit_delay_reg = XPON_GPON_TX_BIT_DELAY,
	.gpon_bit_delay_mask = XPON_GPON_TX_BIT_DELAY_MASK,
	.has_integrated_pma = true,
	.pre_init = an7552_xpon_phy_pre_init,
	.probe = an7552_xpon_phy_probe,
	.manages_fw_ready = true,
	.has_gpon_bip = true,
	.configure = airoha_an7552_xpon_phy_configure,
	.complete_ready = an7552_xpon_phy_complete_ready,
	.reset_rx = an7552_xpon_phy_reset_rx,
};

static const struct of_device_id an7552_xpon_phy_of_match[] = {
	{ .compatible = "airoha,an7552-xpon-phy", .data = &airoha_an7552_xpon_phy_data },
	{ }
};
MODULE_DEVICE_TABLE(of, an7552_xpon_phy_of_match);

static struct platform_driver an7552_xpon_phy_driver = {
	.probe = airoha_xpon_phy_probe,
	.shutdown = airoha_xpon_phy_shutdown,
	.driver = {
		.name = "airoha-an7552-xpon-phy",
		.of_match_table = an7552_xpon_phy_of_match,
	},
};
module_platform_driver(an7552_xpon_phy_driver);

MODULE_DESCRIPTION("Airoha AN7552 xPON PHY driver");
MODULE_AUTHOR("Matheus Sampaio Queiroga <srherobrine20@gmail.com>");
MODULE_LICENSE("GPL");
