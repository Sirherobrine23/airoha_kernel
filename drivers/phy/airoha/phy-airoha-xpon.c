// SPDX-License-Identifier: GPL-2.0-only
/*
 * Airoha/EcoNet xPON PHY driver
 *
 * EN7523 and EN751221 share the digital GPON/EPON register block at
 * 0x1faf0000.  The EN7523 generation also integrates the PMA/SerDes
 * controls used by the EN7571 optical front end, while EN751221 uses the
 * older SoC-specific PHY bring-up sequence. EN7580 has a separate 10G
 * PCS/PMA backend for XGS-PON.
 */

#include <linux/bitfield.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/iopoll.h>
#include <linux/mfd/syscon.h>
#include <linux/mutex.h>
#include <linux/nvmem-consumer.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/phy/phy.h>
#include <linux/phy/phy-airoha-xpon.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/reset.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/workqueue.h>

#define ECONET_XPON_PHY_MIN_SIZE		0x0600
#define EN7523_XPON_PHY_MIN_SIZE		0x480c

#define ECONET_SCU_PHY_CTRL0		0x860
#define ECONET_SCU_PHY_CTRL0_DIS	BIT(10)
#define ECONET_SCU_PHY_CTRL1		0x92c
#define ECONET_SCU_PHY_CTRL1_DIS	BIT(2)

#define EN7523_SCU_WAN_CONF		0x070
#define EN7523_SCU_WAN_MODE_MASK	GENMASK(7, 0)
#define EN7528_SCU_WAN_MODE_MASK	GENMASK(2, 0)
#define EN7528_SCU_REG_284		0x284
#define EN7528_SCU_REG_284_KEEP_REG580	BIT(9)
#define EN7523_SCU_WAN_MODE_GPON	0x00
#define EN7523_SCU_WAN_MODE_EPON	0x01
#define EN7523_SCU_IOMUX_CTRL_3		0x218
#define EN7523_SCU_IOMUX_PON_EN		BIT(0)
#define ECONET_XPON_TDCSET2		0x2d
#define ECONET_XPON_SETTING_EN757X	0x10f

#define XPON_PHYSET1			0x0100
#define XPON_PHYFWREADY			0x0104
#define XPON_PHYSET3			0x0108
#define XPON_PHYSET5			0x0110
#define XPON_PHYSET10			0x0124
#define XPON_PHYSTA1			0x0130
#define XPON_SETTING			0x0138
#define XPON_TDCSET2			0x01f8
#define XPON_RX_FEC_STATUS		0x021c
#define XPON_RX_COUNTER_ENABLE		0x0230
#define XPON_RX_COUNTER_CTRL		0x0234
#define XPON_RX_BIP_COUNTER		0x024c
#define XPON_RX_BIP_LATCH		BIT(2)
#define XPON_RX_BIP_CLEAR		BIT(3)
#define XPON_RX_COUNTER_CTRL2		0x0298
#define XPON_GPON_PREAMBLE		0x0400
#define XPON_GPON_DELIMITER_GUARD	0x0404
#define XPON_GPON_EXT_PREAMBLE		0x0408
#define XPON_TX_FEC_STATUS		0x040c
#define XPON_GPON_TX_COUNTER_CTRL	0x0424
#define XPON_GPON_TX_FRAME_COUNTER	0x0434
#define XPON_GPON_TX_BURST_COUNTER	0x0438
#define XPON_BISTCTL_LOOPBACK_SEL	0x04a0
#define XPON_BISTCTL_PRBS_TX_EN		0x04a4
#define XPON_TRANS_STATUS		0x05e0
#define XPON_INT_ENABLE			0x05f0
#define XPON_INT_STATUS_CLR		0x05f4
#define XPON_INT_STATUS			0x05f8
#define EN7528_XPON_REG_580		0x0580

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
#define XPON_GPON_TX_BIT_DELAY		0x433c
#define XPON_RX_MODE_CTRL		0x4344
#define XPON_CDR_CTRL			0x4530
#define XPON_FREQ_CTRL			0x4608
#define XPON_PMA_INT_STATUS		0x4800
#define XPON_PMA_INT_ENABLE		0x4804
#define XPON_PMA_INT_STATUS_CLR		0x4808

#define XPON_PHYSET1_TX_LOCK_REF	BIT(24)
#define XPON_PHYFWREADY_READY		BIT(0)
#define XPON_PHYSET3_PLL_RST		BIT(31)
#define XPON_PHYSET3_COUNTER_RST	BIT(27)
#define XPON_PHYSET10_GPON		BIT(31)
#define XPON_PHYSTA1_STATE_MASK		GENMASK(20, 18)
#define XPON_PHYSTA1_SYNCING		2
#define XPON_PHYSTA1_READY		6
#define XPON_RX_SYNC_MASK		GENMASK(3, 0)
#define XPON_RX_SYNC_READY		0x0a
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
#define XPON_GPON_TX_BIT_DELAY_MASK	GENMASK(7, 0)
#define XPON_PHYSET5_BIT_DELAY_MASK	GENMASK(21, 19)
#define XPON_PHYSET5_BIT_DELAY_EN	BIT(23)

#define XPON_GPON_TX_ENABLE_PATTERN	0xaa
#define XPON_GPON_TX_COUNTER_ENABLE	BIT(3)
#define XPON_BIST_PRBS23		0x06

#define ECONET_XPON_COUNTER_ENABLE_MASK	GENMASK(2, 0)
#define ECONET_XPON_COUNTER_CLEAR_RX0	BIT(0)
#define ECONET_XPON_COUNTER_CLEAR_RX1	BIT(1)
#define ECONET_XPON_COUNTER_CLEAR_RX2	BIT(2)
#define ECONET_XPON_COUNTER_CLEAR_GPON_TX	BIT(3)
#define ECONET_XPON_COUNTER_CLEAR_RX3	BIT(4)
#define ECONET_XPON_COUNTER_CLEAR_ALL	GENMASK(4, 0)
#define XPON_FEC_STATUS_ACTIVE	BIT(15)

#define XPON_SETTING_EN7571		0x0000014f
/*
 * XPON_SETTING also selects the pin conventions of the attached optical
 * transceiver.  Each of these bits inverts one signal, so RX_SD_INV turns a
 * receive signal-detect input into a receive loss-of-signal input.  The right
 * combination is a property of the board's optics rather than of the SoC, so
 * it is described in the devicetree.
 */
#define XPON_SETTING_TX_SD_INV		BIT(4)
#define XPON_SETTING_TX_FAULT_INV	BIT(5)
#define XPON_SETTING_RX_SD_INV		BIT(6)
#define XPON_SETTING_BURST_EN_INV	BIT(7)
#define XPON_SETTING_INV_MASK		(XPON_SETTING_TX_SD_INV | \
					 XPON_SETTING_TX_FAULT_INV | \
					 XPON_SETTING_RX_SD_INV | \
					 XPON_SETTING_BURST_EN_INV)
#define XPON_TDCSET2_EN7571		0x0000002d
#define XPON_GPON_DELIMITER_DEFAULT	0xaaab5983
#define XPON_READY_RECOVERY_MS		5000

/* EN7580 PCS and PON PMA; offsets are relative to 0x1faf0000. */
#define EN7580_XPON_PHY_PCS_MIN_SIZE	0x1000
#define EN7580_XPON_PHY_PMA_MIN_SIZE	0x1000
#define EN7580_XPON_PHY_PMA_OFFSET		0x3000
#define EN7580_PHY_RX_CTRL			0x0a04
#define EN7580_PHY_PCS_RESET			0x0a0c
#define EN7580_PHY_PCS_INT_ENABLE		0x0a14
#define EN7580_PHY_PCS_DEBUG			0x0a84
#define EN7580_PHY_PCS_SYNC			0x0b1c
#define EN7580_PHY_PHYA_INT_ENABLE		0x0b44
#define EN7580_PHY_SFP_LEVEL			0x0b48
#define EN7580_PHY_SFP_STATUS			0x0b4c
#define EN7580_PHY_PHYA_READY			0x0b54
#define EN7580_PHY_RX_IMPEDANCE			0x312c
#define EN7580_PHY_TX_FIR			0x3148
#define EN7580_PHY_EYE_INDEX2			0x3308
#define EN7580_PHY_EYE_COUNT0			0x3330
#define EN7580_PHY_EYE_COUNT1			0x3334
#define EN7580_PHY_EQ_CTRL0			0x3370
#define EN7580_PHY_PI_CAL			0x3430
#define EN7580_PHY_RX_DEBUG			0x349c
#define EN7580_PHY_EYE_READY			0x3538
#define EN7580_PHY_EYE_DONE			0x3548
#define EN7580_PHY_EYE_WIDTH			0x354c
#define EN7580_PHY_TX_CALIB0			0x3554
#define EN7580_PHY_TX_CALIB1			0x3558
#define EN7580_PHY_PMA_SETTING0			0x3600
#define EN7580_PHY_PMA_INT_ENABLE0		0x3610
#define EN7580_PHY_PMA_INT_ENABLE1		0x3614
#define EN7580_PHY_RX_FORCE0			0x3630
#define EN7580_PHY_RX_DISB0			0x363c
#define EN7580_PHY_RX_DISB2			0x3644
#define EN7580_PHY_RX_FORCE3			0x3648
#define EN7580_PHY_RX_DISB3			0x3658
#define EN7580_PHY_RX_FORCE9			0x366c
#define EN7580_PHY_RX_DISB7			0x3674
#define EN7580_PHY_RX_DISB8			0x3678
#define EN7580_PHY_PMA_MODE			0x3754
#define EN7580_PHY_TX_PLL_STATUS		0x3760
#define EN7580_PHY_PMA_RESET			0x37b0
#define EN7580_PHY_TX_DELAY			0x37b8
#define EN7580_PHY_RX_FREQ_STATUS		0x3820
#define EN7580_PHY_MEM_CLK			0x38a0
#define EN7580_PHY_DELAY			0xffff
#define EN7580_PHY_EYE_VALID		(BIT(16) | BIT(24))

struct en7580_phy_step {
	u16 reg;
	u32 mask;
	u32 value;
};

#include "phy-airoha-en7580-seq.h"

struct airoha_xpon_phy;

struct airoha_xpon_phy_soc_data {
	const char *name;
	u32 min_size;
	u32 gpon_bit_delay_reg;
	u32 gpon_bit_delay_mask;
	u32 gpon_bit_delay_enable;
	bool has_integrated_pma;
	bool manages_fw_ready;
	bool has_gpon_bip;
	bool has_xgspon;
	const struct phy_ops *ops;
	int (*configure)(struct airoha_xpon_phy *priv);
};

struct airoha_xpon_phy {
	struct device *dev;
	const struct airoha_xpon_phy_soc_data *soc;
	void __iomem *base;
	void __iomem *pma_base;
	struct regmap *scu;
	struct gpio_desc *tx_disable_gpio;
	struct gpio_desc *vcc_disable_gpio;
	struct reset_control *reset;
	u32 trans_invert;
	enum airoha_xpon_phy_submode submode;
	struct delayed_work ready_work;
	bool initialized;
	bool powered;
	bool ready_reported;
	/* Serializes the EN7580 calibration, LOS recovery and TX requests. */
	struct mutex xgspon_lock;
	bool xgspon_configured;
	bool xgspon_calibrated;
	bool xgspon_rx_parked;
	bool xgspon_tx_requested;
	u8 rx_impedance;
	u8 txp_impedance;
	u8 txn_impedance;
	u32 last_tx_pll_status;
	int last_power_on_error;
	bool last_tx_pll_valid;
	int last_rx_error;
	u32 last_eye_done;
	u32 last_eye_ready;
	u8 last_eye_peaking;
	bool last_eye_valid;
	/* Serializes the RX counter command register (latch/clear). */
	spinlock_t counter_lock;
};

static u32 airoha_xpon_phy_read(struct airoha_xpon_phy *priv, u32 reg)
{
	if (priv->soc->has_xgspon && reg >= EN7580_XPON_PHY_PMA_OFFSET)
		return readl(priv->pma_base + reg - EN7580_XPON_PHY_PMA_OFFSET);
	return readl(priv->base + reg);
}

static void airoha_xpon_phy_write(struct airoha_xpon_phy *priv, u32 reg,
				  u32 val)
{
	if (priv->soc->has_xgspon && reg >= EN7580_XPON_PHY_PMA_OFFSET)
		writel(val, priv->pma_base + reg - EN7580_XPON_PHY_PMA_OFFSET);
	else
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

static bool airoha_xpon_phy_fw_ready(struct airoha_xpon_phy *priv)
{
	return airoha_xpon_phy_read(priv, XPON_PHYFWREADY) &
		XPON_PHYFWREADY_READY;
}

static bool airoha_xpon_phy_ready(struct airoha_xpon_phy *priv)
{
	if (priv->soc->has_xgspon)
		return priv->xgspon_calibrated && !priv->xgspon_rx_parked &&
			(airoha_xpon_phy_read(priv, EN7580_PHY_PHYA_READY) & BIT(0)) &&
			(airoha_xpon_phy_read(priv, EN7580_PHY_RX_FREQ_STATUS) & BIT(0)) &&
			(airoha_xpon_phy_read(priv, EN7580_PHY_TX_PLL_STATUS) & BIT(16)) &&
			(airoha_xpon_phy_read(priv, EN7580_PHY_PCS_SYNC) & BIT(1));

	return airoha_xpon_phy_state(priv) == XPON_PHYSTA1_READY;
}

static u32 airoha_xpon_phy_rx_sync(struct airoha_xpon_phy *priv)
{
	return FIELD_GET(XPON_RX_SYNC_MASK,
			 airoha_xpon_phy_read(priv, XPON_RX_FEC_STATUS));
}

static bool airoha_xpon_phy_los(struct airoha_xpon_phy *priv)
{
	if (priv->soc->has_xgspon)
		return airoha_xpon_phy_read(priv, EN7580_PHY_SFP_STATUS) & BIT(0);

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

	if (priv->soc->has_xgspon)
		mutex_lock(&priv->xgspon_lock);
	if (!READ_ONCE(priv->powered)) {
		*ready = false;
		*los = true;
	} else {
		*los = airoha_xpon_phy_los(priv);
		*ready = airoha_xpon_phy_ready(priv);
		if (priv->soc->has_xgspon)
			*ready &= !*los;
	}
	if (priv->soc->has_xgspon)
		mutex_unlock(&priv->xgspon_lock);

	return 0;
}
EXPORT_SYMBOL_GPL(airoha_xpon_phy_get_link_state);

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
			 "EN751221 status: fw_ready=%u rx_sync=%#x rx_synced=%u counters=%#010x/%#010x/%#010x/%#010x\n",
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

static void
airoha_xpon_phy_set_tx_gpio(struct airoha_xpon_phy *priv, bool enable)
{
	if (!priv->tx_disable_gpio)
		return;

	/* TX_DISABLE is described using its asserted (disable) polarity. */
	gpiod_set_value_cansleep(priv->tx_disable_gpio, !enable);
}

int airoha_xpon_phy_set_tx_enable(struct phy *phy, bool enable)
{
	struct airoha_xpon_phy *priv;

	if (!phy)
		return -EINVAL;

	priv = phy_get_drvdata(phy);
	if (!priv)
		return -ENODEV;

	if (priv->soc->has_xgspon) {
		mutex_lock(&priv->xgspon_lock);
		if (enable && !priv->powered) {
			mutex_unlock(&priv->xgspon_lock);
			return -EAGAIN;
		}
		/* A request survives LOS, but only a synchronized RX permits TX. */
		priv->xgspon_tx_requested = enable;
		enable = enable && priv->powered &&
			!airoha_xpon_phy_los(priv) && airoha_xpon_phy_ready(priv);
		airoha_xpon_phy_set_tx_gpio(priv, enable);
		mutex_unlock(&priv->xgspon_lock);
	} else {
		airoha_xpon_phy_set_tx_gpio(priv, enable);
	}
	return 0;
}
EXPORT_SYMBOL_GPL(airoha_xpon_phy_set_tx_enable);

static void
airoha_xpon_phy_set_vcc_enabled(struct airoha_xpon_phy *priv, bool enable)
{
	if (!priv->vcc_disable_gpio)
		return;

	/* VCC_DISABLE follows the same logical-disable convention. */
	gpiod_set_value_cansleep(priv->vcc_disable_gpio, !enable);
}

/* EN7580 XGS-PON uses its own PCS/PMA lifecycle and never the 1G registers. */
static void en7580_phy_sequence(struct airoha_xpon_phy *priv,
				const struct en7580_phy_step *seq, size_t count)
{
	size_t i;

	for (i = 0; i < count; i++) {
		if (seq[i].reg == EN7580_PHY_DELAY) {
			if (seq[i].value < 10)
				udelay(seq[i].value);
			else
				usleep_range(seq[i].value,
					     seq[i].value + max(1U, seq[i].value / 10));
			continue;
		}
		airoha_xpon_phy_rmw(priv, seq[i].reg, seq[i].mask, seq[i].value);
	}
	/* Complete all posted calibration/reset writes before proceeding. */
	airoha_xpon_phy_read(priv, EN7580_PHY_PMA_RESET);
}

#define en7580_phy_run(priv, name) \
	en7580_phy_sequence(priv, en7580_##name##_seq, \
			    ARRAY_SIZE(en7580_##name##_seq))

static void en7580_phy_pcs_reset(struct airoha_xpon_phy *priv)
{
	airoha_xpon_phy_write(priv, EN7580_PHY_PCS_RESET, 0);
	airoha_xpon_phy_read(priv, EN7580_PHY_PCS_RESET);
	usleep_range(1000, 1100);
	airoha_xpon_phy_write(priv, EN7580_PHY_PCS_RESET, 3);
	airoha_xpon_phy_read(priv, EN7580_PHY_PCS_RESET);
	usleep_range(1000, 1100);
}

static void en7580_phy_mask_interrupts(struct airoha_xpon_phy *priv)
{
	airoha_xpon_phy_write(priv, EN7580_PHY_PCS_INT_ENABLE, 0);
	airoha_xpon_phy_write(priv, EN7580_PHY_PHYA_INT_ENABLE, 0);
	airoha_xpon_phy_write(priv, EN7580_PHY_PMA_INT_ENABLE0, 0);
	airoha_xpon_phy_write(priv, EN7580_PHY_PMA_INT_ENABLE1, 0);
}

static void en7580_phy_quiesce(struct airoha_xpon_phy *priv)
{
	en7580_phy_mask_interrupts(priv);
	airoha_xpon_phy_rmw(priv, EN7580_PHY_RX_CTRL, BIT(16), 0);
	airoha_xpon_phy_write(priv, EN7580_PHY_PCS_RESET, 0);
	airoha_xpon_phy_rmw(priv, EN7580_PHY_PMA_RESET, GENMASK(4, 0), 0);
	airoha_xpon_phy_read(priv, EN7580_PHY_PMA_RESET);
}

static u32 en7580_phy_eye_status(struct airoha_xpon_phy *priv)
{
	/* Debug results are latched: refresh the latch on every poll. */
	airoha_xpon_phy_rmw(priv, EN7580_PHY_RX_DEBUG, BIT(24), 0);
	airoha_xpon_phy_rmw(priv, EN7580_PHY_RX_DEBUG, BIT(24), BIT(24));
	priv->last_eye_done = airoha_xpon_phy_read(priv, EN7580_PHY_EYE_DONE);
	priv->last_eye_ready = airoha_xpon_phy_read(priv, EN7580_PHY_EYE_READY);
	priv->last_eye_valid = true;
	return priv->last_eye_done & priv->last_eye_ready;
}

static void en7580_phy_eye_stop(struct airoha_xpon_phy *priv)
{
	airoha_xpon_phy_rmw(priv, EN7580_PHY_RX_FORCE9, BIT(8), BIT(8));
	airoha_xpon_phy_rmw(priv, EN7580_PHY_RX_DISB8, BIT(16), 0);
	airoha_xpon_phy_rmw(priv, EN7580_PHY_RX_FORCE9, BIT(16), 0);
	airoha_xpon_phy_rmw(priv, EN7580_PHY_RX_DISB3, BIT(0), 0);
	airoha_xpon_phy_rmw(priv, EN7580_PHY_RX_FORCE3, BIT(0), 0);
}

static int en7580_phy_eye_scan(struct airoha_xpon_phy *priv)
{
	u32 value, left, right, score, best_score = 0;
	unsigned int peaking, best_peaking = 0;
	int ret;

	priv->last_eye_valid = false;
	en7580_phy_run(priv, eye_setup);
	airoha_xpon_phy_rmw(priv, EN7580_PHY_EQ_CTRL0, GENMASK(7, 0), 0x80);
	airoha_xpon_phy_rmw(priv, EN7580_PHY_PI_CAL, GENMASK(10, 8), 4 << 8);
	airoha_xpon_phy_rmw(priv, EN7580_PHY_RX_DISB0, BIT(0), 0);
	airoha_xpon_phy_rmw(priv, EN7580_PHY_RX_FORCE0, GENMASK(1, 0), 1);

	/* SDK EO_Scan(0, 10, 0, 7, 0): choose the widest horizontal eye. */
	for (peaking = 0; peaking < 8; peaking++) {
		if (airoha_xpon_phy_los(priv)) {
			ret = -ENOLINK;
			goto err_stop;
		}
		priv->last_eye_peaking = peaking;
		airoha_xpon_phy_rmw(priv, EN7580_PHY_RX_DISB0, BIT(8), 0);
		airoha_xpon_phy_rmw(priv, EN7580_PHY_RX_FORCE0, GENMASK(10, 8),
				     peaking << 8);
		en7580_phy_run(priv, eye_cal);

		airoha_xpon_phy_rmw(priv, EN7580_PHY_EYE_COUNT0,
				     GENMASK(9, 0), 0xa);
		airoha_xpon_phy_rmw(priv, EN7580_PHY_EYE_INDEX2,
				     GENMASK(19, 0), 0x44c);
		airoha_xpon_phy_rmw(priv, EN7580_PHY_RX_DISB8, BIT(8), 0);
		airoha_xpon_phy_rmw(priv, EN7580_PHY_RX_FORCE9, BIT(8), BIT(8));
		airoha_xpon_phy_rmw(priv, EN7580_PHY_RX_FORCE9, BIT(8), 0);
		airoha_xpon_phy_rmw(priv, EN7580_PHY_RX_DISB8, BIT(16), 0);
		airoha_xpon_phy_rmw(priv, EN7580_PHY_RX_FORCE9, BIT(16), 0);
		airoha_xpon_phy_rmw(priv, EN7580_PHY_RX_FORCE9, BIT(16), BIT(16));
		usleep_range(5500, 6000);
		airoha_xpon_phy_rmw(priv, EN7580_PHY_RX_DISB2,
				     BIT(8) | BIT(24), BIT(8) | BIT(24));
		airoha_xpon_phy_rmw(priv, EN7580_PHY_EYE_COUNT1, BIT(8), BIT(8));
		airoha_xpon_phy_rmw(priv, EN7580_PHY_RX_DISB7, BIT(24), BIT(24));
		airoha_xpon_phy_rmw(priv, EN7580_PHY_EYE_COUNT1, BIT(0), BIT(0));
		ret = read_poll_timeout(en7580_phy_eye_status, value,
					(value & EN7580_PHY_EYE_VALID) == EN7580_PHY_EYE_VALID,
					100, 10000, false, priv);
		if (airoha_xpon_phy_los(priv)) {
			ret = -ENOLINK;
			goto err_stop;
		}

		if (ret)
			goto err_stop;

		value = airoha_xpon_phy_read(priv, EN7580_PHY_EYE_WIDTH);
		left = FIELD_GET(GENMASK(26, 16), value);
		right = FIELD_GET(GENMASK(10, 0), value);
		score = left > right ? left - right : right - left;
		if (score > best_score) {
			best_score = score;
			best_peaking = peaking;
		}
		en7580_phy_eye_stop(priv);
	}

	if (!best_score) {
		ret = -EIO;
		goto err_stop;
	}

	airoha_xpon_phy_rmw(priv, EN7580_PHY_RX_DISB0, BIT(0) | BIT(8), 0);
	airoha_xpon_phy_rmw(priv, EN7580_PHY_RX_FORCE0,
			     GENMASK(1, 0) | GENMASK(10, 8),
			     1 | (best_peaking << 8));
	dev_dbg(priv->dev, "XGS-PON eye width=%u peaking=%u\n",
		best_score, best_peaking);
	return 0;

err_stop:
	en7580_phy_eye_stop(priv);
	return ret;
}

static int en7580_phy_connect(struct airoha_xpon_phy *priv)
{
	int ret;

	airoha_xpon_phy_set_tx_gpio(priv, false);
	if (!priv->xgspon_calibrated) {
		/* Re-run the complete RX calibration after an interrupted eye scan. */
		en7580_phy_run(priv, rx_cal);
		airoha_xpon_phy_rmw(priv, EN7580_PHY_RX_FORCE0,
				     BIT(24) | BIT(16), BIT(24) | BIT(16));
		usleep_range(200, 250);
		ret = en7580_phy_eye_scan(priv);
		if (ret)
			return ret;
		en7580_phy_run(priv, rx_finish);
		airoha_xpon_phy_rmw(priv, EN7580_PHY_RX_IMPEDANCE,
				     GENMASK(26, 25), priv->rx_impedance << 25);
		priv->xgspon_calibrated = true;
	} else {
		en7580_phy_run(priv, rx_connect);
	}
	/* Normal PMA data, with loopback and BIST paths disabled. */
	airoha_xpon_phy_rmw(priv, EN7580_PHY_PMA_MODE, BIT(16) | BIT(8), 0);
	en7580_phy_pcs_reset(priv);
	airoha_xpon_phy_rmw(priv, EN7580_PHY_RX_CTRL, BIT(16), BIT(16));
	usleep_range(8000, 9000);
	return 0;
}

static void en7580_phy_ready_work(struct work_struct *work)
{
	struct airoha_xpon_phy *priv =
		container_of(to_delayed_work(work), struct airoha_xpon_phy, ready_work);
	bool ready, los;
	int ret;

	mutex_lock(&priv->xgspon_lock);
	if (!priv->powered)
		goto out;
	los = airoha_xpon_phy_los(priv);
	ready = !los && airoha_xpon_phy_ready(priv);
	if (los) {
		airoha_xpon_phy_set_tx_gpio(priv, false);
		if (!priv->xgspon_rx_parked) {
			en7580_phy_run(priv, rx_disconnect);
			airoha_xpon_phy_rmw(priv, EN7580_PHY_RX_CTRL, BIT(16), 0);
			priv->xgspon_rx_parked = true;
		}
	} else if (priv->xgspon_rx_parked || !ready) {
		ret = en7580_phy_connect(priv);
		priv->last_rx_error = ret;
		if (ret) {
			dev_warn_ratelimited(priv->dev,
					     "XGS-PON RX recovery failed: %d\n", ret);
			en7580_phy_run(priv, rx_disconnect);
			airoha_xpon_phy_rmw(priv, EN7580_PHY_RX_CTRL, BIT(16), 0);
			priv->xgspon_rx_parked = true;
		} else {
			priv->xgspon_rx_parked = false;
		}
	}
	ready = !airoha_xpon_phy_los(priv) && airoha_xpon_phy_ready(priv);
	if (ready != priv->ready_reported) {
		priv->ready_reported = ready;
		dev_info(priv->dev, "XGS-PON receiver %s\n", ready ? "ready" : "not ready");
	}
	airoha_xpon_phy_set_tx_gpio(priv, ready && priv->xgspon_tx_requested);
	mod_delayed_work(system_wq, &priv->ready_work, msecs_to_jiffies(1000));
out:
	mutex_unlock(&priv->xgspon_lock);
}

static int en7580_phy_power_off_priv(struct airoha_xpon_phy *priv)
{
	int ret;

	mutex_lock(&priv->xgspon_lock);
	WRITE_ONCE(priv->powered, false);
	priv->xgspon_tx_requested = false;
	airoha_xpon_phy_set_tx_gpio(priv, false);
	mutex_unlock(&priv->xgspon_lock);
	cancel_delayed_work_sync(&priv->ready_work);
	mutex_lock(&priv->xgspon_lock);
	if (priv->xgspon_configured)
		en7580_phy_quiesce(priv);
	ret = reset_control_assert(priv->reset);
	priv->xgspon_configured = false;
	priv->xgspon_calibrated = false;
	priv->ready_reported = false;
	airoha_xpon_phy_set_vcc_enabled(priv, false);
	mutex_unlock(&priv->xgspon_lock);
	return ret;
}

static void en7580_phy_shutdown(void *data)
{
	struct airoha_xpon_phy *priv = data;
	int ret = en7580_phy_power_off_priv(priv);

	if (ret)
		dev_warn(priv->dev, "failed to hold XGS-PON PHY reset: %d\n", ret);
}

static int en7580_phy_init(struct phy *phy)
{
	struct airoha_xpon_phy *priv = phy_get_drvdata(phy);

	/* Configuration and reset release are deferred to phy_power_on(). */
	mutex_lock(&priv->xgspon_lock);
	priv->initialized = true;
	mutex_unlock(&priv->xgspon_lock);
	return 0;
}

static int en7580_phy_exit(struct phy *phy)
{
	struct airoha_xpon_phy *priv = phy_get_drvdata(phy);
	int ret;

	ret = en7580_phy_power_off_priv(priv);
	mutex_lock(&priv->xgspon_lock);
	priv->initialized = false;
	mutex_unlock(&priv->xgspon_lock);
	return ret;
}

static int en7580_phy_set_mode(struct phy *phy, enum phy_mode mode, int submode)
{
	struct airoha_xpon_phy *priv = phy_get_drvdata(phy);

	if (mode != PHY_MODE_ETHERNET || submode != AIROHA_XPON_PHY_SUBMODE_XGSPON)
		return -EOPNOTSUPP;
	/* Live changes are owned by the MAC/core and require a stopped PHY. */
	if (priv->powered)
		return -EBUSY;
	priv->submode = submode;
	return 0;
}

static int en7580_phy_power_on(struct phy *phy)
{
	struct airoha_xpon_phy *priv = phy_get_drvdata(phy);
	u32 value;
	int ret;

	if (!priv->initialized)
		return -EINVAL;
	mutex_lock(&priv->xgspon_lock);
	priv->last_power_on_error = 0;
	priv->last_tx_pll_valid = false;
	priv->last_rx_error = 0;
	priv->last_eye_valid = false;
	airoha_xpon_phy_set_tx_gpio(priv, false);
	priv->xgspon_tx_requested = false;
	airoha_xpon_phy_set_vcc_enabled(priv, true);
	ret = reset_control_assert(priv->reset);
	if (ret)
		goto err_power;
	ret = regmap_update_bits(priv->scu, EN7523_SCU_WAN_CONF,
				 EN7523_SCU_WAN_MODE_MASK, 0x0a);
	if (ret)
		goto err_power;
	ret = reset_control_deassert(priv->reset);
	if (ret)
		goto err_power;
	usleep_range(1000, 1100);
	ret = regmap_clear_bits(priv->scu, ECONET_SCU_PHY_CTRL1,
				ECONET_SCU_PHY_CTRL1_DIS);
	if (ret)
		goto err_power;
	en7580_phy_run(priv, init);
	priv->xgspon_configured = true;
	en7580_phy_mask_interrupts(priv);
	airoha_xpon_phy_rmw(priv, EN7580_PHY_RX_CTRL, BIT(16), 0);
	airoha_xpon_phy_write(priv, EN7580_PHY_PCS_RESET, 0);
	airoha_xpon_phy_rmw(priv, EN7580_PHY_TX_CALIB0,
			     GENMASK(25, 24) | BIT(16),
			     (priv->txp_impedance << 24) | BIT(16));
	airoha_xpon_phy_rmw(priv, EN7580_PHY_TX_CALIB1,
			     GENMASK(25, 24) | BIT(16),
			     (priv->txn_impedance << 24) | BIT(16));
	/*
	 * The vendor also programs neighboring JCPLL/XFI fields. Those remain
	 * outside this PHY's resource; their electrical clock relationship is
	 * not established by the register overlap alone.
	 */
	ret = read_poll_timeout(airoha_xpon_phy_read, value, value & BIT(16),
				100, 100000, false, priv, EN7580_PHY_TX_PLL_STATUS);
	priv->last_tx_pll_status = value;
	priv->last_tx_pll_valid = true;
	if (ret) {
		dev_err(priv->dev,
			"XGS-PON TX PLL did not lock (status %#08x)\n", value);
		goto err_power;
	}
	airoha_xpon_phy_rmw(priv, EN7580_PHY_PCS_DEBUG, BIT(0) | BIT(1), 0);
	airoha_xpon_phy_write(priv, EN7580_PHY_TX_FIR, 0xc11a5c20);
	airoha_xpon_phy_rmw(priv, EN7580_PHY_MEM_CLK, GENMASK(1, 0), 3);
	airoha_xpon_phy_rmw(priv, EN7580_PHY_TX_DELAY, GENMASK(30, 28), BIT(28));
	/* Map the existing board polarity properties onto the 10G status inputs. */
	value = !(priv->trans_invert & XPON_SETTING_RX_SD_INV) * BIT(0) |
		!!(priv->trans_invert & XPON_SETTING_TX_SD_INV) * BIT(1) |
		!!(priv->trans_invert & XPON_SETTING_TX_FAULT_INV) * BIT(2);
	airoha_xpon_phy_rmw(priv, EN7580_PHY_SFP_LEVEL, GENMASK(2, 0), value);
	value = !!(priv->trans_invert & XPON_SETTING_BURST_EN_INV) * BIT(8) |
		!!(priv->trans_invert & XPON_SETTING_TX_FAULT_INV) * BIT(24);
	airoha_xpon_phy_rmw(priv, EN7580_PHY_PMA_SETTING0, BIT(8) | BIT(24), value);

	priv->xgspon_calibrated = false;
	priv->xgspon_rx_parked = true;
	en7580_phy_run(priv, rx_disconnect);
	/*
	 * RX calibration depends on the optical signal, not PHY power validity.
	 * Let the worker retry it while keeping local clocks on and TX disabled.
	 */
	WRITE_ONCE(priv->powered, true);
	priv->ready_reported = false;
	mod_delayed_work(system_wq, &priv->ready_work, msecs_to_jiffies(1000));
	mutex_unlock(&priv->xgspon_lock);
	dev_info(priv->dev, "EN7580 XGS-PON PHY powered; optical TX remains disabled\n");
	return 0;

err_power:
	priv->last_power_on_error = ret;
	if (priv->xgspon_configured)
		en7580_phy_quiesce(priv);
	reset_control_assert(priv->reset);
	priv->xgspon_configured = false;
	priv->xgspon_calibrated = false;
	airoha_xpon_phy_set_vcc_enabled(priv, false);
	mutex_unlock(&priv->xgspon_lock);
	return ret;
}

static int en7580_phy_power_off(struct phy *phy)
{
	return en7580_phy_power_off_priv(phy_get_drvdata(phy));
}

static int en7580_phy_status_show(struct seq_file *s, void *unused)
{
	struct airoha_xpon_phy *priv = s->private;
	static const struct {
		u16 reg;
		const char *name;
	} registers[] = {
		{ 0x312c, "rx-impedance" },
		{ 0x3554, "txp-calibration" },
		{ 0x3558, "txn-calibration" },
		{ 0x3200, "tx-pll-power0" },
		{ 0x3204, "tx-pll-power1" },
		{ 0x3600, "pma-setting0" },
		{ 0x3604, "pma-setting1" },
		{ 0x3754, "pma-mode" },
		{ 0x3760, "tx-pll-status" },
		{ 0x37a0, "clock-setting" },
		{ 0x37b0, "pma-reset" },
		{ 0x3820, "rx-frequency-status" },
		{ 0x38a0, "memory-clock" },
		{ 0x0a04, "rx-sync-control" },
		{ 0x0a0c, "pcs-reset" },
		{ 0x0b1c, "rx-sync-state" },
		{ 0x0b48, "signal-polarity" },
		{ 0x0b4c, "signal-status" },
		{ 0x0b54, "phya-ready" },
	};
	unsigned int i;

	mutex_lock(&priv->xgspon_lock);
	seq_printf(s, "initialized: %u\npowered: %u\nconfigured: %u\n",
		   priv->initialized, priv->powered, priv->xgspon_configured);
	seq_printf(s, "rx-calibrated: %u\nrx-parked: %u\ntx-requested: %u\n",
		   priv->xgspon_calibrated, priv->xgspon_rx_parked,
		   priv->xgspon_tx_requested);
	seq_printf(s, "trim-rx: %u\ntrim-txp: %u\ntrim-txn: %u\n",
		   priv->rx_impedance, priv->txp_impedance, priv->txn_impedance);
	seq_printf(s, "last-power-on-error: %d\n", priv->last_power_on_error);
	seq_printf(s, "last-rx-error: %d\n", priv->last_rx_error);
	if (priv->last_tx_pll_valid)
		seq_printf(s, "last-tx-pll-status: %#010x\n",
			   priv->last_tx_pll_status);
	if (priv->last_eye_valid)
		seq_printf(s, "last-eye-peaking: %u\nlast-eye-done: 0x%08x\nlast-eye-ready: 0x%08x\n",
			   priv->last_eye_peaking, priv->last_eye_done, priv->last_eye_ready);

	/* Never read the optical register window while held in reset. */
	if (priv->powered)
		for (i = 0; i < ARRAY_SIZE(registers); i++)
			seq_printf(s, "%04x %-22s %#010x\n", registers[i].reg,
				   registers[i].name,
				   airoha_xpon_phy_read(priv, registers[i].reg));
	mutex_unlock(&priv->xgspon_lock);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(en7580_phy_status);

static void en7580_phy_debugfs_remove(void *data)
{
	debugfs_remove_recursive(data);
}

static void en7580_phy_debugfs_init(struct airoha_xpon_phy *priv)
{
	struct dentry *dir;
	char *name;

	name = devm_kasprintf(priv->dev, GFP_KERNEL, "airoha-xpon-phy-%s",
			      dev_name(priv->dev));
	if (!name)
		return;
	dir = debugfs_create_dir(name, NULL);
	if (IS_ERR_OR_NULL(dir))
		return;
	debugfs_create_file("status", 0444, dir, priv, &en7580_phy_status_fops);
	if (devm_add_action_or_reset(priv->dev, en7580_phy_debugfs_remove, dir))
		dev_warn(priv->dev, "failed to retain PHY debugfs status\n");
}

static const struct phy_ops en7580_xgspon_phy_ops = {
	.init = en7580_phy_init,
	.exit = en7580_phy_exit,
	.set_mode = en7580_phy_set_mode,
	.power_on = en7580_phy_power_on,
	.power_off = en7580_phy_power_off,
	.owner = THIS_MODULE,
};

static int en7580_phy_read_impedance(struct device *dev, const char *name, u8 *val)
{
	struct nvmem_cell *cell;
	u8 *data;
	size_t len;

	/* Missing/unprogrammed trims use the SDK's impedance level 2. */
	*val = 2;
	if (device_property_match_string(dev, "nvmem-cell-names", name) < 0)
		return 0;
	cell = devm_nvmem_cell_get(dev, name);
	if (IS_ERR(cell))
		return PTR_ERR(cell);
	data = nvmem_cell_read(cell, &len);
	if (IS_ERR(data))
		return PTR_ERR(data);
	if (len != 1) {
		kfree(data);
		return -EINVAL;
	}
	if (*data > 0 && *data <= 3)
		*val = *data;
	kfree(data);
	return 0;
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

static void
econet_xpon_phy_counter_clear(struct airoha_xpon_phy *priv, u32 mask)
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

static void
econet_xpon_phy_counter_init(struct airoha_xpon_phy *priv)
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

static int airoha_xpon_phy_configure(struct airoha_xpon_phy *priv)
{
	return priv->soc->configure(priv);
}

static void airoha_xpon_phy_complete_ready(struct airoha_xpon_phy *priv)
{
	if (priv->soc->has_integrated_pma) {
		/* EN7523 phy_ready_handler() retriggers PMA_CTRL8 bit 0. */
		airoha_xpon_phy_rmw(priv, XPON_PMA_CTRL8, BIT(0), 0);
		udelay(1);
		airoha_xpon_phy_rmw(priv, XPON_PMA_CTRL8, BIT(0), BIT(0));
		mdelay(1);
	}

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

static int airoha_xpon_phy_probe(struct platform_device *pdev)
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
	mutex_init(&priv->xgspon_lock);
	INIT_DELAYED_WORK(&priv->ready_work, soc->has_xgspon ?
			  en7580_phy_ready_work : airoha_xpon_phy_ready_work);

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

	if (soc->has_xgspon)
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
	if (soc->has_xgspon) {
		struct resource *pma_res;

		/* Keep the intervening PCIe PHY window out of our reservation. */
		pma_res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "pma");
		if (!pma_res)
			return dev_err_probe(dev, -EINVAL,
					     "missing xPON PMA register resource\n");
		if (resource_size(pma_res) < EN7580_XPON_PHY_PMA_MIN_SIZE)
			return dev_err_probe(dev, -EINVAL,
					     "xPON PMA resource %pR is too small\n", pma_res);
		priv->pma_base = devm_ioremap_resource(dev, pma_res);
		if (IS_ERR(priv->pma_base))
			return PTR_ERR(priv->pma_base);
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

	if (soc->has_xgspon) {
		ret = en7580_phy_read_impedance(dev, "rx-impedance", &priv->rx_impedance);
		if (ret)
			return dev_err_probe(dev, ret, "failed to read RX impedance trim\n");
		ret = en7580_phy_read_impedance(dev, "txp-impedance", &priv->txp_impedance);
		if (ret)
			return dev_err_probe(dev, ret, "failed to read TXP impedance trim\n");
		ret = en7580_phy_read_impedance(dev, "txn-impedance", &priv->txn_impedance);
		if (ret)
			return dev_err_probe(dev, ret, "failed to read TXN impedance trim\n");
		ret = reset_control_assert(priv->reset);
		if (ret)
			return dev_err_probe(dev, ret, "failed to hold PHY reset\n");
		ret = devm_add_action_or_reset(dev, en7580_phy_shutdown, priv);
		if (ret)
			return ret;
	}

	phy = devm_phy_create(dev, NULL, soc->ops ?: &airoha_xpon_phy_ops);
	if (IS_ERR(phy))
		return dev_err_probe(dev, PTR_ERR(phy),
				     "failed to create xPON PHY\n");

	phy_set_drvdata(phy, priv);
	platform_set_drvdata(pdev, priv);

	provider = devm_of_phy_provider_register(dev, of_phy_simple_xlate);
	if (IS_ERR(provider))
		return dev_err_probe(dev, PTR_ERR(provider),
				     "failed to register xPON PHY provider\n");
	if (soc->has_xgspon)
		en7580_phy_debugfs_init(priv);

	dev_info(dev, "%s xPON PHY registered at %pR\n", soc->name, res);
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

static const struct airoha_xpon_phy_soc_data airoha_en7523_xpon_phy_data = {
	.name = "EN7523",
	.min_size = EN7523_XPON_PHY_MIN_SIZE,
	.gpon_bit_delay_reg = XPON_GPON_TX_BIT_DELAY,
	.gpon_bit_delay_mask = XPON_GPON_TX_BIT_DELAY_MASK,
	.has_integrated_pma = true,
	.configure = airoha_en7523_xpon_phy_configure,
};

static const struct airoha_xpon_phy_soc_data airoha_en7528_xpon_phy_data = {
	.name = "EN7528",
	.min_size = ECONET_XPON_PHY_MIN_SIZE,
	.gpon_bit_delay_reg = XPON_PHYSET5,
	.gpon_bit_delay_mask = XPON_PHYSET5_BIT_DELAY_MASK,
	.gpon_bit_delay_enable = XPON_PHYSET5_BIT_DELAY_EN,
	.manages_fw_ready = true,
	.configure = airoha_en7528_xpon_phy_configure,
};

static const struct airoha_xpon_phy_soc_data airoha_en7580_xpon_phy_data = {
	.name = "EN7580",
	.min_size = EN7580_XPON_PHY_PCS_MIN_SIZE,
	.has_xgspon = true,
	.ops = &en7580_xgspon_phy_ops,
};

static const struct of_device_id airoha_xpon_phy_of_match[] = {
	{
		.compatible = "airoha,en7580-xpon-phy",
		.data = &airoha_en7580_xpon_phy_data,
	},
	{
		.compatible = "econet,en751221-xpon-phy",
		.data = &econet_en751221_xpon_phy_data,
	},
	{
		.compatible = "airoha,en7523-xpon-phy",
		.data = &airoha_en7523_xpon_phy_data,
	},
	{
		.compatible = "airoha,en751627-xpon-phy",
		.data = &airoha_en7528_xpon_phy_data,
	},
	{
		.compatible = "airoha,en7528-xpon-phy",
		.data = &airoha_en7528_xpon_phy_data,
	},
	{ }
};
MODULE_DEVICE_TABLE(of, airoha_xpon_phy_of_match);

static void airoha_xpon_phy_shutdown(struct platform_device *pdev)
{
	struct airoha_xpon_phy *priv = platform_get_drvdata(pdev);

	if (priv->soc->has_xgspon)
		en7580_phy_shutdown(priv);
}

static struct platform_driver airoha_xpon_phy_driver = {
	.probe = airoha_xpon_phy_probe,
	.shutdown = airoha_xpon_phy_shutdown,
	.driver = {
		.name = "airoha-xpon-phy",
		.of_match_table = airoha_xpon_phy_of_match,
	},
};
module_platform_driver(airoha_xpon_phy_driver);

MODULE_DESCRIPTION("Airoha/EcoNet xPON PHY driver");
MODULE_AUTHOR("Matheus Sampaio Queiroga <srherobrine20@gmail.com>");
MODULE_LICENSE("GPL");
