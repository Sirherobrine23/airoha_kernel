// SPDX-License-Identifier: GPL-2.0-only
/*
 * Airoha EN7523 xPON PHY driver
 *
 * The programming sequence is derived from the EN7523/EN7571 vendor
 * Mode_Config_7523() and pon_phy_scu_reset_init() paths.  The optical
 * analogue front end remains controlled by the EN7571 LDDLA/SFP driver;
 * this driver owns the SoC digital xPON PHY at 0x1faf0000.
 */

#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/phy/phy.h>
#include <linux/phy/phy-airoha-xpon.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/reset.h>
#include <linux/workqueue.h>

#define EN7523_XPON_PHY_MIN_SIZE		0x480c

#define EN7523_SCU_WAN_CONF		0x070
#define EN7523_SCU_WAN_MODE_MASK	GENMASK(7, 0)
#define EN7523_SCU_WAN_MODE_GPON	0x00
#define EN7523_SCU_WAN_MODE_EPON	0x01
#define EN7523_SCU_IOMUX_CTRL_3		0x218
#define EN7523_SCU_IOMUX_PON_EN		BIT(0)

#define XPON_PHYSET3			0x0108
#define XPON_PHYSET10			0x0124
#define XPON_PHYSTA1			0x0130
#define XPON_SETTING			0x0138
#define XPON_TDCSET2			0x01f8
#define XPON_GPON_PREAMBLE		0x0400
#define XPON_GPON_DELIMITER_GUARD	0x0404
#define XPON_GPON_EXT_PREAMBLE		0x0408
#define XPON_GPON_TX_COUNTER_CTRL	0x0424
#define XPON_GPON_TX_FRAME_COUNTER	0x0434
#define XPON_GPON_TX_BURST_COUNTER	0x0438
#define XPON_TRANS_STATUS		0x05e0
#define XPON_INT_ENABLE			0x05f0
#define XPON_INT_STATUS_CLR		0x05f4
#define XPON_INT_STATUS			0x05f8

#define XPON_RX_CTRL0			0x3028
#define XPON_PMA_CTRL0			0x4100
#define XPON_PMA_CTRL8			0x4110
#define XPON_PMA_CTRL12			0x4120
#define XPON_PMA_CTRL13			0x4124
#define XPON_SERDES_CTRL0		0x4200
#define XPON_SERDES_RESET		0x4204
#define XPON_SERDES_CTRL5		0x4214
#define XPON_SERDES_BEN_CTRL		0x4244
#define XPON_SERDES_CTRL18		0x4248
#define XPON_SERDES_CTRL19		0x424c
#define XPON_RX_MODE_CTRL		0x4344
#define XPON_CDR_CTRL			0x4530
#define XPON_FREQ_CTRL			0x4608
#define XPON_PMA_INT_STATUS		0x4800
#define XPON_PMA_INT_ENABLE		0x4804
#define XPON_PMA_INT_STATUS_CLR		0x4808

#define XPON_PHYSET3_PLL_RST		BIT(31)
#define XPON_PHYSET3_COUNTER_RST	BIT(27)
#define XPON_PHYSET10_GPON		BIT(31)
#define XPON_PHYSTA1_STATE_MASK		GENMASK(20, 18)
#define XPON_PHYSTA1_SYNCING		2
#define XPON_PHYSTA1_READY		6
#define XPON_TRANS_STATUS_LOS		BIT(0)
#define XPON_SERDES_RESET_RX		BIT(9)
#define XPON_SERDES_RESET_CDR		BIT(8)

#define XPON_GPON_PREAMBLE_GUARD_MASK	GENMASK(7, 0)
#define XPON_GPON_PREAMBLE_T1_MASK	GENMASK(15, 8)
#define XPON_GPON_PREAMBLE_T2_MASK	GENMASK(23, 16)
#define XPON_GPON_PREAMBLE_T3_MASK	GENMASK(31, 24)

#define XPON_GPON_EXT_O3_O4_MASK	GENMASK(7, 0)
#define XPON_GPON_EXT_O5_MASK		GENMASK(15, 8)
#define XPON_GPON_EXT_MODE		BIT(16)
#define XPON_GPON_EXT_OPER_MASK	GENMASK(18, 17)

#define XPON_GPON_TX_ENABLE_PATTERN	0xaa
#define XPON_GPON_TX_COUNTER_ENABLE	BIT(3)

#define XPON_SETTING_EN7571		0x0000014f
#define XPON_TDCSET2_EN7571		0x0000002d
#define XPON_GPON_DELIMITER_DEFAULT	0xaaab5983
#define XPON_READY_RECOVERY_MS		5000

struct airoha_xpon_phy {
	struct device *dev;
	void __iomem *base;
	struct regmap *scu;
	struct reset_control *reset;
	enum airoha_xpon_phy_submode submode;
	struct delayed_work ready_work;
	bool initialized;
	bool powered;
	bool ready_reported;
};

static u32 airoha_xpon_phy_read(struct airoha_xpon_phy *priv, u32 reg)
{
	return readl(priv->base + reg);
}

static void airoha_xpon_phy_write(struct airoha_xpon_phy *priv, u32 reg,
				  u32 val)
{
	writel(val, priv->base + reg);
}

static void airoha_xpon_phy_rmw(struct airoha_xpon_phy *priv, u32 reg,
				u32 mask, u32 val)
{
	u32 regval = airoha_xpon_phy_read(priv, reg);

	regval &= ~mask;
	regval |= val & mask;
	airoha_xpon_phy_write(priv, reg, regval);
}

static u32 airoha_xpon_phy_state(struct airoha_xpon_phy *priv)
{
	return FIELD_GET(XPON_PHYSTA1_STATE_MASK,
			 airoha_xpon_phy_read(priv, XPON_PHYSTA1));
}

static bool airoha_xpon_phy_ready(struct airoha_xpon_phy *priv)
{
	return airoha_xpon_phy_state(priv) == XPON_PHYSTA1_READY;
}

static bool airoha_xpon_phy_los(struct airoha_xpon_phy *priv)
{
	return airoha_xpon_phy_read(priv, XPON_TRANS_STATUS) &
		XPON_TRANS_STATUS_LOS;
}

static int airoha_xpon_phy_get_active_gpon(
	struct phy *phy, struct airoha_xpon_phy **privp)
{
	struct airoha_xpon_phy *priv;

	if (!phy || !privp)
		return -EINVAL;

	priv = phy_get_drvdata(phy);
	if (!priv)
		return -ENODEV;

	if (priv->submode != AIROHA_XPON_PHY_SUBMODE_GPON)
		return -EOPNOTSUPP;

	if (!priv->initialized || !priv->powered)
		return -EHOSTDOWN;

	*privp = priv;
	return 0;
}

int airoha_xpon_phy_get_gpon_tx_counters(struct phy *phy,
					 u32 *frame_count,
					 u32 *burst_count)
{
	struct airoha_xpon_phy *priv;
	u32 ctrl;
	int ret;

	if (!frame_count || !burst_count)
		return -EINVAL;

	ret = airoha_xpon_phy_get_active_gpon(phy, &priv);
	if (ret)
		return ret;

	/*
	 * Match xpon_en757x/v1 phy_tx_frame_counter() and
	 * phy_tx_burst_counter(): set TX counter enable before reading the
	 * cumulative frame and burst counters.
	 */
	ctrl = airoha_xpon_phy_read(priv, XPON_GPON_TX_COUNTER_CTRL);
	airoha_xpon_phy_write(priv, XPON_GPON_TX_COUNTER_CTRL,
			      ctrl | XPON_GPON_TX_COUNTER_ENABLE);

	*frame_count = airoha_xpon_phy_read(priv,
					   XPON_GPON_TX_FRAME_COUNTER);
	*burst_count = airoha_xpon_phy_read(priv,
					   XPON_GPON_TX_BURST_COUNTER);

	return 0;
}
EXPORT_SYMBOL_GPL(airoha_xpon_phy_get_gpon_tx_counters);

int airoha_xpon_phy_set_gpon_overhead(struct phy *phy, u8 guard_bits,
				      u8 t1_pbits, u8 t2_pbits,
				      u8 t3_pattern,
				      const u8 delimiter[3])
{
	struct airoha_xpon_phy *priv;
	u32 delimiter_guard, old_preamble, preamble;
	int ret;

	if (!delimiter)
		return -EINVAL;

	ret = airoha_xpon_phy_get_active_gpon(phy, &priv);
	if (ret)
		return ret;

	old_preamble = airoha_xpon_phy_read(priv, XPON_GPON_PREAMBLE);

	/*
	 * Vendor phy_gpon_preamble() writes all enabled fields literally.
	 * In particular, T1=T2=0 from the OLT must replace the reset defaults;
	 * only the field mapping is swapped between the PLOAM and PHY layouts.
	 */
	preamble = FIELD_PREP(XPON_GPON_PREAMBLE_GUARD_MASK, guard_bits) |
		   FIELD_PREP(XPON_GPON_PREAMBLE_T1_MASK, t2_pbits) |
		   FIELD_PREP(XPON_GPON_PREAMBLE_T2_MASK, t1_pbits) |
		   FIELD_PREP(XPON_GPON_PREAMBLE_T3_MASK, t3_pattern);

	delimiter_guard = ((u32)XPON_GPON_TX_ENABLE_PATTERN << 24) |
			  ((u32)delimiter[0] << 16) |
			  ((u32)delimiter[1] << 8) |
			  delimiter[2];

	airoha_xpon_phy_write(priv, XPON_GPON_PREAMBLE, preamble);
	airoha_xpon_phy_write(priv, XPON_GPON_DELIMITER_GUARD,
			      delimiter_guard);

	dev_info(priv->dev,
		 "GPON PHY overhead: guard=%u t1=%u t2=%u t3=%u preamble=%#010x delimiter=%#010x old=%#010x\n",
		 guard_bits, t1_pbits, t2_pbits, t3_pattern,
		 preamble, delimiter_guard, old_preamble);
	return 0;
}
EXPORT_SYMBOL_GPL(airoha_xpon_phy_set_gpon_overhead);

int airoha_xpon_phy_set_gpon_extended_preamble(struct phy *phy,
					       u8 o3_o4_preamble,
					       u8 o5_preamble)
{
	struct airoha_xpon_phy *priv;
	u32 mask, val;
	int ret;

	ret = airoha_xpon_phy_get_active_gpon(phy, &priv);
	if (ret)
		return ret;

	mask = XPON_GPON_EXT_O3_O4_MASK | XPON_GPON_EXT_O5_MASK |
	       XPON_GPON_EXT_MODE | XPON_GPON_EXT_OPER_MASK;
	val = FIELD_PREP(XPON_GPON_EXT_O3_O4_MASK, o3_o4_preamble) |
	      FIELD_PREP(XPON_GPON_EXT_O5_MASK, o5_preamble) |
	      XPON_GPON_EXT_MODE |
	      FIELD_PREP(XPON_GPON_EXT_OPER_MASK,
			 AIROHA_XPON_PHY_GPON_OPER_RANGING);

	airoha_xpon_phy_rmw(priv, XPON_GPON_EXT_PREAMBLE, mask, val);
	dev_info(priv->dev,
		 "GPON PHY extended preamble programmed: O3/O4=%u O5=%u reg=%#010x\n",
		 o3_o4_preamble, o5_preamble,
		 airoha_xpon_phy_read(priv, XPON_GPON_EXT_PREAMBLE));
	return 0;
}
EXPORT_SYMBOL_GPL(airoha_xpon_phy_set_gpon_extended_preamble);

int airoha_xpon_phy_set_gpon_oper_state(
	struct phy *phy, enum airoha_xpon_phy_gpon_oper_state state)
{
	struct airoha_xpon_phy *priv;
	u32 mask, val;
	int ret;

	ret = airoha_xpon_phy_get_active_gpon(phy, &priv);
	if (ret)
		return ret;

	switch (state) {
	case AIROHA_XPON_PHY_GPON_OPER_DISABLED:
		/* Vendor O2 transition clears lengths, extended mode and state. */
		mask = XPON_GPON_EXT_O3_O4_MASK | XPON_GPON_EXT_O5_MASK |
		       XPON_GPON_EXT_MODE | XPON_GPON_EXT_OPER_MASK;
		val = 0;
		break;
	case AIROHA_XPON_PHY_GPON_OPER_RANGING:
	case AIROHA_XPON_PHY_GPON_OPER_OPERATION:
		mask = XPON_GPON_EXT_OPER_MASK;
		val = FIELD_PREP(XPON_GPON_EXT_OPER_MASK, state);
		break;
	default:
		return -EINVAL;
	}

	airoha_xpon_phy_rmw(priv, XPON_GPON_EXT_PREAMBLE, mask, val);
	dev_info(priv->dev,
		 "GPON PHY operational state=%u reg=%#010x\n",
		 state, airoha_xpon_phy_read(priv, XPON_GPON_EXT_PREAMBLE));
	return 0;
}
EXPORT_SYMBOL_GPL(airoha_xpon_phy_set_gpon_oper_state);

static void airoha_xpon_phy_dump(struct airoha_xpon_phy *priv,
				 const char *stage)
{
	dev_info(priv->dev,
		 "%s: mode=%s ready=%u los=%u set3=%#010x set10=%#010x sta1=%#010x setting=%#010x pma0=%#010x serdes0=%#010x ben=%#010x tdc2=%#010x gpon=%#010x/%#010x/%#010x int=%#010x/%#010x pma=%#010x/%#010x\n",
		 stage,
		 priv->submode == AIROHA_XPON_PHY_SUBMODE_GPON ?
		 "GPON" : "EPON",
		 airoha_xpon_phy_ready(priv), airoha_xpon_phy_los(priv),
		 airoha_xpon_phy_read(priv, XPON_PHYSET3),
		 airoha_xpon_phy_read(priv, XPON_PHYSET10),
		 airoha_xpon_phy_read(priv, XPON_PHYSTA1),
		 airoha_xpon_phy_read(priv, XPON_SETTING),
		 airoha_xpon_phy_read(priv, XPON_PMA_CTRL0),
		 airoha_xpon_phy_read(priv, XPON_SERDES_CTRL0),
		 airoha_xpon_phy_read(priv, XPON_SERDES_BEN_CTRL),
		 airoha_xpon_phy_read(priv, XPON_TDCSET2),
		 airoha_xpon_phy_read(priv, XPON_GPON_PREAMBLE),
		 airoha_xpon_phy_read(priv, XPON_GPON_DELIMITER_GUARD),
		 airoha_xpon_phy_read(priv, XPON_GPON_EXT_PREAMBLE),
		 airoha_xpon_phy_read(priv, XPON_INT_STATUS),
		 airoha_xpon_phy_read(priv, XPON_INT_ENABLE),
		 airoha_xpon_phy_read(priv, XPON_PMA_INT_STATUS),
		 airoha_xpon_phy_read(priv, XPON_PMA_INT_ENABLE));
}

static int airoha_xpon_phy_reset(struct phy *phy)
{
	struct airoha_xpon_phy *priv = phy_get_drvdata(phy);
	int ret;

	dev_info(priv->dev, "asserting xPON PHY reset\n");
	ret = reset_control_assert(priv->reset);
	if (ret)
		return dev_err_probe(priv->dev, ret,
				     "failed to assert xPON PHY reset\n");

	udelay(1);

	ret = reset_control_deassert(priv->reset);
	if (ret)
		return dev_err_probe(priv->dev, ret,
				     "failed to deassert xPON PHY reset\n");

	mdelay(1);
	dev_info(priv->dev, "xPON PHY reset released\n");
	return 0;
}

static int airoha_xpon_phy_init(struct phy *phy)
{
	struct airoha_xpon_phy *priv = phy_get_drvdata(phy);
	int ret;

	ret = airoha_xpon_phy_reset(phy);
	if (ret)
		return ret;

	priv->initialized = true;
	return 0;
}

static int airoha_xpon_phy_exit(struct phy *phy)
{
	struct airoha_xpon_phy *priv = phy_get_drvdata(phy);
	int ret;

	WRITE_ONCE(priv->powered, false);
	cancel_delayed_work_sync(&priv->ready_work);
	priv->ready_reported = false;
	priv->initialized = false;

	ret = reset_control_assert(priv->reset);
	if (ret)
		dev_warn(priv->dev, "failed to assert xPON PHY reset: %d\n",
			 ret);
	else
		dev_info(priv->dev, "xPON PHY held in reset\n");

	return ret;
}

static int airoha_xpon_phy_set_mode(struct phy *phy, enum phy_mode mode,
				    int submode)
{
	struct airoha_xpon_phy *priv = phy_get_drvdata(phy);

	if (mode != PHY_MODE_ETHERNET)
		return -EINVAL;

	if (submode != AIROHA_XPON_PHY_SUBMODE_GPON &&
	    submode != AIROHA_XPON_PHY_SUBMODE_EPON)
		return -EINVAL;

	priv->submode = submode;
	dev_info(priv->dev, "xPON PHY mode selected: %s\n",
		 submode == AIROHA_XPON_PHY_SUBMODE_GPON ? "GPON" : "EPON");
	return 0;
}

static int airoha_xpon_phy_configure(struct airoha_xpon_phy *priv)
{
	u32 mode, val;
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

	/* Program the EN7571 mode-dependent values before resetting the xPON
	 * PLL and counters.  The vendor Mode_Config_7523() sequence relies on
	 * these values being present when the reset is released.  Resetting
	 * first makes the initial power-on miss PHY_READY, while a second
	 * down/up works only because the values survived the first attempt.
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

static void airoha_xpon_phy_complete_ready(struct airoha_xpon_phy *priv)
{
	/*
	 * Vendor phy_ready_handler() retriggers PMA_CTRL8 bit 0 after the
	 * receiver reaches PHY_READY.
	 */
	airoha_xpon_phy_rmw(priv, XPON_PMA_CTRL8, BIT(0), 0);
	udelay(1);
	airoha_xpon_phy_rmw(priv, XPON_PMA_CTRL8, BIT(0), BIT(0));
	mdelay(1);

	priv->ready_reported = true;
	airoha_xpon_phy_dump(priv, "xPON PHY ready");
}

static void airoha_xpon_phy_ready_work(struct work_struct *work)
{
	struct airoha_xpon_phy *priv =
		container_of(to_delayed_work(work),
			     struct airoha_xpon_phy, ready_work);
	u32 state, val;

	if (!READ_ONCE(priv->powered))
		return;

	state = airoha_xpon_phy_state(priv);
	if (state == XPON_PHYSTA1_READY) {
		if (!priv->ready_reported)
			airoha_xpon_phy_complete_ready(priv);
	} else {
		if (priv->ready_reported) {
			priv->ready_reported = false;
			dev_info(priv->dev,
				 "xPON PHY lost synchronization: state=%u los=%u\n",
				 state, airoha_xpon_phy_los(priv));
		}

		/*
		 * Match vendor phy_ready_recover_expires(): while the receiver is
		 * in state 2 and LOS is deasserted, pulse the RX PLL/counter reset.
		 * With LOS asserted, leave the PHY powered and simply wait.
		 */
		if (state == XPON_PHYSTA1_SYNCING &&
		    !airoha_xpon_phy_los(priv)) {
			val = airoha_xpon_phy_read(priv, XPON_PHYSET3);
			airoha_xpon_phy_write(priv, XPON_PHYSET3,
					      val | XPON_PHYSET3_PLL_RST |
					       XPON_PHYSET3_COUNTER_RST);
			mdelay(1);
			airoha_xpon_phy_write(priv, XPON_PHYSET3, val);
			dev_dbg(priv->dev,
				"reset RX PLL while LOS=0 and PHY_READY=0\n");
		}
	}

	if (READ_ONCE(priv->powered))
		mod_delayed_work(system_wq, &priv->ready_work,
				 msecs_to_jiffies(XPON_READY_RECOVERY_MS));
}

static int airoha_xpon_phy_power_on(struct phy *phy)
{
	struct airoha_xpon_phy *priv = phy_get_drvdata(phy);
	u32 state;
	int ret;

	if (!priv->initialized)
		return -EINVAL;

	dev_info(priv->dev, "configuring %s xPON PHY\n",
		 priv->submode == AIROHA_XPON_PHY_SUBMODE_GPON ?
		 "GPON" : "EPON");

	ret = airoha_xpon_phy_configure(priv);
	if (ret)
		return ret;

	WRITE_ONCE(priv->powered, true);
	priv->ready_reported = false;
	airoha_xpon_phy_dump(priv, "xPON PHY configured");

	state = airoha_xpon_phy_state(priv);
	if (state == XPON_PHYSTA1_READY) {
		airoha_xpon_phy_complete_ready(priv);
	} else if (airoha_xpon_phy_los(priv)) {
		dev_info(priv->dev,
			 "xPON PHY started without optical signal; waiting for fiber (state=%u)\n",
			 state);
	} else {
		dev_info(priv->dev,
			 "xPON PHY started asynchronously; waiting for PHY_READY (state=%u)\n",
			 state);
	}

	/*
	 * Absence of light is a link condition, not a power-on failure.
	 * Keep the digital PHY configured and monitor/recover it periodically.
	 */
	mod_delayed_work(system_wq, &priv->ready_work,
			 msecs_to_jiffies(XPON_READY_RECOVERY_MS));
	return 0;
}

static int airoha_xpon_phy_power_off(struct phy *phy)
{
	struct airoha_xpon_phy *priv = phy_get_drvdata(phy);

	if (!READ_ONCE(priv->powered))
		return 0;

	WRITE_ONCE(priv->powered, false);
	cancel_delayed_work_sync(&priv->ready_work);
	priv->ready_reported = false;

	airoha_xpon_phy_write(priv, XPON_INT_ENABLE, 0);
	airoha_xpon_phy_write(priv, XPON_PMA_INT_ENABLE, 0);
	dev_info(priv->dev, "xPON PHY powered off\n");
	return 0;
}

static const struct phy_ops airoha_xpon_phy_ops = {
	.init = airoha_xpon_phy_init,
	.exit = airoha_xpon_phy_exit,
	.power_on = airoha_xpon_phy_power_on,
	.power_off = airoha_xpon_phy_power_off,
	.set_mode = airoha_xpon_phy_set_mode,
	.reset = airoha_xpon_phy_reset,
	.owner = THIS_MODULE,
};

static int airoha_xpon_phy_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct phy_provider *provider;
	struct airoha_xpon_phy *priv;
	struct resource *res;
	struct phy *phy;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->dev = dev;
	priv->submode = AIROHA_XPON_PHY_SUBMODE_GPON;
	INIT_DELAYED_WORK(&priv->ready_work, airoha_xpon_phy_ready_work);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return dev_err_probe(dev, -EINVAL,
				     "missing xPON PHY register resource\n");
	if (resource_size(res) < EN7523_XPON_PHY_MIN_SIZE)
		return dev_err_probe(dev, -EINVAL,
				     "xPON PHY resource %pR is too small\n", res);

	priv->base = devm_ioremap_resource(dev, res);
	if (IS_ERR(priv->base))
		return PTR_ERR(priv->base);

	priv->scu = syscon_regmap_lookup_by_phandle(dev->of_node,
						    "airoha,scu");
	if (IS_ERR(priv->scu))
		return dev_err_probe(dev, PTR_ERR(priv->scu),
				     "failed to get SCU regmap\n");

	priv->reset = devm_reset_control_get_exclusive(dev, "phy");
	if (IS_ERR(priv->reset))
		return dev_err_probe(dev, PTR_ERR(priv->reset),
				     "failed to get xPON PHY reset\n");

	phy = devm_phy_create(dev, NULL, &airoha_xpon_phy_ops);
	if (IS_ERR(phy))
		return dev_err_probe(dev, PTR_ERR(phy),
				     "failed to create xPON PHY\n");

	phy_set_drvdata(phy, priv);
	platform_set_drvdata(pdev, priv);

	provider = devm_of_phy_provider_register(dev, of_phy_simple_xlate);
	if (IS_ERR(provider))
		return dev_err_probe(dev, PTR_ERR(provider),
				     "failed to register xPON PHY provider\n");

	dev_info(dev, "EN7523 xPON PHY registered at %pR\n", res);
	return 0;
}

static const struct of_device_id airoha_xpon_phy_of_match[] = {
	{ .compatible = "airoha,en7523-xpon-phy" },
	{ }
};
MODULE_DEVICE_TABLE(of, airoha_xpon_phy_of_match);

static struct platform_driver airoha_xpon_phy_driver = {
	.probe = airoha_xpon_phy_probe,
	.driver = {
		.name = "airoha-en7523-xpon-phy",
		.of_match_table = airoha_xpon_phy_of_match,
	},
};
module_platform_driver(airoha_xpon_phy_driver);

MODULE_DESCRIPTION("Airoha EN7523 xPON PHY driver");
MODULE_AUTHOR("Matheus Sampaio Queiroga <srherobrine20@gmail.com>");
MODULE_LICENSE("GPL");
