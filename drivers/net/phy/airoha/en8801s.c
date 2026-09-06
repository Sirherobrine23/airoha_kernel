// SPDX-License-Identifier: GPL-2.0
/*
 * Driver for Airoha AN8801 and EN8801S Gigabit Ethernet PHYs.
 *
 * Copyright (C) 2023 Airoha Technology Corp.
 */

#include <linux/kernel.h>
#include <linux/bitfield.h>
#include <linux/string.h>
#include <linux/errno.h>
#include <linux/unistd.h>
#include <linux/interrupt.h>
#include <linux/init.h>
#include <linux/delay.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/skbuff.h>
#include <linux/spinlock.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mii.h>
#include <linux/ethtool.h>
#include <linux/phy.h>
#include <linux/leds.h>
#include <linux/property.h>
#include <linux/delay.h>
#include <linux/uaccess.h>
#include <linux/version.h>
#include <linux/types.h>

/* PHY ID EN8801S */
#define EN8801SC_PHY_ID			0x03a29461
#define EN8801SN_PHY_ID			0x03a29471
#define EN8801S_PHY_ID1			0x03a2
#define EN8801S_PHY_ID2			0x9461
#define EN8801S_PHY_ID3			0x9471

/* PHY ID AN8801 */
#define AN8801_PHY_ID			0xc0ff0421

#define EN8801S_PBUS_DEFAULT_ADDR	0x1e
#define EN8801S_PBUS_OUI		0x17a5
#define EN8801S_PHY_DEFAULT_ADDR	0x1d
#define EN8801S_RG_ETHER_PHY_OUI	0x19a4
#define EN8801S_RG_SMI_ADDR		0x19a8
#define EN8801S_RG_BUCK_CTL		0x1a20
#define EN8801S_RG_LTR_CTL		0x0cf8
#define EN8801S_RG_PROD_VER		0x18e0

#define EN8801S_LED_BLINK_UNIT		1024
#define EN8801S_LED_BLINK_MS		50
#define AN8801_LED_BLINK_UNIT		780
#define AN8801_LED_BLINK_MS		32

#define AN8801_MAX_SGMII_AN_RETRY	100
#define AN8801_MCS_LINK_STATUS_MASK	BIT(2)
#define AN8801_SGMII_AN_RESTART		BIT(9)
#define AN8801_SGMII_AN_DONE		BIT(0)
#define AN8801_SGMII_AN_RESET		BIT(15)
#define AN8801_PHY_PRE_SPEED_REG	0x2b
#define AN8801_MMD_VSPEC1		0x1e
#define AN8801_MMD_VSPEC2		0x1f

#define AN8801_RGMII_DELAY_STEP_MASK	GENMASK(2, 0)
#define AN8801_RGMII_RXDELAY_ALIGN	BIT(4)
#define AN8801_RGMII_RXDELAY_FORCE	BIT(24)
#define AN8801_RGMII_TXDELAY_FORCE	BIT(24)
#define AN8801_INTERRUPT_GPIO		3
#define AN8801_R50_SHIFT		(-7)

#define LED_ON_CTRL(i)			(0x024 + ((i)*2))
#define LED_ON_ENABLE			BIT(15)
#define LED_ON_POL			BIT(14)
#define LED_ON_EVT_MASK			(0x7f)

#define LED_ON_EVT_FORCE		BIT(6)
#define LED_ON_EVT_LINK_DOWN		BIT(3)
#define LED_ON_EVT_LINK_10M		BIT(2)
#define LED_ON_EVT_LINK_100M		BIT(1)
#define LED_ON_EVT_LINK_1000M		BIT(0)

#define LED_BLK_CTRL(i)			(0x025 + ((i)*2))
#define LED_BLK_EVT_MASK		0x3ff

#define LED_BLK_EVT_FORCE		BIT(9)
#define LED_BLK_EVT_10M_RX_ACT		BIT(5)
#define LED_BLK_EVT_10M_TX_ACT		BIT(4)
#define LED_BLK_EVT_100M_RX_ACT		BIT(3)
#define LED_BLK_EVT_100M_TX_ACT		BIT(2)
#define LED_BLK_EVT_1000M_RX_ACT	BIT(1)
#define LED_BLK_EVT_1000M_TX_ACT	BIT(0)

#define LED_LINK_10_100_1000 (LED_ON_EVT_LINK_10M | LED_ON_EVT_LINK_100M | LED_ON_EVT_LINK_1000M)
#define LEDS_BLINK_ALL(dir) (LED_BLK_EVT_10M_##dir##_ACT | LED_BLK_EVT_100M_##dir##_ACT | LED_BLK_EVT_1000M_##dir##_ACT)

#define EN8801S_POLARITY(is_rx, reverse) \
	((!!(reverse) ^ !!(is_rx)) << !!(is_rx))

#define MAX_RETRY		5
#define MAX_OUI_CHECK		2
#define MII_MMD_ACC_CTL_REG	0x0d
#define MII_MMD_ADDR_DATA_REG	0x0e
#define MMD_OP_MODE_DATA	BIT(14)

#define MAX_TRG_COUNTER		5

/* CL22 Reg Support Page Select */
#define RGADDR_REG1FH		0x1f
#define CL22_PAGE_REG		0x0000
#define CL22_PAGE_EXTREG	0x0001
#define CL22_PAGE_MISCREG	0x0002
#define CL22_PAGE_LPIREG	0x0003
#define CL22_PAGE_TREG		0x02a3
#define CL22_PAGE_TRREG		0x52b5

/* CL45 Reg Support DEVID */
#define DEVID_03		0x03
#define DEVID_07		0x07
#define DEVID_1E		0x1e
#define DEVID_1F		0x1f

/* TokenRing Reg Access */
#define TOKEN_RING_PKT_XMT_STA	0x8000
#define TOKEN_RING_WR		0x8000
#define TOKEN_RING_RD		0xa000

#define RGADDR_LPI_1CH		0x1c
#define RGADDR_AUXILIARY_1DH	0x1d
#define RGADDR_PMA_00H		0x0f80
#define RGADDR_PMA_01H		0x0f82
#define RGADDR_PMA_17H		0x0fae
#define RGADDR_PMA_18H		0x0fb0
#define RGADDR_DSPF_03H		0x1686
#define RGADDR_DSPF_06H		0x168c
#define RGADDR_DSPF_08H		0x1690
#define RGADDR_DSPF_0CH		0x1698
#define RGADDR_DSPF_0DH		0x169a
#define RGADDR_DSPF_0FH		0x169e
#define RGADDR_DSPF_10H		0x16a0
#define RGADDR_DSPF_11H		0x16a2
#define RGADDR_DSPF_13H		0x16a6
#define RGADDR_DSPF_14H		0x16a8
#define RGADDR_DSPF_1BH		0x16b6
#define RGADDR_DSPF_1CH		0x16b8
#define RGADDR_TR_26H		0x0ecc
#define RGADDR_R1000DEC_15H	0x03aa
#define RGADDR_R1000DEC_17H	0x03ae

#define LED_BCR			0x21
#define LED_BCR_EXT_CTRL	BIT(15)
#define LED_BCR_CLK_EN		BIT(3)
#define LED_BCR_TIME_TEST	BIT(2)
#define LED_BCR_MODE_MASK	(3)
#define LED_BCR_MODE_DISABLE	(0)

#define LED_ON_DUR		0x22
#define LED_ON_DUR_MASK		0xffff

#define LED_BLK_DUR		0x23
#define LED_BLK_DUR_MASK	0xffff

#define LED_GPIO_SEL_MASK	0x7ffffff

#define LPI_1C_SMI_DETON_TH_MASK	GENMASK(13, 8)
#define DEV1E_324_SMI_DET_DEGLITCH_OFF	BIT(9)
#define DEV1E_012_TX_I2MPB_A_TBT_MASK	GENMASK(5, 0)
#define DEV1E_017_TX_I2MPB_B_TBT_MASK	GENMASK(5, 0)

/* Invalid data */
#define INVALID_DATA		0xffffffff

#define airoha_mdio_lock(bus)   mutex_lock(&((bus)->mdio_lock))
#define airoha_mdio_unlock(bus) mutex_unlock(&((bus)->mdio_lock))

// #define phydev_mdio_bus(_dev) (_dev->mdio.bus)
// #define phydev_phy_addr(_dev) (_dev->mdio.addr)
// #define phydev_dev(_dev) (&_dev->mdio.dev)

#define GET_BIT(val, bit) ((val & BIT(bit)) >> bit)
#define LED_SET_GPIO_SEL(gpio, led, val) do { \
	unsigned int __shift = 8 * ((gpio) % 4); \
	(val) &= ~(0xffUL << __shift); \
	(val) |= (unsigned long)(led) << __shift; \
} while (0)

struct airoha_leds_config {
	u16 enable;
	u16 gpio;
	u16 pol;
	u16 on_cfg;
	u16 blk_cfg;
};

enum {
	AIROHA_LED_BLK_DUR_32M,
	AIROHA_LED_BLK_DUR_64M,
	AIROHA_LED_BLK_DUR_128M,
	AIROHA_LED_BLK_DUR_256M,
	AIROHA_LED_BLK_DUR_512M,
	AIROHA_LED_BLK_DUR_1024M,
	AIROHA_LED_BLK_DUR_LAST
};

enum {
	AIROHA_ACTIVE_LOW,
	AIROHA_ACTIVE_HIGH,
};

enum {
	AIROHA_LED_MODE_DISABLE,
	AIROHA_LED_MODE_USER_DEFINE,
	AIROHA_LED_MODE_LAST
};

enum airoha_led_control {
	AIROHA_LED_CONTROL_HW,
	AIROHA_LED_CONTROL_OFF,
	AIROHA_LED_CONTROL_ON,
	AIROHA_LED_CONTROL_BLINK,
};

enum an8801_polarity {
	AN8801_POL_TX_NOR_RX_REV,
	AN8801_POL_TX_REV_RX_REV,
	AN8801_POL_TX_NOR_RX_NOR,
	AN8801_POL_TX_REV_RX_NOR,
};

enum an8801_surge {
	AN8801_SURGE_0R,
	AN8801_SURGE_5R,
};

enum an8801_sgmii_mode {
	AN8801_SGMII_AN,
	AN8801_SGMII_FORCE,
};

enum {
	PHY_STATE_DONE = 0,
	PHY_STATE_INIT = 1,
	PHY_STATE_PROCESS = 2,
	PHY_STATE_FAIL = 3,
};

struct air8801_phy_data {
	const struct airoha_leds_config *default_leds_config;
	int (*led_route_gpio)(struct phy_device *phydev, u8 index);
	u8 led_blink_ms;
	u8 max_leds;
	u8 max_gpio;
};

struct air8801_data {
	struct device *dev;
	const struct air8801_phy_data *data;

	/* LEDs */
	struct airoha_leds_config *led_cfg;
	enum airoha_led_control *led_control;
	unsigned long *led_rules;
	bool leds_initialized;

	u8 pbus_addr;
	bool pbus_addr_from_firmware;

	u16 fail_starts;
	u16 version;

	bool first_init;
	bool rx_reverse;
	bool tx_reverse;

	bool rxdelay_force;
	bool txdelay_force;
	bool rxdelay_align;
	u16 rxdelay_step;
	u16 txdelay_step;
	u8 polarity;
	u8 surge;
	u8 sgmii_mode;
};

/* AN8801 support */
static const u16 an8801_r50ohm_table[] = {
	127, 127, 127, 127, 127, 127, 127, 127, 127, 127,
	127, 127, 127, 127, 127, 127, 127, 127, 127, 124,
	120, 116, 112, 108, 104, 100, 96, 93, 90, 86,
	84, 80, 77, 74, 72, 68, 65, 64, 61, 59,
	56, 54, 52, 48, 48, 45, 43, 40, 39, 36,
	35, 32, 32, 30, 28, 26, 24, 23, 21, 20,
	18, 16, 16, 14,
};

static unsigned long airoha_led_events_to_rules(u16 on_evt, u16 blk_evt)
{
	unsigned long rules = 0;

	if ((on_evt & LED_LINK_10_100_1000) == LED_LINK_10_100_1000)
		rules |= BIT(TRIGGER_NETDEV_LINK);
	else {
		if (on_evt & LED_ON_EVT_LINK_10M)
			rules |= BIT(TRIGGER_NETDEV_LINK_10);
		if (on_evt & LED_ON_EVT_LINK_100M)
			rules |= BIT(TRIGGER_NETDEV_LINK_100);
		if (on_evt & LED_ON_EVT_LINK_1000M)
			rules |= BIT(TRIGGER_NETDEV_LINK_1000);
	}

	if (blk_evt & LEDS_BLINK_ALL(TX))
		rules |= BIT(TRIGGER_NETDEV_TX);
	if (blk_evt & LEDS_BLINK_ALL(RX))
		rules |= BIT(TRIGGER_NETDEV_RX);

	return rules;
}

static void air8801_led_set_defaults(struct air8801_data *priv)
{
	int i;

	memcpy(priv->led_cfg, priv->data->default_leds_config,
	       sizeof(*priv->data->default_leds_config)*priv->data->max_leds);
	priv->leds_initialized = false;

	for (i = 0; i < priv->data->max_leds; i++) {
		priv->led_rules[i] =
			airoha_led_events_to_rules(priv->led_cfg[i].on_cfg,
						       priv->led_cfg[i].blk_cfg);
		priv->led_control[i] = AIROHA_LED_CONTROL_HW;
	}
}

static int airoha_led_rules_to_events(unsigned long rules,
					       u16 *on_evt, u16 *blk_evt)
{
	unsigned long supported = BIT(TRIGGER_NETDEV_LINK) |
				  BIT(TRIGGER_NETDEV_LINK_10) |
				  BIT(TRIGGER_NETDEV_LINK_100) |
				  BIT(TRIGGER_NETDEV_LINK_1000) |
				  BIT(TRIGGER_NETDEV_TX) |
				  BIT(TRIGGER_NETDEV_RX);

	if (rules & ~supported)
		return -EOPNOTSUPP;

	*on_evt = 0;
	*blk_evt = 0;

	if (rules & BIT(TRIGGER_NETDEV_LINK))
		*on_evt |= LED_LINK_10_100_1000;
	if (rules & BIT(TRIGGER_NETDEV_LINK_10))
		*on_evt |= LED_ON_EVT_LINK_10M;
	if (rules & BIT(TRIGGER_NETDEV_LINK_100))
		*on_evt |= LED_ON_EVT_LINK_100M;
	if (rules & BIT(TRIGGER_NETDEV_LINK_1000))
		*on_evt |= LED_ON_EVT_LINK_1000M;

	if (rules & BIT(TRIGGER_NETDEV_TX))
		*blk_evt |= LEDS_BLINK_ALL(TX);
	if (rules & BIT(TRIGGER_NETDEV_RX))
		*blk_evt |= LEDS_BLINK_ALL(RX);

	return 0;
}

/*
 * Raw Clause 22 MMD write used by the EN8801S .write_mmd callback.
 * phylib holds the MDIO bus lock while invoking the callback.
 */
static int __en8801s_write_mmd(struct phy_device *phydev, int devad,
			       u32 reg, u16 val)
{
	struct mii_bus *bus = phydev->mdio.bus;
	int addr = phydev->mdio.addr;
	int err;

	err = __mdiobus_write(bus, addr, MII_MMD_ACC_CTL_REG, devad);
	if (err)
		return err;

	err = __mdiobus_write(bus, addr, MII_MMD_ADDR_DATA_REG, reg);
	if (err)
		return err;

	err = __mdiobus_write(bus, addr, MII_MMD_ACC_CTL_REG,
			      MMD_OP_MODE_DATA | devad);
	if (err)
		return err;

	return __mdiobus_write(bus, addr, MII_MMD_ADDR_DATA_REG, val);
}

static int __airoha_pbus_write(struct mii_bus *ebus, int pbus_id,
			unsigned long pbus_address, unsigned long pbus_data)
{
	int err = 0;

	err = __mdiobus_write(ebus, pbus_id, 0x1f,
				(unsigned int)(pbus_address >> 6));
	if (err)
		return err;
	err = __mdiobus_write(ebus, pbus_id,
				(unsigned int)((pbus_address >> 2) & 0xf),
				(unsigned int)(pbus_data & 0xffff));
	if (err)
		return err;
	err = __mdiobus_write(ebus, pbus_id, 0x10,
				(unsigned int)(pbus_data >> 16));
	if (err)
		return err;
	return err;
}

static int __airoha_pbus_read_checked(struct mii_bus *ebus, int pbus_id,
			unsigned long pbus_address, u32 *pbus_data)
{
	int pbus_data_low, pbus_data_high;
	int err;

	err = __mdiobus_write(ebus, pbus_id, 0x1f,
				(unsigned int)(pbus_address >> 6));
	if (err)
		return err;

	pbus_data_low = __mdiobus_read(ebus, pbus_id,
				(unsigned int)((pbus_address >> 2) & 0xf));
	if (pbus_data_low < 0)
		return pbus_data_low;

	pbus_data_high = __mdiobus_read(ebus, pbus_id, 0x10);
	if (pbus_data_high < 0)
		return pbus_data_high;

	*pbus_data = (u16)pbus_data_low | ((u32)(u16)pbus_data_high << 16);

	return 0;
}

static unsigned long __airoha_pbus_read(struct mii_bus *ebus, int pbus_id,
			unsigned long pbus_address)
{
	struct device *dev = &ebus->dev;
	u32 pbus_data;
	int err;

	err = __airoha_pbus_read_checked(ebus, pbus_id, pbus_address,
					 &pbus_data);
	if (err) {
		dev_err(dev, "%s failed: %pe", __func__, ERR_PTR(err));
		return INVALID_DATA;
	}

	return pbus_data;
}

static int airoha_pbus_write(struct mii_bus *ebus, int pbus_id,
			unsigned long pbus_address, unsigned long pbus_data)
{
	int err = 0;

	airoha_mdio_lock(ebus);
	err = __airoha_pbus_write(ebus, pbus_id, pbus_address, pbus_data);
	airoha_mdio_unlock(ebus);

	return err;
}

static int airoha_pbus_read_checked(struct mii_bus *ebus, int pbus_id,
			unsigned long pbus_address, u32 *pbus_data)
{
	int err;

	airoha_mdio_lock(ebus);
	err = __airoha_pbus_read_checked(ebus, pbus_id, pbus_address,
					  pbus_data);
	airoha_mdio_unlock(ebus);

	return err;
}

static unsigned long airoha_pbus_read(struct mii_bus *ebus, int pbus_id,
			unsigned long pbus_address)
{
	unsigned long pbus_data;

	airoha_mdio_lock(ebus);
	pbus_data = __airoha_pbus_read(ebus, pbus_id, pbus_address);
	airoha_mdio_unlock(ebus);

	return pbus_data;
}

static void en8801s_add_pbus_candidate(u8 *candidates, int *count, u8 addr)
{
	int i;

	if (addr >= PHY_MAX_ADDR)
		return;

	for (i = 0; i < *count; i++) {
		if (candidates[i] == addr)
			return;
	}

	candidates[(*count)++] = addr;
}

static int en8801s_find_pbus_addr(struct phy_device *phydev)
{
	struct air8801_data *priv = phydev->priv;
	struct mii_bus *mbus = phydev->mdio.bus;
	struct device *dev = priv->dev;
	u8 phy_addr = phydev->mdio.addr;
	u8 candidates[3];
	u32 pbus_data, smi_data;
	int count = 0;
	int retry;
	int err;
	int i;

	if (priv->pbus_addr_from_firmware)
		en8801s_add_pbus_candidate(candidates, &count,
					    priv->pbus_addr);

	/* The vendor initialization keeps the PBUS address one slot above PHY. */
	if (phy_addr < PHY_MAX_ADDR - 1)
		en8801s_add_pbus_candidate(candidates, &count, phy_addr + 1);

	en8801s_add_pbus_candidate(candidates, &count,
				    EN8801S_PBUS_DEFAULT_ADDR);

	for (i = 0; i < count; i++) {
		for (retry = 0; retry < MAX_OUI_CHECK; retry++) {
			err = airoha_pbus_read_checked(mbus, candidates[i],
					EN8801S_RG_ETHER_PHY_OUI,
					&pbus_data);
			if (!err) {
				dev_info(dev,
					 "PBUS address %#x returned OUI %#08x",
					 candidates[i], pbus_data);

				if (pbus_data == EN8801S_PBUS_OUI)
					goto found;
			}

			usleep_range(10000, 20000);
		}
	}

	return dev_err_probe(dev, -ENODEV,
			     "unable to locate EN8801S PBUS address\n");

found:
	priv->pbus_addr = candidates[i];

	err = airoha_pbus_read_checked(mbus, priv->pbus_addr,
					EN8801S_RG_SMI_ADDR, &smi_data);
	if (err) {
		dev_warn(dev, "failed to read SMI address register: %pe",
			 ERR_PTR(err));
	} else {
		dev_info(dev,
			 "PBUS detected at %#x, SMI register %#08x (PHY %#x, PBUS %#x)",
			 priv->pbus_addr, smi_data, smi_data & 0x1f,
			 (smi_data >> 8) & 0x1f);
	}

	return 0;
}

/* Airoha Token Ring Write function */
static int airoha_tr_reg_write(struct phy_device *phydev,
			unsigned long tr_address, unsigned long tr_data)
{
	int err = 0;
	int phy_addr = phydev->mdio.addr;
	struct mii_bus *ebus = phydev->mdio.bus;

	airoha_mdio_lock(ebus);
	err = __mdiobus_write(ebus, phy_addr, 0x1f, 0x52b5); /* page select */
	err = __mdiobus_write(ebus, phy_addr, 0x11, (unsigned int)(tr_data & 0xffff));
	err = __mdiobus_write(ebus, phy_addr, 0x12, (unsigned int)(tr_data >> 16));
	err = __mdiobus_write(ebus, phy_addr, 0x10, (unsigned int)(tr_address | TOKEN_RING_WR));
	err = __mdiobus_write(ebus, phy_addr, 0x1f, 0x0); /* page resetore */
	airoha_mdio_unlock(ebus);

	return err;
}

static int airoha_led_set_usr_def(struct phy_device *phydev, u8 entity,
			int polar, u16 on_evt, u16 blk_evt)
{
	int err;

	if (polar == AIROHA_ACTIVE_HIGH)
		on_evt |= LED_ON_POL;
	else
		on_evt &= ~LED_ON_POL;

	err = phy_write_mmd(phydev, MDIO_MMD_VEND2, LED_ON_CTRL(entity),
			    on_evt | LED_ON_ENABLE);
	if (err)
		return err;

	return phy_write_mmd(phydev, MDIO_MMD_VEND2, LED_BLK_CTRL(entity),
			     blk_evt);
}

static int airoha_led_set_mode(struct phy_device *phydev, u8 mode)
{
	switch (mode) {
	case AIROHA_LED_MODE_DISABLE:
		return phy_modify_mmd(phydev, MDIO_MMD_VEND2, LED_BCR,
				      LED_BCR_EXT_CTRL | LED_BCR_MODE_MASK,
				      LED_BCR_MODE_DISABLE);
	case AIROHA_LED_MODE_USER_DEFINE:
		return phy_modify_mmd(phydev, MDIO_MMD_VEND2, LED_BCR,
				      LED_BCR_EXT_CTRL | LED_BCR_CLK_EN,
				      LED_BCR_EXT_CTRL | LED_BCR_CLK_EN);
	default:
		return -EINVAL;
	}
}

static int airoha_led_set_state(struct phy_device *phydev, u8 entity, u8 state)
{
	return phy_modify_mmd(phydev, MDIO_MMD_VEND2, LED_ON_CTRL(entity),
			      LED_ON_ENABLE, state ? LED_ON_ENABLE : 0);
}

static void air8801_led_get_control_events(struct air8801_data *priv, u8 index,
					   u16 *on_evt, u16 *blk_evt)
{
	struct airoha_leds_config *cfg = &priv->led_cfg[index];

	switch (priv->led_control[index]) {
	case AIROHA_LED_CONTROL_HW:
		*on_evt = cfg->on_cfg;
		*blk_evt = cfg->blk_cfg;
		break;
	case AIROHA_LED_CONTROL_ON:
		*on_evt = LED_ON_EVT_FORCE;
		*blk_evt = 0;
		break;
	case AIROHA_LED_CONTROL_BLINK:
		*on_evt = 0;
		*blk_evt = LED_BLK_EVT_FORCE;
		break;
	case AIROHA_LED_CONTROL_OFF:
	default:
		*on_evt = 0;
		*blk_evt = 0;
		break;
	}
}

static int air8801_led_program(struct phy_device *phydev, u8 index,
			       u16 on_evt, u16 blk_evt)
{
	struct air8801_data *priv = phydev->priv;
	struct airoha_leds_config *cfg;
	int err;

	if (index >= priv->data->max_leds)
		return -EINVAL;

	cfg = &priv->led_cfg[index];
	if (!priv->leds_initialized)
		return 0;

	err = priv->data->led_route_gpio(phydev, index);
	if (err)
		return err;

	err = airoha_led_set_mode(phydev, AIROHA_LED_MODE_USER_DEFINE);
	if (err)
		return err;

	err = airoha_led_set_state(phydev, index, true);
	if (err)
		return err;

	return airoha_led_set_usr_def(phydev, index, cfg->pol,
				      on_evt & LED_ON_EVT_MASK,
				      blk_evt & LED_BLK_EVT_MASK);
}

static int air8801_led_apply_control(struct phy_device *phydev, u8 index)
{
	struct air8801_data *priv = phydev->priv;
	u16 on_evt, blk_evt;

	if (index >= priv->data->max_leds)
		return -EINVAL;
	if (!priv->led_cfg[index].enable)
		return 0;

	air8801_led_get_control_events(priv, index, &on_evt, &blk_evt);

	return air8801_led_program(phydev, index, on_evt, blk_evt);
}

static int air8801_led_brightness_set(struct phy_device *phydev, u8 index,
				      enum led_brightness value)
{
	struct air8801_data *priv = phydev->priv;

	if (index >= priv->data->max_leds)
		return -EINVAL;

	priv->led_cfg[index].enable = true;
	priv->led_control[index] = value == LED_OFF ?
				   AIROHA_LED_CONTROL_OFF :
				   AIROHA_LED_CONTROL_ON;

	return air8801_led_apply_control(phydev, index);
}

static int air8801_led_blink_set(struct phy_device *phydev, u8 index,
				 unsigned long *delay_on,
				 unsigned long *delay_off)
{
	struct air8801_data *priv = phydev->priv;

	if (index >= priv->data->max_leds)
		return -EINVAL;

	priv->led_cfg[index].enable = true;
	*delay_on = priv->data->led_blink_ms;
	*delay_off = priv->data->led_blink_ms;
	priv->led_control[index] = AIROHA_LED_CONTROL_BLINK;

	return air8801_led_apply_control(phydev, index);
}

static int air8801_led_hw_is_supported(struct phy_device *phydev, u8 index,
				       unsigned long rules)
{
	struct air8801_data *priv = phydev->priv;
	u16 on_evt, blk_evt;

	if (index >= priv->data->max_leds)
		return -EINVAL;

	return airoha_led_rules_to_events(rules, &on_evt, &blk_evt);
}

static int air8801_led_hw_control_set(struct phy_device *phydev, u8 index,
				      unsigned long rules)
{
	struct air8801_data *priv = phydev->priv;
	struct airoha_leds_config *cfg;
	u16 on_evt, blk_evt;
	int err;

	if (index >= priv->data->max_leds)
		return -EINVAL;

	err = airoha_led_rules_to_events(rules, &on_evt, &blk_evt);
	if (err)
		return err;

	cfg = &priv->led_cfg[index];
	cfg->enable = true;
	cfg->on_cfg = on_evt;
	cfg->blk_cfg = blk_evt;
	priv->led_rules[index] = rules;
	priv->led_control[index] = AIROHA_LED_CONTROL_HW;

	return air8801_led_apply_control(phydev, index);
}

static int air8801_led_hw_control_get(struct phy_device *phydev, u8 index,
				      unsigned long *rules)
{
	struct air8801_data *priv = phydev->priv;

	if (index >= priv->data->max_leds)
		return -EINVAL;
	if (!rules)
		return 0;

	if (priv->led_cfg[index].enable &&
	    priv->led_control[index] == AIROHA_LED_CONTROL_HW)
		*rules = priv->led_rules[index];
	else
		*rules = 0;

	return 0;
}

static int air8801_led_polarity_set(struct phy_device *phydev, int index,
				    unsigned long modes)
{
	const unsigned long supported = BIT(PHY_LED_ACTIVE_LOW) |
					BIT(PHY_LED_ACTIVE_HIGH);
	struct air8801_data *priv = phydev->priv;
	struct airoha_leds_config *cfg;
	bool active_low, active_high;

	if (index < 0 || index >= priv->data->max_leds)
		return -EINVAL;
	if (modes & ~supported)
		return -EOPNOTSUPP;

	active_low = modes & BIT(PHY_LED_ACTIVE_LOW);
	active_high = modes & BIT(PHY_LED_ACTIVE_HIGH);
	if (active_low == active_high)
		return -EINVAL;

	cfg = &priv->led_cfg[index];
	cfg->pol = active_low ? AIROHA_ACTIVE_LOW : AIROHA_ACTIVE_HIGH;
	if (!cfg->enable)
		return 0;

	return air8801_led_apply_control(phydev, index);
}

static int en8801s_led_init(struct phy_device *phydev)
{
	struct mii_bus *mbus = phydev->mdio.bus;
	struct air8801_data *priv = phydev->priv;
	struct device *dev = priv->dev;
	unsigned long led_gpio = 0, reg_value = 0;
	int pbus_addr = priv->pbus_addr;
	int err = 0, led_id;
	int gpio_led_rg[3] = {0x1870, 0x1874, 0x1878};
	u16 blink_duration = EN8801S_LED_BLINK_UNIT << AIROHA_LED_BLK_DUR_64M;

	priv->leds_initialized = false;

	err = phy_write_mmd(phydev, MDIO_MMD_VEND2, LED_BLK_DUR, blink_duration);
	if (err)
		return err;

	blink_duration >>= 1;
	err = phy_write_mmd(phydev, MDIO_MMD_VEND2, LED_ON_DUR, blink_duration);
	if (err)
		return err;

	err = airoha_led_set_mode(phydev, AIROHA_LED_MODE_USER_DEFINE);
	if (err != 0) {
		dev_err(dev, "LED fail to set mode, err %d !", err);
		return err;
	}

	for (led_id = 0; led_id < priv->data->max_leds; led_id++) {
		struct airoha_leds_config *cfg = &priv->led_cfg[led_id];

		reg_value = 0;
		err = airoha_led_set_state(phydev, led_id, cfg->enable);
		if (err != 0) {
			dev_err(dev, "LED fail to set state, err %d !", err);
			return err;
		}

		if (cfg->enable) {
			u16 on_evt, blk_evt;

			if (cfg->gpio > priv->data->max_gpio) {
				dev_err(dev,
					"GPIO%d is out of range; valid GPIOs are 0-%d",
					cfg->gpio, priv->data->max_gpio);
				return -EINVAL;
			}

			led_gpio |= BIT(cfg->gpio);
			reg_value = airoha_pbus_read(mbus, pbus_addr,
					gpio_led_rg[cfg->gpio / 4]);
			LED_SET_GPIO_SEL(cfg->gpio, led_id, reg_value);
			dev_dbg(dev, "[Airoha] gpio%d, reg_value 0x%lx",
				cfg->gpio, reg_value);

			err = airoha_pbus_write(mbus, pbus_addr,
						gpio_led_rg[cfg->gpio / 4],
						reg_value);
			if (err)
				return err;

			air8801_led_get_control_events(priv, led_id,
							&on_evt, &blk_evt);
			err = airoha_led_set_usr_def(phydev, led_id, cfg->pol,
							 on_evt, blk_evt);
			if (err) {
				dev_err(dev,
					"LED fail to set user definition, err %d",
					err);
				return err;
			}
		}
	}

	reg_value = (airoha_pbus_read(mbus, pbus_addr, 0x1880) & ~led_gpio);
	err = airoha_pbus_write(mbus, pbus_addr, 0x1880, reg_value);
	if (err)
		return err;

	err = airoha_pbus_write(mbus, pbus_addr, 0x186c, led_gpio);
	if (err)
		return err;

	priv->leds_initialized = true;
	return 0;
}

static int __an8801_pbus_write(struct phy_device *phydev, u32 addr, u32 data)
{
	struct mii_bus *bus = phydev->mdio.bus;
	int phy_addr = phydev->mdio.addr;
	int restore_err;
	int err;

	err = __mdiobus_write(bus, phy_addr, 0x1f, 4);
	if (err)
		return err;

	err = __mdiobus_write(bus, phy_addr, 0x10, 0);
	if (err)
		goto restore;
	err = __mdiobus_write(bus, phy_addr, 0x11, addr >> 16);
	if (err)
		goto restore;
	err = __mdiobus_write(bus, phy_addr, 0x12, addr & 0xffff);
	if (err)
		goto restore;
	err = __mdiobus_write(bus, phy_addr, 0x13, data >> 16);
	if (err)
		goto restore;
	err = __mdiobus_write(bus, phy_addr, 0x14, data & 0xffff);

restore:
	restore_err = __mdiobus_write(bus, phy_addr, 0x1f, 0);
	return err ? err : restore_err;
}

static int __an8801_pbus_read(struct phy_device *phydev, u32 addr, u32 *data)
{
	struct mii_bus *bus = phydev->mdio.bus;
	int phy_addr = phydev->mdio.addr;
	int data_high, data_low;
	int restore_err;
	int err;

	err = __mdiobus_write(bus, phy_addr, 0x1f, 4);
	if (err)
		return err;

	err = __mdiobus_write(bus, phy_addr, 0x10, 0);
	if (err)
		goto restore;
	err = __mdiobus_write(bus, phy_addr, 0x15, addr >> 16);
	if (err)
		goto restore;
	err = __mdiobus_write(bus, phy_addr, 0x16, addr & 0xffff);
	if (err)
		goto restore;

	data_high = __mdiobus_read(bus, phy_addr, 0x17);
	if (data_high < 0) {
		err = data_high;
		goto restore;
	}

	data_low = __mdiobus_read(bus, phy_addr, 0x18);
	if (data_low < 0) {
		err = data_low;
		goto restore;
	}

	*data = (u16)data_low | ((u32)(u16)data_high << 16);
	err = 0;

restore:
	restore_err = __mdiobus_write(bus, phy_addr, 0x1f, 0);
	return err ? err : restore_err;
}

static int an8801_pbus_write(struct phy_device *phydev, u32 addr, u32 data)
{
	struct mii_bus *bus = phydev->mdio.bus;
	int err;

	airoha_mdio_lock(bus);
	err = __an8801_pbus_write(phydev, addr, data);
	airoha_mdio_unlock(bus);

	return err;
}

static int an8801_pbus_read_checked(struct phy_device *phydev, u32 addr,
				    u32 *data)
{
	struct mii_bus *bus = phydev->mdio.bus;
	int err;

	airoha_mdio_lock(bus);
	err = __an8801_pbus_read(phydev, addr, data);
	airoha_mdio_unlock(bus);

	return err;
}

static u32 an8801_pbus_read(struct phy_device *phydev, u32 addr)
{
	u32 data;
	int err;

	err = an8801_pbus_read_checked(phydev, addr, &data);
	if (err) {
		dev_err(&phydev->mdio.dev, "PBUS read %#x failed: %pe",
			addr, ERR_PTR(err));
		return INVALID_DATA;
	}

	return data;
}

static int an8801_pbus_update_bits(struct phy_device *phydev, u32 addr,
				   u32 mask, u32 set)
{
	struct mii_bus *bus = phydev->mdio.bus;
	u32 old, new;
	int err;

	airoha_mdio_lock(bus);
	err = __an8801_pbus_read(phydev, addr, &old);
	if (err)
		goto unlock;

	new = (old & ~mask) | (set & mask);
	if (new != old)
		err = __an8801_pbus_write(phydev, addr, new);

unlock:
	airoha_mdio_unlock(bus);
	return err;
}

static int en8801s_led_route_gpio(struct phy_device *phydev, u8 index)
{
	static const u32 gpio_led_rg[] = { 0x1870, 0x1874, 0x1878 };
	struct air8801_data *priv = phydev->priv;
	struct airoha_leds_config *cfg;
	struct mii_bus *mbus = phydev->mdio.bus;
	unsigned long reg_value;
	int pbus_addr = priv->pbus_addr;
	int err;

	if (index >= priv->data->max_leds)
		return -EINVAL;

	cfg = &priv->led_cfg[index];
	if (cfg->gpio > priv->data->max_gpio)
		return -EINVAL;

	reg_value = airoha_pbus_read(mbus, pbus_addr,
				     gpio_led_rg[cfg->gpio / 4]);
	LED_SET_GPIO_SEL(cfg->gpio, index, reg_value);
	err = airoha_pbus_write(mbus, pbus_addr,
				 gpio_led_rg[cfg->gpio / 4], reg_value);
	if (err)
		return err;

	reg_value = airoha_pbus_read(mbus, pbus_addr, 0x1880);
	reg_value &= ~BIT(cfg->gpio);
	err = airoha_pbus_write(mbus, pbus_addr, 0x1880, reg_value);
	if (err)
		return err;

	reg_value = airoha_pbus_read(mbus, pbus_addr, 0x186c);
	reg_value |= BIT(cfg->gpio);

	return airoha_pbus_write(mbus, pbus_addr, 0x186c, reg_value);
}

static int an8801_led_route_gpio(struct phy_device *phydev, u8 index)
{
	struct air8801_data *priv = phydev->priv;
	struct airoha_leds_config *cfg;
	u32 reg, mask;
	int err;

	if (index >= priv->data->max_leds)
		return -EINVAL;

	cfg = &priv->led_cfg[index];
	if (cfg->gpio > priv->data->max_gpio)
		return -EINVAL;

	err = an8801_pbus_update_bits(phydev, 0x10000054,
				       BIT(cfg->gpio), BIT(cfg->gpio));
	if (err)
		return err;

	reg = an8801_pbus_read(phydev, 0x10000058);
	if (reg == INVALID_DATA)
		return -EIO;

	mask = GENMASK(cfg->gpio * 3 + 2, cfg->gpio * 3);
	reg &= ~mask;
	reg |= (u32)index << (cfg->gpio * 3);
	err = an8801_pbus_write(phydev, 0x10000058, reg);
	if (err)
		return err;

	return an8801_pbus_update_bits(phydev, 0x10000070,
					 BIT(cfg->gpio), 0);
}

static int an8801_led_init(struct phy_device *phydev)
{
	struct air8801_data *priv = phydev->priv;
	u16 blink_duration = AN8801_LED_BLINK_UNIT << AIROHA_LED_BLK_DUR_64M;
	int i, err;

	priv->leds_initialized = false;

	err = phy_write_mmd(phydev, MDIO_MMD_VEND2, LED_BLK_DUR, blink_duration);
	if (err)
		return err;
	err = phy_write_mmd(phydev, MDIO_MMD_VEND2, LED_ON_DUR, blink_duration >> 1);
	if (err)
		return err;

	err = airoha_led_set_mode(phydev, AIROHA_LED_MODE_USER_DEFINE);
	if (err)
		return err;

	for (i = 0; i < priv->data->max_leds; i++) {
		struct airoha_leds_config *cfg = &priv->led_cfg[i];
		u16 on_evt, blk_evt;

		err = airoha_led_set_state(phydev, i, cfg->enable);
		if (err)
			return err;
		if (cfg->enable != true)
			continue;

		err = an8801_led_route_gpio(phydev, i);
		if (err)
			return err;

		air8801_led_get_control_events(priv, i, &on_evt, &blk_evt);
		err = airoha_led_set_usr_def(phydev, i, cfg->pol,
					     on_evt, blk_evt);
		if (err)
			return err;
	}

	priv->leds_initialized = true;
	return 0;
}

static int an8801_ack_interrupt(struct phy_device *phydev)
{
	u32 status;
	int err;

	err = an8801_pbus_write(phydev, 0x10285404, 0x102);
	if (err)
		return err;
	err = an8801_pbus_read_checked(phydev, 0x10285400, &status);
	if (err)
		return err;
	err = an8801_pbus_write(phydev, 0x10285400, 0);
	if (err)
		return err;
	err = an8801_pbus_write(phydev, 0x10285400, status | 0x10);
	if (err)
		return err;
	err = an8801_pbus_write(phydev, 0x10285404, 0x12);
	if (err)
		return err;

	return an8801_pbus_write(phydev, 0x10285704, 0x1f);
}

static int an8801_config_intr(struct phy_device *phydev)
{
	int err;

	if (phydev->interrupts == PHY_INTERRUPT_ENABLED) {
		err = an8801_pbus_write(phydev, 0x1000007c,
					  BIT(AN8801_INTERRUPT_GPIO) << 16);
		if (err)
			return err;
		err = an8801_pbus_update_bits(phydev, 0x10285700, BIT(0), BIT(0));
	} else {
		err = an8801_pbus_write(phydev, 0x1000007c, 0);
		if (err)
			return err;
		err = an8801_pbus_update_bits(phydev, 0x10285700, BIT(0), 0);
	}
	if (err)
		return err;

	return an8801_ack_interrupt(phydev);
}

static int an8801_did_interrupt(struct phy_device *phydev)
{
	u32 status;
	int err;

	err = an8801_pbus_read_checked(phydev, 0x10285704, &status);
	if (err)
		return err;

	return !!(status & 0x11);
}

static irqreturn_t an8801_handle_interrupt(struct phy_device *phydev)
{
	int err;

	err = an8801_did_interrupt(phydev);
	if (err <= 0)
		return IRQ_NONE;

	err = an8801_ack_interrupt(phydev);
	if (err)
		return IRQ_NONE;

	phy_trigger_machine(phydev);
	return IRQ_HANDLED;
}

static int an8801_find_closest_r50(const u16 *table, size_t size, u16 target)
{
	int left = 0, right = size - 1;

	while (left <= right) {
		int mid = left + ((right - left) >> 1);

		if (table[mid] == target)
			return mid;
		if (table[mid] < target)
			right = mid - 1;
		else
			left = mid + 1;
	}

	if (left >= size)
		return size - 1;
	return max(left - 1, 0);
}

static int an8801_r50_shift_position(int pos, int shift, int table_size)
{
	return clamp(pos + shift, 0, table_size - 1);
}

static int an8801_process_r50(struct phy_device *phydev, int reg,
			      u16 cl45_value, u16 r50_a, u16 r50_b)
{
	int size = ARRAY_SIZE(an8801_r50ohm_table);
	int pos_a, pos_b;

	pos_a = an8801_find_closest_r50(an8801_r50ohm_table, size, r50_a);
	pos_b = an8801_find_closest_r50(an8801_r50ohm_table, size, r50_b);
	pos_a = an8801_r50_shift_position(pos_a, AN8801_R50_SHIFT, size);
	pos_b = an8801_r50_shift_position(pos_b, AN8801_R50_SHIFT, size);

	cl45_value &= ~(GENMASK(14, 8) | GENMASK(6, 0));
	cl45_value |= (an8801_r50ohm_table[pos_a] & 0x7f) << 8;
	cl45_value |= an8801_r50ohm_table[pos_b] & 0x7f;

	return phy_write_mmd(phydev, AN8801_MMD_VSPEC1, reg, cl45_value);
}

static int an8801sb_i2mpb_config(struct phy_device *phydev)
{
	u16 cl45_value, temp_cl45, mask;
	int val, err;

	val = phy_read_mmd(phydev, AN8801_MMD_VSPEC1, 0x12);
	if (val < 0)
		return val;
	cl45_value = val;
	cl45_value = (cl45_value & GENMASK(15, 10)) + (6 << 10);
	err = phy_modify_mmd(phydev, AN8801_MMD_VSPEC1, 0x12,
			     GENMASK(15, 10), cl45_value);
	if (err)
		return err;

	val = phy_read_mmd(phydev, AN8801_MMD_VSPEC1, 0x16);
	if (val < 0)
		return val;
	temp_cl45 = val;
	mask = GENMASK(15, 10) | GENMASK(5, 0);
	cl45_value = (temp_cl45 & GENMASK(15, 10)) + (9 << 10);
	cl45_value |= (temp_cl45 & GENMASK(5, 0)) + 6;
	err = phy_modify_mmd(phydev, AN8801_MMD_VSPEC1, 0x16, mask, cl45_value);
	if (err)
		return err;

	val = phy_read_mmd(phydev, AN8801_MMD_VSPEC1, 0x17);
	if (val < 0)
		return val;
	cl45_value = val;
	cl45_value = (cl45_value & GENMASK(13, 8)) + (6 << 8);
	err = phy_modify_mmd(phydev, AN8801_MMD_VSPEC1, 0x17,
			     GENMASK(13, 8), cl45_value);
	if (err)
		return err;

	val = phy_read_mmd(phydev, AN8801_MMD_VSPEC1, 0x18);
	if (val < 0)
		return val;
	temp_cl45 = val;
	mask = GENMASK(13, 8) | GENMASK(5, 0);
	cl45_value = (temp_cl45 & GENMASK(13, 8)) + (9 << 8);
	cl45_value |= (temp_cl45 & GENMASK(5, 0)) + 6;
	err = phy_modify_mmd(phydev, AN8801_MMD_VSPEC1, 0x18, mask, cl45_value);
	if (err)
		return err;

	val = phy_read_mmd(phydev, AN8801_MMD_VSPEC1, 0x19);
	if (val < 0)
		return val;
	cl45_value = val;
	cl45_value = (cl45_value & GENMASK(13, 8)) + (6 << 8);
	err = phy_modify_mmd(phydev, AN8801_MMD_VSPEC1, 0x19,
			     GENMASK(13, 8), cl45_value);
	if (err)
		return err;

	val = phy_read_mmd(phydev, AN8801_MMD_VSPEC1, 0x20);
	if (val < 0)
		return val;
	cl45_value = (val & GENMASK(5, 0)) + 6;
	err = phy_modify_mmd(phydev, AN8801_MMD_VSPEC1, 0x20,
			     GENMASK(5, 0), cl45_value);
	if (err)
		return err;

	val = phy_read_mmd(phydev, AN8801_MMD_VSPEC1, 0x21);
	if (val < 0)
		return val;
	cl45_value = (val & GENMASK(13, 8)) + (6 << 8);
	err = phy_modify_mmd(phydev, AN8801_MMD_VSPEC1, 0x21,
			     GENMASK(13, 8), cl45_value);
	if (err)
		return err;

	val = phy_read_mmd(phydev, AN8801_MMD_VSPEC1, 0x22);
	if (val < 0)
		return val;
	cl45_value = (val & GENMASK(5, 0)) + 6;
	err = phy_modify_mmd(phydev, AN8801_MMD_VSPEC1, 0x22,
			     GENMASK(5, 0), cl45_value);
	if (err)
		return err;

	static const struct {
		u16 reg;
		u16 val;
	} init[] = {
		{ 0x23, 0x883 }, { 0x24, 0x883 }, { 0x25, 0x883 },
		{ 0x26, 0x883 }, { 0x00, 0x100 }, { 0x01, 0x1bc },
		{ 0x02, 0x1d0 }, { 0x03, 0x186 }, { 0x04, 0x202 },
		{ 0x05, 0x20e }, { 0x06, 0x300 }, { 0x07, 0x3c0 },
		{ 0x08, 0x3d0 }, { 0x09, 0x317 }, { 0x0a, 0x206 },
		{ 0x0b, 0x00e },
	};
	int i;

	for (i = 0; i < ARRAY_SIZE(init); i++) {
		err = phy_write_mmd(phydev, AN8801_MMD_VSPEC1,
				    init[i].reg, init[i].val);
		if (err)
			return err;
	}

	return 0;
}

static int an8801sb_surge_protect_cfg(struct phy_device *phydev)
{
	struct air8801_data *priv = phydev->priv;
	u16 r50_a, r50_b;
	int val, err;

	if (priv->surge != AN8801_SURGE_5R)
		return 0;

	val = phy_read_mmd(phydev, AN8801_MMD_VSPEC1, 0x174);
	if (val < 0)
		return val;
	r50_a = (val >> 8) & 0x7f;
	r50_b = val & 0x7f;
	err = an8801_process_r50(phydev, 0x174, val, r50_a, r50_b);
	if (err)
		return err;

	val = phy_read_mmd(phydev, AN8801_MMD_VSPEC1, 0x175);
	if (val < 0)
		return val;
	r50_a = (val >> 8) & 0x7f;
	r50_b = val & 0x7f;
	err = an8801_process_r50(phydev, 0x175, val, r50_a, r50_b);
	if (err)
		return err;

	return an8801sb_i2mpb_config(phydev);
}

static int an8801r_rgmii_delay_config(struct phy_device *phydev)
{
	struct air8801_data *priv = phydev->priv;
	u32 value;
	int err;

	if (priv->rxdelay_force) {
		value = priv->rxdelay_step & AN8801_RGMII_DELAY_STEP_MASK;
		if (priv->rxdelay_align)
			value |= AN8801_RGMII_RXDELAY_ALIGN;
		value |= AN8801_RGMII_RXDELAY_FORCE;
		err = an8801_pbus_write(phydev, 0x1021c02c, value);
		if (err)
			return err;
	}

	if (priv->txdelay_force) {
		value = priv->txdelay_step & AN8801_RGMII_DELAY_STEP_MASK;
		value |= AN8801_RGMII_TXDELAY_FORCE;
		err = an8801_pbus_write(phydev, 0x1021c024, value);
		if (err)
			return err;
	}

	return 0;
}

static int an8801sb_config_init(struct phy_device *phydev)
{
	struct air8801_data *priv = phydev->priv;
	u32 pbus_value, reg_value;
	int err;

	if (priv->sgmii_mode == AN8801_SGMII_AN) {
		reg_value = phy_read(phydev, MII_BMSR);
		if ((int)reg_value < 0)
			return (int)reg_value;
		if (reg_value & AN8801_MCS_LINK_STATUS_MASK) {
			err = an8801_pbus_write(phydev, 0x10220010, 0x1801);
			if (err)
				return err;
			err = an8801_pbus_write(phydev, 0x10220000, 0x9140);
			if (err)
				return err;
			mdelay(80);
		}
	} else {
		static const struct {
			u32 reg;
			u32 val;
		} force_init[] = {
			{ 0x102260e4, 0xff11 }, { 0x10224004, 0x0700 },
			{ 0x10224018, 0x0000 }, { 0x1022450c, 0x0700 },
			{ 0x1022a140, 0x0005 }, { 0x10226100, 0xf0000000 },
			{ 0x10226300, 0x0000 }, { 0x1022a078, 0x00010050 },
			{ 0x10220034, 0x31120009 }, { 0x10220000, 0x0140 },
		};
		int i;

		for (i = 0; i < ARRAY_SIZE(force_init); i++) {
			err = an8801_pbus_write(phydev, force_init[i].reg,
						  force_init[i].val);
			if (err)
				return err;
		}
	}

	pbus_value = an8801_pbus_read(phydev, 0x1022a0f8);
	if (pbus_value == INVALID_DATA)
		return -EIO;
	pbus_value &= ~GENMASK(1, 0);
	pbus_value |= priv->polarity;
	err = an8801_pbus_write(phydev, 0x1022a0f8, pbus_value);
	if (err)
		return err;

	err = phy_write_mmd(phydev, AN8801_MMD_VSPEC2, 0x600, 0x1e);
	if (err)
		return err;
	err = phy_write_mmd(phydev, AN8801_MMD_VSPEC2, 0x601, 0x02);
	if (err)
		return err;
	err = phy_write_mmd(phydev, MDIO_MMD_AN, 60, 0);
	if (err)
		return err;

	err = an8801_led_init(phydev);
	if (err)
		return err;
	err = an8801_pbus_write(phydev, 0x10270100, 0x0f);
	if (err)
		return err;
	err = an8801_pbus_write(phydev, 0x10270108, 0x0a0a0404);
	if (err)
		return err;
	err = an8801sb_surge_protect_cfg(phydev);
	if (err)
		return err;

	err = phy_set_bits(phydev, MII_CTRL1000, ADVERTISE_1000FULL);
	if (err)
		return err;
	return phy_set_bits(phydev, MII_BMCR, BMCR_ANRESTART);
}

static int an8801r_config_init(struct phy_device *phydev)
{
	static const struct {
		u32 reg;
		u32 val;
	} init[] = {
		{ 0x11f808d0, 0x180 },
		{ 0x1021c004, 0x1 },
		{ 0x10270004, 0x3f },
		{ 0x10270104, 0xff },
		{ 0x10270204, 0xff },
	};
	int i, err;

	err = genphy_soft_reset(phydev);
	if (err)
		return err;

	for (i = 0; i < ARRAY_SIZE(init); i++) {
		err = an8801_pbus_write(phydev, init[i].reg, init[i].val);
		if (err)
			return err;
	}

	err = an8801r_rgmii_delay_config(phydev);
	if (err)
		return err;

	return an8801_led_init(phydev);
}

static int an8801_config_init(struct phy_device *phydev)
{
	switch (phydev->interface) {
	case PHY_INTERFACE_MODE_SGMII:
		return an8801sb_config_init(phydev);
	case PHY_INTERFACE_MODE_RGMII:
		return an8801r_config_init(phydev);
	default:
		return dev_err_probe(&phydev->mdio.dev, -EOPNOTSUPP,
				     "unsupported PHY interface %s\n",
				     phy_modes(phydev->interface));
	}
}

static int an8801sb_read_status(struct phy_device *phydev)
{
	struct air8801_data *priv = phydev->priv;
	int pre_speed = phydev->speed;
	u32 reg_value;
	int retry, err;

	err = genphy_read_status(phydev);
	if (err)
		return err;

	if (!phydev->link) {
		pre_speed = 0;
		phydev->speed = 0;
		err = phy_write_mmd(phydev, AN8801_MMD_VSPEC2,
				    AN8801_PHY_PRE_SPEED_REG, 0);
		if (err)
			return err;

		if (priv->sgmii_mode == AN8801_SGMII_AN) {
			mdelay(10);
			reg_value = an8801_pbus_read(phydev, 0x10220010);
			if (reg_value == INVALID_DATA)
				return -EIO;
			reg_value &= ~BIT(15);
			err = an8801_pbus_write(phydev, 0x10220010, reg_value);
			if (err)
				return err;

			reg_value = an8801_pbus_read(phydev, 0x10220000);
			if (reg_value == INVALID_DATA)
				return -EIO;
			reg_value |= AN8801_SGMII_AN_RESTART;
			err = an8801_pbus_write(phydev, 0x10220000, reg_value);
			if (err)
				return err;
		}
	}

	if (pre_speed == phydev->speed || !phydev->link)
		return 0;

	pre_speed = phydev->speed;
	err = phy_write_mmd(phydev, AN8801_MMD_VSPEC2,
			    AN8801_PHY_PRE_SPEED_REG, pre_speed);
	if (err)
		return err;

	if (priv->sgmii_mode == AN8801_SGMII_AN) {
		for (retry = AN8801_MAX_SGMII_AN_RETRY; retry; retry--) {
			reg_value = an8801_pbus_read(phydev, 0x10220b04);
			if (reg_value == INVALID_DATA)
				return -EIO;
			if (reg_value & AN8801_SGMII_AN_DONE)
				break;
			mdelay(1);
		}
		mdelay(10);

		switch (pre_speed) {
		case SPEED_1000:
			reg_value = 0xd801;
			break;
		case SPEED_100:
			reg_value = 0xd401;
			break;
		default:
			reg_value = 0xd001;
			break;
		}
		err = an8801_pbus_write(phydev, 0x10220010, reg_value);
		if (err)
			return err;

		reg_value = an8801_pbus_read(phydev, 0x10220000);
		if (reg_value == INVALID_DATA)
			return -EIO;
		reg_value |= AN8801_SGMII_AN_RESET | AN8801_SGMII_AN_RESTART;
		return an8801_pbus_write(phydev, 0x10220000, reg_value);
	}

	switch (pre_speed) {
	case SPEED_1000:
		err = an8801_pbus_write(phydev, 0x102260e4, 0xff11);
		if (err)
			return err;
		err = an8801_pbus_write(phydev, 0x10224004, 0x0700);
		if (err)
			return err;
		err = an8801_pbus_write(phydev, 0x10224018, 0);
		if (err)
			return err;
		err = an8801_pbus_write(phydev, 0x1022450c, 0x0700);
		if (err)
			return err;
		err = an8801_pbus_write(phydev, 0x1022a140, 0x5);
		if (err)
			return err;
		err = an8801_pbus_write(phydev, 0x10226100, 0xf0000000);
		if (err)
			return err;
		return an8801_pbus_write(phydev, 0x10270100, 0xf);
	case SPEED_100:
		err = an8801_pbus_write(phydev, 0x102260e4, 0xff11);
		if (err)
			return err;
		err = an8801_pbus_write(phydev, 0x10224004, 0x0755);
		if (err)
			return err;
		err = an8801_pbus_write(phydev, 0x10224018, 0x14);
		if (err)
			return err;
		err = an8801_pbus_write(phydev, 0x1022450c, 0x0755);
		if (err)
			return err;
		err = an8801_pbus_write(phydev, 0x1022a140, 0x10);
		if (err)
			return err;
		err = an8801_pbus_write(phydev, 0x10226100, 0xf000000c);
		if (err)
			return err;
		return an8801_pbus_write(phydev, 0x10270100, 0xc);
	default:
		err = an8801_pbus_write(phydev, 0x102260e4, 0xffaa);
		if (err)
			return err;
		err = an8801_pbus_write(phydev, 0x10224004, 0x07aa);
		if (err)
			return err;
		err = an8801_pbus_write(phydev, 0x10224018, 0x4);
		if (err)
			return err;
		err = an8801_pbus_write(phydev, 0x1022450c, 0x07aa);
		if (err)
			return err;
		err = an8801_pbus_write(phydev, 0x1022a140, 0x20);
		if (err)
			return err;
		err = an8801_pbus_write(phydev, 0x10226100, 0xf000000f);
		if (err)
			return err;
		return an8801_pbus_write(phydev, 0x10270100, 0xc);
	}
}

static int an8801r_read_status(struct phy_device *phydev)
{
	int pre_speed = phydev->speed;
	u32 value;
	int err;

	err = genphy_read_status(phydev);
	if (err)
		return err;
	if (!phydev->link) {
		phydev->speed = 0;
		return 0;
	}
	if (pre_speed == phydev->speed)
		return 0;

	value = an8801_pbus_read(phydev, 0x10005054);
	if (value == INVALID_DATA)
		return -EIO;
	if (phydev->speed == SPEED_1000)
		value |= BIT(0);
	else
		value &= ~BIT(0);

	return an8801_pbus_write(phydev, 0x10005054, value);
}

static int an8801_read_status(struct phy_device *phydev)
{
	switch (phydev->interface) {
	case PHY_INTERFACE_MODE_SGMII:
		return an8801sb_read_status(phydev);
	case PHY_INTERFACE_MODE_RGMII:
		return an8801r_read_status(phydev);
	default:
		return -EOPNOTSUPP;
	}
}

static int en8801s_phy_process(struct phy_device *phydev)
{
	struct air8801_data *priv = phydev->priv;
	struct mii_bus *mbus = phydev->mdio.bus;
	unsigned long reg_value = 0;
	int err = 0;
	int pbus_addr = priv->pbus_addr;

	reg_value = airoha_pbus_read(mbus, pbus_addr, 0x19e0);
	reg_value |= BIT(0);
	err = airoha_pbus_write(mbus, pbus_addr, 0x19e0, reg_value);
	if (err)
		return err;

	reg_value = airoha_pbus_read(mbus, pbus_addr, 0x19e0);
	reg_value &= ~BIT(0);
	return airoha_pbus_write(mbus, pbus_addr, 0x19e0, reg_value);
}

static int en8801s_phase2_init(struct phy_device *phydev)
{
	struct air8801_data *priv = phydev->priv;
	struct mii_bus *mbus = phydev->mdio.bus;
	struct device *dev = priv->dev;
	int pbus_addr = priv->pbus_addr;
	unsigned long pbus_data;
	u16 reg_1e_324;
	int cl22_value;
	int retry, err = 0;

	pbus_data = airoha_pbus_read(mbus, pbus_addr, 0x1690);
	pbus_data |= BIT(31);
	err = airoha_pbus_write(mbus, pbus_addr, 0x1690, pbus_data);
	if (err)
		return err;

	err = airoha_pbus_write(mbus, pbus_addr, 0x0600, 0x0c000c00);
	if (err)
		return err;

	err = airoha_pbus_write(mbus, pbus_addr, 0x10, 0xd801);
	if (err)
		return err;

	err = airoha_pbus_write(mbus, pbus_addr, 0x0, 0x9140);
	if (err)
		return err;

	err = airoha_pbus_write(mbus, pbus_addr, 0x0a14, 0x0003);
	if (err)
		return err;

	err = airoha_pbus_write(mbus, pbus_addr, 0x0600, 0x0c000c00);
	if (err)
		return err;

	/* Set FCM control */
	err = airoha_pbus_write(mbus, pbus_addr, 0x1404, 0x004b);
	if (err)
		return err;

	err = airoha_pbus_write(mbus, pbus_addr, 0x140c, 0x0007);
	if (err)
		return err;

	err = airoha_pbus_write(mbus, pbus_addr, 0x142c, 0x05050505);
	if (err)
		return err;

	pbus_data = airoha_pbus_read(mbus, pbus_addr, 0x1440);
	err = airoha_pbus_write(mbus, pbus_addr, 0x1440, pbus_data & ~BIT(11));
	if (err)
		return err;

	pbus_data = airoha_pbus_read(mbus, pbus_addr, 0x1408);
	err = airoha_pbus_write(mbus, pbus_addr, 0x1408, pbus_data | BIT(5));
	if (err)
		return err;

	/* Set GPHY Perfomance*/
	/* Token Ring */
	err = airoha_tr_reg_write(phydev, RGADDR_R1000DEC_15H, 0x0055a0);
	if (err)
		return err;

	err = airoha_tr_reg_write(phydev, RGADDR_R1000DEC_17H, 0x07ff3f);
	if (err)
		return err;

	err = airoha_tr_reg_write(phydev, RGADDR_PMA_00H, 0x00001e);
	if (err)
		return err;

	err = airoha_tr_reg_write(phydev, RGADDR_PMA_01H, 0x6fb90a);
	if (err)
		return err;

	err = airoha_tr_reg_write(phydev, RGADDR_PMA_17H, 0x060671);
	if (err)
		return err;

	err = airoha_tr_reg_write(phydev, RGADDR_PMA_18H, 0x0e2f00);
	if (err)
		return err;

	err = airoha_tr_reg_write(phydev, RGADDR_TR_26H, 0x444444);
	if (err)
		return err;

	err = airoha_tr_reg_write(phydev, RGADDR_DSPF_03H, 0x000000);
	if (err)
		return err;

	err = airoha_tr_reg_write(phydev, RGADDR_DSPF_06H, 0x2ebaef);
	if (err)
		return err;

	err = airoha_tr_reg_write(phydev, RGADDR_DSPF_08H, 0x00000b);
	if (err)
		return err;

	err = airoha_tr_reg_write(phydev, RGADDR_DSPF_0CH, 0x00504d);
	if (err)
		return err;

	err = airoha_tr_reg_write(phydev, RGADDR_DSPF_0DH, 0x02314f);
	if (err)
		return err;

	err = airoha_tr_reg_write(phydev, RGADDR_DSPF_0FH, 0x003028);
	if (err)
		return err;

	err = airoha_tr_reg_write(phydev, RGADDR_DSPF_10H, 0x005010);
	if (err)
		return err;

	err = airoha_tr_reg_write(phydev, RGADDR_DSPF_11H, 0x040001);
	if (err)
		return err;

	err = airoha_tr_reg_write(phydev, RGADDR_DSPF_13H, 0x018670);
	if (err)
		return err;

	err = airoha_tr_reg_write(phydev, RGADDR_DSPF_14H, 0x00024a);
	if (err)
		return err;

	err = airoha_tr_reg_write(phydev, RGADDR_DSPF_1BH, 0x000072);
	if (err)
		return err;

	err = airoha_tr_reg_write(phydev, RGADDR_DSPF_1CH, 0x003210);
	if (err)
		return err;

	/* CL22 & CL45 */
	err = phy_write(phydev, 0x1f, 0x03);
	if (err)
		return err;

	cl22_value = phy_read(phydev, RGADDR_LPI_1CH);
	if (cl22_value < 0)
		return cl22_value;

	cl22_value &= ~LPI_1C_SMI_DETON_TH_MASK;
	cl22_value |= FIELD_PREP(LPI_1C_SMI_DETON_TH_MASK, 0x0c);
	err = phy_write(phydev, RGADDR_LPI_1CH, cl22_value);
	if (err)
		return err;

	err = phy_write(phydev, RGADDR_LPI_1CH, 0xc92);
	if (err)
		return err;

	err = phy_write(phydev, RGADDR_AUXILIARY_1DH, 0x1);
	if (err)
		return err;

	err = phy_write(phydev, 0x1f, 0x0);
	if (err)
		return err;

	err = phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x120, 0x8014);
	if (err)
		return err;

	err = phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x122, 0xffff);
	if (err)
		return err;

	err = phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x123, 0xffff);
	if (err)
		return err;

	err = phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x144, 0x0200);
	if (err)
		return err;

	err = phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x14a, 0xee20);
	if (err)
		return err;

	err = phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x189, 0x0110);
	if (err)
		return err;

	err = phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x19b, 0x0111);
	if (err)
		return err;

	err = phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x234, 0x0181);
	if (err)
		return err;

	err = phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x238, 0x0120);
	if (err)
		return err;

	err = phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x239, 0x0117);
	if (err)
		return err;

	err = phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x268, 0x07f4);
	if (err)
		return err;

	err = phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x2d1, 0x0733);
	if (err)
		return err;

	err = phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x323, 0x0011);
	if (err)
		return err;

	err = phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x324, 0x013f);
	if (err)
		return err;

	err = phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x326, 0x0037);
	if (err)
		return err;

	err = phy_read_mmd(phydev, MDIO_MMD_VEND1, 0x324);
	if (err < 0)
		return err;

	reg_1e_324 = err & ~DEV1E_324_SMI_DET_DEGLITCH_OFF;
	err = phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x324, reg_1e_324);
	if (err)
		return err;

	err = phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x19e, 0xc2);
	if (err)
		return err;

	err = phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x013, 0x0);
	if (err)
		return err;

	/* EFUSE */
	airoha_pbus_write(mbus, pbus_addr, 0x1c08, 0x40000040);
	retry = MAX_RETRY;
	while (retry != 0) {
		mdelay(1);
		pbus_data = airoha_pbus_read(mbus, pbus_addr, 0x1c08);
		if ((pbus_data & BIT(30)) == 0)
			break;

		retry--;
	}

	pbus_data = airoha_pbus_read(mbus, pbus_addr, 0x1c38); /* RAW#2 */
	err = phy_modify_mmd(phydev, MDIO_MMD_VEND1, 0x12,
			     DEV1E_012_TX_I2MPB_A_TBT_MASK,
			     FIELD_PREP(DEV1E_012_TX_I2MPB_A_TBT_MASK,
					pbus_data & 0x3f));
	if (err)
		return err;

	err = phy_modify_mmd(phydev, MDIO_MMD_VEND1, 0x17,
			     DEV1E_017_TX_I2MPB_B_TBT_MASK,
			     FIELD_PREP(DEV1E_017_TX_I2MPB_B_TBT_MASK,
					(pbus_data >> 8) & 0x3f));
	if (err)
		return err;

	airoha_pbus_write(mbus, pbus_addr, 0x1c08, 0x40400040);
	retry = MAX_RETRY;
	while (retry != 0) {
		mdelay(1);
		pbus_data = airoha_pbus_read(mbus, pbus_addr, 0x1c08);
		if ((pbus_data & BIT(30)) == 0)
			break;

		retry--;
	}

	pbus_data = airoha_pbus_read(mbus, pbus_addr, 0x1c30); /* RAW#16 */
	if (pbus_data & BIT(12))
		reg_1e_324 |= DEV1E_324_SMI_DET_DEGLITCH_OFF;
	else
		reg_1e_324 &= ~DEV1E_324_SMI_DET_DEGLITCH_OFF;

	err = phy_write_mmd(phydev, MDIO_MMD_VEND1, 0x324, reg_1e_324);
	if (err)
		return err;

	err = en8801s_led_init(phydev);
	if (err != 0)
		dev_err(dev, "en8801s_led_init fail (err:%d) !", err);

	err = phy_read_mmd(phydev, MDIO_MMD_AN, MDIO_AN_EEE_ADV);
	if (err < 0)
		return err;

	if (err == 0) {
		pbus_data = airoha_pbus_read(mbus, pbus_addr, 0x1960);
		if (0xa == ((pbus_data & 0x07c00000) >> 22)) {
			pbus_data = (pbus_data & 0xf83fffff) | (0xc << 22);
			err = airoha_pbus_write(mbus, pbus_addr, 0x1960,
						pbus_data);
			if (err)
				return err;
			mdelay(10);
			pbus_data = (pbus_data & 0xf83fffff) | (0xe << 22);
			err = airoha_pbus_write(mbus, pbus_addr, 0x1960,
						pbus_data);
			if (err)
				return err;
			mdelay(10);
		}
	} else {
		pbus_data = airoha_pbus_read(mbus, pbus_addr, 0x1960);
		if (0xe == ((pbus_data & 0x07c00000) >> 22)) {
			pbus_data = (pbus_data & 0xf83fffff) | (0xc << 22);
			err = airoha_pbus_write(mbus, pbus_addr, 0x1960,
						pbus_data);
			if (err)
				return err;
			mdelay(10);
			pbus_data = (pbus_data & 0xf83fffff) | (0xa << 22);
			err = airoha_pbus_write(mbus, pbus_addr, 0x1960,
						pbus_data);
			if (err)
				return err;
			mdelay(10);
		}
	}

	priv->first_init = false;
	dev_dbg(dev, "Phase2 initialize OK !");
	return 0;
}

static int en8801s_config_init(struct phy_device *phydev)
{
	struct air8801_data *priv = phydev->priv;
	struct mii_bus *mbus = phydev->mdio.bus;
	struct device *dev = priv->dev;
	unsigned long pbus_data;
	int pbus_addr;
	u16 reg_value;
	int retry, err = 0;

	priv->fail_starts = 1;
	msleep(1000);
	dev_dbg(priv->dev, "phase1 init");

	err = en8801s_find_pbus_addr(phydev);
	if (err)
		return err;

	pbus_addr = priv->pbus_addr;
	err = airoha_pbus_write(mbus, pbus_addr, EN8801S_RG_BUCK_CTL, 0x03);
	if (err)
		return err;

	pbus_data = airoha_pbus_read(mbus, pbus_addr, EN8801S_RG_PROD_VER);
	priv->version = pbus_data & 0xf;
	dev_dbg(dev, "version: %d", priv->version);

	mdelay(10);
	pbus_data = (airoha_pbus_read(mbus, pbus_addr, EN8801S_RG_LTR_CTL) & 0xfffffffc) | BIT(2);
	err = airoha_pbus_write(mbus, pbus_addr, EN8801S_RG_LTR_CTL, pbus_data);
	if (err)
		return err;

	/* Write phy polarity */
	mdelay(500);
	pbus_data &= ~(BIT(2) | GENMASK(1, 0));
	pbus_data |= EN8801S_POLARITY(true, priv->rx_reverse) |
		     EN8801S_POLARITY(false, priv->tx_reverse);
	err = airoha_pbus_write(mbus, pbus_addr, EN8801S_RG_LTR_CTL, pbus_data);
	if (err)
		return err;

	mdelay(500);
	if (priv->version == 4) {
		pbus_data = airoha_pbus_read(mbus, pbus_addr, 0x1900);
		dev_dbg(dev, "Before 0x1900 0x%lx", pbus_data);
		err = airoha_pbus_write(mbus, pbus_addr, 0x1900, 0x101009f);
		if (err)
			return err;

		pbus_data = airoha_pbus_read(mbus, pbus_addr, 0x1900);
		dev_dbg(dev, "After 0x1900 0x%lx", pbus_data);
		pbus_data = airoha_pbus_read(mbus, pbus_addr, 0x19a8);
		dev_dbg(dev, "Before 19a8 0x%lx", pbus_data);
		err = airoha_pbus_write(mbus, pbus_addr,
				0x19a8, pbus_data & ~BIT(16));
		if (err)
			return err;

		pbus_data = airoha_pbus_read(mbus, pbus_addr, 0x19a8);
		dev_dbg(dev, "After 19a8 0x%lx", pbus_data);
	}

	pbus_data = airoha_pbus_read(mbus, pbus_addr, EN8801S_RG_SMI_ADDR); /* SMI ADDR */
	pbus_data = (pbus_data & 0xffff0000) |
		     ((unsigned long)pbus_addr << 8) |
		     (unsigned long)phydev->mdio.addr;
	dev_dbg(dev, "SMI_ADDR=%lx (renew)", pbus_data);
	err = airoha_pbus_write(mbus, pbus_addr, EN8801S_RG_SMI_ADDR, pbus_data);
	if (err)
		return err;

	mdelay(10);
	retry = MAX_RETRY;
	while (1) {
		mdelay(10);
		reg_value = phy_read(phydev, MII_PHYSID2);
		if (reg_value == EN8801S_PHY_ID2 || reg_value == EN8801S_PHY_ID3) {
			dev_dbg(dev, "PHY ID2 0x%04x ready", reg_value);
			break; /* wait GPHY ready */
		}

		retry--;
		if (retry == 0) {
			dev_err(dev, "Initialize fail! phy=0x%x pbus=0x%x PHYSID2=0x%04x expected 0x%04x/0x%04x",
				phydev->mdio.addr, pbus_addr, reg_value, EN8801S_PHY_ID2, EN8801S_PHY_ID3);
			return -ETIMEDOUT;
		}
	}

	/* Software Reset PHY */
	reg_value = phy_read(phydev, MII_BMCR);
	reg_value |= BMCR_RESET;
	err = phy_write(phydev, MII_BMCR, reg_value);
	if (err)
		return err;

	retry = MAX_RETRY;
	do {
		mdelay(10);
		reg_value = phy_read(phydev, MII_BMCR);
		retry--;
		if (retry == 0) {
			dev_err(dev, "Reset fail !");
			return -ETIMEDOUT;
		}
	} while (reg_value & BMCR_RESET);

	phydev->dev_flags = PHY_STATE_INIT;
	dev_dbg(dev, "Phase1 initialize OK !");

	if (priv->version == 4) {
		err = en8801s_phase2_init(phydev);
		if (err != 0) {
			dev_err(dev, "en8801_phase2_init failed");
			phydev->dev_flags = PHY_STATE_FAIL;
			return 0;
		}
		phydev->dev_flags = PHY_STATE_PROCESS;
	}

	return 0;
}

static int en8801s_read_status(struct phy_device *phydev)
{
	struct air8801_data *priv = phydev->priv;
	struct mii_bus *mbus = phydev->mdio.bus;
	struct device *dev = priv->dev;
	int pbus_addr = priv->pbus_addr;
	int err = 0, preSpeed = phydev->speed;
	u32 reg_value;

	err = genphy_read_status(phydev);
	if (!phydev->link)
		preSpeed = phydev->speed = 0;

	if (phydev->dev_flags == PHY_STATE_PROCESS) {
		en8801s_phy_process(phydev);
		phydev->dev_flags = PHY_STATE_DONE;
	}

	if (phydev->dev_flags == PHY_STATE_INIT) {
		dev_dbg(dev, "phydev->link %d, count %d", phydev->link, priv->fail_starts);
		if ((phydev->link) || (priv->fail_starts >= 5)) {
			if (priv->version != 4) {
				err = en8801s_phase2_init(phydev);
				if (err != 0) {
					dev_err(dev, "en8801_phase2_init failed");
					phydev->dev_flags = PHY_STATE_FAIL;
					return 0;
				}
				phydev->dev_flags = PHY_STATE_PROCESS;
			}
		}
		priv->fail_starts++;
	}

	if ((preSpeed != phydev->speed) && phydev->link) {
		preSpeed = phydev->speed;

		if (preSpeed == SPEED_10) {
			reg_value = airoha_pbus_read(mbus, pbus_addr, 0x1694);
			reg_value |= BIT(31);
			err = airoha_pbus_write(mbus, pbus_addr, 0x1694, reg_value);
			if (err)
				return err;
			phydev->dev_flags = PHY_STATE_PROCESS;
		} else {
			reg_value = airoha_pbus_read(mbus, pbus_addr, 0x1694);
			reg_value &= ~BIT(31);
			err = airoha_pbus_write(mbus, pbus_addr, 0x1694, reg_value);
			if (err)
				return err;

			phydev->dev_flags = PHY_STATE_PROCESS;
		}

		airoha_pbus_write(mbus, pbus_addr, 0x0600, 0x0c000c00);
		dev_dbg(dev, "Pre Speed: %d", preSpeed);

		switch (preSpeed) {
		case SPEED_1000:
			err = airoha_pbus_write(mbus, pbus_addr, 0x10, 0xd801);
			if (err)
				return err;

			err = airoha_pbus_write(mbus, pbus_addr, 0x0, 0x9140);
			if (err)
				return err;

			err = airoha_pbus_write(mbus, pbus_addr, 0x0a14, 0x0003);
			if (err)
				return err;

			err = airoha_pbus_write(mbus, pbus_addr, 0x0600, 0x0c000c00);
			if (err)
				return err;

			/* delay 2 ms */
			mdelay(2);

			err = airoha_pbus_write(mbus, pbus_addr, 0x1404, 0x004b);
			if (err)
				return err;

			err = airoha_pbus_write(mbus, pbus_addr, 0x140c, 0x0007);
			break;
		case SPEED_100:
			err = airoha_pbus_write(mbus, pbus_addr, 0x10, 0xd401);
			if (err)
				return err;
			err = airoha_pbus_write(mbus, pbus_addr, 0x0, 0x9140);
			if (err)
				return err;

			err = airoha_pbus_write(mbus, pbus_addr, 0x0a14, 0x0007);
			if (err)
				return err;
			err = airoha_pbus_write(mbus, pbus_addr, 0x0600, 0x0c11);
			if (err)
				return err;

			/* delay 2 ms */
			mdelay(2);

			err = airoha_pbus_write(mbus, pbus_addr, 0x1404, 0x0027);
			if (err)
				return err;
			err = airoha_pbus_write(mbus, pbus_addr, 0x140c, 0x0007);
			break;
		case SPEED_10:
			err = airoha_pbus_write(mbus, pbus_addr, 0x10, 0xd001);
			if (err)
				return err;
			err = airoha_pbus_write(mbus, pbus_addr, 0x0, 0x9140);
			if (err)
				return err;

			err = airoha_pbus_write(mbus, pbus_addr, 0x0a14, 0x000b);
			if (err)
				return err;
			err = airoha_pbus_write(mbus, pbus_addr, 0x0600, 0x0c11);
			if (err)
				return err;

			/* delay 2 ms */
			mdelay(2);

			err = airoha_pbus_write(mbus, pbus_addr, 0x1404, 0x0027);
			if (err)
				return err;
			err = airoha_pbus_write(mbus, pbus_addr, 0x140c, 0x0007);
			break;
		case 0:
			break;
		default:
			dev_err(dev, "invalid speed value: %d", preSpeed);
			break;
		}
	}
	return err;
}

static int air8801_probe(struct phy_device *phydev)
{
	struct air8801_data *priv;
	struct device *dev = &phydev->mdio.dev;
	struct mdio_device *mdiodev = &phydev->mdio;
	unsigned long phy_addr = phydev->mdio.addr;
	u32 val, pbus_addr = EN8801S_PBUS_DEFAULT_ADDR;
	bool pbus_addr_from_firmware = false;
	int err;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->data = phydev->drv->driver_data;
	if (!priv->data)
		return dev_err_probe(dev, -EINVAL,
				     "missing PHY data");

	if (!priv->data->max_leds || !priv->data->default_leds_config)
		return dev_err_probe(dev, -EINVAL,
				     "invalid PHY LED data");

	priv->led_cfg = devm_kcalloc(dev, priv->data->max_leds,
				      sizeof(*priv->led_cfg), GFP_KERNEL);
	if (!priv->led_cfg)
		return -ENOMEM;

	priv->led_control = devm_kcalloc(dev, priv->data->max_leds,
				          sizeof(*priv->led_control), GFP_KERNEL);
	if (!priv->led_control)
		return -ENOMEM;

	priv->led_rules = devm_kcalloc(dev, priv->data->max_leds,
				        sizeof(*priv->led_rules), GFP_KERNEL);
	if (!priv->led_rules)
		return -ENOMEM;

	phydev->priv = priv;
	priv->fail_starts = 0;
	priv->first_init = true;
	priv->dev = dev;

	switch (phydev->phy_id) {
	case AN8801_PHY_ID:
		priv->polarity = AN8801_POL_TX_NOR_RX_NOR;
		priv->surge = AN8801_SURGE_0R;
		priv->sgmii_mode = AN8801_SGMII_AN;

		if (device_property_present(dev, "airoha,polarity")) {
			err = device_property_read_u32(dev, "airoha,polarity", &val);
			if (err)
				return dev_err_probe(dev, err, "invalid airoha,polarity\n");
			if (val > AN8801_POL_TX_REV_RX_NOR)
				return dev_err_probe(dev, -EINVAL,
						     "invalid airoha,polarity %u\n", val);
			priv->polarity = val;
		}

		if (device_property_present(dev, "airoha,surge")) {
			err = device_property_read_u32(dev, "airoha,surge", &val);
			if (err)
				return dev_err_probe(dev, err, "invalid airoha,surge\n");
			if (val > AN8801_SURGE_5R)
				return dev_err_probe(dev, -EINVAL,
						     "invalid airoha,surge %u\n", val);
			priv->surge = val;
		}

		if (device_property_present(dev, "airoha,sgmii-mode")) {
			err = device_property_read_u32(dev, "airoha,sgmii-mode", &val);
			if (err)
				return dev_err_probe(dev, err, "invalid airoha,sgmii-mode\n");
			if (val > AN8801_SGMII_FORCE)
				return dev_err_probe(dev, -EINVAL,
						     "invalid airoha,sgmii-mode %u\n", val);
			priv->sgmii_mode = val;
		}

		if (device_property_present(dev, "airoha,rxclk-delay")) {
			err = device_property_read_u32(dev, "airoha,rxclk-delay", &val);
			if (err)
				return dev_err_probe(dev, err, "invalid airoha,rxclk-delay\n");
			if (val > 7)
				return dev_err_probe(dev, -EINVAL,
						     "airoha,rxclk-delay out of range: %u\n", val);
			priv->rxdelay_force = true;
			priv->rxdelay_step = val;
			priv->rxdelay_align =
				device_property_present(dev, "airoha,rxclk-delay-align");
		}

		if (device_property_present(dev, "airoha,txclk-delay")) {
			err = device_property_read_u32(dev, "airoha,txclk-delay", &val);
			if (err)
				return dev_err_probe(dev, err, "invalid airoha,txclk-delay\n");
			if (val > 7)
				return dev_err_probe(dev, -EINVAL,
						     "airoha,txclk-delay out of range: %u\n", val);
			priv->txdelay_force = true;
			priv->txdelay_step = val;
		}
		break;
	default:
		err = device_property_read_u32(dev, "airoha,pbus-address",
					       &pbus_addr);
		if (err == -EINVAL) {
			err = device_property_read_u32(dev, "airoha,pbus-addr",
						       &pbus_addr);
			if (!err)
				dev_warn(dev,
					 "airoha,pbus-addr is deprecated; use airoha,pbus-address");
		}

		if (!err) {
			if (pbus_addr >= PHY_MAX_ADDR) {
				return dev_err_probe(dev, -EINVAL,
						     "invalid PBUS address %#x\n",
						     pbus_addr);
			}

			pbus_addr_from_firmware = true;
			dev_info(dev, "firmware PBUS address is %#x", pbus_addr);
		} else if (err != -EINVAL) {
			return dev_err_probe(dev, err,
					     "failed to read PBUS address property\n");
		}

		priv->pbus_addr = pbus_addr;
		priv->pbus_addr_from_firmware = pbus_addr_from_firmware;
		priv->rx_reverse = device_property_present(dev, "airoha,rx-reverse");
		priv->tx_reverse = device_property_present(dev, "airoha,tx-reverse");

		if (mdiodev->reset_gpio) {
			dev_dbg(dev, "Assert PHY %lx HWRST until phy_init_hw", phy_addr);
			phy_device_reset(phydev, 1);
		}
		dev_dbg(dev, "EN8801S initialized, PHY addr 0x%lx", phy_addr);
		break;
	}

	air8801_led_set_defaults(priv);
	return 0;
}

static int en8801s_write_mmd(struct phy_device *phydev,
			int devad, u16 reg, u16 val)
{
	struct air8801_data *priv = phydev->priv;
	struct mii_bus *mbus = phydev->mdio.bus;
	int pbus_addr = priv->pbus_addr;
	unsigned long pbus_data;

	if (MDIO_MMD_AN == devad && MDIO_AN_EEE_ADV == reg) {
		if (val == 0) {
			pbus_data = __airoha_pbus_read(mbus, pbus_addr, 0x1960);
			if (((pbus_data & 0x07c00000) >> 22) == 0xa) {
				pbus_data = (pbus_data & 0xf83fffff) | (0xc << 22);
				__airoha_pbus_write(mbus, pbus_addr, 0x1960, pbus_data);
				mdelay(10);
				pbus_data = (pbus_data & 0xf83fffff) | (0xe << 22);
				__airoha_pbus_write(mbus, pbus_addr, 0x1960, pbus_data);
				mdelay(10);
			}
		} else {
			pbus_data = __airoha_pbus_read(mbus, pbus_addr, 0x1960);
			if (((pbus_data & 0x07c00000) >> 22) == 0xe) {
				pbus_data = (pbus_data & 0xf83fffff) |
							(0xc << 22);
				__airoha_pbus_write(mbus, pbus_addr, 0x1960,
							pbus_data);
				mdelay(10);
				pbus_data = (pbus_data & 0xf83fffff) |
							(0xa << 22);
				__airoha_pbus_write(mbus, pbus_addr, 0x1960,
							pbus_data);
				mdelay(10);
			}
		}
	}
	return __en8801s_write_mmd(phydev, devad, reg, val);
}

static const struct airoha_leds_config an8801_default_leds[] = {
	/* BASE-T LED0 */
	{
		.enable = true,
		.gpio = 5,
		.pol = AIROHA_ACTIVE_LOW,
		.on_cfg = LED_ON_EVT_LINK_10M | LED_ON_EVT_LINK_100M | LED_ON_EVT_LINK_1000M,
	},
	/* BASE-T LED1 */
	{
		.enable = true,
		.gpio = 8,
		.pol = AIROHA_ACTIVE_LOW,
		.blk_cfg = LED_BLK_EVT_1000M_TX_ACT | LED_BLK_EVT_1000M_RX_ACT | \
			   LED_BLK_EVT_100M_TX_ACT | LED_BLK_EVT_100M_RX_ACT | LED_BLK_EVT_10M_TX_ACT | LED_BLK_EVT_10M_RX_ACT
	},
	/* BASE-T LED2 */
	{
		.enable = true,
		.gpio = 9,
		.pol = AIROHA_ACTIVE_LOW,
		.on_cfg = LED_ON_EVT_LINK_100M | LED_ON_EVT_LINK_10M,
		.blk_cfg = LED_BLK_EVT_100M_TX_ACT | LED_BLK_EVT_100M_RX_ACT | LED_BLK_EVT_10M_TX_ACT | LED_BLK_EVT_10M_RX_ACT
	},
};

static const struct air8801_phy_data an8801_data = {
	.default_leds_config = an8801_default_leds,
	.led_route_gpio = an8801_led_route_gpio,
	.led_blink_ms = AN8801_LED_BLINK_MS,
	.max_leds = ARRAY_SIZE(an8801_default_leds),
	.max_gpio = 9,
};

static const struct airoha_leds_config en8801s_default_leds[] = {
	/* BASE-T LED0 */
	{
		.enable = true,
		.gpio = 5,
		.pol = AIROHA_ACTIVE_LOW,
		.on_cfg = LED_ON_EVT_LINK_1000M,
		.blk_cfg = LED_BLK_EVT_1000M_TX_ACT | LED_BLK_EVT_1000M_RX_ACT,
	},
	/* BASE-T LED1 */
	{
		.enable = true,
		.gpio = 9,
		.pol = AIROHA_ACTIVE_LOW,
		.on_cfg = LED_ON_EVT_LINK_100M | LED_ON_EVT_LINK_10M,
		.blk_cfg = LED_BLK_EVT_100M_TX_ACT | LED_BLK_EVT_100M_RX_ACT | LED_BLK_EVT_10M_TX_ACT | LED_BLK_EVT_10M_RX_ACT
	},
	/* BASE-T LED2 */
	{
		.enable = true,
		.gpio = 8,
		.pol = AIROHA_ACTIVE_LOW,
		.on_cfg = LED_ON_EVT_LINK_100M,
		.blk_cfg = LED_BLK_EVT_100M_TX_ACT | LED_BLK_EVT_100M_RX_ACT
	},
	/* BASE-T LED3 */
	{
		.gpio = 1,
		.pol = AIROHA_ACTIVE_LOW,
	},
};

static const struct air8801_phy_data en8801s_data = {
	.default_leds_config = en8801s_default_leds,
	.led_route_gpio = en8801s_led_route_gpio,
	.led_blink_ms = EN8801S_LED_BLINK_MS,
	.max_leds = ARRAY_SIZE(en8801s_default_leds),
	.max_gpio = 9,
};

static struct phy_driver Airoha_driver[] = {
	{
		PHY_ID_MATCH_MODEL(AN8801_PHY_ID),
		.features		= PHY_GBIT_FEATURES,
		.name			= "Airoha AN8801",
		.probe			= air8801_probe,
		.config_init		= an8801_config_init,
		.config_aneg		= genphy_config_aneg,
		.read_status		= an8801_read_status,
		.suspend		= genphy_suspend,
		.resume			= genphy_resume,
		.config_intr		= an8801_config_intr,
		.handle_interrupt	= an8801_handle_interrupt,
		.led_brightness_set	= air8801_led_brightness_set,
		.led_blink_set		= air8801_led_blink_set,
		.led_hw_is_supported	= air8801_led_hw_is_supported,
		.led_hw_control_set	= air8801_led_hw_control_set,
		.led_hw_control_get	= air8801_led_hw_control_get,
		.led_polarity_set	= air8801_led_polarity_set,
		.driver_data		= &an8801_data,
	},
	{
		PHY_ID_MATCH_EXACT(EN8801SC_PHY_ID),
		.features		= PHY_GBIT_FEATURES,
		.name			= "Airoha EN8801SC",
		.probe			= air8801_probe,
		.config_init		= en8801s_config_init,
		.config_aneg		= genphy_config_aneg,
		.read_status		= en8801s_read_status,
		.suspend		= genphy_suspend,
		.resume			= genphy_resume,
		.write_mmd		= en8801s_write_mmd,
		.led_brightness_set	= air8801_led_brightness_set,
		.led_blink_set		= air8801_led_blink_set,
		.led_hw_is_supported	= air8801_led_hw_is_supported,
		.led_hw_control_set	= air8801_led_hw_control_set,
		.led_hw_control_get	= air8801_led_hw_control_get,
		.led_polarity_set	= air8801_led_polarity_set,
		.driver_data		= &en8801s_data,
	},
	{
		PHY_ID_MATCH_EXACT(EN8801SN_PHY_ID),
		.features		= PHY_GBIT_FEATURES,
		.name			= "Airoha EN8801SN",
		.probe			= air8801_probe,
		.config_init		= en8801s_config_init,
		.config_aneg		= genphy_config_aneg,
		.read_status		= en8801s_read_status,
		.suspend		= genphy_suspend,
		.resume			= genphy_resume,
		.write_mmd		= en8801s_write_mmd,
		.led_brightness_set	= air8801_led_brightness_set,
		.led_blink_set		= air8801_led_blink_set,
		.led_hw_is_supported	= air8801_led_hw_is_supported,
		.led_hw_control_set	= air8801_led_hw_control_set,
		.led_hw_control_get	= air8801_led_hw_control_get,
		.led_polarity_set	= air8801_led_polarity_set,
		.driver_data		= &en8801s_data,
	}
};

module_phy_driver(Airoha_driver);

static const struct mdio_device_id __maybe_unused en8801s_tbl[] = {
	{ PHY_ID_MATCH_MODEL(AN8801_PHY_ID) },
	{ PHY_ID_MATCH_EXACT(EN8801SC_PHY_ID) },
	{ PHY_ID_MATCH_EXACT(EN8801SN_PHY_ID) },
	{ }
};

MODULE_DEVICE_TABLE(mdio, en8801s_tbl);

MODULE_DESCRIPTION("Airoha AN8801 and EN8801S PHY driver");
MODULE_AUTHOR("Airoha");
MODULE_AUTHOR("Matheus Sampaio Queiroga <srherobrine20@gmail.com>");
MODULE_LICENSE("GPL");
