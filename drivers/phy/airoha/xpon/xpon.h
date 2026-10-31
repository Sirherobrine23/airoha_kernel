/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __AIROHA_XPON_PHY_H
#define __AIROHA_XPON_PHY_H

#include <linux/bits.h>
#include <linux/io.h>
#include <linux/mutex.h>
#include <linux/phy/phy-airoha-xpon.h>
#include <linux/phy/phy.h>
#include <linux/platform_device.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>

struct gpio_desc;
struct regmap;
struct reset_control;

#define ECONET_XPON_PHY_MIN_SIZE		0x0600
#define ECONET_SCU_PHY_CTRL0			0x860
#define ECONET_SCU_PHY_CTRL0_DIS		BIT(10)
#define ECONET_SCU_PHY_CTRL1			0x92c
#define ECONET_SCU_PHY_CTRL1_DIS		BIT(2)

#define EN7523_SCU_WAN_CONF			0x070
#define EN7523_SCU_WAN_MODE_MASK		GENMASK(7, 0)
#define EN7528_SCU_WAN_MODE_MASK		GENMASK(2, 0)
#define EN7523_SCU_WAN_MODE_GPON		0x00
#define EN7523_SCU_WAN_MODE_EPON		0x01
#define ECONET_XPON_TDCSET2			0x2d
#define ECONET_XPON_SETTING_EN757X		0x10f

#define XPON_PHYSET1				0x0100
#define XPON_PHYFWREADY				0x0104
#define XPON_PHYSET3				0x0108
#define XPON_PHYSET5				0x0110
#define XPON_PHYSET10				0x0124
#define XPON_PHYSTA1				0x0130
#define XPON_SETTING				0x0138
#define XPON_TDCSET2				0x01f8
#define XPON_RX_FEC_STATUS			0x021c
#define XPON_RX_COUNTER_ENABLE			0x0230
#define XPON_RX_COUNTER_CTRL			0x0234
#define XPON_RX_BIP_COUNTER			0x024c
#define XPON_RX_BIP_LATCH			BIT(2)
#define XPON_RX_BIP_CLEAR			BIT(3)
#define XPON_RX_COUNTER_CTRL2			0x0298
#define XPON_GPON_PREAMBLE			0x0400
#define XPON_GPON_DELIMITER_GUARD		0x0404
#define XPON_GPON_EXT_PREAMBLE			0x0408
#define XPON_TX_FEC_STATUS			0x040c
#define XPON_GPON_TX_COUNTER_CTRL		0x0424
#define XPON_GPON_TX_FRAME_COUNTER		0x0434
#define XPON_GPON_TX_BURST_COUNTER		0x0438
#define XPON_BISTCTL_LOOPBACK_SEL		0x04a0
#define XPON_BISTCTL_PRBS_TX_EN			0x04a4
#define XPON_TRANS_STATUS			0x05e0
#define XPON_INT_ENABLE				0x05f0
#define XPON_INT_STATUS_CLR			0x05f4
#define XPON_INT_STATUS				0x05f8
#define EN7528_XPON_REG_580			0x0580

#define XPON_RX_CTRL0				0x3028
#define XPON_PMA_CTRL0				0x4100
#define XPON_PMA_CTRL8				0x4110
#define XPON_PMA_CTRL12				0x4120
#define XPON_PMA_CTRL13				0x4124
#define XPON_SERDES_CTRL0			0x4200
#define XPON_SERDES_RESET			0x4204
#define XPON_SERDES_CTRL5			0x4214
#define XPON_SERDES_BEN_CTRL			0x4244
#define XPON_SERDES_CTRL18			0x4248
#define XPON_SERDES_CTRL19			0x424c
#define XPON_GPON_TX_BIT_DELAY			0x433c
#define XPON_RX_MODE_CTRL			0x4344
#define XPON_CDR_CTRL				0x4530
#define XPON_FREQ_CTRL				0x4608
#define XPON_PMA_INT_STATUS			0x4800
#define XPON_PMA_INT_ENABLE			0x4804
#define XPON_PMA_INT_STATUS_CLR			0x4808

#define XPON_PHYSET1_TX_LOCK_REF		BIT(24)
#define XPON_PHYFWREADY_READY			BIT(0)
#define XPON_PHYSET3_PLL_RST			BIT(31)
#define XPON_PHYSET3_COUNTER_RST		BIT(27)
#define XPON_PHYSET10_GPON			BIT(31)
#define XPON_PHYSTA1_STATE_MASK			GENMASK(20, 18)
#define XPON_PHYSTA1_SYNCING			2
#define XPON_PHYSTA1_READY			6
#define XPON_RX_SYNC_MASK			GENMASK(3, 0)
#define XPON_RX_SYNC_READY			0x0a
#define XPON_TRANS_STATUS_LOS			BIT(0)
#define XPON_SERDES_RESET_RX			BIT(9)
#define XPON_SERDES_RESET_CDR			BIT(8)

#define XPON_GPON_PREAMBLE_GUARD_MASK		GENMASK(7, 0)
#define XPON_GPON_PREAMBLE_T1_MASK		GENMASK(15, 8)
#define XPON_GPON_PREAMBLE_T2_MASK		GENMASK(23, 16)
#define XPON_GPON_PREAMBLE_T3_MASK		GENMASK(31, 24)

#define XPON_GPON_EXT_O3_O4_MASK		GENMASK(7, 0)
#define XPON_GPON_EXT_O5_MASK			GENMASK(15, 8)
#define XPON_GPON_EXT_MODE			BIT(16)
#define XPON_GPON_EXT_OPER_MASK			GENMASK(18, 17)
#define XPON_GPON_TX_BIT_DELAY_MASK		GENMASK(7, 0)
#define XPON_PHYSET5_BIT_DELAY_MASK		GENMASK(21, 19)
#define XPON_PHYSET5_BIT_DELAY_EN		BIT(23)

#define XPON_GPON_TX_ENABLE_PATTERN		0xaa
#define XPON_GPON_TX_COUNTER_ENABLE		BIT(3)
#define XPON_BIST_PRBS23			0x06

#define ECONET_XPON_COUNTER_ENABLE_MASK		GENMASK(2, 0)
#define ECONET_XPON_COUNTER_CLEAR_RX0		BIT(0)
#define ECONET_XPON_COUNTER_CLEAR_RX1		BIT(1)
#define ECONET_XPON_COUNTER_CLEAR_RX2		BIT(2)
#define ECONET_XPON_COUNTER_CLEAR_GPON_TX	BIT(3)
#define ECONET_XPON_COUNTER_CLEAR_RX3		BIT(4)
#define ECONET_XPON_COUNTER_CLEAR_ALL		GENMASK(4, 0)
#define XPON_FEC_STATUS_ACTIVE			BIT(15)

#define XPON_SETTING_EN7571			0x0000014f
/*
 * XPON_SETTING also selects the pin conventions of the attached optical
 * transceiver.  Each of these bits inverts one signal, so RX_SD_INV turns a
 * receive signal-detect input into a receive loss-of-signal input.  The right
 * combination is a property of the board's optics rather than of the SoC, so
 * it is described in the devicetree.
 */
#define XPON_SETTING_TX_SD_INV			BIT(4)
#define XPON_SETTING_TX_FAULT_INV		BIT(5)
#define XPON_SETTING_RX_SD_INV			BIT(6)
#define XPON_SETTING_BURST_EN_INV		BIT(7)
#define XPON_SETTING_INV_MASK			(XPON_SETTING_TX_SD_INV | \
						 XPON_SETTING_TX_FAULT_INV | \
						 XPON_SETTING_RX_SD_INV | \
						 XPON_SETTING_BURST_EN_INV)
#define XPON_TDCSET2_EN7571			0x0000002d
#define XPON_GPON_DELIMITER_DEFAULT		0xaaab5983
#define XPON_READY_RECOVERY_MS			5000

#define AIROHA_XPON_PHY_DELAY			0xffff

struct airoha_xpon_phy;
struct airoha_an758x_xgspon;

struct airoha_xpon_phy_soc_data {
	const char *name;
	u32 min_size;
	u32 pma_offset;
	u32 ana_offset;
	u32 gpon_bit_delay_reg;
	u32 gpon_bit_delay_mask;
	u32 gpon_bit_delay_enable;
	bool has_integrated_pma;
	bool manages_fw_ready;
	bool has_gpon_bip;
	enum airoha_xpon_phy_submode default_submode;
	const struct phy_ops *ops;
	int (*pre_init)(struct airoha_xpon_phy *priv);
	int (*probe)(struct airoha_xpon_phy *priv, struct platform_device *pdev);
	void (*shutdown)(struct airoha_xpon_phy *priv);
	int (*get_link_state)(struct airoha_xpon_phy *priv, bool *ready, bool *los);
	int (*set_tx_enable)(struct airoha_xpon_phy *priv, bool enable);
	int (*set_tx_calibration_mode)(struct airoha_xpon_phy *priv, bool enable);
	int (*configure)(struct airoha_xpon_phy *priv);
	void (*complete_ready)(struct airoha_xpon_phy *priv);
	void (*reset_rx)(struct airoha_xpon_phy *priv);
};

struct airoha_xpon_phy {
	struct device *dev;
	const struct airoha_xpon_phy_soc_data *soc;
	void __iomem *base;
	void __iomem *pma_base;
	void __iomem *ana_base;
	void *soc_priv;
	struct airoha_an758x_xgspon *xgspon;
	/* Each instance pins its backend module rather than the shared core. */
	struct phy_ops phy_ops;
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
	/* Serializes the RX counter command register (latch/clear). */
	spinlock_t counter_lock;
};

/* Register offsets are relative to the backend's PCS/ANA/PMA windows. */
struct airoha_xpon_phy_step {
	u16 reg;
	u32 mask;
	u32 value;
};

u32 airoha_xpon_phy_read(struct airoha_xpon_phy *priv, u32 reg);
void airoha_xpon_phy_write(struct airoha_xpon_phy *priv, u32 reg, u32 val);
void airoha_xpon_phy_rmw(struct airoha_xpon_phy *priv, u32 reg, u32 mask, u32 val);
void airoha_xpon_phy_run(struct airoha_xpon_phy *priv,
			 const struct airoha_xpon_phy_step *seq, size_t count);
void airoha_xpon_phy_set_tx_gpio(struct airoha_xpon_phy *priv, bool enable);
void airoha_xpon_phy_set_vcc_enabled(struct airoha_xpon_phy *priv, bool enable);

int airoha_xpon_phy_probe(struct platform_device *pdev);
void airoha_xpon_phy_shutdown(struct platform_device *pdev);
void econet_xpon_phy_counter_init(struct airoha_xpon_phy *priv);
void econet_xpon_phy_counter_clear(struct airoha_xpon_phy *priv, u32 mask);

#endif /* __AIROHA_XPON_PHY_H */
