// SPDX-License-Identifier: GPL-2.0-only
/* Shared digital xPON PHY services and backend provider setup. */

#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/phy/phy-airoha-xpon.h>
#include <linux/phy/phy.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/reset.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/workqueue.h>

#include "xpon.h"

u32 airoha_xpon_phy_read(struct airoha_xpon_phy *priv, u32 reg)
{
	if (priv->soc->pma_offset && reg >= priv->soc->pma_offset)
		return readl(priv->pma_base + reg - priv->soc->pma_offset);
	if (priv->soc->ana_offset && reg >= priv->soc->ana_offset)
		return readl(priv->ana_base + reg - priv->soc->ana_offset);
	return readl(priv->base + reg);
}
EXPORT_SYMBOL_GPL(airoha_xpon_phy_read);

void airoha_xpon_phy_write(struct airoha_xpon_phy *priv, u32 reg,
			   u32 val)
{
	if (priv->soc->pma_offset && reg >= priv->soc->pma_offset)
		writel(val, priv->pma_base + reg - priv->soc->pma_offset);
	else if (priv->soc->ana_offset && reg >= priv->soc->ana_offset)
		writel(val, priv->ana_base + reg - priv->soc->ana_offset);
	else
		writel(val, priv->base + reg);
}
EXPORT_SYMBOL_GPL(airoha_xpon_phy_write);

void airoha_xpon_phy_rmw(struct airoha_xpon_phy *priv, u32 reg,
			 u32 mask, u32 val)
{
	u32 regval = airoha_xpon_phy_read(priv, reg);

	regval &= ~mask;
	regval |= val & mask;
	airoha_xpon_phy_write(priv, reg, regval);
}
EXPORT_SYMBOL_GPL(airoha_xpon_phy_rmw);

void airoha_xpon_phy_run(struct airoha_xpon_phy *priv,
			 const struct airoha_xpon_phy_step *seq, size_t count)
{
	size_t i;

	for (i = 0; i < count; i++) {
		if (seq[i].reg == AIROHA_XPON_PHY_DELAY)
			fsleep(seq[i].value);
		else
			airoha_xpon_phy_rmw(priv, seq[i].reg, seq[i].mask,
					    seq[i].value);
	}
}
EXPORT_SYMBOL_GPL(airoha_xpon_phy_run);

static u32 airoha_xpon_phy_state(struct airoha_xpon_phy *priv)
{
	return FIELD_GET(XPON_PHYSTA1_STATE_MASK,
			 airoha_xpon_phy_read(priv, XPON_PHYSTA1));
}

static bool airoha_xpon_phy_fw_ready(struct airoha_xpon_phy *priv)
{
	return airoha_xpon_phy_read(priv, XPON_PHYFWREADY) &
		XPON_PHYFWREADY_READY;
}

static bool airoha_xpon_phy_ready(struct airoha_xpon_phy *priv)
{
	return airoha_xpon_phy_state(priv) == XPON_PHYSTA1_READY;
}

static u32 airoha_xpon_phy_rx_sync(struct airoha_xpon_phy *priv)
{
	return FIELD_GET(XPON_RX_SYNC_MASK,
			 airoha_xpon_phy_read(priv, XPON_RX_FEC_STATUS));
}

static bool airoha_xpon_phy_los(struct airoha_xpon_phy *priv)
{
	return airoha_xpon_phy_read(priv, XPON_TRANS_STATUS) &
		XPON_TRANS_STATUS_LOS;
}

int airoha_xpon_phy_get_link_state(struct phy *phy, bool *ready, bool *los)
{
	struct airoha_xpon_phy *priv;

	if (!phy || !ready || !los)
		return -EINVAL;

	priv = phy_get_drvdata(phy);
	if (!priv)
		return -ENODEV;

	if (priv->soc->get_link_state)
		return priv->soc->get_link_state(priv, ready, los);

	if (!READ_ONCE(priv->powered)) {
		*ready = false;
		*los = true;
	} else {
		*los = airoha_xpon_phy_los(priv);
		*ready = airoha_xpon_phy_ready(priv);
	}

	return 0;
}
EXPORT_SYMBOL_GPL(airoha_xpon_phy_get_link_state);

static int airoha_xpon_phy_get_active_gpon(struct phy *phy,
					   struct airoha_xpon_phy **privp)
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

/*
 * Latch, read and clear the GPON BIP error counter, as the vendor
 * phy_bip_counter() does.  The command register takes one command at a
 * time and is never read-modify-written.  Safe in atomic context.
 */
int airoha_xpon_phy_take_gpon_bip(struct phy *phy, u32 *count)
{
	struct airoha_xpon_phy *priv;
	unsigned long flags;
	int ret;

	if (!count)
		return -EINVAL;

	ret = airoha_xpon_phy_get_active_gpon(phy, &priv);
	if (ret)
		return ret;
	if (!priv->soc->has_gpon_bip)
		return -EOPNOTSUPP;

	spin_lock_irqsave(&priv->counter_lock, flags);
	airoha_xpon_phy_write(priv, XPON_RX_COUNTER_CTRL, XPON_RX_BIP_LATCH);
	/* The vendor reads the latched value twice. */
	airoha_xpon_phy_read(priv, XPON_RX_BIP_COUNTER);
	*count = airoha_xpon_phy_read(priv, XPON_RX_BIP_COUNTER);
	airoha_xpon_phy_write(priv, XPON_RX_COUNTER_CTRL, XPON_RX_BIP_CLEAR);
	/* Flush the posted clear before the lock is released. */
	airoha_xpon_phy_read(priv, XPON_RX_COUNTER_CTRL);
	spin_unlock_irqrestore(&priv->counter_lock, flags);

	return 0;
}
EXPORT_SYMBOL_GPL(airoha_xpon_phy_take_gpon_bip);

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

int airoha_xpon_phy_set_tx_calibration_mode(struct phy *phy, bool enable)
{
	struct airoha_xpon_phy *priv;
	int ret;

	ret = airoha_xpon_phy_get_active_gpon(phy, &priv);
	if (ret)
		return ret;

	if (priv->soc->set_tx_calibration_mode)
		return priv->soc->set_tx_calibration_mode(priv, enable);

	/*
	 * Match the vendor 1G TGEN sequence used by EN7570/EN7571.
	 * Calibration locks the TX CDR to the reference clock and feeds
	 * PRBS23 into the burst timing detector. Normal operation restores
	 * idle data and lock-to-data.
	 */
	if (enable) {
		airoha_xpon_phy_rmw(priv, XPON_PHYSET1,
				    XPON_PHYSET1_TX_LOCK_REF,
				     XPON_PHYSET1_TX_LOCK_REF);
		airoha_xpon_phy_write(priv, XPON_BISTCTL_LOOPBACK_SEL,
				      XPON_BIST_PRBS23);
		airoha_xpon_phy_write(priv, XPON_BISTCTL_PRBS_TX_EN, 1);
	} else {
		airoha_xpon_phy_write(priv, XPON_BISTCTL_PRBS_TX_EN, 0);
		airoha_xpon_phy_write(priv, XPON_BISTCTL_LOOPBACK_SEL, 0);
		airoha_xpon_phy_rmw(priv, XPON_PHYSET1,
				    XPON_PHYSET1_TX_LOCK_REF, 0);
	}

	return 0;
}
EXPORT_SYMBOL_GPL(airoha_xpon_phy_set_tx_calibration_mode);

int airoha_xpon_phy_get_gpon_fec_status(struct phy *phy,
					bool *downstream, bool *upstream)
{
	struct airoha_xpon_phy *priv;
	int ret;

	if (!downstream || !upstream)
		return -EINVAL;

	ret = airoha_xpon_phy_get_active_gpon(phy, &priv);
	if (ret)
		return ret;

	*downstream = airoha_xpon_phy_read(priv, XPON_RX_FEC_STATUS) &
			  XPON_FEC_STATUS_ACTIVE;
	*upstream = airoha_xpon_phy_read(priv, XPON_TX_FEC_STATUS) &
		       XPON_FEC_STATUS_ACTIVE;

	return 0;
}
EXPORT_SYMBOL_GPL(airoha_xpon_phy_get_gpon_fec_status);

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
	dev_dbg(priv->dev,
		"GPON PHY extended preamble programmed: O3/O4=%u O5=%u reg=%#010x\n",
		 o3_o4_preamble, o5_preamble,
		 airoha_xpon_phy_read(priv, XPON_GPON_EXT_PREAMBLE));
	return 0;
}
EXPORT_SYMBOL_GPL(airoha_xpon_phy_set_gpon_extended_preamble);

/*
 * Sub-byte part of the ranging equalisation delay.  The GPON MAC only takes
 * the byte-aligned part of EqD in G_EQD; the remaining 0-7 bits are a
 * transmitter delay applied here.  Leaving it at zero shifts every upstream
 * burst by up to 7 bit times against the position the OLT ranged, which eats
 * into the guard band and makes bursts marginal on OLTs with a tight one.
 */
int airoha_xpon_phy_set_gpon_bit_delay(struct phy *phy, u8 delay)
{
	struct airoha_xpon_phy *priv;
	int ret;

	if (delay > 7)
		return -EINVAL;

	ret = airoha_xpon_phy_get_active_gpon(phy, &priv);
	if (ret)
		return ret;

	airoha_xpon_phy_rmw(priv, priv->soc->gpon_bit_delay_reg,
			    priv->soc->gpon_bit_delay_mask |
			     priv->soc->gpon_bit_delay_enable,
			    field_prep(priv->soc->gpon_bit_delay_mask, delay) |
			     priv->soc->gpon_bit_delay_enable);
	dev_dbg(priv->dev, "GPON PHY TX bit delay=%u reg=%#010x\n", delay,
		airoha_xpon_phy_read(priv, priv->soc->gpon_bit_delay_reg));
	return 0;
}
EXPORT_SYMBOL_GPL(airoha_xpon_phy_set_gpon_bit_delay);

int airoha_xpon_phy_set_gpon_oper_state(struct phy *phy,
					enum airoha_xpon_phy_gpon_oper_state state)
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
		 "%s: soc=%s mode=%s ready=%u los=%u set3=%#010x set10=%#010x sta1=%#010x setting=%#010x tdc2=%#010x gpon=%#010x/%#010x/%#010x int=%#010x/%#010x\n",
		 stage, priv->soc->name,
		 priv->submode == AIROHA_XPON_PHY_SUBMODE_GPON ?
		 "GPON" : "EPON",
		 airoha_xpon_phy_ready(priv), airoha_xpon_phy_los(priv),
		 airoha_xpon_phy_read(priv, XPON_PHYSET3),
		 airoha_xpon_phy_read(priv, XPON_PHYSET10),
		 airoha_xpon_phy_read(priv, XPON_PHYSTA1),
		 airoha_xpon_phy_read(priv, XPON_SETTING),
		 airoha_xpon_phy_read(priv, XPON_TDCSET2),
		 airoha_xpon_phy_read(priv, XPON_GPON_PREAMBLE),
		 airoha_xpon_phy_read(priv, XPON_GPON_DELIMITER_GUARD),
		 airoha_xpon_phy_read(priv, XPON_GPON_EXT_PREAMBLE),
		 airoha_xpon_phy_read(priv, XPON_INT_STATUS),
		 airoha_xpon_phy_read(priv, XPON_INT_ENABLE));

	if (priv->soc->manages_fw_ready)
		dev_info(priv->dev,
			 "digital status: fw_ready=%u rx_sync=%#x rx_synced=%u counters=%#010x/%#010x/%#010x/%#010x\n",
			 airoha_xpon_phy_fw_ready(priv),
			 airoha_xpon_phy_rx_sync(priv),
			 airoha_xpon_phy_rx_sync(priv) == XPON_RX_SYNC_READY,
			 airoha_xpon_phy_read(priv, XPON_RX_COUNTER_ENABLE),
			 airoha_xpon_phy_read(priv, XPON_RX_COUNTER_CTRL),
			 airoha_xpon_phy_read(priv, XPON_RX_COUNTER_CTRL2),
			 airoha_xpon_phy_read(priv,
					      XPON_GPON_TX_COUNTER_CTRL));

	if (priv->soc->has_integrated_pma)
		dev_info(priv->dev,
			 "PMA: ctrl0=%#010x serdes0=%#010x ben=%#010x int=%#010x/%#010x\n",
			 airoha_xpon_phy_read(priv, XPON_PMA_CTRL0),
			 airoha_xpon_phy_read(priv, XPON_SERDES_CTRL0),
			 airoha_xpon_phy_read(priv, XPON_SERDES_BEN_CTRL),
			 airoha_xpon_phy_read(priv, XPON_PMA_INT_STATUS),
			 airoha_xpon_phy_read(priv, XPON_PMA_INT_ENABLE));
}

void airoha_xpon_phy_set_tx_gpio(struct airoha_xpon_phy *priv, bool enable)
{
	if (!priv->tx_disable_gpio)
		return;

	/* TX_DISABLE is described using its asserted (disable) polarity. */
	gpiod_set_value_cansleep(priv->tx_disable_gpio, !enable);
}
EXPORT_SYMBOL_GPL(airoha_xpon_phy_set_tx_gpio);

int airoha_xpon_phy_set_tx_enable(struct phy *phy, bool enable)
{
	struct airoha_xpon_phy *priv;

	if (!phy)
		return -EINVAL;

	priv = phy_get_drvdata(phy);
	if (!priv)
		return -ENODEV;

	if (priv->soc->set_tx_enable)
		return priv->soc->set_tx_enable(priv, enable);

	airoha_xpon_phy_set_tx_gpio(priv, enable);
	return 0;
}
EXPORT_SYMBOL_GPL(airoha_xpon_phy_set_tx_enable);

void airoha_xpon_phy_set_vcc_enabled(struct airoha_xpon_phy *priv, bool enable)
{
	if (!priv->vcc_disable_gpio)
		return;

	/* VCC_DISABLE follows the same logical-disable convention. */
	gpiod_set_value_cansleep(priv->vcc_disable_gpio, !enable);
}
EXPORT_SYMBOL_GPL(airoha_xpon_phy_set_vcc_enabled);

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

	if (priv->soc->pre_init) {
		ret = priv->soc->pre_init(priv);
		if (ret)
			return ret;
	}
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

void econet_xpon_phy_counter_clear(struct airoha_xpon_phy *priv, u32 mask)
{
	unsigned long flags;

	spin_lock_irqsave(&priv->counter_lock, flags);
	/*
	 * EcoNet 1G phy_counter_clear().  The RX counter clear register is a
	 * command register: the vendor writes one command at a time rather than
	 * ORing them together.
	 */
	if (mask & ECONET_XPON_COUNTER_CLEAR_RX0)
		airoha_xpon_phy_write(priv, XPON_RX_COUNTER_CTRL, BIT(1));
	if (mask & ECONET_XPON_COUNTER_CLEAR_RX1)
		airoha_xpon_phy_write(priv, XPON_RX_COUNTER_CTRL, BIT(3));
	if (mask & ECONET_XPON_COUNTER_CLEAR_RX2)
		airoha_xpon_phy_write(priv, XPON_RX_COUNTER_CTRL, BIT(5));
	if (mask & ECONET_XPON_COUNTER_CLEAR_GPON_TX)
		airoha_xpon_phy_rmw(priv, XPON_GPON_TX_COUNTER_CTRL, BIT(2),
				    BIT(2));
	if (mask & ECONET_XPON_COUNTER_CLEAR_RX3)
		airoha_xpon_phy_write(priv, XPON_RX_COUNTER_CTRL2, BIT(4));
	spin_unlock_irqrestore(&priv->counter_lock, flags);
}
EXPORT_SYMBOL_GPL(econet_xpon_phy_counter_clear);

void econet_xpon_phy_counter_init(struct airoha_xpon_phy *priv)
{
	u32 val;

	/* Match phy_cnt_enable(1, 1, 1), including its initial clear. */
	val = airoha_xpon_phy_read(priv, XPON_RX_COUNTER_ENABLE);
	val &= ~ECONET_XPON_COUNTER_ENABLE_MASK;
	val |= ECONET_XPON_COUNTER_ENABLE_MASK;
	udelay(1);
	airoha_xpon_phy_write(priv, XPON_RX_COUNTER_ENABLE, val);
	econet_xpon_phy_counter_clear(priv,
				      ECONET_XPON_COUNTER_ENABLE_MASK);
}
EXPORT_SYMBOL_GPL(econet_xpon_phy_counter_init);

static int airoha_xpon_phy_configure(struct airoha_xpon_phy *priv)
{
	return priv->soc->configure(priv);
}

static void airoha_xpon_phy_complete_ready(struct airoha_xpon_phy *priv)
{
	if (priv->soc->complete_ready)
		priv->soc->complete_ready(priv);

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
			if (priv->soc->reset_rx) {
				priv->soc->reset_rx(priv);
			} else {
				val = airoha_xpon_phy_read(priv, XPON_PHYSET3);
				airoha_xpon_phy_write(priv, XPON_PHYSET3,
						      val | XPON_PHYSET3_PLL_RST |
					 XPON_PHYSET3_COUNTER_RST);
				mdelay(1);
				airoha_xpon_phy_write(priv, XPON_PHYSET3, val);
			}
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

	/*
	 * Vendor phy_mode_config() asserts TX_DISABLE while switching GPON/
	 * EPON mode, then releases it after the PHY reset/configuration has
	 * completed.  LED_PHY_VCC_DISABLE, when present, is deasserted before
	 * the transmitter is configured.
	 */
	airoha_xpon_phy_set_vcc_enabled(priv, true);
	airoha_xpon_phy_set_tx_gpio(priv, false);

	dev_info(priv->dev, "configuring %s xPON PHY\n",
		 priv->submode == AIROHA_XPON_PHY_SUBMODE_GPON ?
		 "GPON" : "EPON");

	ret = airoha_xpon_phy_configure(priv);
	if (ret)
		goto err_power;

	airoha_xpon_phy_set_tx_gpio(priv, true);

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

err_power:
	airoha_xpon_phy_set_tx_gpio(priv, false);
	airoha_xpon_phy_set_vcc_enabled(priv, false);
	return ret;
}

static int airoha_xpon_phy_power_off(struct phy *phy)
{
	struct airoha_xpon_phy *priv = phy_get_drvdata(phy);

	if (!READ_ONCE(priv->powered))
		return 0;

	WRITE_ONCE(priv->powered, false);
	cancel_delayed_work_sync(&priv->ready_work);
	priv->ready_reported = false;

	/* Block optical TX before quiescing the digital PHY. */
	airoha_xpon_phy_set_tx_gpio(priv, false);

	if (priv->soc->manages_fw_ready)
		airoha_xpon_phy_rmw(priv, XPON_PHYFWREADY,
				    XPON_PHYFWREADY_READY, 0);

	airoha_xpon_phy_write(priv, XPON_INT_ENABLE, 0);
	if (priv->soc->has_integrated_pma)
		airoha_xpon_phy_write(priv, XPON_PMA_INT_ENABLE, 0);

	airoha_xpon_phy_set_vcc_enabled(priv, false);
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

/*
 * Collect the transceiver pin conventions described by the board.  A property
 * that is absent leaves its signal in the default, non-inverted sense.
 */
static u32 airoha_xpon_phy_trans_invert(struct device *dev)
{
	static const struct {
		const char *name;
		u32 bit;
	} signals[] = {
		{ "airoha,tx-sd-inverted", XPON_SETTING_TX_SD_INV },
		{ "airoha,tx-fault-inverted", XPON_SETTING_TX_FAULT_INV },
		{ "airoha,rx-sd-inverted", XPON_SETTING_RX_SD_INV },
		{ "airoha,burst-enable-inverted", XPON_SETTING_BURST_EN_INV },
	};
	u32 invert = 0;
	size_t i;

	for (i = 0; i < ARRAY_SIZE(signals); i++)
		if (device_property_read_bool(dev, signals[i].name))
			invert |= signals[i].bit;

	return invert;
}

static void airoha_xpon_phy_cleanup(void *data)
{
	struct airoha_xpon_phy *priv = data;
	int ret;

	WRITE_ONCE(priv->powered, false);
	cancel_delayed_work_sync(&priv->ready_work);
	airoha_xpon_phy_set_tx_gpio(priv, false);
	airoha_xpon_phy_set_vcc_enabled(priv, false);
	ret = reset_control_assert(priv->reset);
	if (ret)
		dev_warn(priv->dev, "failed to hold xPON PHY reset: %d\n", ret);
}

int airoha_xpon_phy_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	const struct airoha_xpon_phy_soc_data *soc;
	struct phy_provider *provider;
	struct airoha_xpon_phy *priv;
	struct resource *res;
	struct phy *phy;
	int ret;

	soc = device_get_match_data(dev);
	if (!soc)
		return -ENODEV;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->dev = dev;
	priv->soc = soc;
	priv->submode = soc->has_xgspon ? AIROHA_XPON_PHY_SUBMODE_XGSPON :
		AIROHA_XPON_PHY_SUBMODE_GPON;
	priv->trans_invert = airoha_xpon_phy_trans_invert(dev);
	spin_lock_init(&priv->counter_lock);
	INIT_DELAYED_WORK(&priv->ready_work, airoha_xpon_phy_ready_work);

	/*
	 * These signals are part of the xPON PHY electrical interface, not of
	 * the LDD/LA device.  Request them fail-safe with both disable signals
	 * asserted until phy_power_on() has configured the digital PHY.
	 */
	priv->tx_disable_gpio =
		devm_gpiod_get(dev, "tx-disable", GPIOD_OUT_HIGH);
	if (IS_ERR(priv->tx_disable_gpio))
		return dev_err_probe(dev, PTR_ERR(priv->tx_disable_gpio),
				     "failed to get TX disable GPIO\n");

	priv->vcc_disable_gpio =
		devm_gpiod_get_optional(dev, "vcc-disable", GPIOD_OUT_HIGH);
	if (IS_ERR(priv->vcc_disable_gpio))
		return dev_err_probe(dev, PTR_ERR(priv->vcc_disable_gpio),
				     "failed to get VCC disable GPIO\n");

	if (soc->pma_offset)
		res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "pcs");
	else
		res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return dev_err_probe(dev, -EINVAL,
				     "missing xPON PHY register resource\n");
	if (resource_size(res) < soc->min_size)
		return dev_err_probe(dev, -EINVAL,
				     "%s xPON PHY resource %pR is too small\n",
				     soc->name, res);

	priv->base = devm_ioremap_resource(dev, res);
	if (IS_ERR(priv->base))
		return PTR_ERR(priv->base);
	if (soc->pma_offset) {
		struct resource *pma_res;

		/* Keep the intervening PCIe PHY window out of our reservation. */
		pma_res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "pma");
		if (!pma_res)
			return dev_err_probe(dev, -EINVAL,
					     "missing xPON PMA register resource\n");
		if (resource_size(pma_res) < SZ_4K)
			return dev_err_probe(dev, -EINVAL,
					     "xPON PMA resource %pR is too small\n", pma_res);
		priv->pma_base = devm_ioremap_resource(dev, pma_res);
		if (IS_ERR(priv->pma_base))
			return PTR_ERR(priv->pma_base);
	}

	if (soc->ana_offset) {
		struct resource *ana_res;

		ana_res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "ana");
		if (!ana_res || resource_size(ana_res) < SZ_4K)
			return dev_err_probe(dev, -EINVAL,
					     "missing or undersized xPON ANA resource\n");
		priv->ana_base = devm_ioremap_resource(dev, ana_res);
		if (IS_ERR(priv->ana_base))
			return PTR_ERR(priv->ana_base);
	}

	priv->scu = syscon_regmap_lookup_by_phandle(dev->of_node,
						    "airoha,scu");
	if (IS_ERR(priv->scu))
		return dev_err_probe(dev, PTR_ERR(priv->scu),
				     "failed to get SCU regmap\n");

	priv->reset = devm_reset_control_get_exclusive(dev, "phy");
	if (IS_ERR(priv->reset))
		return dev_err_probe(dev, PTR_ERR(priv->reset),
				     "failed to get xPON PHY reset\n");

	if (soc->probe) {
		ret = soc->probe(priv, pdev);
		if (ret)
			return ret;
	}

	if (!soc->ops) {
		ret = devm_add_action_or_reset(dev, airoha_xpon_phy_cleanup, priv);
		if (ret)
			return ret;
	}

	/* Hold the backend module while a consumer owns this PHY. */
	priv->phy_ops = *(soc->ops ?: &airoha_xpon_phy_ops);
	priv->phy_ops.owner = dev->driver->owner;
	phy = devm_phy_create(dev, NULL, &priv->phy_ops);
	if (IS_ERR(phy))
		return dev_err_probe(dev, PTR_ERR(phy),
				     "failed to create xPON PHY\n");

	phy_set_drvdata(phy, priv);
	platform_set_drvdata(pdev, priv);

	provider = devm_of_phy_provider_register(dev, of_phy_simple_xlate);
	if (IS_ERR(provider))
		return dev_err_probe(dev, PTR_ERR(provider),
				     "failed to register xPON PHY provider\n");

	dev_info(dev, "%s xPON PHY registered at %pR\n", soc->name, res);
	return 0;
}
EXPORT_SYMBOL_GPL(airoha_xpon_phy_probe);

void airoha_xpon_phy_shutdown(struct platform_device *pdev)
{
	struct airoha_xpon_phy *priv = platform_get_drvdata(pdev);

	if (priv->soc->shutdown)
		priv->soc->shutdown(priv);
	else
		airoha_xpon_phy_cleanup(priv);
}
EXPORT_SYMBOL_GPL(airoha_xpon_phy_shutdown);

MODULE_DESCRIPTION("Airoha shared xPON PHY core");
MODULE_AUTHOR("Matheus Sampaio Queiroga <srherobrine20@gmail.com>");
MODULE_LICENSE("GPL");
