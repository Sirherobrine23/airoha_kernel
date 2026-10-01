// SPDX-License-Identifier: GPL-2.0+
#include <linux/of.h>
#include <linux/bitfield.h>
#include <linux/mdio.h>
#include <linux/module.h>
#include <linux/phy.h>
#include <linux/slab.h>

#include "mtk.h"

#define MTK_GPHY_ID_MT7530		0x03a29412
#define MTK_GPHY_ID_MT7531		0x03a29441

/* MT7530 switch-global LED GPIO registers. */
#define MT7530_SW_LED_EN		0x7d00
#define MT7530_SW_LED_IO_MODE		0x7d04
#define MT7530_SW_LED_GPIO_DIR		0x7d10
#define MT7530_SW_LED_GPIO_OE		0x7d14
#define MT7530_SW_LED_GPIO_DATA		0x7d18

#define MT7530_PHY_NUM_PORTS		5
#define MT7530_PHY_NUM_LEDS		2

/* mtk_socphy_priv::led_state uses LED0 bits 0..2 and LED1 bits 16..18. */
#define MT7530_LED_STATE_ACTIVE_LOW	3
#define MT7530_LED_STATE_GPIO_MODE	4

#define MTK_PHY_PAGE_EXTENDED_2			0x0002
#define MTK_PHY_PAGE_EXTENDED_3			0x0003
#define MTK_PHY_RG_LPI_PCS_DSP_CTRL_REG11	0x11

#define MTK_PHY_PAGE_EXTENDED_2A30		0x2a30

/* Registers on Token Ring debug nodes */
/* ch_addr = 0x1, node_addr = 0xf, data_addr = 0x17 */
#define SLAVE_DSP_READY_TIME_MASK		GENMASK(22, 15)

/* Registers on MDIO_MMD_VEND1 */
#define MTK_PHY_GBE_MODE_TX_DELAY_SEL		0x13
#define MTK_PHY_TEST_MODE_TX_DELAY_SEL		0x14
#define   MTK_TX_DELAY_PAIR_B_MASK		GENMASK(10, 8)
#define   MTK_TX_DELAY_PAIR_D_MASK		GENMASK(2, 0)

#define MTK_PHY_MCC_CTRL_AND_TX_POWER_CTRL	0xa6
#define   MTK_MCC_NEARECHO_OFFSET_MASK		GENMASK(15, 8)

#define MTK_PHY_RXADC_CTRL_RG7			0xc6
#define   MTK_PHY_DA_AD_BUF_BIAS_LP_MASK	GENMASK(9, 8)

#define MTK_PHY_RG_LPI_PCS_DSP_CTRL_REG123	0x123
#define   MTK_PHY_LPI_NORM_MSE_LO_THRESH100_MASK	GENMASK(15, 8)
#define   MTK_PHY_LPI_NORM_MSE_HI_THRESH100_MASK	GENMASK(7, 0)

static void mtk_gephy_config_init(struct phy_device *phydev)
{
	/* Enable HW auto downshift */
	phy_modify_paged(phydev, MTK_PHY_PAGE_EXTENDED_1,
			 MTK_PHY_AUX_CTRL_AND_STATUS,
			 0, MTK_PHY_ENABLE_DOWNSHIFT);

	/* Increase SlvDPSready time */
	mtk_tr_modify(phydev, 0x1, 0xf, 0x17, SLAVE_DSP_READY_TIME_MASK,
		      FIELD_PREP(SLAVE_DSP_READY_TIME_MASK, 0x5e));

	/* Adjust 100_mse_threshold */
	phy_modify_mmd(phydev, MDIO_MMD_VEND1,
		       MTK_PHY_RG_LPI_PCS_DSP_CTRL_REG123,
		       MTK_PHY_LPI_NORM_MSE_LO_THRESH100_MASK |
		       MTK_PHY_LPI_NORM_MSE_HI_THRESH100_MASK,
		       FIELD_PREP(MTK_PHY_LPI_NORM_MSE_LO_THRESH100_MASK,
				  0xff) |
		       FIELD_PREP(MTK_PHY_LPI_NORM_MSE_HI_THRESH100_MASK,
				  0xff));

	/* If echo time is narrower than 0x3, it will be regarded as noise */
	phy_modify_mmd(phydev, MDIO_MMD_VEND1,
		       MTK_PHY_MCC_CTRL_AND_TX_POWER_CTRL,
		       MTK_MCC_NEARECHO_OFFSET_MASK,
		       FIELD_PREP(MTK_MCC_NEARECHO_OFFSET_MASK, 0x3));
}

static unsigned int mt7530_phy_led_state_bit(u8 index, unsigned int state)
{
	return state + (index ? 16 : 0);
}

static u32 mt7530_phy_led_switch_bit(struct phy_device *phydev, u8 index)
{
	if (phydev->mdio.addr >= MT7530_PHY_NUM_PORTS ||
	    index >= MT7530_PHY_NUM_LEDS)
		return 0;

	return BIT(phydev->mdio.addr * 4 + index);
}

/* The PHY's MDIO bus is parented by the MDIO device for the MT7530 switch. */
static struct mdio_device *
mt7530_phy_switch_mdiodev(struct phy_device *phydev)
{
	struct device *parent = phydev->mdio.bus->parent;

	if (!parent)
		return NULL;

	return to_mdio_device(parent);
}

static int mt7530_phy_switch_read(struct phy_device *phydev, u32 reg,
				  u32 *val)
{
	struct mdio_device *sw = mt7530_phy_switch_mdiodev(phydev);
	struct mii_bus *bus;
	u16 page, r;
	int lo, hi, ret;

	if (!sw || !sw->bus)
		return -ENODEV;

	bus = sw->bus;
	if (!bus->read || !bus->write)
		return -EOPNOTSUPP;

	page = (reg >> 6) & 0x3ff;
	r = (reg >> 2) & 0xf;
	mutex_lock_nested(&bus->mdio_lock, MDIO_MUTEX_NESTED);

	ret = bus->write(bus, sw->addr, 0x1f, page);
	if (ret < 0)
		goto out;
	lo = bus->read(bus, sw->addr, r);
	if (lo < 0) {
		ret = lo;
		goto out;
	}
	hi = bus->read(bus, sw->addr, 0x10);
	if (hi < 0) {
		ret = hi;
		goto out;
	}

	*val = ((u32)(u16)hi << 16) | (u16)lo;
	ret = 0;
out:
	mutex_unlock(&bus->mdio_lock);
	return ret;
}

static int mt7530_phy_switch_rmw(struct phy_device *phydev, u32 reg,
				 u32 mask, u32 set)
{
	struct mdio_device *sw = mt7530_phy_switch_mdiodev(phydev);
	struct mii_bus *bus;
	u16 page, r;
	u32 val;
	int lo, hi, ret;

	if (!sw || !sw->bus)
		return -ENODEV;

	bus = sw->bus;
	if (!bus->read || !bus->write)
		return -EOPNOTSUPP;

	page = (reg >> 6) & 0x3ff;
	r = (reg >> 2) & 0xf;
	mutex_lock_nested(&bus->mdio_lock, MDIO_MUTEX_NESTED);

	ret = bus->write(bus, sw->addr, 0x1f, page);
	if (ret < 0)
		goto out;
	lo = bus->read(bus, sw->addr, r);
	if (lo < 0) {
		ret = lo;
		goto out;
	}
	hi = bus->read(bus, sw->addr, 0x10);
	if (hi < 0) {
		ret = hi;
		goto out;
	}

	val = ((u32)(u16)hi << 16) | (u16)lo;
	val = (val & ~mask) | set;
	ret = bus->write(bus, sw->addr, r, val & 0xffff);
	if (ret < 0)
		goto out;
	ret = bus->write(bus, sw->addr, 0x10, val >> 16);
out:
	mutex_unlock(&bus->mdio_lock);
	return ret;
}

static int mt7530_phy_led_set_gpio(struct phy_device *phydev, u8 index,
				   bool on)
{
	struct mtk_socphy_priv *priv = phydev->priv;
	unsigned int gpio_state, polarity_state;
	u32 bit;
	bool level;
	int ret;

	bit = mt7530_phy_led_switch_bit(phydev, index);
	if (!bit)
		return -EINVAL;
	gpio_state = mt7530_phy_led_state_bit(index, MT7530_LED_STATE_GPIO_MODE);
	polarity_state = mt7530_phy_led_state_bit(index,
					       MT7530_LED_STATE_ACTIVE_LOW);
	level = on;
	if (test_bit(polarity_state, &priv->led_state))
		level = !level;

	if (test_bit(gpio_state, &priv->led_state))
		return mt7530_phy_switch_rmw(phydev, MT7530_SW_LED_GPIO_DATA,
					     bit, level ? bit : 0);

	ret = mt7530_phy_switch_rmw(phydev, MT7530_SW_LED_GPIO_DATA,
				    bit, level ? bit : 0);
	if (ret)
		return ret;
	ret = mt7530_phy_switch_rmw(phydev, MT7530_SW_LED_GPIO_DIR, bit, bit);
	if (ret)
		return ret;
	ret = mt7530_phy_switch_rmw(phydev, MT7530_SW_LED_GPIO_OE, bit, bit);
	if (ret)
		return ret;
	ret = mt7530_phy_switch_rmw(phydev, MT7530_SW_LED_EN, bit, bit);
	if (ret)
		return ret;
	ret = mt7530_phy_switch_rmw(phydev, MT7530_SW_LED_IO_MODE, bit, 0);
	if (ret)
		return ret;
	set_bit(gpio_state, &priv->led_state);
	return 0;
}

static int mt7530_phy_led_set_phy_mode(struct phy_device *phydev, u8 index)
{
	struct mtk_socphy_priv *priv = phydev->priv;
	unsigned int gpio_state;
	u32 bit;
	int ret;

	bit = mt7530_phy_led_switch_bit(phydev, index);
	if (!bit)
		return -EINVAL;
	gpio_state = mt7530_phy_led_state_bit(index, MT7530_LED_STATE_GPIO_MODE);
	ret = mt7530_phy_switch_rmw(phydev, MT7530_SW_LED_EN, bit, bit);
	if (ret)
		return ret;
	ret = mt7530_phy_switch_rmw(phydev, MT7530_SW_LED_IO_MODE, bit, bit);
	if (ret)
		return ret;
	ret = mt7530_phy_switch_rmw(phydev, MT7530_SW_LED_GPIO_OE, bit, 0);
	if (ret)
		return ret;
	ret = mt7530_phy_switch_rmw(phydev, MT7530_SW_LED_GPIO_DIR, bit, 0);
	if (ret)
		return ret;
	clear_bit(gpio_state, &priv->led_state);
	return 0;
}

static int mt7530_phy_led_brightness_set(struct phy_device *phydev, u8 index,
					 enum led_brightness value)
{
	return mt7530_phy_led_set_gpio(phydev, index, value != LED_OFF);
}

static const unsigned long mt7530_supported_led_triggers =
	BIT(TRIGGER_NETDEV_FULL_DUPLEX) |
	BIT(TRIGGER_NETDEV_HALF_DUPLEX) |
	BIT(TRIGGER_NETDEV_LINK) |
	BIT(TRIGGER_NETDEV_LINK_10) |
	BIT(TRIGGER_NETDEV_LINK_100) |
	BIT(TRIGGER_NETDEV_LINK_1000) |
	BIT(TRIGGER_NETDEV_RX) |
	BIT(TRIGGER_NETDEV_TX);

static int mt7530_phy_led_hw_is_supported(struct phy_device *phydev, u8 index,
					  unsigned long rules)
{
	return mtk_phy_led_hw_is_supported(phydev, index, rules,
					   mt7530_supported_led_triggers);
}

static int mt7530_phy_led_hw_control_get(struct phy_device *phydev, u8 index,
					 unsigned long *rules)
{
	struct mtk_socphy_priv *priv = phydev->priv;
	unsigned int gpio_state;
	u32 bit, mode;
	int ret;

	bit = mt7530_phy_led_switch_bit(phydev, index);
	if (!bit)
		return -EINVAL;
	ret = mt7530_phy_switch_read(phydev, MT7530_SW_LED_IO_MODE, &mode);
	if (ret)
		return ret;
	gpio_state = mt7530_phy_led_state_bit(index, MT7530_LED_STATE_GPIO_MODE);
	if (!(mode & bit)) {
		set_bit(gpio_state, &priv->led_state);
		if (rules)
			*rules = 0;
		return 0;
	}
	clear_bit(gpio_state, &priv->led_state);
	return mtk_phy_led_hw_ctrl_get(phydev, index, rules,
				       MTK_GPHY_LED_ON_SET,
				       MTK_GPHY_LED_RX_BLINK_SET,
				       MTK_GPHY_LED_TX_BLINK_SET);
}

static int mt7530_phy_led_hw_control_set(struct phy_device *phydev, u8 index,
					 unsigned long rules)
{
	u32 reg;
	int ret;

	if (index >= MT7530_PHY_NUM_LEDS)
		return -EINVAL;
	reg = index ? MTK_PHY_LED1_ON_CTRL : MTK_PHY_LED0_ON_CTRL;
	ret = phy_clear_bits_mmd(phydev, MDIO_MMD_VEND2, reg,
				 MTK_PHY_LED_ON_FORCE_ON);
	if (ret)
		return ret;
	ret = mtk_phy_led_hw_ctrl_set(phydev, index, rules,
				      MTK_GPHY_LED_ON_SET,
				      MTK_GPHY_LED_RX_BLINK_SET,
				      MTK_GPHY_LED_TX_BLINK_SET);
	if (ret)
		return ret;
	return mt7530_phy_led_set_phy_mode(phydev, index);
}

static int mt7530_match_phy_device(struct phy_device *phydev,
				   const struct phy_driver *phydrv)
{
	int estatus;

	if ((phydev->phy_id & phydrv->phy_id_mask) !=
	    (phydrv->phy_id & phydrv->phy_id_mask))
		return 0;

	/*
	 * EN7512 FE shares the 0x03a29412 PHY ID with MT7530.  The vendor
	 * SDK distinguishes MT7530 by MII reg 15 bit 13 (1000BASE-T full).
	 */
	estatus = phy_read(phydev, MII_ESTATUS);
	if (estatus < 0)
		return 0;

	return !!(estatus & ESTATUS_1000_TFULL);
}

static bool mt7530_phy_is_en751221_companion(struct phy_device *phydev)
{
	struct device *parent = phydev->mdio.bus->parent;

	return parent && parent->of_node &&
	       of_device_is_compatible(parent->of_node,
				       "econet,en751221-switch");
}

static int mt7530_phy_leds_init(struct phy_device *phydev)
{
	struct mtk_socphy_priv *priv = phydev->priv;
	unsigned int polarity_state;
	u16 val;
	int i;
	int ret;

	/*
	 * Select the PHY LED engine used by the vendor profile.  Keep the
	 * per-LED event and polarity bits under control of the LED core.
	 */
	ret = phy_write_mmd(phydev, MDIO_MMD_VEND2, MTK_PHY_LED_BCR,
			    MTK_PHY_LED_BCR_DEFAULT);
	if (ret < 0)
		return ret;

	for (i = 0; i < MT7530_PHY_NUM_LEDS; i++) {
		polarity_state = mt7530_phy_led_state_bit(i,
						       MT7530_LED_STATE_ACTIVE_LOW);
		val = MTK_PHY_LED_ON_ENABLE;
		if (test_bit(polarity_state, &priv->led_state))
			val |= MTK_PHY_LED_ON_POLARITY;

		ret = phy_modify_mmd(phydev, MDIO_MMD_VEND2,
					     i ? MTK_PHY_LED1_ON_CTRL :
						 MTK_PHY_LED0_ON_CTRL,
					     MTK_PHY_LED_ON_ENABLE |
						 MTK_PHY_LED_ON_POLARITY,
					     val);
		if (ret < 0)
			return ret;
	}

	return 0;
}

static int mt7530_phy_probe(struct phy_device *phydev)
{
	struct mtk_socphy_priv *priv;
	int ret;

	/*
	 * The EN751221 companion profile expects a clean MT7530 PHY before
	 * configuration.  Do the reset before OF registers the PHY LEDs so
	 * that active-low/high settings applied by the LED core survive.
	 */
	if (mt7530_phy_is_en751221_companion(phydev)) {
		ret = genphy_soft_reset(phydev);
		if (ret)
			return ret;
	}

	priv = devm_kzalloc(&phydev->mdio.dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	phydev->priv = priv;

	ret = mt7530_phy_leds_init(phydev);
	if (ret)
		return ret;

	mtk_phy_leds_state_init(phydev);

	return 0;
}

static int mt7530_phy_led_polarity_set(struct phy_device *phydev, int index,
				       unsigned long modes)
{
	struct mtk_socphy_priv *priv = phydev->priv;
	unsigned int polarity_state;
	bool active_low = false;
	u16 val = 0;
	u32 mode;
	int ret;

	if (index > 1)
		return -EINVAL;

	for_each_set_bit(mode, &modes, __PHY_LED_MODES_NUM) {
		switch (mode) {
		case PHY_LED_ACTIVE_LOW:
			active_low = true;
			val = MTK_PHY_LED_ON_POLARITY;
			break;
		case PHY_LED_ACTIVE_HIGH:
			active_low = false;
			break;
		default:
			return -EINVAL;
		}
	}

	ret = phy_modify_mmd(phydev, MDIO_MMD_VEND2,
			     index ? MTK_PHY_LED1_ON_CTRL :
				     MTK_PHY_LED0_ON_CTRL,
			     MTK_PHY_LED_ON_POLARITY, val);
	if (ret)
		return ret;

	polarity_state = mt7530_phy_led_state_bit(index,
					       MT7530_LED_STATE_ACTIVE_LOW);
	if (active_low)
		set_bit(polarity_state, &priv->led_state);
	else
		clear_bit(polarity_state, &priv->led_state);

	return 0;
}

static int mt7530_phy_en751221_config_init(struct phy_device *phydev)
{
	int ret;

	/*
	 * The EN7512 vendor SDK applies the MT7530 E3.0 PHY profile to
	 * all five PHYs after reset.  Without this profile the PHY IDs are
	 * readable over MDIO, but link detection on EN751221 companion MCM
	 * boards is unreliable or never completes.
	 */
	/* Clause 22 local data. */
	ret = phy_write(phydev, MII_CTRL1000, 0x1e00);
	if (ret < 0)
		return ret;

	ret = phy_write_paged(phydev, MTK_PHY_PAGE_EXTENDED_1, 0x14, 0x3a04);
	if (ret < 0)
		return ret;

	/* Clause 45 global/local data from mt7530GePhyCfgLoad(E3.0). */
	ret = phy_write_mmd(phydev, MDIO_MMD_VEND2, 0x0417, 0x7775);
	if (ret < 0)
		return ret;

	ret = phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x00a6, 0x0350);
	if (ret < 0)
		return ret;

	ret = phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x0012, 0xd210);
	if (ret < 0)
		return ret;

	/* Vendor profile disables 100/1000BASE-T EEE advertisement. */
	ret = phy_write_mmd(phydev, MDIO_MMD_AN, 0x003c, 0x0000);
	if (ret < 0)
		return ret;

	ret = mt7530_phy_leds_init(phydev);
	if (ret)
		return ret;

	mtk_phy_leds_state_init(phydev);

	return 0;
}

static int mt7530_phy_config_init(struct phy_device *phydev)
{
	int ret;

	if (mt7530_phy_is_en751221_companion(phydev)) {
		ret = mt7530_phy_en751221_config_init(phydev);
		if (ret)
			return ret;
	}

	mtk_gephy_config_init(phydev);

	/* Increase post_update_timer */
	ret = phy_write_paged(phydev, MTK_PHY_PAGE_EXTENDED_3,
			      MTK_PHY_RG_LPI_PCS_DSP_CTRL_REG11, 0x4b);
	if (ret < 0)
		return ret;

	return 0;
}

static int mt7530_led_config_of(struct phy_device *phydev)
{
	struct device_node *np = phydev->mdio.dev.of_node;
	const __be32 *paddr;
	int len;
	int i;

	paddr = of_get_property(np, "mediatek,led-config", &len);
	if (!paddr)
		return 0;

	if (len < (2 * sizeof(*paddr)))
		return -EINVAL;

	len /= sizeof(*paddr);

	phydev_warn(phydev, "Configure LED registers (num=%d)\n", len);
	for (i = 0; i < len - 1; i += 2) {
		u32 reg;
		u32 val;

		reg = be32_to_cpup(paddr + i);
		val = be32_to_cpup(paddr + i + 1);

		phy_write_mmd(phydev, MDIO_MMD_VEND2, reg, val);
	}

	return 0;
}

static int mt7531_phy_config_init(struct phy_device *phydev)
{
	mtk_gephy_config_init(phydev);

	/* PHY link down power saving enable */
	phy_set_bits(phydev, 0x17, BIT(4));
	phy_modify_mmd(phydev, MDIO_MMD_VEND1, MTK_PHY_RXADC_CTRL_RG7,
		       MTK_PHY_DA_AD_BUF_BIAS_LP_MASK,
		       FIELD_PREP(MTK_PHY_DA_AD_BUF_BIAS_LP_MASK, 0x3));

	/* Set TX Pair delay selection */
	phy_modify_mmd(phydev, MDIO_MMD_VEND1, MTK_PHY_GBE_MODE_TX_DELAY_SEL,
		       MTK_TX_DELAY_PAIR_B_MASK | MTK_TX_DELAY_PAIR_D_MASK,
		       FIELD_PREP(MTK_TX_DELAY_PAIR_B_MASK, 0x4) |
		       FIELD_PREP(MTK_TX_DELAY_PAIR_D_MASK, 0x4));
	phy_modify_mmd(phydev, MDIO_MMD_VEND1, MTK_PHY_TEST_MODE_TX_DELAY_SEL,
		       MTK_TX_DELAY_PAIR_B_MASK | MTK_TX_DELAY_PAIR_D_MASK,
		       FIELD_PREP(MTK_TX_DELAY_PAIR_B_MASK, 0x4) |
		       FIELD_PREP(MTK_TX_DELAY_PAIR_D_MASK, 0x4));

	/* LED Config*/
	mt7530_led_config_of(phydev);

	return 0;
}

static struct phy_driver mtk_gephy_driver[] = {
	{
		PHY_ID_MATCH_EXACT(MTK_GPHY_ID_MT7530),
		.name		= "MediaTek MT7530 PHY",
		.match_phy_device = mt7530_match_phy_device,
		.probe		= mt7530_phy_probe,
		.config_init	= mt7530_phy_config_init,
		/* Interrupts are handled by the switch, not the PHY
		 * itself.
		 */
		.config_intr	= genphy_no_config_intr,
		.handle_interrupt = genphy_handle_interrupt_no_ack,
		.suspend	= genphy_suspend,
		.resume		= genphy_resume,
		.read_page	= mtk_phy_read_page,
		.write_page	= mtk_phy_write_page,
		.led_brightness_set = mt7530_phy_led_brightness_set,
		.led_hw_is_supported = mt7530_phy_led_hw_is_supported,
		.led_hw_control_set = mt7530_phy_led_hw_control_set,
		.led_hw_control_get = mt7530_phy_led_hw_control_get,
		.led_polarity_set = mt7530_phy_led_polarity_set,
	},
	{
		PHY_ID_MATCH_EXACT(MTK_GPHY_ID_MT7531),
		.name		= "MediaTek MT7531 PHY",
		.config_init	= mt7531_phy_config_init,
		/* Interrupts are handled by the switch, not the PHY
		 * itself.
		 */
		.config_intr	= genphy_no_config_intr,
		.handle_interrupt = genphy_handle_interrupt_no_ack,
		.suspend	= genphy_suspend,
		.resume		= genphy_resume,
		.read_page	= mtk_phy_read_page,
		.write_page	= mtk_phy_write_page,
	},
};

module_phy_driver(mtk_gephy_driver);

static const struct mdio_device_id __maybe_unused mtk_gephy_tbl[] = {
	{ PHY_ID_MATCH_EXACT(MTK_GPHY_ID_MT7530) },
	{ PHY_ID_MATCH_EXACT(MTK_GPHY_ID_MT7531) },
	{ }
};

MODULE_DESCRIPTION("MediaTek Gigabit Ethernet PHY driver");
MODULE_AUTHOR("DENG, Qingfang <dqfext@gmail.com>");
MODULE_LICENSE("GPL");

MODULE_DEVICE_TABLE(mdio, mtk_gephy_tbl);
