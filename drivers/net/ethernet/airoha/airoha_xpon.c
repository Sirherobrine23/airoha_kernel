// SPDX-License-Identifier: GPL-2.0-only
/*
 * Airoha/EcoNet EN7523 xPON MAC driver
 *
 * Unified GPON/EPON driver for the shared EN7523 xPON MAC complex.
 * The mode-specific protocol engines remain separate inside this file,
 * while probe/remove, DT matching and FE/GDM2 ownership are shared.
 *
 * The EN7523 register layout and bring-up order follow the vendor xpon_1g
 * implementation:
 *   - shared xPON region at 0x1fb60000
 *   - GPON registers at +0x4000
 *   - EPON registers at +0x6000
 *   - GDM2 is the WAN datapath for both modes
 */

#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/etherdevice.h>
#include <linux/export.h>
#include <linux/interrupt.h>
#include <linux/i2c.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/netdevice.h>
#include <linux/of.h>
#include <linux/of_net.h>
#include <linux/platform_device.h>
#include <linux/phy/airoha-lddla.h>
#include <linux/phy/phy.h>
#include <linux/phy/phy-airoha-xpon.h>
#include <linux/random.h>
#include <linux/regmap.h>
#include <linux/sched.h>
#include <linux/sfp.h>
#include <linux/timer.h>
#include <linux/unaligned.h>
#include <linux/workqueue.h>
#include <net/omci.h>

#include "airoha_eth.h"
#include "airoha_gpon_omci.h"
#include "airoha_regs.h"
#include "airoha_ploam.h"

/* Linux 6.18 replaced from_timer() with timer_container_of().
 * Keep the driver buildable on older kernel trees as well.
 */
#ifndef timer_container_of
#define timer_container_of(var, callback_timer, timer_fieldname) \
	from_timer(var, callback_timer, timer_fieldname)
#endif

#define EN7523_XPON_REGION_BASE         0x1fb60000
#define EN7523_GPON_REG_OFFSET          0x00004000
#define EN7523_EPON_REG_OFFSET          0x00006000
#define EN7523_EPON_REG_BASE            0x1fb66000

#define EN7523_SCU_WAN_CONF             0x070
#define EN7523_SCU_WAN_MODE_MASK        GENMASK(7, 0)
#define EN7523_SCU_WAN_MODE_GPON        0x00
#define EN7523_SCU_WAN_MODE_EPON        0x01

struct airoha_xpon_match_data {
	enum airoha_xpon_mode mode;
	bool mode_from_dt;
};

static struct device *airoha_xpon_find_lddla(struct device *dev)
{
	struct device_node *sfp_node, *i2c_node, *lddla_node;
	struct i2c_client *client;

	if (!IS_REACHABLE(CONFIG_AIROHA_LDDLA_PHY))
		return NULL;

	sfp_node = of_parse_phandle(dev->of_node, "sfp", 0);
	if (!sfp_node)
		return NULL;

	i2c_node = of_parse_phandle(sfp_node, "i2c-bus", 0);
	of_node_put(sfp_node);
	if (!i2c_node)
		return NULL;

	lddla_node = of_get_parent(i2c_node);
	of_node_put(i2c_node);
	if (!lddla_node)
		return NULL;

	if (!of_device_is_compatible(lddla_node, "airoha,en7570") &&
	    !of_device_is_compatible(lddla_node, "airoha,en7571") &&
	    !of_device_is_compatible(lddla_node, "airoha,en7572")) {
		of_node_put(lddla_node);
		return NULL;
	}

	client = of_find_i2c_device_by_node(lddla_node);
	of_node_put(lddla_node);
	if (!client)
		return ERR_PTR(-EPROBE_DEFER);

	if (!dev_get_drvdata(&client->dev)) {
		put_device(&client->dev);
		return ERR_PTR(-EPROBE_DEFER);
	}

	return &client->dev;
}

static int airoha_xpon_tx_rearm(struct device *dev, struct device *lddla_dev)
{
	int ret;

	if (!lddla_dev)
		return 0;

	ret = airoha_lddla_tx_rearm(lddla_dev);
	if (ret)
		dev_err(dev, "failed to rearm optical transmitter: %d\n", ret);
	else
		dev_info(dev, "optical transmitter safety circuit rearmed\n");

	return ret;
}

static const char *airoha_xpon_mode_name(enum airoha_xpon_mode mode)
{
	switch (mode) {
	case AIROHA_XPON_MODE_GPON:
		return "GPON";
	case AIROHA_XPON_MODE_EPON:
		return "EPON";
	default:
		return "unknown";
	}
}

static int airoha_xpon_get_mode(struct device *dev,
				 const struct airoha_xpon_match_data *data,
				 enum airoha_xpon_mode *mode)
{
	const char *name;

	*mode = data->mode;
	if (!data->mode_from_dt)
		return 0;

	if (of_property_read_string(dev->of_node, "airoha,pon-mode", &name))
		return 0;

	if (!strcmp(name, "gpon"))
		*mode = AIROHA_XPON_MODE_GPON;
	else if (!strcmp(name, "epon"))
		*mode = AIROHA_XPON_MODE_EPON;
	else
		return dev_err_probe(dev, -EINVAL,
				     "invalid airoha,pon-mode '%s'\n", name);

	return 0;
}

static int airoha_xpon_find_gdm2(struct device *dev,
				 struct device_node *eth_node,
				 struct net_device **netdev)
{
	*netdev = of_find_net_device_by_node(eth_node);
	if (!*netdev)
		return dev_err_probe(dev, -EPROBE_DEFER,
				     "GDM2 netdev is not registered yet\n");

	dev_info(dev, "resolved xPON datapath to %s (%pOF)\n",
		(*netdev)->name, eth_node);
	return 0;
}

static int airoha_xpon_set_fe_mode(struct device *dev,
				   struct device_node *eth_node,
				   enum airoha_xpon_mode mode)
{
	struct net_device *netdev;
	int ret;

	ret = airoha_xpon_find_gdm2(dev, eth_node, &netdev);
	if (ret)
		return ret;

	dev_info(dev, "configuring %s FE mode on %s\n",
		 airoha_xpon_mode_name(mode), netdev->name);
	ret = airoha_eth_set_xpon_mode(netdev, mode);
	if (ret)
		dev_err(dev, "failed to configure %s FE mode on %s: %d\n",
			airoha_xpon_mode_name(mode), netdev->name, ret);
	else
		dev_info(dev, "%s FE mode configured on %s\n",
			 airoha_xpon_mode_name(mode), netdev->name);
	dev_put(netdev);

	return ret;
}

static int airoha_xpon_set_fe_datapath(struct device *dev,
				       struct device_node *eth_node,
				       enum airoha_xpon_mode mode,
				       bool enable)
{
	struct net_device *netdev;
	int ret;

	ret = airoha_xpon_find_gdm2(dev, eth_node, &netdev);
	if (ret)
		return ret;

	dev_info(dev, "%s %s datapath on %s\n",
		 enable ? "enabling" : "disabling",
		 airoha_xpon_mode_name(mode), netdev->name);
	ret = airoha_eth_set_xpon_datapath(netdev, mode, enable);
	if (ret)
		dev_err(dev, "failed to %s %s datapath on %s: %d\n",
			enable ? "enable" : "disable",
			 airoha_xpon_mode_name(mode), netdev->name, ret);
	else
		dev_info(dev, "%s datapath %s on %s\n",
			 airoha_xpon_mode_name(mode),
			 enable ? "enabled" : "disabled", netdev->name);
	dev_put(netdev);

	return ret;
}

static int airoha_xpon_select_wan(struct regmap *scu,
				  enum airoha_xpon_mode mode)
{
	return regmap_update_bits(scu, EN7523_SCU_WAN_CONF,
				  EN7523_SCU_WAN_MODE_MASK,
				  mode == AIROHA_XPON_MODE_GPON ?
				  EN7523_SCU_WAN_MODE_GPON :
				  EN7523_SCU_WAN_MODE_EPON);
}

static int airoha_xpon_phy_start(struct device *dev, struct phy *phy,
				 enum airoha_xpon_mode mode,
				 bool *initialized, bool *powered)
{
	int submode, ret;

	submode = mode == AIROHA_XPON_MODE_GPON ?
		  AIROHA_XPON_PHY_SUBMODE_GPON :
		  AIROHA_XPON_PHY_SUBMODE_EPON;

	dev_info(dev, "initializing %s digital xPON PHY\n",
		 airoha_xpon_mode_name(mode));
	ret = phy_init(phy);
	if (ret)
		return dev_err_probe(dev, ret, "failed to initialize xPON PHY\n");
	*initialized = true;

	ret = phy_set_mode_ext(phy, PHY_MODE_ETHERNET, submode);
	if (ret) {
		dev_err(dev, "failed to select %s PHY mode: %d\n",
			airoha_xpon_mode_name(mode), ret);
		goto err_exit;
	}

	ret = phy_power_on(phy);
	if (ret) {
		dev_err(dev, "failed to start %s digital xPON PHY: %d\n",
			airoha_xpon_mode_name(mode), ret);
		goto err_exit;
	}
	*powered = true;

	/*
	 * Optical synchronization is asynchronous.  The generic PHY remains
	 * powered while LOS is asserted and reports/retries PHY_READY itself.
	 */
	dev_info(dev, "%s digital xPON PHY started\n",
		 airoha_xpon_mode_name(mode));
	return 0;

err_exit:
	phy_exit(phy);
	*initialized = false;
	return ret;
}

static void airoha_xpon_phy_stop(struct device *dev, struct phy *phy,
				 enum airoha_xpon_mode mode,
				 bool *initialized, bool *powered)
{
	int ret;

	if (*powered) {
		ret = phy_power_off(phy);
		if (ret)
			dev_warn(dev, "failed to power off %s PHY: %d\n",
				 airoha_xpon_mode_name(mode), ret);
		*powered = false;
	}

	if (*initialized) {
		ret = phy_exit(phy);
		if (ret)
			dev_warn(dev, "failed to exit %s PHY: %d\n",
				 airoha_xpon_mode_name(mode), ret);
		*initialized = false;
	}
}

/* -------------------------------------------------------------------------
 * GPON implementation
 * ------------------------------------------------------------------------- */
/* -----------------------------------------------------------------------
 * Register offsets from the GPON register window at physical 0x1fb64000.
 *
 * The vendor driver maps the encompassing region at 0x1fb60000 and the
 * generated register structure reserves the first 0x4000 bytes.  Some DTs
 * instead expose the register window directly at 0x1fb64000.  Probe accepts
 * both layouts and sets priv->regs to the actual GPON register window.
 * -------------------------------------------------------------------- */

#define EN7523_GPON_REG_BASE		0x1fb64000

#define GPON_ONU_ID		0x000
#define GPON_GBL_CFG		0x004
#define GPON_INT_STATUS		0x008
#define GPON_INT_ENABLE		0x00C
/* T-CONT pair registers (direct access, T-CONTs 0–15) */
#define GPON_TCONT_ID_0_1	0x020	/* 8 regs, each holds 2 T-CONTs */
#define GPON_TCONT_ID_14_15	0x03C
/* T-CONTs 16–31 indirect access */
#define GPON_TCONT_ID_16_31_CFG	0x180
#define GPON_TCONT_ID_16_31_STS	0x184
/* GEM port indirect access */
#define GPON_GEM_PORT_CFG	0x040
#define GPON_GEM_PORT_STS	0x044
/* OMCI GEM port */
#define GPON_OMCI_ID		0x048
/* GEM table init control.  The EN7523 vendor runtime path does not use
 * this register during normal activation; it clears GEM entries through
 * G_GEM_PORT_CFG instead.  Keep the offset documented for diagnostics.
 */
#define GPON_GEM_TBL_INIT	0x04C
/* Upstream PLOAM FIFO */
#define GPON_PLOAMu_FIFO_STS	0x050
#define GPON_PLOAMu_WDATA	0x054
/* Downstream PLOAM FIFO */
#define GPON_PLOAMd_FIFO_STS	0x058
#define GPON_PLOAMd_RDATA	0x05C
/* AES shadow key and switch-time config */
#define GPON_AES_CFG		0x060
#define GPON_AES_ACTIVE_KEY0	0x064	/* 4 × 32-bit, read-only */
#define GPON_AES_SHADOW_KEY0	0x074	/* 4 × 32-bit, write to program */
/* PLOu burst parameters */
#define GPON_PLOu_OVERHEAD	0x090
#define GPON_PLOu_GUARD_BIT	0x094
#define GPON_PLOu_PRMBL_TYPE1_2	0x098
#define GPON_PLOu_PRMBL_TYPE3	0x09C
#define GPON_PLOu_DELM_BIT	0x0A0
#define GPON_PRE_ASSIGNED_DLY	0x0A4
#define GPON_EQD		0x0A8
#define GPON_RSP_TIME		0x0AC
/* Serial number registers */
#define GPON_VENDOR_ID		0x0B0
#define GPON_VS_SN		0x0B4
#define GPON_SN_MSG_CFG		0x0B8
#define GPON_ACTIVATION_ST	0x0BC
/* Time of Day */
#define GPON_TOD_CFG		0x0D0
#define GPON_NEW_TOD_SEC_L32	0x0D4
#define GPON_NEW_TOD_NANO_SEC	0x0D8
#define GPON_CUR_TOD_SEC_L32	0x0DC
#define GPON_CUR_TOD_NANO_SEC	0x0E0
/* FCS / MIB tables */
#define GPON_TX_FCS_TBL_INIT	0x100
#define GPON_MIB_CTRL_STS	0x120
#define GPON_MIB_RDATA_L32	0x124
#define GPON_MIB_RDATA_H32	0x128
#define GPON_MIB_TBL_INIT	0x134
/* GPON/PSE memory-bus interface control */
#define GPON_MBI_MPI_STOP	0x160
/* EN7523 GPON debug and timing registers */
#define GPON_DBG_DLY		0x208
#define GPON_DBG_IDLE_GEM_THLD	0x20C
#define GPON_DBG_BWM_FILTER_CTRL	0x220
#define GPON_DBG_BWM_SFIFO_STS	0x224
#define GPON_DBG_GRP_0		0x228
#define GPON_DBG_GRP_1		0x22C
#define GPON_DBG_BWM_BFIFO_STS	0x250
#define GPON_DBG_ERR_CTRL	0x260
#define GPON_DBG_RX_GEM_CNT	0x300
#define GPON_DBG_RX_CRC_ERR_CNT	0x304
#define GPON_DBG_RX_GTC_CNT	0x308
#define GPON_DBG_TX_GEM_CNT	0x30C
#define GPON_DBG_TX_BST_CNT	0x310
#define GPON_DBG_GEM_HEC_ONE_ERR_CNT	0x330
#define GPON_DBG_GEM_HEC_TWO_ERR_CNT	0x334
#define GPON_DBG_GEM_HEC_UC_ERR_CNT	0x338
/* Debug / TX sync (EN7521 EqD adjustment) */
#define GPON_DBG_TX_SYNC_OFFSET	0x35C
/* Power management (always-on domain) */
#define GPON_SLEEP_GLB_CFG	0x3A4
#define GPON_SLEEP_CNT		0x3A8

/* -----------------------------------------------------------------------
 * Register bit definitions
 * -------------------------------------------------------------------- */

/* G_ONU_ID */
#define ONU_ID_VLD		BIT(15)
#define ONU_ID_MASK		0xFF

/* G_GBL_CFG */
#define GBL_CFG_US_FEC_EN	BIT(16)

/* G_SN_MSG_CFG */
#define SN_MSG_CFG_SN_REQ_THR_MASK	GENMASK(31, 24)
#define SN_MSG_CFG_TX_POWER_MODE_MASK	GENMASK(17, 16)
#define SN_MSG_CFG_RANDOM_DELAY_MASK	GENMASK(11, 0)

/* G_INT_STATUS / G_INT_ENABLE */
#define INT_PLOAMD_RECV		BIT(0)
#define INT_PLOAMU_SEND		BIT(1)
#define INT_SN_REQ_RECV		BIT(2)
#define INT_SN_ONU_SEND_O3	BIT(3)
#define INT_RANGING_REQ_RECV	BIT(4)
#define INT_SN_ONU_SEND_O4	BIT(5)
#define INT_SN_REQ_CRS		BIT(6)
#define INT_LOSS_GEM_DEL	BIT(7)
#define INT_AES_KEY_SWITCH_DONE	BIT(8)
#define INT_TOD_UPDATE_DONE	BIT(9)
#define INT_TOD_1PPS		BIT(10)
#define INT_DYING_GASP		BIT(11)
/*
 * G_INT_STATUS layout from the vendor EN7521/EN7523 register header.
 * Bits 13 and 14 are reserved on EN7523. Bit 15 reports completion of
 * the grant-size calculation used by the EN7523 DBA block.
 */
#define INT_CAL_GNT_SIZE_DONE	BIT(15)
#define INT_RX_ERR		BIT(16)
#define INT_FIFO_ERR		BIT(17)
#define INT_BST_SGL_DIFF	BIT(18)
#define INT_TX_LATE_START	BIT(19)
#define INT_RX_EOF_ERR		BIT(20)
#define INT_RX_GEM_INTLV_ERR	BIT(21)
#define INT_BFIFO_FULL		BIT(22)
#define INT_SFIFO_FULL		BIT(23)
#define INT_O5_EQD_ADJ_DONE	BIT(24)
#define INT_OLT_DS_FEC_CHG	BIT(25)
#define INT_ONU_US_FEC_CHG	BIT(26)
#define INT_POP_UP_RECV_O6	BIT(27)
#define INT_FWI			BIT(28)
#define INT_LWI			BIT(29)
#define INT_BWM_STOP_TIME_ERR	BIT(30)
#define INT_BWM_US_FEC_ERR	BIT(31)

#define GPON_INT_ACTIVATION_MASK	(INT_PLOAMD_RECV | INT_SN_REQ_RECV | \
				 INT_SN_ONU_SEND_O3 | INT_RANGING_REQ_RECV | \
				 INT_SN_ONU_SEND_O4 | INT_SN_REQ_CRS | \
				 INT_LOSS_GEM_DEL | INT_AES_KEY_SWITCH_DONE | \
				 INT_DYING_GASP | INT_CAL_GNT_SIZE_DONE)
/*
 * The EN7523 vendor driver enables only the common receive/burst errors.
 * The interleave, BWM FIFO and BWM timing interrupts are EN7521-only and
 * may expose unrelated status bits when enabled on EN7523.
 */
#define GPON_INT_ERROR_MASK		(INT_RX_ERR | INT_FIFO_ERR | \
				 INT_BST_SGL_DIFF | INT_TX_LATE_START | \
				 INT_RX_EOF_ERR)
#define GPON_INT_DEFAULT_MASK		(GPON_INT_ACTIVATION_MASK | \
				 GPON_INT_ERROR_MASK)

/* G_GEM_PORT_CFG */
#define GEM_CMD_WRITE		BIT(31)
#define GEM_ENCRYPT		BIT(17)
#define GEM_VALID		BIT(16)

/* G_GEM_PORT_STS */
#define GEM_CMD_DONE		BIT(31)
#define GEM_STS_ENCRYPT		BIT(1)
#define GEM_STS_VALID		BIT(0)

/* G_GEM_TBL_INIT */
#define GEM_TBL_INIT_DONE	BIT(8)
#define GEM_TBL_INIT_START	BIT(0)

/* G_PLOAMu_FIFO_STS */
#define PLOAMu_FIFO_AVAIL_MASK	0xFF

/* G_PLOAMd_FIFO_STS */
#define PLOAMd_FIFO_USED_MASK	0xFF

/* G_TCONT_ID pair register */
#define TCONT_ID_MASK		0x0FFF
#define TCONT0_VALID		BIT(15)
#define TCONT1_ID_SHIFT		16
#define TCONT1_VALID		BIT(31)
#define TCONT_PAIR_INVALID	0x00000000	/* validity bits cleared */

/* G_TCONT_ID_16_31_CFG */
#define TCONT16_CMD_EXEC	BIT(31)
#define TCONT16_VALID		BIT(27)
#define TCONT16_IDX_SHIFT	16
#define TCONT16_ALLOC_MASK	0x0FFF

/* G_TCONT_ID_16_31_STS */
#define TCONT16_CMD_DONE	BIT(31)

/* G_MIB_TBL_INIT */
#define MIB_TBL_INIT_DONE	BIT(8)
#define MIB_TBL_INIT_START	BIT(0)

/* G_OMCI_ID */
#define OMCI_PORT_VLD		BIT(16)
#define OMCI_GPID_MASK		0xFFF

/* G_AES_CFG: bits[29:0] = key-switch superframe counter */
#define AES_KEY_SWITCH_CNT_MASK	0x3FFFFFFF

/* G_PRE_ASSIGNED_DLY */
#define PRE_DLY_EN		BIT(31)
#define PRE_DLY_MASK		0xFFFF

/* G_MBI_MPI_STOP */
#define MBI_RX_STOP		BIT(0)
#define MBI_TX_STOP		BIT(8)

/* DBG_DLY */
#define DBG_DLY_FINE_INT_MASK	GENMASK(15, 8)
#define DBG_DLY_FINE_INT_DEFAULT	0x0D
#define DBG_DLY_RESET_DEFAULT	0x80800F00

/* DBG_BWM_FILTER_CTRL */
#define BWM_FILTER_LEN_VALID_CHECK_EN	BIT(17)

/* G_DBG_TX_SYNC_OFFSET bits[1:0] = internal byte delay */
#define DBG_TX_SYNC_OFFSET_MASK	GENMASK(1, 0)

/*
 * The EN7523 vendor driver keeps 0x058b while the MAC is reset/O1, then
 * switches to 0x0577 before serial-number activation in O2.
 */
#define GPON_RSP_TIME_RESET		0x058b
#define GPON_RSP_TIME_ACTIVATION	0x0577
#define GPON_IDLE_GEM_THLD_DEFAULT	0x001A

/* TO1 timer: 10 seconds in O3/O4 without Ranging_Time → return to O2 */
#define GPON_TO1_MS		10000
/* TO2 timer: 100 ms in O6 without Popup/Swift_Popup → reset to O1 */
#define GPON_TO2_MS		100
/* Restart delay after an OLT Deactivate_ONU-ID request. */
#define GPON_DEACTIVATE_RESTART_MS	500
#define GPON_REARM_RETRY_MS		1000

#define GPON_MAX_GEM_ID		4096
#define GPON_MAX_TCONT		32
#define GPON_TCONT_UNASSIGNED	0xffff
#define GPON_TCONT_ENTITY_UNASSIGNED	0xffff
#define GPON_SN_REQ_THRESHOLD	10

#define GPON_PLOAM_RX_QUEUE_LEN	128
#define GPON_PLOAM_RX_QUEUE_MASK	(GPON_PLOAM_RX_QUEUE_LEN - 1)

#define GPON_CMD_TIMEOUT_US		10000
#define GPON_TABLE_TIMEOUT_US		100000
#define GPON_PLOAM_TX_TIMEOUT_US	1000

/*
 * Effective TX-enable guard observed on the stock RTF8225VW SDK.
 * The generic v1 source uses 24, but the board firmware programs 20 in
 * both the GPON MAC and digital PHY after Upstream_Overhead.
 */
#define GPON_PHY_GUARD_BIT_NUM	20

/* -----------------------------------------------------------------------
 * GPON private data
 * -------------------------------------------------------------------- */

static const char *gpon_state_name(enum gpon_state state)
{
	switch (state) {
	case GPON_O1_INITIAL:
		return "O1-initial";
	case GPON_O2_STANDBY:
		return "O2-standby";
	case GPON_O3_SERIAL_NUMBER:
		return "O3-serial-number";
	case GPON_O4_RANGING:
		return "O4-ranging";
	case GPON_O5_OPERATION:
		return "O5-operation";
	case GPON_O6_POPUP:
		return "O6-popup";
	case GPON_O7_EMERGENCY_STOP:
		return "O7-emergency-stop";
	default:
		return "unknown";
	}
}

struct gpon_priv {
	void __iomem		*base;
	void __iomem		*regs;
	struct device		*dev;
	struct regmap		*scu;
	struct phy		*phy;
	bool			phy_initialized;
	bool			phy_powered;
	struct device_node	*eth_node;
	struct net_device	*gdm_dev;
	bool			started;
	struct airoha_gpon_omci omci;
	struct airoha_xpon_oam_handler omci_handler;
	struct sfp_bus		*sfp_bus;
	struct device		*lddla_dev;
	int			irq;
	bool			irq_enabled;

	struct ploam_priv	*ploam;

	u8			sn[8];
	u8			passwd[10];
	struct omci_identity	identity;
	u8			aes_key[16];

	/*
	 * Serialize downstream PLOAM protocol handling and activation
	 * timeouts. The IRQ top half only performs the hardware-critical
	 * FIFO drain and ONU-ID fast path.
	 */
	struct workqueue_struct	*fsm_wq;
	struct work_struct	irq_work;
	struct delayed_work	to1_work;	/* O3/O4: 10 s → O2 */
	struct delayed_work	to2_work;	/* O6: 100 ms → O1 */
	struct delayed_work	restart_work;	/* OLT deactivation recovery */
	atomic_t		pending_irqs;

	/*
	 * The IRQ top half drains the hardware FIFO into this single-producer,
	 * single-consumer queue. Keeping the FIFO drain in hard IRQ avoids
	 * losing the first ranging allocation while the ordered workqueue is
	 * processing printk output or earlier PLOAM copies.
	 */
	struct ploam_msg	ploam_rx_queue[GPON_PLOAM_RX_QUEUE_LEN];
	u16			ploam_rx_head;
	u16			ploam_rx_tail;
	u32			ploam_rx_drops;
	u32			ploam_rx_messages;
	u32			assign_onu_fastpath;

	/* BER measurement timer */
	struct timer_list	ber_timer;
	u32			ber_interval_ms;

	/* Protects the hardware T-CONT table and its software allocation maps. */
	struct mutex		tcont_lock;
	u16			tcont_alloc_id[GPON_MAX_TCONT];
	u16			tcont_entity_id[GPON_MAX_TCONT];

	/* EqD state for O5 incremental adjustment */
	u32			byte_delay;
	u32			bit_delay;

};

/* -----------------------------------------------------------------------
 * Register accessors
 * -------------------------------------------------------------------- */

static inline u32 gpon_read(struct gpon_priv *priv, u32 reg)
{
	return readl(priv->regs + reg);
}

static inline void gpon_write(struct gpon_priv *priv, u32 reg, u32 val)
{
	writel(val, priv->regs + reg);
}

static void gpon_dump_activation_regs(struct gpon_priv *priv,
				      const char *reason)
{
	u32 phy_tx_frames = 0, phy_tx_bursts = 0;
	int phy_ret;

	phy_ret = airoha_xpon_phy_get_gpon_tx_counters(priv->phy,
						       &phy_tx_frames,
						       &phy_tx_bursts);
	dev_info(priv->dev,
		 "GPON activation dump (%s): state=%s onu=%#06x act=%#08x rsp=%#06x pre_delay=%#010x eqd=%#010x sn_cfg=%#010x guard=%#010x type12=%#010x type3=%#010x dbg_dly=%#010x tx_sync=%#010x plou_fifo=%#010x int=%#010x/%#010x pending=%#010x rxq=%u/%u fast_assign=%u phy_tx=%#010x/%#010x phy_ret=%d\n",
		 reason, gpon_state_name(ploam_get_state(priv->ploam)),
		 gpon_read(priv, GPON_ONU_ID),
		 gpon_read(priv, GPON_ACTIVATION_ST),
		 gpon_read(priv, GPON_RSP_TIME),
		 gpon_read(priv, GPON_PRE_ASSIGNED_DLY),
		 gpon_read(priv, GPON_EQD),
		 gpon_read(priv, GPON_SN_MSG_CFG),
		 gpon_read(priv, GPON_PLOu_GUARD_BIT),
		 gpon_read(priv, GPON_PLOu_PRMBL_TYPE1_2),
		 gpon_read(priv, GPON_PLOu_PRMBL_TYPE3),
		 gpon_read(priv, GPON_DBG_DLY),
		 gpon_read(priv, GPON_DBG_TX_SYNC_OFFSET),
		 gpon_read(priv, GPON_PLOAMu_FIFO_STS),
		 gpon_read(priv, GPON_INT_STATUS),
		 gpon_read(priv, GPON_INT_ENABLE),
		 (u32)atomic_read(&priv->pending_irqs),
		 READ_ONCE(priv->ploam_rx_messages),
		 READ_ONCE(priv->ploam_rx_drops),
		 READ_ONCE(priv->assign_onu_fastpath),
		 phy_tx_frames, phy_tx_bursts, phy_ret);
}

static inline void gpon_set_bits(struct gpon_priv *priv, u32 reg, u32 bits)
{
	gpon_write(priv, reg, gpon_read(priv, reg) | bits);
}

static inline void gpon_clear_bits(struct gpon_priv *priv, u32 reg, u32 bits)
{
	gpon_write(priv, reg, gpon_read(priv, reg) & ~bits);
}

static inline void gpon_rmw(struct gpon_priv *priv, u32 reg, u32 mask, u32 val)
{
	gpon_write(priv, reg, (gpon_read(priv, reg) & ~mask) | val);
}

/* -----------------------------------------------------------------------
 * Low-level hardware helpers
 * -------------------------------------------------------------------- */

static int gpon_wait_bits(struct gpon_priv *priv, u32 reg, u32 mask,
			  unsigned int timeout_us)
{
	u32 val;

	return readl_poll_timeout_atomic(priv->regs + reg, val, val & mask,
					  1, timeout_us);
}

static int gpon_set_fe_mode(struct gpon_priv *priv)
{
	return airoha_xpon_set_fe_mode(priv->dev, priv->eth_node,
				      AIROHA_XPON_MODE_GPON);
}

static int gpon_set_fe_datapath(struct gpon_priv *priv, bool enable)
{
	return airoha_xpon_set_fe_datapath(priv->dev, priv->eth_node,
					  AIROHA_XPON_MODE_GPON, enable);
}

static int gpon_prepare_hardware(struct gpon_priv *priv)
{
	u32 mbi;
	int ret;

	dev_info(priv->dev,
		 "GPON prepare: mbi=%#08x activation=%#08x int_status=%#08x int_enable=%#08x\n",
		 gpon_read(priv, GPON_MBI_MPI_STOP),
		 gpon_read(priv, GPON_ACTIVATION_ST),
		 gpon_read(priv, GPON_INT_STATUS),
		 gpon_read(priv, GPON_INT_ENABLE));

	/* The EN7523 FE must release GDM2 in GPON mode before the MAC starts. */
	ret = gpon_set_fe_mode(priv);
	if (ret)
		return ret;

	/* Select GPON instead of EPON on the shared xPON WAN interface. */
	dev_info(priv->dev, "selecting GPON on SCU WAN mux\n");
	ret = airoha_xpon_select_wan(priv->scu, AIROHA_XPON_MODE_GPON);
	if (ret)
		return dev_err_probe(priv->dev, ret,
				     "failed to select GPON WAN mode\n");

	/* Match gponDevMbiStop(XPON_DISABLE). The EN757x vendor sequence
	 * releases the GPON/PSE MBI first, waits 1 ms, releases the two GDM2
	 * downstream channels, then waits another 1 ms. Do not collapse these
	 * steps: the GEM indirect command engine is not ready immediately.
	 */
	dev_info(priv->dev, "releasing GPON RX/TX MBI\n");
	gpon_clear_bits(priv, GPON_MBI_MPI_STOP, MBI_RX_STOP | MBI_TX_STOP);
	mdelay(1);
	dev_info(priv->dev, "GPON MBI after release: %#08x\n",
		gpon_read(priv, GPON_MBI_MPI_STOP));

	ret = gpon_set_fe_datapath(priv, true);
	if (ret)
		return ret;
	mdelay(1);

	gpon_write(priv, GPON_DBG_GRP_0, ~0U);
	gpon_write(priv, GPON_DBG_GRP_1, ~0U);

	/*
	 * Restore the complete EN7523 ASIC burst-delay value before applying
	 * the runtime fine-delay value. Preserving bootloader leftovers here
	 * changes the fixed RX delay and TX delay fields and makes upstream
	 * burst timing depend on the previous firmware.
	 */
	gpon_write(priv, GPON_DBG_DLY, DBG_DLY_RESET_DEFAULT);
	gpon_rmw(priv, GPON_DBG_DLY, DBG_DLY_FINE_INT_MASK,
		 FIELD_PREP(DBG_DLY_FINE_INT_MASK, DBG_DLY_FINE_INT_DEFAULT));
	gpon_rmw(priv, GPON_DBG_IDLE_GEM_THLD, GENMASK(15, 0),
		 GPON_IDLE_GEM_THLD_DEFAULT);

	/* Apply the vendor workaround for invalid BWmap length filtering. */
	gpon_clear_bits(priv, GPON_DBG_BWM_FILTER_CTRL,
			BWM_FILTER_LEN_VALID_CHECK_EN);

	mbi = gpon_read(priv, GPON_MBI_MPI_STOP);
	if (mbi & (MBI_RX_STOP | MBI_TX_STOP))
		return dev_err_probe(priv->dev, -EIO,
				     "failed to start GPON MBI: %#08x\n", mbi);

	dev_info(priv->dev,
		 "GPON hardware prepared: mbi=%#08x dbg_dly=%#08x idle_gem=%#08x bwm_filter=%#08x\n",
		 mbi, gpon_read(priv, GPON_DBG_DLY),
		 gpon_read(priv, GPON_DBG_IDLE_GEM_THLD),
		 gpon_read(priv, GPON_DBG_BWM_FILTER_CTRL));
	return 0;
}

static void gpon_reset_activation_context(struct gpon_priv *priv)
{
	u32 sn_cfg;

	gpon_clear_bits(priv, GPON_GBL_CFG, GBL_CFG_US_FEC_EN);
	gpon_write(priv, GPON_ONU_ID, PLOAM_ONU_UNASSIGNED);
	gpon_write(priv, GPON_ACTIVATION_ST, GPON_O1_INITIAL);
	gpon_write(priv, GPON_PRE_ASSIGNED_DLY, 0);
	gpon_write(priv, GPON_EQD, 0);
	gpon_write(priv, GPON_RSP_TIME, GPON_RSP_TIME_RESET);

	/*
	 * Preserve the transmitter power mode while restoring the vendor
	 * serial-request threshold and clearing only the random delay. A zero
	 * threshold makes INT_SN_REQ_CRS continuously retrigger.
	 */
	sn_cfg = gpon_read(priv, GPON_SN_MSG_CFG);
	sn_cfg &= ~(SN_MSG_CFG_SN_REQ_THR_MASK |
		    SN_MSG_CFG_RANDOM_DELAY_MASK);
	sn_cfg |= FIELD_PREP(SN_MSG_CFG_SN_REQ_THR_MASK,
			     GPON_SN_REQ_THRESHOLD);
	gpon_write(priv, GPON_SN_MSG_CFG, sn_cfg);
	priv->byte_delay = 0;
	priv->bit_delay = 0;
}

static int gpon_dev_init(struct gpon_priv *priv)
{
	/*
	 * Reset all activation state that can survive an optical or interface
	 * restart. Do not sweep the indirect GEM/T-CONT tables here; the EN7523
	 * command engine may not acknowledge those accesses during startup.
	 */
	gpon_reset_activation_context(priv);
	mutex_lock(&priv->tcont_lock);
	memset(priv->tcont_alloc_id, 0xff, sizeof(priv->tcont_alloc_id));
	memset(priv->tcont_entity_id, 0xff, sizeof(priv->tcont_entity_id));
	mutex_unlock(&priv->tcont_lock);
	dev_info(priv->dev,
		 "GPON activation reset: onu=%#x state=%#x rsp=%#x pre=%#x eqd=%#x\n",
		 gpon_read(priv, GPON_ONU_ID),
		 gpon_read(priv, GPON_ACTIVATION_ST),
		 gpon_read(priv, GPON_RSP_TIME),
		 gpon_read(priv, GPON_PRE_ASSIGNED_DLY),
		 gpon_read(priv, GPON_EQD));

	return 0;
}

static int gpon_load_credentials(struct gpon_priv *priv)
{
	static const u8 default_sn[8] = {
		'M', 'T', 'K', 'G', 0x00, 0x00, 0x00, 0x01
	};
	struct omci_identity *identity = &priv->identity;
	int ret;

	ret = omci_identity_load(priv->dev, identity);
	if (ret)
		return ret;
	if (!(identity->valid & OMCI_IDENTITY_F_SERIAL_NUMBER)) {
		memcpy(identity->serial_number, default_sn,
		       sizeof(identity->serial_number));
		memcpy(identity->vendor_id, default_sn,
		       sizeof(identity->vendor_id));
		identity->valid |= OMCI_IDENTITY_F_SERIAL_NUMBER |
				   OMCI_IDENTITY_F_VENDOR_ID;
		identity->serial_source = OMCI_CONFIG_SOURCE_DEFAULT;
		identity->vendor_source = OMCI_CONFIG_SOURCE_DEFAULT;
		dev_warn(priv->dev,
			 "GPON serial number missing; using development default\n");
	}
	if (!(identity->valid & OMCI_IDENTITY_F_PASSWORD)) {
		memset(identity->password, 0, sizeof(identity->password));
		identity->valid |= OMCI_IDENTITY_F_PASSWORD;
		identity->password_source = OMCI_CONFIG_SOURCE_DEFAULT;
	}

	memcpy(priv->sn, identity->serial_number, sizeof(priv->sn));
	memcpy(priv->passwd, identity->password, sizeof(priv->passwd));
	dev_info(priv->dev,
		 "loaded normalized GPON identity (serial source %u, password source %u)\n",
		 identity->serial_source, identity->password_source);
	return 0;
}

static void gpon_set_serial_number_regs(struct gpon_priv *priv)
{
	u32 random_delay, sn_cfg, sn_req_threshold, tx_power_mode;
	u32 vendor = get_unaligned_be32(priv->sn);
	u32 vs_sn = get_unaligned_be32(priv->sn + 4);

	/*
	 * Match vendor gponDevSetSerialNumber(): sn[0] occupies bits 31:24
	 * and sn[7] occupies bits 7:0.  These registers feed the automatic
	 * Serial_Number_ONU generator directly.
	 */
	gpon_write(priv, GPON_VENDOR_ID, vendor);
	gpon_write(priv, GPON_VS_SN, vs_sn);

	sn_cfg = gpon_read(priv, GPON_SN_MSG_CFG);
	sn_req_threshold = FIELD_GET(SN_MSG_CFG_SN_REQ_THR_MASK, sn_cfg);
	tx_power_mode = FIELD_GET(SN_MSG_CFG_TX_POWER_MODE_MASK, sn_cfg);
	random_delay = FIELD_GET(SN_MSG_CFG_RANDOM_DELAY_MASK, sn_cfg);

	dev_info(priv->dev,
		 "GPON hardware SN response: serial=%8phN vendor=%#08x vs_sn=%#08x readback=%#08x/%#08x cfg=%#08x threshold=%u tx_power=%u random_delay=%u\n",
		 priv->sn, vendor, vs_sn,
		 gpon_read(priv, GPON_VENDOR_ID),
		 gpon_read(priv, GPON_VS_SN),
		 sn_cfg, sn_req_threshold, tx_power_mode, random_delay);
}

static void gpon_load_aes_shadow(struct gpon_priv *priv, const u8 key[16],
				  u32 switch_superframe)
{
	int i;

	for (i = 0; i < 4; i++)
		gpon_write(priv, GPON_AES_SHADOW_KEY0 + i * 4,
			   get_unaligned_be32(key + i * 4));

	gpon_write(priv, GPON_AES_CFG,
		   switch_superframe & AES_KEY_SWITCH_CNT_MASK);
}

static int __gpon_set_tcont_hw(struct gpon_priv *priv, unsigned int index,
			       u16 alloc_id, bool valid)
{
	if (index < 16) {
		unsigned int reg_idx = index / 2;
		u32 val = gpon_read(priv, GPON_TCONT_ID_0_1 + reg_idx * 4);

		if (index & 1) {
			val &= ~((TCONT_ID_MASK << TCONT1_ID_SHIFT) |
				 TCONT1_VALID);
			if (valid)
				val |= ((alloc_id & TCONT_ID_MASK) <<
					TCONT1_ID_SHIFT) | TCONT1_VALID;
		} else {
			val &= ~(TCONT_ID_MASK | TCONT0_VALID);
			if (valid)
				val |= (alloc_id & TCONT_ID_MASK) |
				       TCONT0_VALID;
		}
		gpon_write(priv, GPON_TCONT_ID_0_1 + reg_idx * 4, val);
		return 0;
	}

	if (index < GPON_MAX_TCONT) {
		u32 cfg = TCONT16_CMD_EXEC |
			  ((index - 16) << TCONT16_IDX_SHIFT);

		if (valid)
			cfg |= TCONT16_VALID |
			       (alloc_id & TCONT16_ALLOC_MASK);
		gpon_write(priv, GPON_TCONT_ID_16_31_CFG, cfg);
		if (gpon_wait_bits(priv, GPON_TCONT_ID_16_31_STS,
				   TCONT16_CMD_DONE, GPON_CMD_TIMEOUT_US))
			return -ETIMEDOUT;
		return 0;
	}

	return -EINVAL;
}

static int gpon_config_tcont_hw(struct gpon_priv *priv, unsigned int index,
				u16 alloc_id, bool valid)
{
	int ret;

	if (!priv->gdm_dev)
		return -ENODEV;

	if (!valid) {
		ret = airoha_eth_set_xpon_tcont_channel(priv->gdm_dev, index,
							false);
		if (ret)
			return ret;
		return __gpon_set_tcont_hw(priv, index, alloc_id, false);
	}

	ret = __gpon_set_tcont_hw(priv, index, alloc_id, true);
	if (ret)
		return ret;

	ret = airoha_eth_set_xpon_tcont_channel(priv->gdm_dev, index, true);
	if (ret)
		__gpon_set_tcont_hw(priv, index, alloc_id, false);

	return ret;
}

static int gpon_set_tcont_hw(struct gpon_priv *priv, unsigned int index,
			     u16 alloc_id, bool valid)
{
	int ret;

	if (index >= GPON_MAX_TCONT)
		return -EINVAL;

	mutex_lock(&priv->tcont_lock);
	ret = gpon_config_tcont_hw(priv, index, alloc_id, valid);
	if (!ret) {
		priv->tcont_alloc_id[index] = valid ? alloc_id :
						 GPON_TCONT_UNASSIGNED;
		if (!valid)
			priv->tcont_entity_id[index] =
				GPON_TCONT_ENTITY_UNASSIGNED;
	}
	mutex_unlock(&priv->tcont_lock);

	return ret;
}

static int gpon_find_tcont_alloc_locked(struct gpon_priv *priv, u16 alloc_id)
{
	int index;

	for (index = 1; index < GPON_MAX_TCONT; index++)
		if (priv->tcont_alloc_id[index] == alloc_id)
			return index;

	return -ENOENT;
}

static int gpon_find_tcont_entity_locked(struct gpon_priv *priv, u16 entity_id)
{
	int index;

	for (index = 1; index < GPON_MAX_TCONT; index++)
		if (priv->tcont_entity_id[index] == entity_id)
			return index;

	return -ENOENT;
}

static int gpon_find_free_tcont_locked(struct gpon_priv *priv)
{
	int index;

	for (index = 1; index < GPON_MAX_TCONT; index++)
		if (priv->tcont_alloc_id[index] == GPON_TCONT_UNASSIGNED)
			return index;

	return -ENOSPC;
}

static int gpon_tcont_entity_to_index(struct gpon_priv *priv, u16 entity_id,
				      unsigned int *index)
{
	int found;

	mutex_lock(&priv->tcont_lock);
	found = gpon_find_tcont_entity_locked(priv, entity_id);
	if (found >= 0)
		*index = found;
	mutex_unlock(&priv->tcont_lock);

	return found < 0 ? found : 0;
}

static int gpon_read_gem_port_hw(struct gpon_priv *priv, u16 gem_port_id,
				 bool *valid, bool *encrypted)
{
	u32 status;

	gpon_write(priv, GPON_GEM_PORT_CFG, gem_port_id);
	/* Let the command engine clear the completion bit from the previous
	 * transaction before polling it again.
	 */
	udelay(1);
	if (gpon_wait_bits(priv, GPON_GEM_PORT_STS, GEM_CMD_DONE,
			   GPON_CMD_TIMEOUT_US))
		return -ETIMEDOUT;

	status = gpon_read(priv, GPON_GEM_PORT_STS);
	*valid = !!(status & GEM_STS_VALID);
	*encrypted = !!(status & GEM_STS_ENCRYPT);

	return 0;
}

static int gpon_set_gem_port_hw(struct gpon_priv *priv, u16 gem_port_id,
				bool valid, bool encrypted)
{
	bool read_encrypted, read_valid;
	u32 cfg;
	int ret, retry;

	if (gem_port_id >= GPON_MAX_GEM_ID)
		return -EINVAL;

	cfg = GEM_CMD_WRITE | gem_port_id;
	if (valid)
		cfg |= GEM_VALID;
	if (valid && encrypted)
		cfg |= GEM_ENCRYPT;

	for (retry = 0; retry < 3; retry++) {
		gpon_write(priv, GPON_GEM_PORT_CFG, cfg);
		/* G_GEM_PORT_STS may still contain the completion state of the
		 * preceding command on the first bus read.
		 */
		udelay(1);
		ret = gpon_wait_bits(priv, GPON_GEM_PORT_STS, GEM_CMD_DONE,
				     GPON_CMD_TIMEOUT_US);
		if (ret)
			continue;

		ret = gpon_read_gem_port_hw(priv, gem_port_id,
					    &read_valid, &read_encrypted);
		if (!ret && read_valid == valid &&
		    read_encrypted == (valid && encrypted))
			return 0;

		usleep_range(10, 20);
	}

	dev_err(priv->dev,
		"GPON GEM port %u configuration failed: valid=%u encrypted=%u ret=%d\n",
		gem_port_id, valid, valid && encrypted, ret);

	return ret ?: -EIO;
}

static int gpon_set_alloc_id_hw(struct gpon_priv *priv, u16 alloc_id,
				bool allocate)
{
	int index, ret = 0;

	if (alloc_id > TCONT_ID_MASK)
		return -EINVAL;

	mutex_lock(&priv->tcont_lock);
	index = gpon_find_tcont_alloc_locked(priv, alloc_id);
	if (index >= 0) {
		if (!allocate) {
			ret = gpon_config_tcont_hw(priv, index, alloc_id, false);
			if (!ret) {
				priv->tcont_alloc_id[index] = GPON_TCONT_UNASSIGNED;
				priv->tcont_entity_id[index] =
					GPON_TCONT_ENTITY_UNASSIGNED;
			}
		}
		goto out;
	}

	if (!allocate)
		goto out;

	index = gpon_find_free_tcont_locked(priv);
	if (index < 0) {
		ret = index;
		goto out;
	}

	ret = gpon_config_tcont_hw(priv, index, alloc_id, true);
	if (!ret)
		priv->tcont_alloc_id[index] = alloc_id;
out:
	mutex_unlock(&priv->tcont_lock);
	return ret;
}

static int gpon_set_omci_tcont_hw(struct gpon_priv *priv, u16 entity_id,
				  u16 alloc_id, bool valid,
				  unsigned int *channel)
{
	int old_index, index, ret = 0;

	if (valid && alloc_id > TCONT_ID_MASK)
		return -EINVAL;

	mutex_lock(&priv->tcont_lock);
	old_index = gpon_find_tcont_entity_locked(priv, entity_id);
	if (!valid) {
		if (old_index < 0)
			goto out;

		alloc_id = priv->tcont_alloc_id[old_index];
		ret = gpon_config_tcont_hw(priv, old_index, alloc_id, false);
		if (!ret) {
			priv->tcont_alloc_id[old_index] = GPON_TCONT_UNASSIGNED;
			priv->tcont_entity_id[old_index] =
				GPON_TCONT_ENTITY_UNASSIGNED;
			*channel = old_index;
		}
		goto out;
	}

	index = gpon_find_tcont_alloc_locked(priv, alloc_id);
	if (index < 0) {
		index = gpon_find_free_tcont_locked(priv);
		if (index < 0) {
			ret = index;
			goto out;
		}

		ret = gpon_config_tcont_hw(priv, index, alloc_id, true);
		if (ret)
			goto out;
		priv->tcont_alloc_id[index] = alloc_id;
	}

	if (old_index >= 0 && old_index != index)
		priv->tcont_entity_id[old_index] =
			GPON_TCONT_ENTITY_UNASSIGNED;
	priv->tcont_entity_id[index] = entity_id;
	*channel = index;
out:
	mutex_unlock(&priv->tcont_lock);
	return ret;
}

/* -----------------------------------------------------------------------
 * Hardware-side PLOAM FIFO access
 * -------------------------------------------------------------------- */

static inline u32 gpon_ploam_read_word(struct gpon_priv *priv)
{
	/*
	 * The GPON FIFO exposes each PLOAM word directly in protocol order:
	 * bits 31:24 contain ONU-ID and bits 23:16 contain the message type.
	 * This is a register value, not a byte array in CPU memory, so applying
	 * be32_to_cpu() swaps valid messages on little-endian EN7523 systems.
	 */
	return gpon_read(priv, GPON_PLOAMd_RDATA);
}

static inline void gpon_ploam_write_word(struct gpon_priv *priv, u32 val)
{
	/* Keep the same register/protocol ordering for upstream messages. */
	gpon_write(priv, GPON_PLOAMu_WDATA, val);
}

static int gpon_wait_ploam_tx_space(struct gpon_priv *priv, u32 *available)
{
	u32 status;
	int ret;

	ret = readl_poll_timeout_atomic(priv->regs + GPON_PLOAMu_FIFO_STS,
					status,
					(status & PLOAMu_FIFO_AVAIL_MASK) >=
					PLOAM_WORDS, 1,
					GPON_PLOAM_TX_TIMEOUT_US);
	*available = status & PLOAMu_FIFO_AVAIL_MASK;

	return ret;
}

static void gpon_hw_send_ploam(struct gpon_priv *priv,
			       const struct ploam_msg *msg, int times)
{
	u8 onu_id = msg->value[0] >> 24;
	u8 type = msg->value[0] >> 16;
	int t;

	dev_info(priv->dev,
			    "PLOAM TX: onu=%u type=%#04x copies=%d words=%08x/%08x/%08x\n",
		 onu_id, type, times, msg->value[0], msg->value[1],
		 msg->value[2]);
	for (t = 0; t < times; t++) {
		u32 avail;
		int ret;

		ret = gpon_wait_ploam_tx_space(priv, &avail);
		if (ret) {
			dev_warn_ratelimited(priv->dev,
					     "PLOAM TX FIFO timeout: avail=%u requested_words=%u copy=%d/%d\n",
					     avail, PLOAM_WORDS, t + 1, times);
			break;
		}

		gpon_ploam_write_word(priv, msg->value[0]);
		gpon_ploam_write_word(priv, msg->value[1]);
		gpon_ploam_write_word(priv, msg->value[2]);
	}
}

static void gpon_ploam_rx_queue_reset(struct gpon_priv *priv)
{
	WRITE_ONCE(priv->ploam_rx_head, 0);
	WRITE_ONCE(priv->ploam_rx_tail, 0);
}

static bool gpon_ploam_rx_queue_push(struct gpon_priv *priv,
				     const struct ploam_msg *msg)
{
	u16 head = READ_ONCE(priv->ploam_rx_head);
	u16 next = (head + 1) & GPON_PLOAM_RX_QUEUE_MASK;

	/* Pairs with the consumer release when it advances the tail. */
	if (next == smp_load_acquire(&priv->ploam_rx_tail)) {
		priv->ploam_rx_drops++;
		return false;
	}

	priv->ploam_rx_queue[head] = *msg;
	/* Publish the message before making the new head visible. */
	smp_store_release(&priv->ploam_rx_head, next);
	priv->ploam_rx_messages++;

	return true;
}

static bool gpon_ploam_rx_queue_pop(struct gpon_priv *priv,
				    struct ploam_msg *msg)
{
	u16 tail = READ_ONCE(priv->ploam_rx_tail);

	/* Pairs with the producer release after storing a message. */
	if (tail == smp_load_acquire(&priv->ploam_rx_head))
		return false;

	*msg = priv->ploam_rx_queue[tail];
	/* Finish reading the slot before allowing the producer to reuse it. */
	smp_store_release(&priv->ploam_rx_tail,
			  (tail + 1) & GPON_PLOAM_RX_QUEUE_MASK);

	return true;
}

static void gpon_fastpath_assign_onu_id(struct gpon_priv *priv,
					const struct ploam_msg *msg)
{
	u8 msg_sn[8];
	u8 onu_id;

	if ((msg->value[0] >> 24) != PLOAM_ONU_BCAST ||
	    ((msg->value[0] >> 16) & 0xff) != PLOAM_DOWN_ASSIGN_ONU_ID)
		return;

	if ((gpon_read(priv, GPON_ACTIVATION_ST) & 0x7) !=
	    GPON_O3_SERIAL_NUMBER)
		return;

	msg_sn[0] = msg->value[0];
	msg_sn[1] = msg->value[1] >> 24;
	msg_sn[2] = msg->value[1] >> 16;
	msg_sn[3] = msg->value[1] >> 8;
	msg_sn[4] = msg->value[1];
	msg_sn[5] = msg->value[2] >> 24;
	msg_sn[6] = msg->value[2] >> 16;
	msg_sn[7] = msg->value[2] >> 8;
	if (memcmp(msg_sn, priv->sn, sizeof(msg_sn)))
		return;

	onu_id = (msg->value[0] >> 8) & ONU_ID_MASK;

	/*
	 * Match the vendor ISR ordering. The OLT may issue the first ranging
	 * allocation immediately after Assign_ONU-ID, so both registers must
	 * be visible before the deferred protocol state machine runs.
	 */
	gpon_write(priv, GPON_ACTIVATION_ST, GPON_O4_RANGING);
	/* Make O4 visible before validating the assigned ONU-ID. */
	wmb();
	gpon_write(priv, GPON_ONU_ID, ONU_ID_VLD | onu_id);
	priv->assign_onu_fastpath++;
}

static void gpon_drain_ploam_fifo_irq(struct gpon_priv *priv)
{
	int budget = GPON_PLOAM_RX_QUEUE_LEN - 1;

	while (budget--) {
		struct ploam_msg msg;
		u32 depth;

		depth = gpon_read(priv, GPON_PLOAMd_FIFO_STS) &
			PLOAMd_FIFO_USED_MASK;
		if (depth < PLOAM_WORDS)
			break;

		msg.value[0] = gpon_ploam_read_word(priv);
		msg.value[1] = gpon_ploam_read_word(priv);
		msg.value[2] = gpon_ploam_read_word(priv);

		gpon_fastpath_assign_onu_id(priv, &msg);
		gpon_ploam_rx_queue_push(priv, &msg);
	}
}

static void gpon_process_ploam_queue(struct gpon_priv *priv)
{
	struct ploam_msg msg;

	while (gpon_ploam_rx_queue_pop(priv, &msg)) {
		dev_dbg(priv->dev,
			"PLOAM RX: onu=%u type=%#04x words=%08x/%08x/%08x\n",
			msg.value[0] >> 24, (msg.value[0] >> 16) & 0xff,
			msg.value[0], msg.value[1], msg.value[2]);
		ploam_handle_downstream(priv->ploam, &msg);
	}
}

/* -----------------------------------------------------------------------
 * ploam_ops callbacks
 * -------------------------------------------------------------------- */

static void gpon_cb_send_upstream(void *hw_priv, const struct ploam_msg *msg,
				   int times)
{
	gpon_hw_send_ploam(hw_priv, msg, times);
}

static void gpon_cb_set_onu_id(void *hw_priv, u8 onu_id)
{
	struct gpon_priv *priv = hw_priv;

	dev_info(priv->dev, "GPON assigned ONU-ID %u\n", onu_id);
	gpon_write(priv, GPON_ONU_ID, ONU_ID_VLD | (onu_id & ONU_ID_MASK));
	airoha_gpon_omci_set_onu_id(&priv->omci, onu_id);
	gpon_dump_activation_regs(priv, "ONU-ID assigned");
}

static void gpon_cb_set_eqd_o4(void *hw_priv, u32 byte_delay, u32 bit_delay)
{
	struct gpon_priv *priv = hw_priv;

	dev_info(priv->dev, "GPON O4 EqD: byte_delay=%u bit_delay=%u\n",
		 byte_delay, bit_delay);
	priv->byte_delay = byte_delay;
	priv->bit_delay  = bit_delay;
	/* Write byte delay to G_EQD; bit delay goes to PHY (not abstracted) */
	gpon_write(priv, GPON_EQD, byte_delay);
	gpon_dump_activation_regs(priv, "O4 EqD programmed");
}

static void gpon_cb_adjust_eqd_o5(void *hw_priv, u32 new_eqd)
{
	struct gpon_priv *priv = hw_priv;
	u32 curr_eqd = ploam_get_eqd(priv->ploam);
	int delta = (int)new_eqd - (int)curr_eqd;
	u32 sync_raw;
	int int_byte_delay;
	int K, A, B, a;

	if (delta == 0) {
		dev_info(priv->dev, "GPON O5 EqD unchanged: %u\n", new_eqd);
		return;
	}

	dev_info(priv->dev,
		 "GPON O5 EqD adjustment: current=%u new=%u delta=%d\n",
		 curr_eqd, new_eqd, delta);

	/* Read internal byte delay from hardware (EN7521 specific register) */
	sync_raw = gpon_read(priv, GPON_DBG_TX_SYNC_OFFSET);
	int_byte_delay = sync_raw & DBG_TX_SYNC_OFFSET_MASK;

	if (delta > 0) {
		K = delta + (int)priv->bit_delay;
		A = K >> 3;
		B = K & 7;
		a = (((int_byte_delay + (A & 3)) <= 3) ? 0 : 8) +
		    (A - (A & 3)) - (A & 3);
		priv->byte_delay += (u32)(a << 3);
		priv->bit_delay   = (u32)B;
	} else {
		K = -delta - (int)priv->bit_delay;
		A = (K > 0) ? ((K >> 3) + ((K & 7) ? 1 : 0)) : 0;
		B = (A << 3) - K;
		a = ((int_byte_delay >= (A & 3)) ? 0 : 8) +
		    (A - (A & 3)) - (A & 3);
		priv->byte_delay -= (u32)(a << 3);
		priv->bit_delay   = (u32)B;
	}

	gpon_write(priv, GPON_EQD, priv->byte_delay);
	dev_info(priv->dev,
		 "GPON O5 EqD programmed: byte_delay=%u bit_delay=%u sync=%#08x\n",
		 priv->byte_delay, priv->bit_delay, sync_raw);
}

static void gpon_cb_enable_us_fec(void *hw_priv)
{
	struct gpon_priv *priv = hw_priv;

	dev_info(priv->dev, "enabling GPON upstream FEC\n");
	gpon_set_bits(priv, GPON_GBL_CFG, GBL_CFG_US_FEC_EN);
}

static void gpon_cb_set_overhead(void *hw_priv,
				  u8 guard_bits, u8 t1_pbits, u8 t2_pbits,
				  u8 t3_pbits, const u8 delim[3],
				  bool delay_mode, u16 delay_time)
{
	struct gpon_priv *priv = hw_priv;
	u32 prmbl, pre_dly;
	int ret;

	dev_info(priv->dev,
		 "GPON overhead: OLT guard=%u SDK guard=%u t1=%u t2=%u t3=%u delay_mode=%u delay=%u delim=%02x:%02x:%02x\n",
		 guard_bits, GPON_PHY_GUARD_BIT_NUM, t1_pbits, t2_pbits,
		 t3_pbits, delay_mode, delay_time,
		 delim[0], delim[1], delim[2]);

	/*
	 * Match the effective RTF8225VW stock configuration:
	 * use the 20-bit TX-enable guard, map PLOAM T2 to PHY T1 and
	 * PLOAM T1 to PHY T2, and preserve explicit zero values.
	 */
	ret = airoha_xpon_phy_set_gpon_overhead(priv->phy,
					       GPON_PHY_GUARD_BIT_NUM,
					       t1_pbits, t2_pbits,
					       t3_pbits, delim);
	if (ret)
		dev_warn(priv->dev,
			 "failed to program GPON PHY overhead: %d\n", ret);

	gpon_write(priv, GPON_PLOu_GUARD_BIT, GPON_PHY_GUARD_BIT_NUM);

	/* G_PLOu_PRMBL_TYPE1_2: t1 in upper 16 bits, t2 in lower 16 bits */
	prmbl = ((u32)t1_pbits << 16) | t2_pbits;
	gpon_write(priv, GPON_PLOu_PRMBL_TYPE1_2, prmbl);

	/* G_PRE_ASSIGNED_DLY */
	pre_dly = (delay_mode ? PRE_DLY_EN : 0) | (delay_time & PRE_DLY_MASK);
	gpon_write(priv, GPON_PRE_ASSIGNED_DLY, pre_dly);

	/*
	 * G_PLOu_OVERHEAD and G_PLOu_DELM_BIT are currently undocumented in
	 * this driver.  Dump the complete burst-generator window before
	 * assigning layouts to those registers.
	 */
	dev_info(priv->dev,
		 "GPON MAC burst regs: overhead=%#010x guard=%#010x type12=%#010x type3=%#010x delimiter=%#010x pre_delay=%#010x\n",
		 gpon_read(priv, GPON_PLOu_OVERHEAD),
		 gpon_read(priv, GPON_PLOu_GUARD_BIT),
		 gpon_read(priv, GPON_PLOu_PRMBL_TYPE1_2),
		 gpon_read(priv, GPON_PLOu_PRMBL_TYPE3),
		 gpon_read(priv, GPON_PLOu_DELM_BIT),
		 gpon_read(priv, GPON_PRE_ASSIGNED_DLY));
}

static void gpon_cb_set_t3_preamble(void *hw_priv, u8 o3_t3, u8 o5_t3)
{
	struct gpon_priv *priv = hw_priv;
	u32 val;
	int ret;

	/*
	 * G_PLOu_PRMBL_TYPE3:
	 *   bit 24    ebl_en
	 *   bits 15:8 O5 extended T3 preamble
	 *   bits 7:0  O3/O4 extended T3 preamble
	 *
	 * The stock RTF8225VW SDK reads back 0x01003860 for O3/O4=96
	 * and O5=56.  BIT(16) writes a reserved field and leaves EBL off.
	 */
	val = BIT(24) | ((u32)o5_t3 << 8) | o3_t3;
	dev_info(priv->dev, "GPON T3 preamble: O3=%u O5=%u reg=%#08x\n",
		 o3_t3, o5_t3, val);
	gpon_write(priv, GPON_PLOu_PRMBL_TYPE3, val);

	ret = airoha_xpon_phy_set_gpon_extended_preamble(priv->phy,
							o3_t3, o5_t3);
	if (ret)
		dev_warn(priv->dev,
			 "failed to program GPON PHY extended preamble: %d\n",
			 ret);
}

static void gpon_cb_set_key_switch_time(void *hw_priv, u32 superframe)
{
	struct gpon_priv *priv = hw_priv;

	dev_info(priv->dev, "GPON AES key switch superframe=%u\n",
		 superframe & AES_KEY_SWITCH_CNT_MASK);
	gpon_write(priv, GPON_AES_CFG, superframe & AES_KEY_SWITCH_CNT_MASK);
}

static void gpon_cb_request_new_key(void *hw_priv)
{
	struct gpon_priv *priv = hw_priv;

	dev_info(priv->dev, "GPON generating a new AES key\n");
	get_random_bytes(priv->aes_key, 16);
	/* Load into shadow registers; switch time set later by Key_Switching_Time */
	gpon_load_aes_shadow(priv, priv->aes_key, 0);
	/* Inform PLOAM layer of the key so it can transmit Encryption_Key */
	ploam_set_aes_key(priv->ploam, priv->aes_key);
}

static void gpon_cb_set_ber_interval(void *hw_priv, u32 interval_ms)
{
	struct gpon_priv *priv = hw_priv;

	dev_info(priv->dev, "GPON BER reporting interval=%u ms\n",
		 interval_ms);
	priv->ber_interval_ms = interval_ms;
	if (interval_ms)
		mod_timer(&priv->ber_timer,
			  jiffies + msecs_to_jiffies(interval_ms));
	else
		timer_delete(&priv->ber_timer);
}

static int gpon_cb_set_omci_gem(void *hw_priv, u16 gem_port_id, bool valid)
{
	struct gpon_priv *priv = hw_priv;
	u8 onu_id = ploam_get_onu_id(priv->ploam);
	u32 reg_val;
	int ret, tcont_ret;

	if (!valid) {
		gpon_write(priv, GPON_OMCI_ID, 0);
		airoha_gpon_omci_set_channel(&priv->omci, gem_port_id, false);

		ret = gpon_set_gem_port_hw(priv, gem_port_id, false, false);
		if (ret)
			dev_err(priv->dev,
				"failed to remove GPON OMCC GEM port %u: %d\n",
				gem_port_id, ret);
		tcont_ret = gpon_set_tcont_hw(priv, 0, 0, false);
		if (!ret)
			ret = tcont_ret;
		dev_info(priv->dev, "GPON OMCI GEM disabled: port=%u\n",
			 gem_port_id);
		return ret;
	}

	if (onu_id == PLOAM_ONU_UNASSIGNED) {
		dev_err(priv->dev,
			"cannot enable GPON OMCC GEM %u without an ONU-ID\n",
			gem_port_id);
		return -EINVAL;
	}

	ret = gpon_set_tcont_hw(priv, 0, onu_id, true);
	if (ret) {
		dev_err(priv->dev,
			"failed to configure GPON OMCC T-CONT: %d\n", ret);
		return ret;
	}

	ret = gpon_set_gem_port_hw(priv, gem_port_id, true, false);
	if (ret) {
		dev_err(priv->dev,
			"failed to configure GPON OMCC GEM port %u: %d\n",
			gem_port_id, ret);
		gpon_set_tcont_hw(priv, 0, 0, false);
		return ret;
	}

	reg_val = OMCI_PORT_VLD | (gem_port_id & OMCI_GPID_MASK);
	gpon_write(priv, GPON_OMCI_ID, reg_val);
	if ((gpon_read(priv, GPON_OMCI_ID) &
	     (OMCI_PORT_VLD | OMCI_GPID_MASK)) != reg_val) {
		dev_err(priv->dev,
			"failed to enable GPON OMCC register for GEM port %u\n",
			gem_port_id);
		gpon_set_gem_port_hw(priv, gem_port_id, false, false);
		gpon_set_tcont_hw(priv, 0, 0, false);
		return -EIO;
	}

	airoha_gpon_omci_set_channel(&priv->omci, gem_port_id, true);
	dev_info(priv->dev,
		 "GPON OMCC datapath enabled: onu-id=%u tcont=0 gem=%u reg=%#08x\n",
		 onu_id, gem_port_id, reg_val);
	dev_info(priv->dev,
		 "GPON OMCC readback: omci=%#010x gem-status=%#010x tcont0-1=%#010x\n",
		 gpon_read(priv, GPON_OMCI_ID),
		 gpon_read(priv, GPON_GEM_PORT_STS),
		 gpon_read(priv, GPON_TCONT_ID_0_1));
	airoha_eth_xpon_dump_oam_rx_state(priv->gdm_dev);

	return 0;
}

static void gpon_cb_set_gem_encryption(void *hw_priv, u16 port_id,
					u8 encrypt_mode)
{
	struct gpon_priv *priv = hw_priv;
	int ret;

	ret = gpon_set_gem_port_hw(priv, port_id, true, encrypt_mode == 3);
	if (ret)
		dev_err(priv->dev,
			"failed to update GEM port %u encryption: %d\n",
			port_id, ret);
	else
		dev_info(priv->dev,
			 "GPON GEM encryption update: port=%u mode=%u\n",
			 port_id, encrypt_mode);
}

static void gpon_cb_set_alloc_id(void *hw_priv, u16 alloc_id, bool allocate)
{
	struct gpon_priv *priv = hw_priv;
	int ret;

	dev_info(priv->dev, "GPON Alloc-ID %s: %u\n",
		 allocate ? "assign" : "remove", alloc_id);
	ret = gpon_set_alloc_id_hw(priv, alloc_id, allocate);
	if (ret)
		dev_err(priv->dev, "failed to %s Alloc-ID %u: %d\n",
			allocate ? "assign" : "remove", alloc_id, ret);
}

int airoha_gpon_omci_hw_set_tcont(void *hw_priv, u16 entity_id,
				  u16 alloc_id, bool valid)
{
	struct gpon_priv *priv = hw_priv;
	unsigned int channel = 0;
	int ret;

	ret = gpon_set_omci_tcont_hw(priv, entity_id, alloc_id, valid,
				     &channel);
	if (ret) {
		dev_err(priv->dev,
			"failed to configure OMCI T-CONT %#x alloc-id %u: %d\n",
			entity_id, alloc_id, ret);
	} else if (valid) {
		dev_info(priv->dev,
			 "OMCI T-CONT %#x enabled alloc-id %u channel %u\n",
			 entity_id, alloc_id, channel);
	} else {
		dev_info(priv->dev, "OMCI T-CONT %#x disabled channel %u\n",
			 entity_id, channel);
	}
	return ret;
}

int airoha_gpon_omci_hw_set_gem_port(void *hw_priv, u16 entity_id,
				     u16 gem_port_id, u16 tcont_entity_id,
				     u8 direction, bool valid, bool encrypted)
{
	struct gpon_priv *priv = hw_priv;
	struct airoha_xpon_service_cfg service = {
		.gem_port_id = gem_port_id,
		.queue = 0,
		.default_service = true,
	};
	unsigned int tcont_index = 0;
	int ret;

	ret = gpon_set_gem_port_hw(priv, gem_port_id, valid, encrypted);
	if (ret)
		return ret;

	airoha_eth_xpon_del_service(priv->gdm_dev, gem_port_id);

	if (valid && direction != OMCI_GEM_PORT_DIRECTION_ANI_TO_UNI) {
		ret = gpon_tcont_entity_to_index(priv, tcont_entity_id,
						 &tcont_index);
		if (ret)
			goto err_disable_gem;

		service.tcont = tcont_index;
		ret = airoha_eth_xpon_add_service(priv->gdm_dev, &service);
		if (ret)
			goto err_disable_gem;
	}

	dev_info(priv->dev,
		 "OMCI GEM port %u %s (ME %#x T-CONT %#x direction %u channel %u)\n",
		 gem_port_id, valid ? "enabled" : "disabled", entity_id,
		 tcont_entity_id, direction, tcont_index);
	return 0;

err_disable_gem:
	gpon_set_gem_port_hw(priv, gem_port_id, false, false);
	return ret;
}

int airoha_gpon_omci_hw_set_uni(void *hw_priv, u16 entity_id, bool enable)
{
	struct gpon_priv *priv = hw_priv;

	dev_dbg(priv->dev, "OMCI UNI %#x requested %s\n", entity_id,
		enable ? "enabled" : "disabled");
	return 0;
}

int airoha_gpon_omci_hw_get_telemetry(void *hw_priv,
				      struct omci_telemetry *telemetry)
{
	struct airoha_lddla_telemetry optical = {};
	struct gpon_priv *priv = hw_priv;
	bool downstream_fec, upstream_fec;
	int ret;

	if (!telemetry)
		return -EINVAL;

	memset(telemetry, 0, sizeof(*telemetry));
	ret = airoha_xpon_phy_get_gpon_fec_status(priv->phy,
						  &downstream_fec,
						  &upstream_fec);
	if (!ret) {
		telemetry->downstream_fec = downstream_fec ?
			OMCI_FEC_STATUS_UP : OMCI_FEC_STATUS_DOWN;
		telemetry->upstream_fec = upstream_fec ?
			OMCI_FEC_STATUS_UP : OMCI_FEC_STATUS_DOWN;
		telemetry->valid |= OMCI_TELEMETRY_F_FEC_DOWNSTREAM |
				    OMCI_TELEMETRY_F_FEC_UPSTREAM;
	}

	if (!priv->lddla_dev)
		return telemetry->valid ? 0 : -ENODATA;

	ret = airoha_lddla_get_telemetry(priv->lddla_dev, &optical);
	if (ret)
		return telemetry->valid ? 0 : ret;

	if (optical.valid & AIROHA_LDDLA_TELEMETRY_F_TEMPERATURE) {
		telemetry->bosa_temperature_mc = optical.bosa_temperature_mc;
		telemetry->valid |= OMCI_TELEMETRY_F_BOSA_TEMPERATURE;
	}
	if (optical.valid & AIROHA_LDDLA_TELEMETRY_F_VOLTAGE) {
		telemetry->bosa_voltage_uv = optical.voltage_uv;
		telemetry->valid |= OMCI_TELEMETRY_F_BOSA_VOLTAGE;
	}
	if (optical.valid & AIROHA_LDDLA_TELEMETRY_F_BIAS) {
		telemetry->bosa_bias_ua = optical.bias_ua;
		telemetry->valid |= OMCI_TELEMETRY_F_BOSA_BIAS;
	}
	if (optical.valid & AIROHA_LDDLA_TELEMETRY_F_TX_POWER) {
		telemetry->bosa_tx_power_nw = optical.tx_power_nw;
		telemetry->valid |= OMCI_TELEMETRY_F_BOSA_TX_POWER;
	}
	if (optical.valid & AIROHA_LDDLA_TELEMETRY_F_RX_POWER) {
		telemetry->bosa_rx_power_nw = optical.rx_power_nw;
		telemetry->valid |= OMCI_TELEMETRY_F_BOSA_RX_POWER;
	}
	if (optical.valid & AIROHA_LDDLA_TELEMETRY_F_ALARMS) {
		telemetry->bosa_alarms = optical.alarms;
		telemetry->valid |= OMCI_TELEMETRY_F_BOSA_ALARMS;
	}

	return 0;
}

/* Forward declaration needed by gpon_disable */
static void gpon_disable(struct gpon_priv *priv);

static void gpon_cb_state_changed(void *hw_priv, enum gpon_state state)
{
	struct gpon_priv *priv = hw_priv;
	enum airoha_xpon_phy_gpon_oper_state phy_state;
	bool update_phy_state = true;
	int ret;

	/*
	 * Mirror the vendor PHY state sequencing before updating the MAC
	 * activation state: disabled in O2, ranging formatter in O3/O4 and
	 * operational formatter in O5.
	 */
	switch (state) {
	case GPON_O2_STANDBY:
		phy_state = AIROHA_XPON_PHY_GPON_OPER_DISABLED;
		break;
	case GPON_O3_SERIAL_NUMBER:
	case GPON_O4_RANGING:
		phy_state = AIROHA_XPON_PHY_GPON_OPER_RANGING;
		break;
	case GPON_O5_OPERATION:
		phy_state = AIROHA_XPON_PHY_GPON_OPER_OPERATION;
		break;
	default:
		update_phy_state = false;
		break;
	}

	if (update_phy_state) {
		ret = airoha_xpon_phy_set_gpon_oper_state(priv->phy, phy_state);
		if (ret)
			dev_warn(priv->dev,
				 "failed to update GPON PHY operational state: %d\n",
				 ret);
	}

	/* Keep the hardware activation state in sync with the PLOAM FSM. */
	gpon_write(priv, GPON_ACTIVATION_ST, state & 0x7);
	dev_info(priv->dev, "GPON state -> %s (%u), activation_reg=%#08x\n",
		 gpon_state_name(state), state,
		 gpon_read(priv, GPON_ACTIVATION_ST));
	airoha_gpon_omci_set_state(&priv->omci, state);

	if (state == GPON_O4_RANGING || state == GPON_O5_OPERATION)
		gpon_dump_activation_regs(priv, "state transition");

	switch (state) {
	case GPON_O2_STANDBY:
		/*
		 * Match the stock SDK: 0x058b is the reset/O1 value, while
		 * activation starts from O2 with 0x0577.
		 */
		gpon_write(priv, GPON_RSP_TIME, GPON_RSP_TIME_ACTIVATION);
		dev_info(priv->dev,
			 "GPON O2 activation response time=%#06x\n",
			 gpon_read(priv, GPON_RSP_TIME));
		fallthrough;
	case GPON_O3_SERIAL_NUMBER:
	case GPON_O4_RANGING:
		/* Start / restart TO1 timer */
		mod_delayed_work(priv->fsm_wq, &priv->to1_work,
				 msecs_to_jiffies(GPON_TO1_MS));
		break;
	case GPON_O5_OPERATION:
		/* Cancel TO1 */
		cancel_delayed_work(&priv->to1_work);
		dev_info(priv->dev, "GPON O5: operational, ONU-ID=%u\n",
			 ploam_get_onu_id(priv->ploam));
		airoha_eth_xpon_set_carrier(priv->gdm_dev, true);
		break;
	case GPON_O6_POPUP:
		/* Cancel TO1, start TO2 */
		cancel_delayed_work(&priv->to1_work);
		mod_delayed_work(priv->fsm_wq, &priv->to2_work,
				 msecs_to_jiffies(GPON_TO2_MS));
		airoha_eth_xpon_set_carrier(priv->gdm_dev, false);
		break;
	case GPON_O7_EMERGENCY_STOP:
		cancel_delayed_work(&priv->to1_work);
		cancel_delayed_work(&priv->to2_work);
		airoha_eth_xpon_set_carrier(priv->gdm_dev, false);
		break;
	case GPON_O1_INITIAL:
		cancel_delayed_work(&priv->to1_work);
		cancel_delayed_work(&priv->to2_work);
		airoha_eth_xpon_set_carrier(priv->gdm_dev, false);
		break;
	default:
		break;
	}
}

static void gpon_cb_deactivate(void *hw_priv)
{
	struct gpon_priv *priv = hw_priv;

	/*
	 * The callback runs while the ordered IRQ worker is processing the
	 * downstream FIFO. Defer the stop/restart sequence until that worker
	 * has returned; gpon_disable() may cancel work and power down the PHY.
	 */
	if (READ_ONCE(priv->started))
		mod_delayed_work(priv->fsm_wq, &priv->restart_work, 0);
}

static const struct ploam_ops gpon_ploam_ops = {
	.send_upstream       = gpon_cb_send_upstream,
	.set_onu_id          = gpon_cb_set_onu_id,
	.set_eqd_o4          = gpon_cb_set_eqd_o4,
	.adjust_eqd_o5       = gpon_cb_adjust_eqd_o5,
	.enable_us_fec       = gpon_cb_enable_us_fec,
	.set_overhead        = gpon_cb_set_overhead,
	.set_t3_preamble     = gpon_cb_set_t3_preamble,
	.set_key_switch_time = gpon_cb_set_key_switch_time,
	.request_new_key     = gpon_cb_request_new_key,
	.set_ber_interval    = gpon_cb_set_ber_interval,
	.set_omci_gem        = gpon_cb_set_omci_gem,
	.set_gem_encryption  = gpon_cb_set_gem_encryption,
	.set_alloc_id        = gpon_cb_set_alloc_id,
	.state_changed       = gpon_cb_state_changed,
	.deactivate          = gpon_cb_deactivate,
};

/* -----------------------------------------------------------------------
 * Enable / disable
 * -------------------------------------------------------------------- */

static int gpon_enable(struct gpon_priv *priv)
{
	u32 fifo_depth, known, pending, unknown;
	int ret;

	dev_info(priv->dev, "starting GPON MAC from state %s\n",
		 gpon_state_name(ploam_get_state(priv->ploam)));

	ret = airoha_xpon_phy_start(priv->dev, priv->phy,
				    AIROHA_XPON_MODE_GPON,
				    &priv->phy_initialized,
				    &priv->phy_powered);
	if (ret)
		return ret;

	ret = gpon_prepare_hardware(priv);
	if (ret) {
		dev_err(priv->dev, "failed to prepare GPON hardware: %d\n", ret);
		airoha_xpon_phy_stop(priv->dev, priv->phy,
				     AIROHA_XPON_MODE_GPON,
				     &priv->phy_initialized,
				     &priv->phy_powered);
		return ret;
	}

	ret = gpon_dev_init(priv);
	if (ret) {
		gpon_set_fe_datapath(priv, false);
		airoha_xpon_phy_stop(priv->dev, priv->phy,
				     AIROHA_XPON_MODE_GPON,
				     &priv->phy_initialized,
				     &priv->phy_powered);
		dev_err(priv->dev, "GPON hardware init failed: %d\n", ret);
		return ret;
	}

	gpon_set_serial_number_regs(priv);
	gpon_load_aes_shadow(priv, priv->aes_key, 0);

	/* Inspect the events accumulated while the digital PHY was locking.
	 * Known activation/error bits help distinguish CDR lock from valid GPON
	 * frame alignment; unknown bits are retained in the log for decoding.
	 */
	pending = gpon_read(priv, GPON_INT_STATUS);
	known = pending & (u32)GPON_INT_DEFAULT_MASK;
	unknown = pending & ~(u32)GPON_INT_DEFAULT_MASK;
	fifo_depth = gpon_read(priv, GPON_PLOAMd_FIFO_STS) &
		     PLOAMd_FIFO_USED_MASK;
	if (pending || fifo_depth)
		dev_info(priv->dev,
			 "GPON events before IRQ enable: raw=%#08x known=%#08x unknown=%#08x fifo=%u activation=%#08x\n",
			 pending, known, unknown, fifo_depth,
			 gpon_read(priv, GPON_ACTIVATION_ST));

	/* Vendor gpon_INT_init() clears all latched W1C status before unmasking. */
	gpon_write(priv, GPON_INT_STATUS, ~0U);
	gpon_write(priv, GPON_INT_ENABLE, GPON_INT_DEFAULT_MASK);

	dev_info(priv->dev,
		 "enabling GPON IRQ %d with mask=%#08x status=%#08x\n",
		 priv->irq, gpon_read(priv, GPON_INT_ENABLE),
		 gpon_read(priv, GPON_INT_STATUS));

	/*
	 * Enter O2 before unmasking the IRQ line.  Any PLOAM already queued
	 * in hardware is then processed against a fully initialized FSM.
	 */
	atomic_set(&priv->pending_irqs, 0);
	gpon_ploam_rx_queue_reset(priv);
	ploam_start(priv->ploam);
	WRITE_ONCE(priv->irq_enabled, true);
	enable_irq(priv->irq);

	if (priv->ber_interval_ms)
		mod_timer(&priv->ber_timer,
			  jiffies + msecs_to_jiffies(priv->ber_interval_ms));
	dev_info(priv->dev,
		 "GPON MAC started: state=%s mbi=%#08x int_enable=%#08x\n",
		 gpon_state_name(ploam_get_state(priv->ploam)),
		 gpon_read(priv, GPON_MBI_MPI_STOP),
		 gpon_read(priv, GPON_INT_ENABLE));

	return 0;
}

static void gpon_disable(struct gpon_priv *priv)
{
	int ret;

	dev_info(priv->dev,
		 "stopping GPON MAC: state=%s irq_enabled=%u mbi=%#08x int_status=%#08x\n",
		 gpon_state_name(ploam_get_state(priv->ploam)),
		 priv->irq_enabled, gpon_read(priv, GPON_MBI_MPI_STOP),
		 gpon_read(priv, GPON_INT_STATUS));

	/*
	 * Stop new IRQ snapshots before cancelling queued FSM work.  This
	 * function may itself run from irq_work or to2_work.
	 */
	if (priv->irq_enabled) {
		disable_irq(priv->irq);
		WRITE_ONCE(priv->irq_enabled, false);
	}

	atomic_set(&priv->pending_irqs, 0);
	if (current_work() != &priv->irq_work)
		cancel_work_sync(&priv->irq_work);
	gpon_ploam_rx_queue_reset(priv);

	if (current_work() == &priv->to1_work.work)
		cancel_delayed_work(&priv->to1_work);
	else
		cancel_delayed_work_sync(&priv->to1_work);

	if (current_work() == &priv->to2_work.work)
		cancel_delayed_work(&priv->to2_work);
	else
		cancel_delayed_work_sync(&priv->to2_work);

	timer_delete_sync(&priv->ber_timer);

	/* Match the vendor shutdown path: mask and clear MAC interrupts,
	 * invalidate the runtime identities, disconnect GDM2, then stop the
	 * GPON/PSE MBI. Do not sweep all 4096 GEM entries here. sfp_upstream_stop()
	 * can call us after the optical path has already started shutting down,
	 * at which point the indirect GEM/T-CONT command engine no longer
	 * acknowledges requests.
	 */
	gpon_write(priv, GPON_INT_ENABLE, 0);
	gpon_write(priv, GPON_INT_STATUS, ~0U);
	gpon_reset_activation_context(priv);
	gpon_write(priv, GPON_OMCI_ID, 0);

	airoha_gpon_omci_set_channel(&priv->omci, 0xffff, false);
	airoha_gpon_omci_set_onu_id(&priv->omci, 0xffff);

	ret = gpon_set_fe_datapath(priv, false);
	if (ret)
		dev_warn(priv->dev,
			 "failed to disable GPON FE datapath: %d\n", ret);
	airoha_eth_xpon_flush_services(priv->gdm_dev);
	ret = airoha_eth_set_xpon_mode(priv->gdm_dev,
				       AIROHA_XPON_MODE_GPON);
	if (ret)
		dev_warn(priv->dev,
			 "failed to quiesce GPON FE channels: %d\n", ret);

	gpon_set_bits(priv, GPON_MBI_MPI_STOP, MBI_RX_STOP | MBI_TX_STOP);
	dev_info(priv->dev, "GPON MBI stopped: %#08x\n",
		 gpon_read(priv, GPON_MBI_MPI_STOP));

	airoha_xpon_phy_stop(priv->dev, priv->phy,
			      AIROHA_XPON_MODE_GPON,
			      &priv->phy_initialized,
			      &priv->phy_powered);

	ploam_reset(priv->ploam);
	airoha_gpon_omci_set_state(&priv->omci, GPON_O1_INITIAL);
	airoha_eth_xpon_set_carrier(priv->gdm_dev, false);
	dev_info(priv->dev, "GPON MAC stopped, state reset to %s\n",
		 gpon_state_name(ploam_get_state(priv->ploam)));
}

/* -----------------------------------------------------------------------
 * Timers
 * -------------------------------------------------------------------- */

static void gpon_ber_timer_fn(struct timer_list *t)
{
	struct gpon_priv *priv = timer_container_of(priv, t, ber_timer);
	u32 depth;

	/* Report a missing MAC IRQ while retaining the FIFO contents for debug. */
	depth = gpon_read(priv, GPON_PLOAMd_FIFO_STS) & PLOAMd_FIFO_USED_MASK;
	if (depth && ploam_get_state(priv->ploam) != GPON_O5_OPERATION)
		dev_warn(priv->dev,
			"PLOAM FIFO has %u messages without a MAC IRQ: status=%#08x mask=%#08x\n",
			depth, gpon_read(priv, GPON_INT_STATUS),
			gpon_read(priv, GPON_INT_ENABLE));

	ploam_notify_ber(priv->ploam, 0);
	mod_timer(&priv->ber_timer,
		  jiffies + msecs_to_jiffies(priv->ber_interval_ms));
}

/* TO1: O3/O4 timeout — no Ranging_Time received within 10 s → return to O2 */
static void gpon_to1_work_fn(struct work_struct *work)
{
	struct gpon_priv *priv =
		container_of(to_delayed_work(work), struct gpon_priv, to1_work);
	enum gpon_state st = ploam_get_state(priv->ploam);

	/*
	 * This work runs on the same ordered queue as downstream PLOAM
	 * processing, so Upstream_Overhead cannot re-enter O3 in the middle
	 * of the O3/O4 -> O2 transition.
	 */
	if (st == GPON_O3_SERIAL_NUMBER || st == GPON_O4_RANGING) {
		gpon_dump_activation_regs(priv, "TO1 expired");
		dev_warn(priv->dev,
			 "GPON TO1 expired in O%d, returning to O2\n", (int)st);
		gpon_write(priv, GPON_ONU_ID, PLOAM_ONU_UNASSIGNED);
		ploam_reset(priv->ploam);
		ploam_start(priv->ploam);
	}
}

/* TO2: O6 timeout — no Popup received within 100 ms → full disable */
static void gpon_to2_work_fn(struct work_struct *work)
{
	struct gpon_priv *priv =
		container_of(to_delayed_work(work), struct gpon_priv, to2_work);

	if (ploam_get_state(priv->ploam) == GPON_O6_POPUP) {
		dev_warn(priv->dev, "GPON TO2 expired in O6, resetting\n");
		gpon_disable(priv);
	}
}

static void gpon_restart_work_fn(struct work_struct *work)
{
	struct gpon_priv *priv =
		container_of(to_delayed_work(work), struct gpon_priv,
			     restart_work);
	int ret;

	if (!READ_ONCE(priv->started))
		return;

	if (ploam_get_state(priv->ploam) != GPON_O1_INITIAL) {
		dev_warn(priv->dev,
			 "restarting GPON after Deactivate_ONU-ID from the OLT\n");
		gpon_disable(priv);

		msleep(GPON_DEACTIVATE_RESTART_MS);
		if (!READ_ONCE(priv->started))
			return;
	}

	ret = airoha_xpon_tx_rearm(priv->dev, priv->lddla_dev);
	if (ret) {
		dev_warn(priv->dev,
			 "retrying optical transmitter rearm in %u ms\n",
			 GPON_REARM_RETRY_MS);
		mod_delayed_work(priv->fsm_wq, &priv->restart_work,
				 msecs_to_jiffies(GPON_REARM_RETRY_MS));
		return;
	}

	ret = gpon_enable(priv);
	if (ret)
		dev_err(priv->dev,
			"failed to restart GPON after deactivation: %d\n", ret);
}

/* -----------------------------------------------------------------------
 * Interrupt handler
 * -------------------------------------------------------------------- */

static void gpon_dump_error_counters(struct gpon_priv *priv, u32 errors)
{
	dev_warn_ratelimited(priv->dev,
			     "GPON error: irq=%#x sfifo=%#x bfifo=%#x ctrl=%#x\n",
			     errors, gpon_read(priv, GPON_DBG_BWM_SFIFO_STS),
			     gpon_read(priv, GPON_DBG_BWM_BFIFO_STS),
			     gpon_read(priv, GPON_DBG_ERR_CTRL));
	dev_warn_ratelimited(priv->dev,
			     "GPON counters: rx=%u/%u/%u tx=%u/%u hec=%u/%u/%u\n",
			     gpon_read(priv, GPON_DBG_RX_GEM_CNT),
			     gpon_read(priv, GPON_DBG_RX_CRC_ERR_CNT),
			     gpon_read(priv, GPON_DBG_RX_GTC_CNT),
			     gpon_read(priv, GPON_DBG_TX_GEM_CNT),
			     gpon_read(priv, GPON_DBG_TX_BST_CNT),
			     gpon_read(priv, GPON_DBG_GEM_HEC_ONE_ERR_CNT),
			     gpon_read(priv, GPON_DBG_GEM_HEC_TWO_ERR_CNT),
			     gpon_read(priv, GPON_DBG_GEM_HEC_UC_ERR_CNT));
}

static void gpon_irq_work_fn(struct work_struct *work)
{
	struct gpon_priv *priv =
		container_of(work, struct gpon_priv, irq_work);
	u32 active;

	/*
	 * TO1, TO2 and downstream PLOAM callbacks all execute on fsm_wq.
	 * Loop because the top half may accumulate more bits while the worker
	 * is draining the hardware FIFO.
	 */
	while ((active = (u32)atomic_xchg(&priv->pending_irqs, 0))) {
		u32 phy_tx_frames = 0, phy_tx_bursts = 0;
		u32 sn_cfg;
		int phy_ret;

		if (!READ_ONCE(priv->irq_enabled))
			break;

		gpon_process_ploam_queue(priv);

		/*
		 * Keep the serial-number diagnostics compact: one line only when
		 * the MAC receives a serial grant, sends Serial_Number_ONU, or
		 * crosses the serial-request threshold.
		 */
		if (active & (INT_SN_REQ_RECV | INT_SN_ONU_SEND_O3 |
			      INT_SN_REQ_CRS)) {
			sn_cfg = gpon_read(priv, GPON_SN_MSG_CFG);
			phy_ret = airoha_xpon_phy_get_gpon_tx_counters(
				priv->phy, &phy_tx_frames, &phy_tx_bursts);
			dev_info(priv->dev,
				 "GPON SN event: irq=%#08x cfg=%#010x threshold=%lu tx_power=%lu random_delay=%lu rsp=%#06x act=%u serial=%#010x/%#010x guard=%#010x type12=%#010x type3=%#010x pre_delay=%#010x dbg_dly=%#010x tx_sync=%#010x phy_tx=%#010x/%#010x phy_ret=%d\n",
				 active, sn_cfg,
				 FIELD_GET(SN_MSG_CFG_SN_REQ_THR_MASK, sn_cfg),
				 FIELD_GET(SN_MSG_CFG_TX_POWER_MODE_MASK, sn_cfg),
				 FIELD_GET(SN_MSG_CFG_RANDOM_DELAY_MASK, sn_cfg),
				 gpon_read(priv, GPON_RSP_TIME),
				 gpon_read(priv, GPON_ACTIVATION_ST) & 0x7,
				 gpon_read(priv, GPON_VENDOR_ID),
				 gpon_read(priv, GPON_VS_SN),
				 gpon_read(priv, GPON_PLOu_GUARD_BIT),
				 gpon_read(priv, GPON_PLOu_PRMBL_TYPE1_2),
				 gpon_read(priv, GPON_PLOu_PRMBL_TYPE3),
				 gpon_read(priv, GPON_PRE_ASSIGNED_DLY),
				 gpon_read(priv, GPON_DBG_DLY),
				 gpon_read(priv, GPON_DBG_TX_SYNC_OFFSET),
				 phy_tx_frames, phy_tx_bursts, phy_ret);
		}

		if (active & INT_DYING_GASP) {
			dev_warn(priv->dev, "GPON dying-gasp interrupt\n");
			ploam_notify_dying_gasp(priv->ploam);
		}

		if (active & INT_LOSS_GEM_DEL)
			dev_warn_ratelimited(priv->dev,
					     "GPON loss of GEM delineation interrupt\n");

		if (active & GPON_INT_ERROR_MASK)
			gpon_dump_error_counters(priv, active &
					 (u32)GPON_INT_ERROR_MASK);

		if (active & (INT_RX_ERR | INT_FIFO_ERR))
			airoha_eth_xpon_dump_oam_rx_state(priv->gdm_dev);

		if (active & INT_TX_LATE_START) {
			gpon_dump_activation_regs(priv, "TX late start");
			dev_warn(priv->dev,
				 "GPON upstream burst started late: rsp_time=%#06x state=%s\n",
				 gpon_read(priv, GPON_RSP_TIME),
				 gpon_state_name(ploam_get_state(priv->ploam)));
		}
	}

	if (READ_ONCE(priv->irq_enabled))
		gpon_process_ploam_queue(priv);
}

static irqreturn_t gpon_isr(int irq, void *data)
{
	struct gpon_priv *priv = data;
	u32 active, enabled, raw;

	raw = gpon_read(priv, GPON_INT_STATUS);
	if (!raw)
		return IRQ_NONE;

	enabled = gpon_read(priv, GPON_INT_ENABLE);
	active = raw & enabled;

	/* G_INT_STATUS is W1C; acknowledge the complete hardware snapshot. */
	gpon_write(priv, GPON_INT_STATUS, raw);

	if (active & INT_PLOAMD_RECV)
		gpon_drain_ploam_fifo_irq(priv);

	if (active) {
		atomic_or(active, &priv->pending_irqs);
		queue_work(priv->fsm_wq, &priv->irq_work);
	}

	return IRQ_HANDLED;
}

/* -----------------------------------------------------------------------
 * SFP upstream ops
 * -------------------------------------------------------------------- */

static void gpon_sfp_attach(void *upstream, struct sfp_bus *bus)
{
	struct gpon_priv *priv = upstream;

	dev_info(priv->dev, "GPON SFP bus attached\n");
}

static void gpon_sfp_detach(void *upstream, struct sfp_bus *bus)
{
	struct gpon_priv *priv = upstream;

	dev_info(priv->dev, "GPON SFP bus detached\n");
}

static int gpon_sfp_module_insert(void *upstream,
				   const struct sfp_eeprom_id *id)
{
	struct gpon_priv *priv = upstream;

	dev_info(priv->dev, "GPON SFP module inserted\n");
	return 0;
}

static void gpon_sfp_module_remove(void *upstream)
{
	struct gpon_priv *priv = upstream;

	dev_info(priv->dev, "GPON SFP module removed\n");
	cancel_delayed_work_sync(&priv->restart_work);
	if (ploam_get_state(priv->ploam) > GPON_O1_INITIAL)
		gpon_disable(priv);
}

static int gpon_sfp_module_start(void *upstream)
{
	struct gpon_priv *priv = upstream;
	enum gpon_state state;

	state = ploam_get_state(priv->ploam);
	dev_info(priv->dev, "GPON SFP module start: state=%s\n",
		 gpon_state_name(state));

	/* PHY ready: if we were in emergency stop, stay there */
	if (state == GPON_O1_INITIAL) {
		int ret;

		ret = airoha_xpon_tx_rearm(priv->dev, priv->lddla_dev);
		if (ret)
			return ret;

		return gpon_enable(priv);
	}

	return 0;
}

static void gpon_sfp_module_stop(void *upstream)
{
	struct gpon_priv *priv = upstream;

	dev_info(priv->dev, "GPON SFP module stop\n");
	cancel_delayed_work_sync(&priv->restart_work);
	gpon_disable(priv);
}

static void gpon_sfp_link_down(void *upstream)
{
	struct gpon_priv *priv = upstream;

	dev_warn(priv->dev, "GPON optical link down / LOS\n");
	ploam_notify_los(priv->ploam);
}

static void gpon_sfp_link_up(void *upstream)
{
	struct gpon_priv *priv = upstream;

	dev_info(priv->dev, "GPON optical link up\n");
}

static const struct sfp_upstream_ops gpon_sfp_ops = {
	.attach		  = gpon_sfp_attach,
	.detach		  = gpon_sfp_detach,
	.module_insert	  = gpon_sfp_module_insert,
	.module_remove	  = gpon_sfp_module_remove,
	.module_start	  = gpon_sfp_module_start,
	.module_stop	  = gpon_sfp_module_stop,
	.link_up	  = gpon_sfp_link_up,
	.link_down	  = gpon_sfp_link_down,
};

/* -----------------------------------------------------------------------
 * GDM2 xPON lifecycle
 * -------------------------------------------------------------------- */

static int gpon_link_start(void *data)
{
	struct gpon_priv *priv = data;

	if (READ_ONCE(priv->started))
		return 0;

	WRITE_ONCE(priv->started, true);
	airoha_eth_xpon_set_carrier(priv->gdm_dev, false);
	if (priv->sfp_bus)
		sfp_upstream_start(priv->sfp_bus);

	return 0;
}

static void gpon_link_stop(void *data)
{
	struct gpon_priv *priv = data;

	if (!READ_ONCE(priv->started))
		return;

	WRITE_ONCE(priv->started, false);
	cancel_delayed_work_sync(&priv->restart_work);
	airoha_eth_xpon_set_carrier(priv->gdm_dev, false);
	if (priv->sfp_bus)
		sfp_upstream_stop(priv->sfp_bus);
}

static const struct airoha_xpon_link_ops gpon_link_ops = {
	.start = gpon_link_start,
	.stop = gpon_link_stop,
};

/* -----------------------------------------------------------------------
 * Platform driver
 * -------------------------------------------------------------------- */

static int gpon_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct gpon_priv *priv;
	struct resource *res;
	u32 reg_offset;
	int ret;

	dev_info(dev, "probing GPON MAC\n");
	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;
	priv->dev = dev;

	priv->scu = syscon_regmap_lookup_by_phandle(dev->of_node,
					    "airoha,scu");
	if (IS_ERR(priv->scu)) {
		ret = dev_err_probe(dev, PTR_ERR(priv->scu),
				    "failed to get SCU regmap\n");
		goto err_put_eth_node;
	}

	priv->phy = devm_phy_get(dev, "xpon");
	if (IS_ERR(priv->phy)) {
		ret = dev_err_probe(dev, PTR_ERR(priv->phy),
				    "failed to get digital xPON PHY\n");
		goto err_put_eth_node;
	}

	priv->eth_node = of_parse_phandle(dev->of_node, "ethernet", 0);
	if (!priv->eth_node) {
		ret = dev_err_probe(dev, -EINVAL,
				    "missing Ethernet datapath phandle\n");
		goto err_put_eth_node;
	}
	dev_info(dev, "GPON datapath phandle: %pOF\n", priv->eth_node);

	ret = airoha_xpon_find_gdm2(dev, priv->eth_node, &priv->gdm_dev);
	if (ret)
		goto err_put_eth_node;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "mac");
	if (!res)
		res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res) {
		ret = dev_err_probe(dev, -EINVAL, "missing mac resource\n");
		goto err_put_eth_node;
	}

	priv->base = devm_ioremap_resource(dev, res);
	if (IS_ERR(priv->base)) {
		ret = dev_err_probe(dev, PTR_ERR(priv->base), "needs xPON mac base");
		goto err_put_eth_node;
	}

	if (res->start == EN7523_XPON_REGION_BASE) {
		if (resource_size(res) < EN7523_GPON_REG_OFFSET +
					 GPON_SLEEP_CNT + sizeof(u32)) {
			ret = dev_err_probe(dev, -EINVAL,
				"GPON region is too small: %pR\n", res);
			goto err_put_eth_node;
		}
		reg_offset = EN7523_GPON_REG_OFFSET;
	} else if (res->start == EN7523_GPON_REG_BASE) {
		if (resource_size(res) < GPON_SLEEP_CNT + sizeof(u32)) {
			ret = dev_err_probe(dev, -EINVAL,
				"GPON register window is too small: %pR\n", res);
			goto err_put_eth_node;
		}
		reg_offset = 0;
	} else {
		ret = dev_err_probe(dev, -EINVAL,
			"unsupported GPON MAC resource: %pR\n", res);
		goto err_put_eth_node;
	}

	priv->regs = priv->base + reg_offset;
	dev_info(dev, "GPON resource %pR, register offset %#x\n",
		 res, reg_offset);

	priv->fsm_wq = alloc_ordered_workqueue("%s-gpon-fsm",
					       WQ_MEM_RECLAIM, dev_name(dev));
	if (!priv->fsm_wq) {
		ret = -ENOMEM;
		goto err_put_eth_node;
	}

	mutex_init(&priv->tcont_lock);
	memset(priv->tcont_alloc_id, 0xff, sizeof(priv->tcont_alloc_id));
	memset(priv->tcont_entity_id, 0xff, sizeof(priv->tcont_entity_id));

	INIT_WORK(&priv->irq_work, gpon_irq_work_fn);
	INIT_DELAYED_WORK(&priv->to1_work, gpon_to1_work_fn);
	INIT_DELAYED_WORK(&priv->to2_work, gpon_to2_work_fn);
	INIT_DELAYED_WORK(&priv->restart_work, gpon_restart_work_fn);
	atomic_set(&priv->pending_irqs, 0);

	priv->irq = platform_get_irq(pdev, 0);
	if (priv->irq < 0) {
		ret = dev_err_probe(dev, priv->irq, "needs mac irq");
		goto err_destroy_fsm_wq;
	}

	ret = devm_request_irq(dev, priv->irq, gpon_isr, 0,
			       dev_name(dev), priv);
	if (ret)
		goto err_destroy_fsm_wq;
	disable_irq(priv->irq);
	priv->irq_enabled = false;
	dev_info(dev, "GPON IRQ %d requested and initially disabled\n",
		 priv->irq);

	ret = gpon_load_credentials(priv);
	if (ret)
		goto err_destroy_fsm_wq;

	priv->ber_interval_ms = 1000;

	timer_setup(&priv->ber_timer, gpon_ber_timer_fn, 0);

	priv->ploam = ploam_alloc(&gpon_ploam_ops, priv, priv->sn, priv->passwd);
	if (!priv->ploam) {
		ret = -ENOMEM;
		goto err_destroy_fsm_wq;
	}

	priv->lddla_dev = airoha_xpon_find_lddla(dev);
	if (IS_ERR(priv->lddla_dev)) {
		ret = dev_err_probe(dev, PTR_ERR(priv->lddla_dev),
				    "failed to find LDDLA device\n");
		priv->lddla_dev = NULL;
		goto err_free_ploam;
	}

	priv->sfp_bus = sfp_bus_find_fwnode(dev->fwnode);
	if (!priv->sfp_bus) {
		ret = -ENODEV;
		dev_err(dev, "missing SFP reference\n");
		goto err_put_lddla;
	}
	if (IS_ERR(priv->sfp_bus)) {
		ret = PTR_ERR(priv->sfp_bus);
		dev_err(dev, "failed to find SFP bus: %d\n", ret);
		goto err_put_lddla;
	}

	ret = sfp_bus_add_upstream(priv->sfp_bus, priv, &gpon_sfp_ops);
	if (ret)
		goto err_put_sfp;

	ret = airoha_gpon_omci_register(&priv->omci, dev, priv->gdm_dev,
					priv, &priv->identity);
	if (ret)
		goto err_put_gdm;

	/*
	 * The OMCI device is zero-initialized. Publish O1 before the first
	 * PLOAM state callback so userspace never observes an invalid O0.
	 */
	airoha_gpon_omci_set_state(&priv->omci, GPON_O1_INITIAL);

	priv->omci_handler.rx = airoha_gpon_omci_receive;
	priv->omci_handler.priv = &priv->omci;
	ret = airoha_eth_register_xpon_oam(priv->gdm_dev,
					   &priv->omci_handler);
	if (ret)
		goto err_unregister_omci;

	ret = airoha_eth_register_xpon(priv->gdm_dev, AIROHA_XPON_MODE_GPON,
				       &gpon_link_ops, priv);
	if (ret)
		goto err_unregister_oam;

	platform_set_drvdata(pdev, priv);
	dev_info(dev,
		 "GPON probe complete: datapath=%s omci-genl=%u irq=%d default_state=%s\n",
		 priv->gdm_dev->name, omci_device_id(priv->omci.odev),
		 priv->irq, gpon_state_name(ploam_get_state(priv->ploam)));
	return 0;

err_unregister_oam:
	airoha_eth_unregister_xpon_oam(priv->gdm_dev,
				       &priv->omci_handler);
err_unregister_omci:
	airoha_gpon_omci_unregister(&priv->omci);
err_put_gdm:
	dev_put(priv->gdm_dev);
	priv->gdm_dev = NULL;
	sfp_bus_del_upstream(priv->sfp_bus);
err_put_sfp:
	sfp_bus_put(priv->sfp_bus);
err_put_lddla:
	if (priv->lddla_dev)
		put_device(priv->lddla_dev);
err_free_ploam:
	ploam_free(priv->ploam);
err_destroy_fsm_wq:
	destroy_workqueue(priv->fsm_wq);
err_put_eth_node:
	if (priv->gdm_dev) {
		dev_put(priv->gdm_dev);
		priv->gdm_dev = NULL;
	}
	of_node_put(priv->eth_node);
	return ret;
}

static void gpon_remove(struct platform_device *pdev)
{
	struct gpon_priv *priv = platform_get_drvdata(pdev);

	dev_info(priv->dev, "removing GPON MAC driver\n");
	cancel_delayed_work_sync(&priv->restart_work);
	airoha_eth_unregister_xpon(priv->gdm_dev, &gpon_link_ops, priv);
	if (priv->irq_enabled)
		gpon_disable(priv);
	else {
		cancel_work_sync(&priv->irq_work);
		cancel_delayed_work_sync(&priv->to1_work);
		cancel_delayed_work_sync(&priv->to2_work);
	}
	airoha_eth_unregister_xpon_oam(priv->gdm_dev,
				       &priv->omci_handler);
	airoha_gpon_omci_unregister(&priv->omci);
	dev_put(priv->gdm_dev);
	priv->gdm_dev = NULL;
	sfp_bus_del_upstream(priv->sfp_bus);
	sfp_bus_put(priv->sfp_bus);
	if (priv->lddla_dev)
		put_device(priv->lddla_dev);
	ploam_free(priv->ploam);
	destroy_workqueue(priv->fsm_wq);
	of_node_put(priv->eth_node);
}

/* -------------------------------------------------------------------------
 * EPON implementation
 * ------------------------------------------------------------------------- */
/* Register offsets from EPON MAC base */
#define EPON_GLB_CFG		0x000
#define EPON_INT_STATUS		0x004
#define EPON_INT_EN		0x008
#define EPON_RPT_MPCP_TIMEOUT	0x00C
#define EPON_DYINGGSP_CFG	0x010
#define EPON_PENDING_GNT_NUM	0x014
#define EPON_LLID0_3_CFG	0x020
#define EPON_LLID4_7_CFG	0x024
#define EPON_LLID_DSCVRY_CTRL	0x028
#define EPON_LLID0_DSCVRY_STS	0x02C
#define EPON_MAC_ADDR_CFG	0x050
#define EPON_MAC_ADDR_VALUE	0x054
#define EPON_SECURITY_KEY_CFG	0x058
#define EPON_SECURITY_KEY_DATA	0x05C
#define EPON_RPT_DATA		0x060
#define EPON_RPT_LEN		0x064
#define EPON_RPT_CFG		0x068
#define EPON_LOCAL_TIME		0x080
#define EPON_TOD_SYNC_X		0x084
#define EPON_TOD_LTNCY		0x088
#define EPON_P2P_TX_TAG1	0x08C
#define EPON_P2P_TX_TAG2	0x090
#define EPON_TXFETCH_CFG	0x0D0
#define EPON_SYNC_TIME		0x0D4
#define EPON_TX_CAL_CNST	0x0D8
#define EPON_LASER_ONOFF_TIME	0x0DC
#define EPON_GRD_THRSHLD	0x0E0
#define EPON_MPCP_TIMEOUT_INTVL	0x0E4
#define EPON_RPT_TIMEOUT_INTVL	0x0E8
#define EPON_MAX_FUTURE_GNT	0x0EC
#define EPON_MIN_PROC_TIME	0x0F0
#define EPON_TRX_ADJUST_TIME1	0x0F4
#define EPON_TRX_ADJUST_TIME2	0x0F8
#define EPON_TIME_DRFT_STAT	0x134

/* e_glb_cfg bits */
#define GLB_CFG_MODE_SEL	BIT(0)
#define GLB_CFG_RPT_TXPRI_CTRL	BIT(1)
#define GLB_CFG_EPON_MAC_SW_RST	BIT(4)
#define GLB_CFG_TXMBI_STOP	BIT(8)
#define GLB_CFG_RXMBI_STOP	BIT(9)
#define GLB_CFG_FCS_ERR_FWD	BIT(17)
#define GLB_CFG_MPCP_FWD	BIT(22)
#define GLB_CFG_DISCV_BURST_EN	BIT(23)

/*
 * e_int_status / e_int_en bit positions (from epon_reg.h):
 *   bit  0: RCV_DSCVRY_GATE_INT   — discovery gate received
 *   bits 1-8: LLID0..7_RCV_RGST_INT — per-LLID REGISTER frame received
 *   bit  9: GNT_BUF_OVRRUN_INT
 *   bit 13: TIMEDRFT_INT          — time drift
 *   bit 14: MPCP_TIMEOUT_INT
 *   bit 15: RPT_OVERINTVL_INT
 *   bit 24: REG_REQ_DONE_INT      — REGISTER_REQUEST sent by HW
 *   bit 25: REG_ACK_DONE_INT      — REGISTER_ACK sent by HW
 */
#define EPON_INT_DISCV_GATE	BIT(0)
#define EPON_INT_LLID0_RGST	BIT(1)	/* BIT(1+n) for LLID n */
#define EPON_INT_GNT_OVRRUN	BIT(9)
#define EPON_INT_TIMEDRFT	BIT(13)
#define EPON_INT_MPCP_TIMEOUT	BIT(14)
#define EPON_INT_RPT_OVRFLW	BIT(15)
#define EPON_INT_REG_REQ_DONE	BIT(24)
#define EPON_INT_REG_ACK_DONE	BIT(25)

/* e_llid_dscvry_ctrl bits (from epon_reg.h REG_e_llid_dscvry_ctrl) */
#define DSCVRY_MPCP_CMD_MASK	(3U << 30)
#define DSCVRY_MPCP_REG_REQ	BIT(30)	/* send REGISTER_REQUEST */
#define DSCVRY_MPCP_NORMAL	BIT(31)
#define DSCVRY_MPCP_ACK		(3U << 30)	/* send REGISTER_ACK */
#define DSCVRY_CMD_DONE		BIT(16)
#define DSCVRY_RGSTR_ACK_FLG	BIT(12)
#define DSCVRY_RGSTR_REQ_FLG	BIT(8)
#define DSCVRY_TX_MPCP_LLID_MASK 0x7

/*
 * e_llid0_dscvry_sts bit layout (little-endian packed struct from epon_reg.h):
 *   bits [15:0]  llidValue
 *   bit  16      llidValid
 *   bits [23:17] reserved
 *   bits [25:24] rgstrFlgSts   — see MPCP_REG_* below
 *   bits [29:26] reserved
 *   bits [31:30] llidDscvrySts — 0=unregistered, 1=registering, 2=registered
 */
#define LLID_STS_DSCVRY_SHIFT	30
#define LLID_STS_RGST_FLG_SHIFT	24
#define LLID_STS_RGST_FLG_MASK	(3U << 24)
#define LLID_STS_VALID		BIT(16)
#define LLID_STS_VALUE_MASK	0xFFFF

/* rgstrFlgSts values */
#define MPCP_REG_RE_REGISTER	0
#define MPCP_REG_DE_REGISTER	1
#define MPCP_REG_ACK		2
#define MPCP_REG_NACK		3

/*
 * e_mac_addr_cfg indirect register (from epon_reg.h REG_e_mac_addr_cfg):
 *   bit  0      mac_addr_dw_idx   — 0=low 32 bits, 1=high 16 bits
 *   bits [3:1]  mac_addr_llid_indx
 *   bit  16     mac_addr_rwcmd_done — 1=busy/in-progress
 *   bit  31     mac_addr_rwcmd    — write 1 to trigger write
 */
#define MAC_ADDR_RWCMD		BIT(31)
#define MAC_ADDR_DONE		BIT(16)
#define MAC_ADDR_LLID_SHIFT	1
#define MAC_ADDR_DW_IDX		BIT(0)

/* e_security_key_cfg */
#define SEC_KEY_WRITE_CMD	BIT(31)
#define SEC_KEY_LLID_SHIFT	24
#define SEC_KEY_IDX_SHIFT	16
#define SEC_KEY_DW_SHIFT	8

/* Dying gasp init value per ref (hw_dying_gasp_en=1, dygsp_num_of_times=1,
 * dygsp_code=0, other fields=0x02) */
#define DYINGGSP_CFG_HW_ENABLE	0x80000102

/* Guard threshold for time drift detection (ref: EPON_TIMEDRIFT_THRSHLD) */
#define EPON_TIMEDRIFT_THRSHLD	0x10

/* Default register values */
#define EPON_PENDING_GNT_DEFAULT	0x40
#define EPON_MPCP_TIMEOUT_DEFAULT	0x03B9ACA0
#define EPON_RPT_TIMEOUT_DEFAULT	0x002FAF08
#define EPON_MAX_FUTURE_GNT_DEFAULT	0x03B9ACA0
#define EPON_MIN_PROC_TIME_DEFAULT	0x400
#define EPON_LASER_ONOFF_DEFAULT	0x2020
#define EPON_TX_CAL_CNST_DEFAULT	0x2612040C
#define EPON_TXFETCH_DEFAULT		0x202403E8
#define EPON_TRX_ADJUST_TIME1_DEF	0x004FFFF1
#define EPON_TRX_ADJUST_TIME2_DEF	0x6

/* External SW reset register bit (REG_E_SW_RST, outside EPON MAC block) */
#define EPON_EXT_SW_RST_BIT	BIT(31)
#define EPON_RESET_DELAY_US	100

#define EPON_MAX_LLID		8

/* LLID registration states */
enum llid_state {
	LLID_STATE_WAIT		= 0,
	LLID_STATE_REGISTERING	= 1,
	LLID_STATE_REGISTERED	= 2,
};

static const char *epon_llid_state_name(enum llid_state state)
{
	switch (state) {
	case LLID_STATE_WAIT:
		return "wait";
	case LLID_STATE_REGISTERING:
		return "registering";
	case LLID_STATE_REGISTERED:
		return "registered";
	default:
		return "unknown";
	}
}

struct epon_llid {
	enum llid_state	state;
	bool		valid;
	u16		value;
};

struct epon_priv {
	void __iomem		*base;
	void __iomem		*rst_base;	/* optional: external SW reset reg */
	struct device		*dev;
	struct regmap		*scu;
	struct phy		*phy;
	bool			phy_initialized;
	bool			phy_powered;
	struct device_node	*eth_node;
	struct net_device	*gdm_dev;
	bool			started;
	struct sfp_bus		*sfp_bus;
	struct device		*lddla_dev;
	int			irq;
	bool			irq_enabled;

	struct epon_llid	llid[EPON_MAX_LLID];
	int			registered_llids;

};

static inline u32 epon_read(struct epon_priv *priv, u32 reg)
{
	return readl(priv->base + reg);
}

static inline void epon_write(struct epon_priv *priv, u32 reg, u32 val)
{
	writel(val, priv->base + reg);
}

/* --- LLID discovery status helpers --- */

static u32 epon_llid_sts(struct epon_priv *priv, int idx)
{
	return epon_read(priv, EPON_LLID0_DSCVRY_STS + idx * 4);
}

static void epon_llid_sts_write(struct epon_priv *priv, int idx, u32 val)
{
	epon_write(priv, EPON_LLID0_DSCVRY_STS + idx * 4, val);
}

/*
 * Set LLID HW discovery state to REGISTERING.
 * Pattern from ref eponMpcpDscvFsmWaitHandler / eponMpcpDiscvGateIntHandler:
 * clear bits[31:30] (set unregistered) 10 times, then set bits[31:30]=01
 * while setting all lower bits (0x7FFFFFFF gives bit30=1, rest all-ones).
 */
static void epon_llid_set_registering(struct epon_priv *priv, int idx)
{
	u32 tmp;
	int i;

	dev_info(priv->dev, "EPON LLID%d -> registering\n", idx);

	for (i = 0; i < 10; i++) {
		tmp = epon_llid_sts(priv, idx) & 0x3FFFFFFF;
		epon_llid_sts_write(priv, idx, tmp);
	}
	tmp = epon_llid_sts(priv, idx) & 0x3FFFFFFF;
	tmp |= 0x7FFFFFFF;	/* bit30=1 → llidDscvrySts=01 (registering) */
	epon_llid_sts_write(priv, idx, tmp);
}

/* --- MAC address programming (indirect via e_mac_addr_cfg) --- */

static int epon_wait_mac_cfg(struct epon_priv *priv)
{
	int i;

	/* MAC_ADDR_DONE=1 means busy; wait for it to clear */
	for (i = 0; i < 100; i++) {
		if (!(epon_read(priv, EPON_MAC_ADDR_CFG) & MAC_ADDR_DONE))
			return 0;
	}
	return -ETIMEDOUT;
}

/*
 * Program per-LLID source MAC address into hardware table.
 * Two indirect writes: dw_idx=0 for bytes[2-5] (low 32), dw_idx=1 for
 * bytes[0-1] (high 16).  Matches ref eponMacSetMacAddr().
 */
static int epon_program_mac_address(struct epon_priv *priv, int llid_idx,
				    const u8 mac[ETH_ALEN])
{
	u32 mac_low  = ((u32)mac[2] << 24) | ((u32)mac[3] << 16) |
		       ((u32)mac[4] <<  8) | mac[5];
	u32 mac_high = ((u32)mac[0] << 8) | mac[1];
	u32 cfg_base = MAC_ADDR_RWCMD | ((llid_idx & 7) << MAC_ADDR_LLID_SHIFT);

	if (epon_wait_mac_cfg(priv) < 0) {
		dev_err(priv->dev, "LLID%d MAC cfg busy\n", llid_idx);
		return -ETIMEDOUT;
	}
	epon_write(priv, EPON_MAC_ADDR_VALUE, mac_low);
	epon_write(priv, EPON_MAC_ADDR_CFG, cfg_base);		/* dw_idx=0 */

	if (epon_wait_mac_cfg(priv) < 0) {
		dev_err(priv->dev, "LLID%d MAC cfg timeout (low)\n", llid_idx);
		return -ETIMEDOUT;
	}
	dev_info(priv->dev, "programming EPON LLID%d MAC %pM\n",
		 llid_idx, mac);
	epon_write(priv, EPON_MAC_ADDR_VALUE, mac_high);
	epon_write(priv, EPON_MAC_ADDR_CFG, cfg_base | MAC_ADDR_DW_IDX);	/* dw_idx=1 */

	if (epon_wait_mac_cfg(priv) < 0) {
		dev_err(priv->dev, "LLID%d MAC cfg timeout (high)\n", llid_idx);
		return -ETIMEDOUT;
	}
	return 0;
}

/* --- MPCP discovery control --- */

/*
 * Submit an MPCP command to hardware and poll for completion.
 * Setting CMD_DONE in the write triggers execution; poll until HW confirms.
 */
static void epon_discv_cmd(struct epon_priv *priv, u32 cmd, int llid_idx)
{
	u32 ctrl = cmd | DSCVRY_CMD_DONE | (llid_idx & DSCVRY_TX_MPCP_LLID_MASK);

	dev_info(priv->dev, "EPON discovery command: llid=%d cmd=%#08x ctrl=%#08x\n",
		llid_idx, cmd, ctrl);
	epon_write(priv, EPON_LLID_DSCVRY_CTRL, ctrl);
	while (!(epon_read(priv, EPON_LLID_DSCVRY_CTRL) & DSCVRY_CMD_DONE))
		cpu_relax();
}

/* --- Security key --- */

static void epon_set_security_key(struct epon_priv *priv, int llid_idx,
				  int key_idx, u8 key[16])
{
	int dw;

	for (dw = 0; dw < 4; dw++) {
		u32 cfg = SEC_KEY_WRITE_CMD |
			  ((llid_idx & 7) << SEC_KEY_LLID_SHIFT) |
			  ((key_idx  & 1) << SEC_KEY_IDX_SHIFT)  |
			  ((dw        & 3) << SEC_KEY_DW_SHIFT);
		u32 data = ((u32)key[dw * 4 + 0] << 24) |
			   ((u32)key[dw * 4 + 1] << 16) |
			   ((u32)key[dw * 4 + 2] <<  8) |
				    key[dw * 4 + 3];

		epon_write(priv, EPON_SECURITY_KEY_CFG, cfg);
		epon_write(priv, EPON_SECURITY_KEY_DATA, data);
	}
}

/* --- SW reset sequence (ref: eponMacSwReset) --- */

static void epon_sw_reset(struct epon_priv *priv)
{
	u32 raw;

	dev_info(priv->dev, "resetting EPON MAC (external_reset=%u)\n",
		 !!priv->rst_base);
	/* Assert external system-level SW reset if mapped. */
	if (priv->rst_base) {
		raw = readl(priv->rst_base);
		writel(raw | EPON_EXT_SW_RST_BIT, priv->rst_base);
		udelay(EPON_RESET_DELAY_US);
		writel(raw & ~EPON_EXT_SW_RST_BIT, priv->rst_base);
	}

	/* Assert EPON MAC internal SW reset (GLB_CFG bit 4). */
	raw = epon_read(priv, EPON_GLB_CFG);
	epon_write(priv, EPON_GLB_CFG, raw | GLB_CFG_EPON_MAC_SW_RST);
	udelay(EPON_RESET_DELAY_US);
	raw &= ~GLB_CFG_EPON_MAC_SW_RST;
	epon_write(priv, EPON_GLB_CFG, raw);
	udelay(EPON_RESET_DELAY_US);

	/* Enable RPT_TXPRI_CTRL and write post-reset timing parameters. */
	raw |= GLB_CFG_RPT_TXPRI_CTRL;
	epon_write(priv, EPON_GLB_CFG, raw);

	epon_write(priv, EPON_GRD_THRSHLD,      EPON_TIMEDRIFT_THRSHLD);
	epon_write(priv, EPON_TRX_ADJUST_TIME1, EPON_TRX_ADJUST_TIME1_DEF);
	epon_write(priv, EPON_TRX_ADJUST_TIME2, EPON_TRX_ADJUST_TIME2_DEF);
	epon_write(priv, EPON_TXFETCH_CFG,      EPON_TXFETCH_DEFAULT);
	dev_info(priv->dev,
		 "EPON reset complete: glb_cfg=%#08x txfetch=%#08x guard=%#08x\n",
		 epon_read(priv, EPON_GLB_CFG),
		 epon_read(priv, EPON_TXFETCH_CFG),
		 epon_read(priv, EPON_GRD_THRSHLD));
}

/* --- Hardware init --- */

/* Vendor xpon_1g order for EPON on EN7523:
 * select EPON WAN mode, clear GDM2 GPON release mode, stop the EPON MBI,
 * initialise MAC/LLID state, enable GDM2/CDM2 channels, then release MBI.
 */
static int epon_prepare_hardware(struct epon_priv *priv)
{
	int ret;

	dev_info(priv->dev, "preparing EPON FE and WAN mux\n");
	ret = airoha_xpon_set_fe_mode(priv->dev, priv->eth_node,
				      AIROHA_XPON_MODE_EPON);
	if (ret)
		return ret;

	dev_info(priv->dev, "selecting EPON on SCU WAN mux\n");
	ret = airoha_xpon_select_wan(priv->scu, AIROHA_XPON_MODE_EPON);
	if (ret)
		return dev_err_probe(priv->dev, ret,
				     "failed to select EPON WAN mode\n");

	dev_info(priv->dev, "EPON FE and WAN mux prepared\n");
	return 0;
}

static void epon_hw_init(struct epon_priv *priv)
{
	u32 cfg;
	int i;
	u8 zero_key[16] = {};

	epon_sw_reset(priv);

	cfg = epon_read(priv, EPON_GLB_CFG);
	cfg |= GLB_CFG_TXMBI_STOP | GLB_CFG_RXMBI_STOP;
	cfg |= GLB_CFG_MPCP_FWD | GLB_CFG_FCS_ERR_FWD | GLB_CFG_DISCV_BURST_EN;
	epon_write(priv, EPON_GLB_CFG, cfg);

	epon_write(priv, EPON_PENDING_GNT_NUM,    EPON_PENDING_GNT_DEFAULT);
	epon_write(priv, EPON_MPCP_TIMEOUT_INTVL, EPON_MPCP_TIMEOUT_DEFAULT);
	epon_write(priv, EPON_RPT_TIMEOUT_INTVL,  EPON_RPT_TIMEOUT_DEFAULT);
	epon_write(priv, EPON_MAX_FUTURE_GNT,     EPON_MAX_FUTURE_GNT_DEFAULT);
	epon_write(priv, EPON_MIN_PROC_TIME,      EPON_MIN_PROC_TIME_DEFAULT);
	epon_write(priv, EPON_LASER_ONOFF_TIME,   EPON_LASER_ONOFF_DEFAULT);
	epon_write(priv, EPON_TX_CAL_CNST,        EPON_TX_CAL_CNST_DEFAULT);

	/* Enable hardware dying gasp detection per ref (write magic value) */
	epon_write(priv, EPON_DYINGGSP_CFG, DYINGGSP_CFG_HW_ENABLE);

	for (i = 0; i < EPON_MAX_LLID; i++)
		epon_set_security_key(priv, i, 0, zero_key);

	dev_info(priv->dev,
		 "EPON hardware initialized: glb_cfg=%#08x pending_gnt=%#08x mpcp_timeout=%#08x\n",
		 epon_read(priv, EPON_GLB_CFG),
		 epon_read(priv, EPON_PENDING_GNT_NUM),
		 epon_read(priv, EPON_MPCP_TIMEOUT_INTVL));
}

/* --- ISR --- */

static irqreturn_t epon_isr(int irq, void *data)
{
	struct epon_priv *priv = data;
	u32 status;
	int idx;

	status = epon_read(priv, EPON_INT_STATUS);
	dev_info(priv->dev,
			    "EPON IRQ: status=%#08x enabled=%#08x registered_llids=%d\n",
		 status, epon_read(priv, EPON_INT_EN), priv->registered_llids);
	/* W1C: clear all at once */
	epon_write(priv, EPON_INT_STATUS, 0xFFFFFFFF);

	if (!status)
		return IRQ_NONE;

	/* Time drift: read stat, log, reset counter */
	if (status & EPON_INT_TIMEDRFT) {
		u32 drift = epon_read(priv, EPON_TIME_DRFT_STAT) & 0xFF;

		dev_info(priv->dev, "EPON: time drift %u\n", drift);
		epon_write(priv, EPON_TIME_DRFT_STAT, 0);
	}

	/*
	 * MPCP timeout: e_rpt_mpcp_timeout_llid_idx layout (little-endian):
	 *   bits [23:16] = mpcpTmoutLlid bitmask
	 * Clear by writing back with bits [31:16] zeroed.
	 */
	if (status & EPON_INT_MPCP_TIMEOUT) {
		u32 tmout    = epon_read(priv, EPON_RPT_MPCP_TIMEOUT);
		u8  llidmask = (tmout >> 16) & 0xFF;

		for (idx = 0; idx < EPON_MAX_LLID; idx++) {
			if (!(llidmask & BIT(idx)))
				continue;
			dev_info(priv->dev, "EPON: MPCP timeout LLID%d\n", idx);
			if (priv->llid[idx].valid) {
				priv->llid[idx].valid = false;
				if (priv->registered_llids > 0)
					priv->registered_llids--;
			}
			priv->llid[idx].state = LLID_STATE_REGISTERING;
		}
		epon_write(priv, EPON_RPT_MPCP_TIMEOUT, tmout & 0x0000FF00);
		if (!priv->registered_llids)
			airoha_eth_xpon_set_carrier(priv->gdm_dev, false);
	}

	/*
	 * Discovery gate: for each LLID in REGISTERING state, prepare HW
	 * state and send REGISTER_REQUEST.  Send one at a time (ref pattern).
	 */
	if (status & EPON_INT_DISCV_GATE) {
		dev_info(priv->dev, "EPON discovery GATE received\n");
		for (idx = 0; idx < EPON_MAX_LLID; idx++) {
			if (priv->llid[idx].state != LLID_STATE_REGISTERING)
				continue;
			epon_llid_set_registering(priv, idx);
			epon_discv_cmd(priv, DSCVRY_MPCP_REG_REQ, idx);
			/* State advances to WAIT until the REGISTER frame arrives */
			priv->llid[idx].state = LLID_STATE_WAIT;
			break;
		}
	}

	/*
	 * Per-LLID REGISTER frame received (bits 1..8 for LLID0..7).
	 * Read rgstrFlgSts from e_llid{n}_dscvry_sts bits[25:24].
	 */
	for (idx = 0; idx < EPON_MAX_LLID; idx++) {
		u32 sts;
		int flag;
		u8  mac[ETH_ALEN];
		u32 mac_low;

		if (!(status & (EPON_INT_LLID0_RGST << idx)))
			continue;

		sts  = epon_llid_sts(priv, idx);
		flag = (sts >> LLID_STS_RGST_FLG_SHIFT) & 3;
		dev_info(priv->dev,
			 "EPON LLID%d registration event: flag=%d status=%#08x state=%s valid=%u\n",
			 idx, flag, sts,
			 epon_llid_state_name(priv->llid[idx].state),
			 priv->llid[idx].valid);

		switch (flag) {
		case MPCP_REG_ACK:
			if (!(sts & LLID_STS_VALID)) {
				dev_err(priv->dev,
					"EPON: LLID%d ACK but LLID invalid\n", idx);
				break;
			}
			priv->llid[idx].value = sts & LLID_STS_VALUE_MASK;

			/* Assign unique MAC per LLID (add llid_idx to low bytes) */
			ether_addr_copy(mac, priv->gdm_dev->dev_addr);
			mac_low = ((u32)mac[2] << 24) | ((u32)mac[3] << 16) |
				  ((u32)mac[4] <<  8) | mac[5];
			mac_low += idx;
			mac[3] = (mac_low >> 16) & 0xFF;
			mac[4] = (mac_low >>  8) & 0xFF;
			mac[5] =  mac_low        & 0xFF;
			epon_program_mac_address(priv, idx, mac);

			/* Send REGISTER_ACK */
			epon_discv_cmd(priv, DSCVRY_MPCP_ACK | DSCVRY_RGSTR_ACK_FLG, idx);

			/* Update HW discovery status to REGISTERED (bits[31:30]=10) */
			sts = (epon_llid_sts(priv, idx) & 0x3FFFFFFF) | (2U << 30);
			epon_llid_sts_write(priv, idx, sts);

			priv->llid[idx].state = LLID_STATE_REGISTERED;
			priv->llid[idx].valid = true;
			priv->registered_llids++;
			dev_info(priv->dev, "EPON LLID%d registered: 0x%04X\n",
				 idx, priv->llid[idx].value);
			airoha_eth_xpon_set_carrier(priv->gdm_dev, true);
			break;

		case MPCP_REG_NACK:
			dev_info(priv->dev, "EPON: LLID%d NACK, retrying\n", idx);
			priv->llid[idx].state = LLID_STATE_REGISTERING;
			break;

		case MPCP_REG_DE_REGISTER:
			dev_info(priv->dev, "EPON: LLID%d deregistered\n", idx);
			if (priv->llid[idx].valid) {
				priv->llid[idx].valid = false;
				if (priv->registered_llids > 0)
					priv->registered_llids--;
			}
			priv->llid[idx].state = LLID_STATE_REGISTERING;
			if (!priv->registered_llids)
				airoha_eth_xpon_set_carrier(priv->gdm_dev, false);
			break;

		case MPCP_REG_RE_REGISTER:
			dev_info(priv->dev, "EPON: LLID%d re-register\n", idx);
			epon_discv_cmd(priv, DSCVRY_MPCP_ACK | DSCVRY_RGSTR_ACK_FLG, idx);
			break;
		}
	}

	if (status & EPON_INT_REG_REQ_DONE)
		dev_info(priv->dev, "EPON: REGISTER_REQUEST sent\n");

	if (status & EPON_INT_REG_ACK_DONE)
		dev_info(priv->dev, "EPON: REGISTER_ACK sent\n");

	/* Report over-interval: clear bits[31:24] of e_rpt_mpcp_timeout */
	if (status & EPON_INT_RPT_OVRFLW) {
		u32 tmout = epon_read(priv, EPON_RPT_MPCP_TIMEOUT);

		epon_write(priv, EPON_RPT_MPCP_TIMEOUT, tmout & 0x000000FF);
	}

	return IRQ_HANDLED;
}

/* --- Enable / Disable --- */

static int epon_enable(struct epon_priv *priv)
{
	int idx, ret;
	u32 int_en;

	dev_info(priv->dev, "starting EPON MAC\n");
	ret = airoha_xpon_phy_start(priv->dev, priv->phy,
				     AIROHA_XPON_MODE_EPON,
				     &priv->phy_initialized,
				     &priv->phy_powered);
	if (ret)
		return ret;

	ret = epon_prepare_hardware(priv);
	if (ret) {
		airoha_xpon_phy_stop(priv->dev, priv->phy,
				      AIROHA_XPON_MODE_EPON,
				      &priv->phy_initialized,
				      &priv->phy_powered);
		return ret;
	}

	epon_hw_init(priv);

	/* Put all LLIDs into REGISTERING state */
	for (idx = 0; idx < EPON_MAX_LLID; idx++) {
		priv->llid[idx].state = LLID_STATE_REGISTERING;
		priv->llid[idx].valid = false;
		epon_llid_set_registering(priv, idx);
	}

	/* Base interrupts */
	int_en = EPON_INT_DISCV_GATE  |
		 EPON_INT_GNT_OVRRUN  |
		 EPON_INT_TIMEDRFT    |
		 EPON_INT_MPCP_TIMEOUT |
		 EPON_INT_RPT_OVRFLW  |
		 EPON_INT_REG_REQ_DONE |
		 EPON_INT_REG_ACK_DONE;

	/* Per-LLID register-frame interrupts */
	for (idx = 0; idx < EPON_MAX_LLID; idx++)
		int_en |= (EPON_INT_LLID0_RGST << idx);

	epon_write(priv, EPON_INT_STATUS, ~0U);
	epon_write(priv, EPON_INT_EN, int_en);
	dev_info(priv->dev, "EPON interrupt mask=%#08x irq=%d\n",
		 int_en, priv->irq);

	ret = airoha_xpon_set_fe_datapath(priv->dev, priv->eth_node,
					  AIROHA_XPON_MODE_EPON, true);
	if (ret) {
		epon_write(priv, EPON_INT_EN, 0);
		airoha_xpon_phy_stop(priv->dev, priv->phy,
				      AIROHA_XPON_MODE_EPON,
				      &priv->phy_initialized,
				      &priv->phy_powered);
		return ret;
	}

	/* Release EPON TX/RX MBI only after GDM2/CDM2 channels are ready. */
	epon_write(priv, EPON_GLB_CFG,
		   epon_read(priv, EPON_GLB_CFG) &
		   ~(GLB_CFG_TXMBI_STOP | GLB_CFG_RXMBI_STOP));
	usleep_range(50, 100);

	enable_irq(priv->irq);
	priv->irq_enabled = true;
	dev_info(priv->dev,
		 "EPON MAC started: glb_cfg=%#08x int_enable=%#08x\n",
		 epon_read(priv, EPON_GLB_CFG), epon_read(priv, EPON_INT_EN));
	return 0;
}

static void epon_disable(struct epon_priv *priv)
{
	int idx, ret;

	dev_info(priv->dev,
		 "stopping EPON MAC: registered_llids=%d irq_enabled=%u glb_cfg=%#08x\n",
		 priv->registered_llids, priv->irq_enabled,
		 epon_read(priv, EPON_GLB_CFG));
	if (priv->irq_enabled) {
		disable_irq(priv->irq);
		priv->irq_enabled = false;
	}
	epon_write(priv, EPON_INT_EN, 0);
	epon_write(priv, EPON_GLB_CFG,
		   epon_read(priv, EPON_GLB_CFG) |
		   GLB_CFG_TXMBI_STOP | GLB_CFG_RXMBI_STOP);
	ret = airoha_xpon_set_fe_datapath(priv->dev, priv->eth_node,
					  AIROHA_XPON_MODE_EPON, false);
	if (ret)
		dev_warn(priv->dev, "failed to disable EPON datapath: %d\n", ret);

	airoha_xpon_phy_stop(priv->dev, priv->phy,
			      AIROHA_XPON_MODE_EPON,
			      &priv->phy_initialized,
			      &priv->phy_powered);

	for (idx = 0; idx < EPON_MAX_LLID; idx++) {
		priv->llid[idx].state = LLID_STATE_WAIT;
		priv->llid[idx].valid = false;
	}
	priv->registered_llids = 0;
	airoha_eth_xpon_set_carrier(priv->gdm_dev, false);
	dev_info(priv->dev, "EPON MAC stopped: glb_cfg=%#08x\n",
		 epon_read(priv, EPON_GLB_CFG));
}

/* ---------- SFP upstream ops ---------- */

static void epon_sfp_attach(void *upstream, struct sfp_bus *bus)
{
	struct epon_priv *priv = upstream;

	dev_info(priv->dev, "EPON SFP bus attached\n");
}

static void epon_sfp_detach(void *upstream, struct sfp_bus *bus)
{
	struct epon_priv *priv = upstream;

	dev_info(priv->dev, "EPON SFP bus detached\n");
}

static int epon_sfp_module_insert(void *upstream,
				  const struct sfp_eeprom_id *id)
{
	struct epon_priv *priv = upstream;

	dev_info(priv->dev, "EPON SFP module inserted\n");
	return 0;
}

static void epon_sfp_module_remove(void *upstream)
{
	struct epon_priv *priv = upstream;

	dev_info(priv->dev, "EPON SFP module removed\n");
	epon_disable(priv);
}

static int epon_sfp_module_start(void *upstream)
{
	struct epon_priv *priv = upstream;
	int ret;

	dev_info(priv->dev, "EPON SFP module start\n");

	ret = airoha_xpon_tx_rearm(priv->dev, priv->lddla_dev);
	if (ret)
		return ret;

	return epon_enable(priv);
}

static void epon_sfp_module_stop(void *upstream)
{
	struct epon_priv *priv = upstream;

	dev_info(priv->dev, "EPON SFP module stop\n");
	epon_disable(priv);
}

static void epon_sfp_link_down(void *upstream)
{
	struct epon_priv *priv = upstream;

	dev_warn(priv->dev, "EPON optical link down / LOS\n");
	airoha_eth_xpon_set_carrier(priv->gdm_dev, false);
}

static void epon_sfp_link_up(void *upstream)
{
	struct epon_priv *priv = upstream;

	dev_info(priv->dev, "EPON optical link up\n");
}

static const struct sfp_upstream_ops epon_sfp_ops = {
	.attach		  = epon_sfp_attach,
	.detach		  = epon_sfp_detach,
	.module_insert	  = epon_sfp_module_insert,
	.module_remove	  = epon_sfp_module_remove,
	.module_start	  = epon_sfp_module_start,
	.module_stop	  = epon_sfp_module_stop,
	.link_up	  = epon_sfp_link_up,
	.link_down	  = epon_sfp_link_down,
};

/* ---------- GDM2 xPON lifecycle ---------- */

static int epon_link_start(void *data)
{
	struct epon_priv *priv = data;

	if (READ_ONCE(priv->started))
		return 0;
	WRITE_ONCE(priv->started, true);
	airoha_eth_xpon_set_carrier(priv->gdm_dev, false);
	if (priv->sfp_bus)
		sfp_upstream_start(priv->sfp_bus);

	return 0;
}

static void epon_link_stop(void *data)
{
	struct epon_priv *priv = data;

	if (!READ_ONCE(priv->started))
		return;
	WRITE_ONCE(priv->started, false);
	airoha_eth_xpon_set_carrier(priv->gdm_dev, false);
	if (priv->sfp_bus)
		sfp_upstream_stop(priv->sfp_bus);
}

static const struct airoha_xpon_link_ops epon_link_ops = {
	.start = epon_link_start,
	.stop = epon_link_stop,
};

/* ---------- Platform driver ---------- */

static int epon_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct epon_priv *priv;
	struct resource *res;
	int ret;

	dev_info(dev, "probing EPON MAC\n");
	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;
	priv->dev = dev;

	priv->eth_node = of_parse_phandle(dev->of_node, "ethernet", 0);
	if (!priv->eth_node) {
		ret = dev_err_probe(dev, -ENODEV,
				    "missing ethernet phandle for EPON datapath\n");
		goto err_put_eth_node;
	}
	dev_info(dev, "EPON datapath phandle: %pOF\n", priv->eth_node);

	ret = airoha_xpon_find_gdm2(dev, priv->eth_node, &priv->gdm_dev);
	if (ret)
		goto err_put_eth_node;

	priv->scu = syscon_regmap_lookup_by_phandle(dev->of_node,
					    "airoha,scu");
	if (IS_ERR(priv->scu)) {
		ret = dev_err_probe(dev, PTR_ERR(priv->scu),
				    "failed to get SCU regmap\n");
		goto err_put_eth_node;
	}

	priv->phy = devm_phy_get(dev, "xpon");
	if (IS_ERR(priv->phy)) {
		ret = dev_err_probe(dev, PTR_ERR(priv->phy),
				    "failed to get digital xPON PHY\n");
		goto err_put_eth_node;
	}

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "mac");
	if (!res)
		res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res) {
		ret = dev_err_probe(dev, -EINVAL, "missing EPON MAC resource\n");
		goto err_put_eth_node;
	}

	priv->base = devm_ioremap_resource(dev, res);
	if (IS_ERR(priv->base)) {
		ret = PTR_ERR(priv->base);
		goto err_put_eth_node;
	}

	if (res->start == EN7523_XPON_REGION_BASE) {
		if (resource_size(res) < EN7523_EPON_REG_OFFSET + 0x400) {
			ret = dev_err_probe(dev, -EINVAL,
					    "xPON region too small for EPON: %pR\n",
					    res);
			goto err_put_eth_node;
		}
		priv->base += EN7523_EPON_REG_OFFSET;
	} else if (res->start != EN7523_EPON_REG_BASE) {
		ret = dev_err_probe(dev, -EINVAL,
				    "unsupported EPON MAC resource: %pR\n", res);
		goto err_put_eth_node;
	} else if (resource_size(res) < 0x400) {
		ret = dev_err_probe(dev, -EINVAL,
				    "EPON register window is too small: %pR\n", res);
		goto err_put_eth_node;
	}

	dev_info(dev, "EPON resource %pR, register base offset %#x\n",
		 res, res->start == EN7523_XPON_REGION_BASE ?
		 EN7523_EPON_REG_OFFSET : 0);

	/* Optional second resource: external SW reset register */
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM,
					   "epon-reset");
	if (res) {
		priv->rst_base = devm_ioremap_resource(dev, res);
		if (IS_ERR(priv->rst_base))
			priv->rst_base = NULL;
	}

	priv->irq = platform_get_irq(pdev, 0);
	if (priv->irq < 0) {
		ret = priv->irq;
		goto err_put_eth_node;
	}

	ret = devm_request_irq(dev, priv->irq, epon_isr, 0,
			       dev_name(dev), priv);
	if (ret)
		goto err_put_eth_node;

	/* IRQ starts disabled; enabled in epon_enable() when SFP is ready */
	disable_irq(priv->irq);
	priv->irq_enabled = false;
	dev_info(dev, "EPON IRQ %d requested and initially disabled\n",
		 priv->irq);

	priv->lddla_dev = airoha_xpon_find_lddla(dev);
	if (IS_ERR(priv->lddla_dev)) {
		ret = dev_err_probe(dev, PTR_ERR(priv->lddla_dev),
				    "failed to find LDDLA device\n");
		priv->lddla_dev = NULL;
		goto err_put_eth_node;
	}

	priv->sfp_bus = sfp_bus_find_fwnode(dev->fwnode);
	if (!priv->sfp_bus) {
		ret = dev_err_probe(dev, -ENODEV, "missing SFP reference\n");
		goto err_put_lddla;
	}
	if (IS_ERR(priv->sfp_bus)) {
		ret = PTR_ERR(priv->sfp_bus);
		dev_err(dev, "failed to find SFP bus: %d\n", ret);
		goto err_put_lddla;
	}

	ret = sfp_bus_add_upstream(priv->sfp_bus, priv, &epon_sfp_ops);
	if (ret)
		goto err_put_sfp;

	ret = airoha_eth_register_xpon(priv->gdm_dev, AIROHA_XPON_MODE_EPON,
				       &epon_link_ops, priv);
	if (ret)
		goto err_del_upstream;

	platform_set_drvdata(pdev, priv);
	dev_info(dev, "EPON probe complete: datapath=%s irq=%d\n",
		 priv->gdm_dev->name, priv->irq);
	return 0;

err_del_upstream:
	sfp_bus_del_upstream(priv->sfp_bus);
err_put_sfp:
	sfp_bus_put(priv->sfp_bus);
err_put_lddla:
	if (priv->lddla_dev)
		put_device(priv->lddla_dev);
err_put_eth_node:
	if (priv->gdm_dev) {
		dev_put(priv->gdm_dev);
		priv->gdm_dev = NULL;
	}
	of_node_put(priv->eth_node);
	return ret;
}

static void epon_remove(struct platform_device *pdev)
{
	struct epon_priv *priv = platform_get_drvdata(pdev);

	dev_info(priv->dev, "removing EPON MAC driver\n");
	airoha_eth_unregister_xpon(priv->gdm_dev, &epon_link_ops, priv);
	if (priv->irq_enabled)
		epon_disable(priv);
	sfp_bus_del_upstream(priv->sfp_bus);
	sfp_bus_put(priv->sfp_bus);
	if (priv->lddla_dev)
		put_device(priv->lddla_dev);
	dev_put(priv->gdm_dev);
	priv->gdm_dev = NULL;
	of_node_put(priv->eth_node);
}

/* -------------------------------------------------------------------------
 * Unified platform driver
 * ------------------------------------------------------------------------- */

static const struct airoha_xpon_match_data en7523_xpon_data = {
	.mode = AIROHA_XPON_MODE_GPON,
	.mode_from_dt = true,
};

static const struct airoha_xpon_match_data gpon_data = {
	.mode = AIROHA_XPON_MODE_GPON,
};

static const struct airoha_xpon_match_data epon_data = {
	.mode = AIROHA_XPON_MODE_EPON,
};

static int airoha_xpon_probe(struct platform_device *pdev)
{
	const struct airoha_xpon_match_data *data;
	enum airoha_xpon_mode mode;
	int ret;

	data = device_get_match_data(&pdev->dev);
	if (!data)
		return -EINVAL;

	ret = airoha_xpon_get_mode(&pdev->dev, data, &mode);
	if (ret)
		return ret;

	dev_info(&pdev->dev, "xPON mode selected: %s (%s)\n",
		 airoha_xpon_mode_name(mode),
		 data->mode_from_dt ? "Device Tree/default" : "compatible");
	if (mode == AIROHA_XPON_MODE_GPON)
		return gpon_probe(pdev);

	return epon_probe(pdev);
}

static void airoha_xpon_remove(struct platform_device *pdev)
{
	const struct airoha_xpon_match_data *data;
	enum airoha_xpon_mode mode;

	data = device_get_match_data(&pdev->dev);
	if (!data || airoha_xpon_get_mode(&pdev->dev, data, &mode))
		return;

	dev_info(&pdev->dev, "removing xPON mode %s\n",
		 airoha_xpon_mode_name(mode));
	if (mode == AIROHA_XPON_MODE_GPON)
		gpon_remove(pdev);
	else
		epon_remove(pdev);
}

static const struct of_device_id airoha_xpon_of_match[] = {
	{ .compatible = "airoha,en7523-xpon", .data = &en7523_xpon_data },
	{ .compatible = "airoha,en7523-gpon", .data = &gpon_data },
	{ .compatible = "airoha,en7523-epon", .data = &epon_data },
	{ .compatible = "econet,en7521-gpon", .data = &gpon_data },
	{ .compatible = "econet,en7526-gpon", .data = &gpon_data },
	{ .compatible = "econet,en751221-gpon", .data = &gpon_data },
	{ .compatible = "econet,en7521-epon", .data = &epon_data },
	{ .compatible = "econet,en7526-epon", .data = &epon_data },
	{ .compatible = "econet,en751221-epon", .data = &epon_data },
	{}
};
MODULE_DEVICE_TABLE(of, airoha_xpon_of_match);

static struct platform_driver airoha_xpon_driver = {
	.probe = airoha_xpon_probe,
	.remove = airoha_xpon_remove,
	.driver = {
		.name = "airoha-xpon",
		.of_match_table = airoha_xpon_of_match,
	},
};

module_platform_driver(airoha_xpon_driver);

MODULE_DESCRIPTION("Airoha/EcoNet unified GPON and EPON MAC driver");
MODULE_AUTHOR("Benjamin Larsson <benjamin.larsson@genexis.eu>");
MODULE_LICENSE("GPL");
