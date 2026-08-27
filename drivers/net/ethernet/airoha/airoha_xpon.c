// SPDX-License-Identifier: GPL-2.0-only
/*
 * Airoha/EcoNet xPON MAC driver
 *
 * Unified GPON/EPON driver for the shared EN751221/EN7523 xPON MAC complex.
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
#include <linux/bitmap.h>
#include <linux/delay.h>
#include <linux/etherdevice.h>
#include <linux/ethtool.h>
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
#include <linux/property.h>
#include <linux/optical_frontend.h>
#include <linux/phy/phy.h>
#include <linux/phy/phy-airoha-xpon.h>
#include <linux/random.h>
#include <linux/regmap.h>
#include <linux/reset.h>
#include <linux/sched.h>
#include <linux/sfp.h>
#include <linux/timer.h>
#include <linux/unaligned.h>
#include <linux/workqueue.h>
#include <net/xpon/omci.h>

#include "airoha_eth.h"
#include "airoha_xpon.h"
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

static const u8 airoha_default_vendor_id[4] = {'M', 'T', 'K', 'G'};

static int airoha_xpon_tx_rearm(struct device *dev,
				struct optical_frontend *frontend)
{
	int ret;

	if (!frontend)
		return 0;

	ret = optical_frontend_tx_rearm(frontend);
	if (ret == -EOPNOTSUPP)
		return 0;
	if (ret)
		dev_err(dev, "failed to rearm optical transmitter: %d\n", ret);
	else
		dev_info(dev, "optical transmitter safety circuit rearmed\n");

	return ret;
}

static int airoha_xpon_tx_enable(struct device *dev,
				 struct optical_frontend *frontend,
				 bool enable)
{
	int ret;

	if (!frontend)
		return 0;

	ret = optical_frontend_tx_enable(frontend, enable);
	if (ret == -EOPNOTSUPP)
		return 0;
	if (ret)
		dev_err(dev, "failed to %s optical transmitter: %d\n",
			enable ? "enable" : "disable", ret);

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

static enum xpon_mode airoha_xpon_core_mode(enum airoha_xpon_mode mode)
{
	switch (mode) {
	case AIROHA_XPON_MODE_GPON:
		return XPON_MODE_GPON;
	case AIROHA_XPON_MODE_EPON:
		return XPON_MODE_EPON;
	default:
		return XPON_MODE_GPON;
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

	if (device_property_read_string(dev, "airoha,pon-mode", &name))
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

static void airoha_xpon_update_netdev_link(struct xpon_priv *priv, bool link)
{
	struct airoha_xpon_link_state state = {
		.mode = priv->mode,
		.link = link,
		.duplex = DUPLEX_FULL,
		.autoneg = AUTONEG_DISABLE,
		.port = PORT_FIBRE,
	};

	switch (priv->mode) {
	case AIROHA_XPON_MODE_GPON:
		state.speed = SPEED_2500;
		state.rx_line_rate_bps = 2488320000ULL;
		state.tx_line_rate_bps = 1244160000ULL;
		break;
	case AIROHA_XPON_MODE_EPON:
		state.speed = SPEED_1000;
		state.rx_line_rate_bps = 1000000000ULL;
		state.tx_line_rate_bps = 1000000000ULL;
		break;
	default:
		return;
	}

	if (priv->xpon)
		xpon_device_report_carrier(priv->xpon, link);
	airoha_eth_xpon_update_link(priv->gdm_dev, &state);
}

static int airoha_xpon_set_fe_mode(struct device *dev,
				   struct net_device *netdev,
				   enum airoha_xpon_mode mode)
{
	int ret;

	dev_info(dev, "configuring %s FE mode on %s\n",
		 airoha_xpon_mode_name(mode), netdev->name);
	ret = airoha_eth_set_xpon_mode(netdev, mode);
	if (ret)
		dev_err(dev, "failed to configure %s FE mode on %s: %d\n",
			airoha_xpon_mode_name(mode), netdev->name, ret);
	else
		dev_info(dev, "%s FE mode configured on %s\n",
			 airoha_xpon_mode_name(mode), netdev->name);

	return ret;
}

static int airoha_xpon_set_fe_datapath(struct device *dev,
				       struct net_device *netdev,
				       enum airoha_xpon_mode mode,
				       bool enable)
{
	int ret;

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

	return ret;
}

static int airoha_xpon_select_wan(struct regmap *scu,
				  const struct airoha_xpon_match_data *data,
				  enum airoha_xpon_mode mode)
{
	u32 value;

	value = mode == AIROHA_XPON_MODE_GPON ?
		XPON_SCU_WAN_MODE_GPON : XPON_SCU_WAN_MODE_EPON;

	return regmap_update_bits(scu, XPON_SCU_WAN_CONF,
				  data->wan_mode_mask, value);
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
 * generated register structure reserves the first 0x4000 bytes.  GPON and
 * EPON live at fixed offsets in that shared xPON MAC region.
 * -------------------------------------------------------------------- */

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

static int airoha_xpon_frontend_set_mode(struct xpon_priv *priv)
{
	struct optical_frontend_mode mode = {};

	if (!priv->frontend)
		return 0;

	switch (priv->mode) {
	case AIROHA_XPON_MODE_GPON:
		mode.protocol = OPTICAL_FRONTEND_PROTO_GPON;
		mode.tx_rate = 1244160000ULL;
		mode.rx_rate = 2488320000ULL;
		mode.flags = OPTICAL_FRONTEND_MODE_BURST_TX;
		break;
	case AIROHA_XPON_MODE_EPON:
		mode.protocol = OPTICAL_FRONTEND_PROTO_EPON;
		mode.tx_rate = 1250000000ULL;
		mode.rx_rate = 1250000000ULL;
		mode.flags = OPTICAL_FRONTEND_MODE_BURST_TX;
		break;
	default:
		return -EINVAL;
	}

	return optical_frontend_set_mode(priv->frontend, &mode);
}

static int airoha_xpon_reset_mac(struct xpon_priv *priv)
{
	int ret;

	if (!priv->mac_reset)
		return 0;

	dev_info(priv->dev, "resetting %s MAC before session start\n",
		 airoha_xpon_mode_name(priv->mode));
	ret = reset_control_reset(priv->mac_reset);
	if (ret) {
		dev_err(priv->dev, "failed to reset xPON MAC: %d\n", ret);
		return ret;
	}

	/*
	 * The reset controller supplies the required reset pulse. Give the MAC
	 * register and indirect table engines time to leave reset before any
	 * GPON/EPON register is accessed.
	 */
	usleep_range(1000, 2000);

	return 0;
}

static void gpon_refresh_netdev_link(struct xpon_priv *priv, bool force)
{
	bool started, o5, omci, service, link, changed;

	mutex_lock(&priv->link_state_lock);
	started = READ_ONCE(priv->started);
	o5 = priv->gpon_o5;
	omci = priv->omci_operational;
	service = !bitmap_empty(priv->service_gems, GPON_MAX_GEM_ID);
	link = started && o5 && omci && service;
	changed = force || priv->netdev_link != link;
	priv->netdev_link = link;
	mutex_unlock(&priv->link_state_lock);

	if (!changed)
		return;

	dev_info(priv->dev,
		 "GPON netdev link %s: O5=%u OMCI=%u service=%u started=%u\n",
		 link ? "ready" : "not-ready", o5, omci, service, started);
	airoha_xpon_update_netdev_link(priv, link);
}

/* -----------------------------------------------------------------------
 * Register accessors
 * -------------------------------------------------------------------- */

static inline u32 gpon_read(struct xpon_priv *priv, u32 reg)
{
	return readl(priv->gpon_reg + reg);
}

static inline void gpon_write(struct xpon_priv *priv, u32 reg, u32 val)
{
	writel(val, priv->gpon_reg + reg);
}

static void gpon_dump_activation_regs(struct xpon_priv *priv,
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

static inline void gpon_set_bits(struct xpon_priv *priv, u32 reg, u32 bits)
{
	gpon_write(priv, reg, gpon_read(priv, reg) | bits);
}

static inline void gpon_clear_bits(struct xpon_priv *priv, u32 reg, u32 bits)
{
	gpon_write(priv, reg, gpon_read(priv, reg) & ~bits);
}

static inline void gpon_rmw(struct xpon_priv *priv, u32 reg, u32 mask, u32 val)
{
	gpon_write(priv, reg, (gpon_read(priv, reg) & ~mask) | val);
}

/* -----------------------------------------------------------------------
 * Low-level hardware helpers
 * -------------------------------------------------------------------- */

static int gpon_wait_bits(struct xpon_priv *priv, u32 reg, u32 mask,
			  unsigned int timeout_us)
{
	u32 val;

	return readl_poll_timeout_atomic(priv->gpon_reg + reg, val, val & mask,
					  1, timeout_us);
}

static int gpon_set_fe_mode(struct xpon_priv *priv)
{
	return airoha_xpon_set_fe_mode(priv->dev, priv->gdm_dev,
				      AIROHA_XPON_MODE_GPON);
}

static int gpon_set_fe_datapath(struct xpon_priv *priv, bool enable)
{
	return airoha_xpon_set_fe_datapath(priv->dev, priv->gdm_dev,
					  AIROHA_XPON_MODE_GPON, enable);
}

static int gpon_prepare_hardware(struct xpon_priv *priv)
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
	ret = airoha_xpon_select_wan(priv->scu, priv->match_data,
				     AIROHA_XPON_MODE_GPON);
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
	 * EN7523 resets the complete 0x4208 delay register and applies two
	 * additional DBA/BWmap defaults.  The older EN7521/EN751221 path does
	 * not execute those writes; only its 0x1c fine internal delay is common
	 * to the configuration data.  Keep the generation-specific writes out
	 * of the common MAC path.
	 */
	if (priv->match_data->en7523_gpon_defaults)
		gpon_write(priv, GPON_DBG_DLY, DBG_DLY_RESET_DEFAULT);
	gpon_rmw(priv, GPON_DBG_DLY, DBG_DLY_FINE_INT_MASK,
		 FIELD_PREP(DBG_DLY_FINE_INT_MASK,
			    priv->match_data->gpon_fine_delay));
	gpon_rmw(priv, GPON_DBG_IDLE_GEM_THLD, GENMASK(15, 0),
		 GPON_IDLE_GEM_THLD_DEFAULT);

	if (priv->match_data->en7523_gpon_defaults) {
		gpon_rmw(priv, GPON_GBL_CFG, GBL_CFG_SR_BLK_SIZE_MASK,
			 GPON_DBRU_BLOCK_SIZE_48B);
		gpon_clear_bits(priv, GPON_DBG_BWM_FILTER_CTRL,
				BWM_FILTER_LEN_VALID_CHECK_EN);
	}

	mbi = gpon_read(priv, GPON_MBI_MPI_STOP);
	if (mbi & (MBI_RX_STOP | MBI_TX_STOP))
		return dev_err_probe(priv->dev, -EIO,
				     "failed to start GPON MBI: %#08x\n", mbi);

	dev_info(priv->dev,
		 "GPON hardware prepared: mbi=%#08x gbl=%#08x dbg_dly=%#08x idle_gem=%#08x bwm_filter=%#08x\n",
		 mbi, gpon_read(priv, GPON_GBL_CFG),
		 gpon_read(priv, GPON_DBG_DLY),
		 gpon_read(priv, GPON_DBG_IDLE_GEM_THLD),
		 gpon_read(priv, GPON_DBG_BWM_FILTER_CTRL));
	return 0;
}

static void gpon_reset_activation_context(struct xpon_priv *priv)
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

static int gpon_dev_init(struct xpon_priv *priv)
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

static int gpon_load_credentials(struct xpon_priv *priv)
{
	struct omci_identity *identity = &priv->identity;
	const u8 *mac;
	int ret;

	ret = omci_identity_load(priv->dev, identity);
	if (ret)
		return ret;

	if (!(identity->valid & OMCI_IDENTITY_F_SERIAL_NUMBER) &&
	    device_property_read_bool(priv->dev,
				      "airoha,gpon-serial-from-mac")) {
		if (!(identity->valid & OMCI_IDENTITY_F_VENDOR_ID))
			return dev_err_probe(priv->dev, -EINVAL,
					     "missing OMCI vendor ID for MAC-derived GPON serial\n");

		mac = priv->gdm_dev->dev_addr;
		if (!is_valid_ether_addr(mac))
			return dev_err_probe(priv->dev, -EINVAL,
					     "invalid GDM2 MAC for GPON serial derivation\n");

		memcpy(identity->serial_number, identity->vendor_id,
		       sizeof(identity->vendor_id));
		memcpy(identity->serial_number + sizeof(identity->vendor_id),
		       mac + 2, 4);
		identity->valid |= OMCI_IDENTITY_F_SERIAL_NUMBER;
		identity->serial_source = OMCI_CONFIG_SOURCE_DRIVER;
		dev_info(priv->dev,
			 "derived GPON serial %8phN from GDM2 MAC %pM\n",
			 identity->serial_number, mac);
	}

	if (!(identity->valid & OMCI_IDENTITY_F_SERIAL_NUMBER)) {
		gpon_random_serial_number(airoha_default_vendor_id, identity);
		dev_warn(priv->dev,
			 "GPON serial number missing; generated random development serial %8phN\n",
			 identity->serial_number);
	}
	if (!(identity->valid & OMCI_IDENTITY_F_PASSWORD)) {
		memset(identity->password, 0, sizeof(identity->password));
		identity->valid |= OMCI_IDENTITY_F_PASSWORD;
		identity->password_source = OMCI_CONFIG_SOURCE_DEFAULT;
	}

	memcpy(priv->hw_sn, identity->serial_number, sizeof(priv->hw_sn));
	memcpy(priv->hw_passwd, identity->password, sizeof(priv->hw_passwd));
	dev_info(priv->dev,
		 "loaded normalized GPON identity (serial source %u, password source %u)\n",
		 identity->serial_source, identity->password_source);
	return 0;
}

static void gpon_set_serial_number_regs(struct xpon_priv *priv)
{
	u32 random_delay, sn_cfg, sn_req_threshold, tx_power_mode;
	u32 vendor = get_unaligned_be32(priv->hw_sn);
	u32 vs_sn = get_unaligned_be32(priv->hw_sn + 4);

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
		 priv->hw_sn, vendor, vs_sn,
		 gpon_read(priv, GPON_VENDOR_ID),
		 gpon_read(priv, GPON_VS_SN),
		 sn_cfg, sn_req_threshold, tx_power_mode, random_delay);
}

static void gpon_load_aes_shadow(struct xpon_priv *priv, const u8 key[16],
				  u32 switch_superframe)
{
	int i;

	/*
	 * The shadow key registers are laid out with the least significant
	 * word first, so the key bytes go in reversed word order: word 0 takes
	 * key[12..15]. Loading them straight through decrypts every downstream
	 * frame with a byte-swapped key and the whole GEM port fails CRC.
	 */
	for (i = 0; i < 4; i++)
		gpon_write(priv, GPON_AES_SHADOW_KEY0 + i * 4,
			   get_unaligned_be32(key + (3 - i) * 4));

	gpon_write(priv, GPON_AES_CFG,
		   switch_superframe & AES_KEY_SWITCH_CNT_MASK);
}

static int __gpon_set_tcont_hw(struct xpon_priv *priv, unsigned int index,
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

static int gpon_config_tcont_hw(struct xpon_priv *priv, unsigned int index,
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

static int gpon_set_tcont_hw(struct xpon_priv *priv, unsigned int index,
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

static int gpon_find_tcont_alloc_locked(struct xpon_priv *priv, u16 alloc_id)
{
	int index;

	for (index = 1; index < GPON_MAX_TCONT; index++)
		if (priv->tcont_alloc_id[index] == alloc_id)
			return index;

	return -ENOENT;
}

static int gpon_find_tcont_entity_locked(struct xpon_priv *priv, u16 entity_id)
{
	int index;

	for (index = 1; index < GPON_MAX_TCONT; index++)
		if (priv->tcont_entity_id[index] == entity_id)
			return index;

	return -ENOENT;
}

static int gpon_find_free_tcont_locked(struct xpon_priv *priv)
{
	int index;

	for (index = 1; index < GPON_MAX_TCONT; index++)
		if (priv->tcont_alloc_id[index] == GPON_TCONT_UNASSIGNED)
			return index;

	return -ENOSPC;
}

static int gpon_tcont_entity_to_index(struct xpon_priv *priv, u16 entity_id,
				      unsigned int *index)
{
	int found;

	mutex_lock(&priv->tcont_lock);
	found = gpon_find_tcont_entity_locked(priv, entity_id);
	if (found < 0 && entity_id >= 0x8000 &&
	    entity_id < 0x8000 + GPON_MAX_TCONT - 1) {
		unsigned int fallback = (entity_id - 0x8000) + 1;

		if (fallback < GPON_MAX_TCONT &&
		    priv->tcont_alloc_id[fallback] != GPON_TCONT_UNASSIGNED)
			found = fallback;
	}
	if (found >= 0)
		*index = found;
	mutex_unlock(&priv->tcont_lock);

	return found < 0 ? found : 0;
}

static int gpon_read_gem_port_hw(struct xpon_priv *priv, u16 gem_port_id,
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

static int gpon_set_gem_port_hw(struct xpon_priv *priv, u16 gem_port_id,
				bool valid, bool encrypted)
{
	bool read_encrypted, read_valid;
	u32 cfg;
	int ret, retry;

	if (gem_port_id >= GPON_MAX_GEM_ID)
		return -EINVAL;

	if (gem_port_id == 0)
		encrypted = false;

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

static int gpon_set_alloc_id_hw(struct xpon_priv *priv, u16 alloc_id,
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
	if (!ret) {
		priv->tcont_alloc_id[index] = alloc_id;
		if (priv->tcont_entity_id[index] == GPON_TCONT_ENTITY_UNASSIGNED)
			priv->tcont_entity_id[index] = 0x8000 + index - 1;
	}
out:
	mutex_unlock(&priv->tcont_lock);
	return ret;
}

static int gpon_set_omci_tcont_hw(struct xpon_priv *priv, u16 entity_id,
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

static inline u32 gpon_ploam_read_word(struct xpon_priv *priv)
{
	/*
	 * The GPON FIFO exposes each PLOAM word directly in protocol order:
	 * bits 31:24 contain ONU-ID and bits 23:16 contain the message type.
	 * This is a register value, not a byte array in CPU memory, so applying
	 * be32_to_cpu() swaps valid messages on little-endian EN7523 systems.
	 */
	return gpon_read(priv, GPON_PLOAMd_RDATA);
}

static inline void gpon_ploam_write_word(struct xpon_priv *priv, u32 val)
{
	/* Keep the same register/protocol ordering for upstream messages. */
	gpon_write(priv, GPON_PLOAMu_WDATA, val);
}

static int gpon_wait_ploam_tx_space(struct xpon_priv *priv, u32 *available)
{
	u32 status;
	int ret;

	ret = readl_poll_timeout_atomic(priv->gpon_reg + GPON_PLOAMu_FIFO_STS,
					status,
					(status & PLOAMu_FIFO_AVAIL_MASK) >=
					PLOAM_WORDS, 1,
					GPON_PLOAM_TX_TIMEOUT_US);
	*available = status & PLOAMu_FIFO_AVAIL_MASK;

	return ret;
}

static void gpon_hw_send_ploam(struct xpon_priv *priv,
			       const struct ploam_msg *msg, int times)
{
	u8 onu_id = msg->value[0] >> 24;
	u8 type = msg->value[0] >> 16;
	bool ready, los;
	int ret, t;

	if (type != PLOAM_UP_DYING_GASP) {
		ret = airoha_xpon_phy_get_link_state(priv->phy, &ready, &los);
		if (!ret && (!ready || los)) {
			dev_dbg_ratelimited(priv->dev,
					    "dropping PLOAM type %#04x while PHY ready=%u LOS=%u\n",
					     type, ready, los);
			return;
		}
	}

	if (type != PLOAM_UP_REI)
		dev_info(priv->dev,
			 "PLOAM TX: onu=%u type=%#04x copies=%d words=%08x/%08x/%08x\n",
			 onu_id, type, times, msg->value[0], msg->value[1], msg->value[2]);

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

static void gpon_ploam_rx_queue_reset(struct xpon_priv *priv)
{
	WRITE_ONCE(priv->ploam_rx_head, 0);
	WRITE_ONCE(priv->ploam_rx_tail, 0);
}

static bool gpon_ploam_rx_queue_push(struct xpon_priv *priv,
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

static bool gpon_ploam_rx_queue_pop(struct xpon_priv *priv,
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

static void gpon_fastpath_assign_onu_id(struct xpon_priv *priv,
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
	if (memcmp(msg_sn, priv->hw_sn, sizeof(msg_sn)))
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

static void gpon_drain_ploam_fifo_irq(struct xpon_priv *priv)
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

static void gpon_process_ploam_queue(struct xpon_priv *priv)
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
	struct xpon_priv *priv = hw_priv;

	dev_info(priv->dev, "GPON assigned ONU-ID %u\n", onu_id);
	gpon_write(priv, GPON_ONU_ID, ONU_ID_VLD | (onu_id & ONU_ID_MASK));
	airoha_gpon_omci_set_onu_id(&priv->omci, onu_id);
	gpon_dump_activation_regs(priv, "ONU-ID assigned");
}

/*
 * EqD is split across the two blocks: the byte-aligned part goes to the MAC
 * in G_EQD and the remaining 0-7 bits are a transmitter delay in the PHY.
 * Program both, or every upstream burst sits up to 7 bit times away from the
 * position the OLT ranged.
 */
static void gpon_set_bit_delay(struct xpon_priv *priv, u32 bit_delay)
{
	int ret;

	ret = airoha_xpon_phy_set_gpon_bit_delay(priv->phy, bit_delay & 7);
	if (ret)
		dev_warn(priv->dev,
			 "failed to program GPON PHY bit delay %u: %d\n",
			 bit_delay & 7, ret);
}

static void gpon_cb_set_eqd_o4(void *hw_priv, u32 byte_delay, u32 bit_delay)
{
	struct xpon_priv *priv = hw_priv;

	dev_info(priv->dev, "GPON O4 EqD: byte_delay=%u bit_delay=%u\n",
		 byte_delay, bit_delay);
	priv->byte_delay = byte_delay;
	priv->bit_delay  = bit_delay;
	gpon_write(priv, GPON_EQD, byte_delay);
	gpon_set_bit_delay(priv, bit_delay);
	gpon_dump_activation_regs(priv, "O4 EqD programmed");
}

static void gpon_cb_adjust_eqd_o5(void *hw_priv, u32 new_eqd)
{
	struct xpon_priv *priv = hw_priv;
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
	gpon_set_bit_delay(priv, priv->bit_delay);
	dev_info(priv->dev,
		 "GPON O5 EqD programmed: byte_delay=%u bit_delay=%u sync=%#08x\n",
		 priv->byte_delay, priv->bit_delay, sync_raw);
}

static void gpon_cb_enable_us_fec(void *hw_priv)
{
	struct xpon_priv *priv = hw_priv;

	dev_info(priv->dev, "enabling GPON upstream FEC\n");
	gpon_set_bits(priv, GPON_GBL_CFG, GBL_CFG_US_FEC_EN);
}

static void gpon_cb_set_overhead(void *hw_priv,
				  u8 guard_bits, u8 t1_pbits, u8 t2_pbits,
				  u8 t3_pbits, const u8 delim[3],
				  bool delay_mode, u16 delay_time)
{
	struct xpon_priv *priv = hw_priv;
	u32 prmbl, pre_dly;
	int ret;

	dev_info(priv->dev,
		 "GPON overhead: OLT guard=%u fallback guard=%u t1=%u t2=%u t3=%u delay_mode=%u delay=%u delim=%02x:%02x:%02x\n",
		 guard_bits, GPON_PHY_GUARD_BIT_NUM, t1_pbits, t2_pbits,
		 t3_pbits, delay_mode, delay_time,
		 delim[0], delim[1], delim[2]);

	/*
	 * Program the guard the OLT asked for.  Overriding it with a value
	 * taken from another board's firmware leaves every ranged burst
	 * misaligned with the window the OLT reserved for this ONU, so the
	 * burst is transmitted and never received.  Map PLOAM T2 to PHY T1 and
	 * PLOAM T1 to PHY T2, and preserve explicit zero values.
	 */
	if (!guard_bits)
		guard_bits = GPON_PHY_GUARD_BIT_NUM;

	ret = airoha_xpon_phy_set_gpon_overhead(priv->phy,
					       guard_bits,
					       t1_pbits, t2_pbits,
					       t3_pbits, delim);
	if (ret)
		dev_warn(priv->dev,
			 "failed to program GPON PHY overhead: %d\n", ret);

	gpon_write(priv, GPON_PLOu_GUARD_BIT, guard_bits);

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
	struct xpon_priv *priv = hw_priv;
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
	dev_dbg(priv->dev, "GPON T3 preamble: O3=%u O5=%u reg=%#08x\n",
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
	struct xpon_priv *priv = hw_priv;

	dev_info(priv->dev, "GPON AES key switch superframe=%u\n",
		 superframe & AES_KEY_SWITCH_CNT_MASK);
	gpon_write(priv, GPON_AES_CFG, superframe & AES_KEY_SWITCH_CNT_MASK);
}

static void gpon_cb_request_new_key(void *hw_priv)
{
	struct xpon_priv *priv = hw_priv;

	dev_info(priv->dev, "GPON generating a new AES key\n");
	get_random_bytes(priv->aes_key, 16);
	/* Load into shadow registers; switch time set later by Key_Switching_Time */
	gpon_load_aes_shadow(priv, priv->aes_key, 0);
	/* Inform PLOAM layer of the key so it can transmit Encryption_Key */
	ploam_set_aes_key(priv->ploam, priv->aes_key);
}

static void gpon_cb_set_ber_interval(void *hw_priv, u32 interval_ms)
{
	struct xpon_priv *priv = hw_priv;

	dev_info(priv->dev, "GPON BER reporting interval=%u ms\n",
		 interval_ms);
	priv->ber_interval_ms = interval_ms;
	if (interval_ms && READ_ONCE(priv->mac_enabled) &&
	    ploam_get_state(priv->ploam) == GPON_O5_OPERATION)
		mod_timer(&priv->ber_timer,
			  jiffies + msecs_to_jiffies(interval_ms));
	else
		timer_delete(&priv->ber_timer);
}

static int gpon_cb_set_omci_gem(void *hw_priv, u16 gem_port_id, bool valid)
{
	struct xpon_priv *priv = hw_priv;
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
	struct xpon_priv *priv = hw_priv;
	int ret;

	/*
	 * The OMCC is never encrypted in GPON (G.984.3 12.2), but this
	 * Nokia/ALCL OLT announces encrypt=3 for GEM 0 as well as for the
	 * service ports. Honouring that on GEM 0 decrypts a plaintext OMCC and
	 * every OMCI frame fails, so the ONU never answers and the OLT
	 * deactivates it. Exempt GEM 0 and take the OLT at its word everywhere
	 * else: with the port left in the clear the service ports deliver
	 * high-entropy frames with random MACs and nonsense ethertypes, which
	 * is ciphertext handed up undecrypted.
	 */
	ret = gpon_set_gem_port_hw(priv, port_id, true,
				   port_id && encrypt_mode == 3);
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
	struct xpon_priv *priv = hw_priv;
	int ret;

	dev_info(priv->dev, "GPON Alloc-ID %s: %u\n",
		 allocate ? "assign" : "remove", alloc_id);
	ret = gpon_set_alloc_id_hw(priv, alloc_id, allocate);
	if (ret)
		dev_err(priv->dev, "failed to %s Alloc-ID %u: %d\n",
			allocate ? "assign" : "remove", alloc_id, ret);
	else
		airoha_gpon_omci_reconcile_services(&priv->omci);
}

int airoha_gpon_omci_hw_set_olt_profile(void *hw_priv,
					const struct omci_olt_profile_state *state)
{
	struct xpon_priv *priv = hw_priv;

	if (!state || state->effective == OMCI_OLT_PROFILE_UNSPEC ||
	    state->effective == OMCI_OLT_PROFILE_AUTO)
		return -EINVAL;

	mutex_lock(&priv->omci_profile_lock);
	priv->omci_profile = *state;
	mutex_unlock(&priv->omci_profile_lock);

	dev_info(priv->dev,
		 "OMCI OLT profile applied: configured=%s effective=%s forced=%s quirks=%#x\n",
		 omci_olt_profile_name(state->configured),
		 omci_olt_profile_name(state->effective),
		 omci_olt_profile_name(state->forced), state->quirks);
	dev_info(priv->dev, "OMCI OLT identity: vendor=%s equipment=%s\n",
		 state->olt.vendor_id_valid ? state->olt.vendor_id : "unknown",
		 state->olt.equipment_id_valid ?
		 state->olt.equipment_id : "unknown");

	return 0;
}

void airoha_gpon_omci_hw_set_operational(void *hw_priv, bool operational)
{
	struct xpon_priv *priv = hw_priv;

	mutex_lock(&priv->link_state_lock);
	priv->omci_operational = operational;
	mutex_unlock(&priv->link_state_lock);

	gpon_refresh_netdev_link(priv, false);
}

static bool airoha_gpon_config_requires_restart(u16 key)
{
	switch (key) {
	case OMCI_CONFIG_SERIAL_NUMBER:
	case OMCI_CONFIG_VENDOR_ID:
	case OMCI_CONFIG_VERSION:
	case OMCI_CONFIG_EQUIPMENT_ID:
	case OMCI_CONFIG_PASSWORD:
	case OMCI_CONFIG_TRAFFIC_MGMT_OPTION:
	case OMCI_CONFIG_ONU_TYPE:
	case OMCI_CONFIG_UNI_COUNT:
	case OMCI_CONFIG_AGENT_FAKE_OMCI:
	case OMCI_CONFIG_OLT_PROFILE:
	case OMCI_CONFIG_OLT_PROFILE_FORCE:
	case OMCI_CONFIG_OMCC_VERSION:
	case OMCI_CONFIG_HARDWARE_VERSION:
	case OMCI_CONFIG_SOFTWARE_VERSION_0:
	case OMCI_CONFIG_SOFTWARE_VERSION_1:
		return true;
	default:
		return false;
	}
}

void airoha_gpon_omci_hw_config_changed(void *hw_priv, u16 key,
					const struct omci_identity *identity)
{
	struct xpon_priv *priv = hw_priv;

	if (!identity)
		return;

	/*
	 * A complete provider/profile apply changes both PLOAM activation data
	 * and the OMCI view exposed after O5. Restart for those session-defining
	 * values, but keep live agent policy toggles such as permissive mode and
	 * dying-gasp handling from tearing down an operational service.
	 *
	 * net/omci writes a profile one key at a time. Delay the work briefly so
	 * serial number, password, profile and managed-entity identity are all
	 * committed before the MAC and digital PHY are reset once.
	 */
	if (!airoha_gpon_config_requires_restart(key)) {
		dev_dbg(priv->dev,
			"OMCI configuration changed (key %u), GPON restart not required\n",
			key);
		return;
	}

	mutex_lock(&priv->omci_config_lock);
	priv->pending_identity = *identity;
	priv->config_restart_key = key;
	priv->config_restart_pending = true;
	mutex_unlock(&priv->omci_config_lock);

	dev_info(priv->dev,
		 "GPON session configuration changed (OMCI key %u), scheduling full restart\n",
		 key);
	mod_delayed_work(priv->fsm_wq, &priv->restart_work,
			 msecs_to_jiffies(GPON_CONFIG_RESTART_DEBOUNCE_MS));
}

int airoha_gpon_omci_hw_get_ani_topology(void *hw_priv,
					 struct omci_ani_topology *topology)
{
	struct xpon_priv *priv = hw_priv;

	if (!topology)
		return -EINVAL;

	*topology = (struct omci_ani_topology) {
		.tcont_base = 0x8000,
		.scheduler_base = 0x8000,
		.queue_base = 0x8000,
		.maximum_queue_size = 0xffff,
		.allocated_queue_size = 4,
		.tcont_count = GPON_MAX_TCONT - 1,
		.queues_per_tcont = AIROHA_NUM_QOS_QUEUES,
		.queue_config_option = 1,
		.scheduler_policy = 1,
	};

	dev_dbg(priv->dev,
		"OMCI ANI topology: %u T-CONTs, %u queues per T-CONT\n",
		topology->tcont_count, topology->queues_per_tcont);

	return 0;
}

int airoha_gpon_omci_hw_set_tcont(void *hw_priv, u16 entity_id,
				  u16 alloc_id, bool valid)
{
	struct xpon_priv *priv = hw_priv;
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
	struct xpon_priv *priv = hw_priv;
	int ret;

	ret = gpon_set_gem_port_hw(priv, gem_port_id, valid, encrypted);
	if (ret)
		return ret;

	gpon_refresh_netdev_link(priv, false);
	dev_info(priv->dev,
		 "OMCI GEM port %u %s (ME %#x T-CONT %#x direction %u)\n",
		 gem_port_id, valid ? "enabled" : "disabled", entity_id,
		 tcont_entity_id, direction);
	return 0;
}

int airoha_gpon_omci_hw_replace_service(void *hw_priv,
					const struct omci_service_config *service)
{
	struct xpon_priv *priv = hw_priv;
	struct airoha_xpon_service_cfg cfg;
	unsigned int tcont_index;
	int ret;

	if (!service || service->multicast || service->vlan_treatment_valid ||
	    service->direction == OMCI_GEM_PORT_DIRECTION_ANI_TO_UNI)
		return -EOPNOTSUPP;

	cfg = (struct airoha_xpon_service_cfg) {
		.cookie = service->cookie,
		.gem_port_id = service->gem_port_id,
		.vlan_id = service->vlan_id,
		.queue = service->queue,
		.pcp = service->pcp,
		.vlan_valid = service->vlan_valid,
		.pcp_valid = service->pcp_valid,
		.default_service = service->default_service,
	};
	ret = gpon_tcont_entity_to_index(priv, service->tcont_entity_id,
					 &tcont_index);
	if (ret && service->alloc_id && service->alloc_id != 0xffff) {
		unsigned int channel;

		/*
		 * A GPON restart clears the entity->index map in
		 * gpon_dev_init(), but the OLT does not re-run OMCI when the
		 * MIB data sync still matches: it only re-sends Assign_Alloc-ID
		 * over PLOAM, which never reaches the OMCI agent.  The agent
		 * then re-applies its retained service graph, every lookup here
		 * returns -ENOENT, and because apply_services is all-or-nothing
		 * the whole set is rolled back -- leaving an ONU in O5 with OMCI
		 * up, service=0 and no datapath at all, permanently.
		 *
		 * The Alloc-ID is enough to rebuild it: program the T-CONT and
		 * re-bind entity->index, which is exactly what a first-time
		 * provisioning does.
		 */
		ret = gpon_set_omci_tcont_hw(priv, service->tcont_entity_id,
					     service->alloc_id, true, &channel);
		if (!ret) {
			tcont_index = channel;
			dev_info(priv->dev,
				 "rebuilt T-CONT %#x (alloc-id %u) as channel %u after restart\n",
				 service->tcont_entity_id, service->alloc_id,
				 channel);
		}
	}
	if (ret)
		return ret;
	cfg.tcont = tcont_index;

	ret = airoha_eth_xpon_add_service(priv->gdm_dev, &cfg);
	if (ret)
		return ret;

	mutex_lock(&priv->link_state_lock);
	__set_bit(service->gem_port_id, priv->service_gems);
	mutex_unlock(&priv->link_state_lock);
	gpon_refresh_netdev_link(priv, false);
	dev_dbg(priv->dev,
		"OMCI service %#x: UNI %#x GEM %u T-CONT %#x channel %u queue %u VLAN %s%u PCP %s%u ANI %s%#x\n",
		service->cookie, service->uni_entity_id, service->gem_port_id,
		service->tcont_entity_id, cfg.tcont, service->queue,
		service->vlan_valid ? "" : "any/", service->vlan_id,
		service->pcp_valid ? "" : "any/", service->pcp,
		service->multicast_ani_valid ? "" : "none/",
		service->multicast_ani_entity_id);

	return 0;
}

int airoha_gpon_omci_hw_delete_service(void *hw_priv, u32 cookie)
{
	struct xpon_priv *priv = hw_priv;
	u16 gem_port_id;

	if (!airoha_eth_xpon_del_service(priv->gdm_dev, cookie,
					  &gem_port_id))
		return -ENOENT;

	if (!airoha_eth_xpon_has_gem_service(priv->gdm_dev, gem_port_id)) {
		mutex_lock(&priv->link_state_lock);
		__clear_bit(gem_port_id, priv->service_gems);
		mutex_unlock(&priv->link_state_lock);
	}
	gpon_refresh_netdev_link(priv, false);

	return 0;
}

int airoha_gpon_omci_hw_set_uni(void *hw_priv, u16 entity_id, bool enable)
{
	struct xpon_priv *priv = hw_priv;

	dev_dbg(priv->dev, "OMCI UNI port %#x requested %s\n", entity_id,
		enable ? "enabled" : "disabled");

	/* The EN7523 switch UNI administrative state remains owned by DSA. */
	return 0;
}

int airoha_gpon_omci_hw_get_telemetry(void *hw_priv,
				      struct omci_telemetry *telemetry)
{
	struct optical_frontend_telemetry optical = {};
	struct xpon_priv *priv = hw_priv;
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

	if (!priv->frontend)
		return telemetry->valid ? 0 : -ENODATA;

	ret = optical_frontend_get_telemetry(priv->frontend, &optical);
	if (ret)
		return telemetry->valid ? 0 : ret;

	if (optical.valid & OPTICAL_FRONTEND_TELEMETRY_F_TEMPERATURE) {
		telemetry->bosa_temperature_mc = optical.temperature_mc;
		telemetry->valid |= OMCI_TELEMETRY_F_BOSA_TEMPERATURE;
	}
	if (optical.valid & OPTICAL_FRONTEND_TELEMETRY_F_VOLTAGE) {
		telemetry->bosa_voltage_uv = optical.voltage_uv;
		telemetry->valid |= OMCI_TELEMETRY_F_BOSA_VOLTAGE;
	}
	if (optical.valid & OPTICAL_FRONTEND_TELEMETRY_F_BIAS) {
		telemetry->bosa_bias_ua = optical.bias_ua;
		telemetry->valid |= OMCI_TELEMETRY_F_BOSA_BIAS;
	}
	if (optical.valid & OPTICAL_FRONTEND_TELEMETRY_F_TX_POWER) {
		telemetry->bosa_tx_power_nw = optical.tx_power_nw;
		telemetry->valid |= OMCI_TELEMETRY_F_BOSA_TX_POWER;
	}
	if (optical.valid & OPTICAL_FRONTEND_TELEMETRY_F_RX_POWER) {
		telemetry->bosa_rx_power_nw = optical.rx_power_nw;
		telemetry->valid |= OMCI_TELEMETRY_F_BOSA_RX_POWER;
	}
	if (optical.valid & OPTICAL_FRONTEND_TELEMETRY_F_ALARMS) {
		telemetry->bosa_alarms = optical.alarms;
		telemetry->valid |= OMCI_TELEMETRY_F_BOSA_ALARMS;
	}

	return 0;
}

/* Forward declaration needed by gpon_disable */
static void gpon_disable(struct xpon_priv *priv);

static void gpon_cb_state_changed(void *hw_priv, enum gpon_state state)
{
	struct xpon_priv *priv = hw_priv;
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
	if (priv->xpon) {
		enum xpon_registration_state registration;

		switch (state) {
		case GPON_O1_INITIAL:
		case GPON_O7_EMERGENCY_STOP:
			registration = XPON_REGISTRATION_DOWN;
			break;
		case GPON_O2_STANDBY:
			registration = XPON_REGISTRATION_DISCOVERY;
			break;
		case GPON_O3_SERIAL_NUMBER:
		case GPON_O4_RANGING:
		case GPON_O6_POPUP:
			registration = XPON_REGISTRATION_REGISTERING;
			break;
		case GPON_O5_OPERATION:
			registration = XPON_REGISTRATION_OPERATIONAL;
			break;
		default:
			registration = XPON_REGISTRATION_DOWN;
			break;
		}
		xpon_device_report_registration(priv->xpon, registration);
	}

	if (state == GPON_O4_RANGING || state == GPON_O5_OPERATION)
		gpon_dump_activation_regs(priv, "state transition");

	mutex_lock(&priv->link_state_lock);
	priv->gpon_o5 = state == GPON_O5_OPERATION;
	mutex_unlock(&priv->link_state_lock);
	gpon_refresh_netdev_link(priv, false);

	if (state != GPON_O5_OPERATION)
		timer_delete(&priv->ber_timer);

	switch (state) {
	case GPON_O2_STANDBY:
		/*
		 * Match the stock SDK for this generation: 0x058b is the
		 * reset/O1 value, and activation starts from O2 with the
		 * generation's own response time.
		 */
		gpon_write(priv, GPON_RSP_TIME,
			   priv->match_data->gpon_rsp_time_activation);
		dev_info(priv->dev,
			 "GPON O2 activation response time=%#06x\n",
			 gpon_read(priv, GPON_RSP_TIME));
		/*
		 * A stale bit delay from the previous session would offset the
		 * serial-number burst of the next ranging cycle.
		 */
		priv->byte_delay = 0;
		priv->bit_delay = 0;
		gpon_set_bit_delay(priv, 0);
		fallthrough;
	case GPON_O3_SERIAL_NUMBER:
	case GPON_O4_RANGING:
		/* Start / restart TO1 timer */
		mod_delayed_work(priv->fsm_wq, &priv->to1_work,
				 msecs_to_jiffies(GPON_TO1_MS));
		break;
	case GPON_O5_OPERATION:
		/* Cancel TO1 and start periodic BER reporting only in O5. */
		cancel_delayed_work(&priv->to1_work);
		if (priv->ber_interval_ms)
			mod_timer(&priv->ber_timer,
				  jiffies +
				  msecs_to_jiffies(priv->ber_interval_ms));
		priv->to1_failures = 0;
		dev_info(priv->dev, "GPON O5: operational, ONU-ID=%u\n",
			 ploam_get_onu_id(priv->ploam));
		break;
	case GPON_O6_POPUP:
		/* Cancel TO1, start TO2 */
		cancel_delayed_work(&priv->to1_work);
		mod_delayed_work(priv->fsm_wq, &priv->to2_work,
				 msecs_to_jiffies(GPON_TO2_MS));
		break;
	case GPON_O7_EMERGENCY_STOP:
		cancel_delayed_work(&priv->to1_work);
		cancel_delayed_work(&priv->to2_work);
		break;
	case GPON_O1_INITIAL:
		cancel_delayed_work(&priv->to1_work);
		cancel_delayed_work(&priv->to2_work);
		break;
	default:
		break;
	}
}

static void gpon_cb_deactivate(void *hw_priv)
{
	struct xpon_priv *priv = hw_priv;

	/*
	 * The callback runs while the ordered IRQ worker is processing the
	 * downstream FIFO. Defer the stop/restart sequence until that worker
	 * has returned; gpon_disable() may cancel work and power down the PHY.
	 */
	if (READ_ONCE(priv->started) &&
	    READ_ONCE(priv->optical_active))
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

static int gpon_enable(struct xpon_priv *priv)
{
	u32 fifo_depth, irq_mask, known, pending, unknown;
	int ret;

	if (READ_ONCE(priv->mac_enabled))
		return 0;

	if (!READ_ONCE(priv->started) || !READ_ONCE(priv->optical_active))
		return -ENETDOWN;

	dev_info(priv->dev, "starting GPON MAC from state %s\n",
		 gpon_state_name(ploam_get_state(priv->ploam)));

	/*
	 * EN7523 starts each GPON session from a MAC reset.  The EN7521 SDK
	 * initial-enable path instead calls gponDevResetCtrl(XPON_DISABLE),
	 * which releases MBI without asserting RST_CTRL1[31].  A real gen1
	 * recovery reset also stops MBI before asserting that bit, so do not
	 * issue a bare reset here on EN751221.
	 */
	if (priv->match_data->gpon_reset_on_start) {
		ret = airoha_xpon_reset_mac(priv);
		if (ret)
			goto err_disable_frontend;
	}

	ret = airoha_xpon_phy_start(priv->dev, priv->phy,
				    AIROHA_XPON_MODE_GPON,
				    &priv->phy_initialized,
				    &priv->phy_powered);
	if (ret)
		goto err_disable_frontend;

	ret = gpon_prepare_hardware(priv);
	if (ret) {
		dev_err(priv->dev, "failed to prepare GPON hardware: %d\n", ret);
		goto err_stop_phy;
	}

	ret = gpon_dev_init(priv);
	if (ret) {
		gpon_set_fe_datapath(priv, false);
		dev_err(priv->dev, "GPON hardware init failed: %d\n", ret);
		goto err_stop_phy;
	}

	gpon_set_serial_number_regs(priv);
	gpon_load_aes_shadow(priv, priv->aes_key, 0);

	/*
	 * Inspect events accumulated while the digital PHY was locking. Known
	 * activation bits help distinguish CDR lock from valid GPON framing.
	 * Error-only interrupts remain masked because the EN7523 can assert
	 * them continuously while GEM delineation is being acquired; counters
	 * remain available through the diagnostic paths.
	 */
	irq_mask = GPON_INT_DEFAULT_MASK;
	if (priv->dying_gasp_irq >= 0)
		irq_mask &= ~INT_DYING_GASP;

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

	/*
	 * Enter O2 before unmasking the MAC interrupt source. Any PLOAM queued
	 * in hardware is then processed against a fully initialized FSM.
	 */
	atomic_set(&priv->pending_irqs, 0);
	gpon_ploam_rx_queue_reset(priv);
	WRITE_ONCE(priv->mac_enabled, true);
	ret = airoha_xpon_tx_enable(priv->dev, priv->frontend, true);
	if (ret)
		goto err_disable_mac;

	/* Do not enter O2 until the external transmitter interlock is open. */
	ploam_start(priv->ploam);
	WRITE_ONCE(priv->phy_link_known, false);
	mod_delayed_work(priv->fsm_wq, &priv->phy_link_work, 0);

	/* Vendor gpon_INT_init() clears W1C status before unmasking the MAC. */
	gpon_write(priv, GPON_INT_STATUS, ~0U);
	gpon_write(priv, GPON_INT_ENABLE, irq_mask);

	dev_info(priv->dev,
		 "enabling GPON interrupts on IRQ %d with mask=%#08x status=%#08x\n",
		 priv->irq, gpon_read(priv, GPON_INT_ENABLE),
		 gpon_read(priv, GPON_INT_STATUS));

	dev_info(priv->dev,
		 "GPON MAC started: state=%s mbi=%#08x int_enable=%#08x\n",
		 gpon_state_name(ploam_get_state(priv->ploam)),
		 gpon_read(priv, GPON_MBI_MPI_STOP),
		 gpon_read(priv, GPON_INT_ENABLE));

	return 0;

err_disable_mac:
	gpon_disable(priv);
	return ret;
err_stop_phy:
	airoha_xpon_tx_enable(priv->dev, priv->frontend, false);
	airoha_xpon_phy_stop(priv->dev, priv->phy,
			     AIROHA_XPON_MODE_GPON,
			     &priv->phy_initialized,
			     &priv->phy_powered);
	return ret;
err_disable_frontend:
	airoha_xpon_tx_enable(priv->dev, priv->frontend, false);
	return ret;
}

static void gpon_disable(struct xpon_priv *priv)
{
	bool mac_enabled = READ_ONCE(priv->mac_enabled);
	bool phy_active = priv->phy_initialized || priv->phy_powered;
	bool omci_reset = false;
	int ret;

	airoha_xpon_tx_enable(priv->dev, priv->frontend, false);

	if (!mac_enabled && !phy_active)
		goto reset_session;

	dev_info(priv->dev,
		 "stopping GPON MAC: state=%s mbi=%#08x int_status=%#08x\n",
		 gpon_state_name(ploam_get_state(priv->ploam)),
		 gpon_read(priv, GPON_MBI_MPI_STOP),
		 gpon_read(priv, GPON_INT_STATUS));

	/*
	 * Mask the MAC before cancelling queued FSM work. The Linux IRQ line
	 * remains enabled so no activation edge is lost across a restart.
	 */
	WRITE_ONCE(priv->mac_enabled, false);
	if (mac_enabled) {
		gpon_write(priv, GPON_INT_ENABLE, 0);
		gpon_write(priv, GPON_INT_STATUS, ~0U);
		synchronize_irq(priv->irq);
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
	if (current_work() == &priv->phy_link_work.work)
		cancel_delayed_work(&priv->phy_link_work);
	else
		cancel_delayed_work_sync(&priv->phy_link_work);
	WRITE_ONCE(priv->phy_link_known, false);
	WRITE_ONCE(priv->phy_link_up, false);

	if (mac_enabled) {
		/*
		 * Drain OMCI before disconnecting GDM2. Deactivate_ONU-ID can
		 * share a downstream frame with the last OMCI request, and the
		 * response must reach QDMA before the datapath is disabled.
		 */
		airoha_gpon_omci_reset_session(&priv->omci);
		omci_reset = true;

		/*
		 * Match the vendor shutdown path: invalidate the runtime
		 * identities, disconnect GDM2, then stop the GPON/PSE MBI.
		 */
		gpon_reset_activation_context(priv);
		gpon_write(priv, GPON_OMCI_ID, 0);

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

		gpon_set_bits(priv, GPON_MBI_MPI_STOP,
			      MBI_RX_STOP | MBI_TX_STOP);
		dev_info(priv->dev, "GPON MBI stopped: %#08x\n",
			 gpon_read(priv, GPON_MBI_MPI_STOP));
	}

	if (phy_active)
		airoha_xpon_phy_stop(priv->dev, priv->phy,
				     AIROHA_XPON_MODE_GPON,
				     &priv->phy_initialized,
				     &priv->phy_powered);

reset_session:
	ploam_reset(priv->ploam);
	if (!omci_reset)
		airoha_gpon_omci_reset_session(&priv->omci);
	airoha_gpon_omci_set_state(&priv->omci, GPON_O1_INITIAL);
	mutex_lock(&priv->link_state_lock);
	priv->gpon_o5 = false;
	priv->omci_operational = false;
	bitmap_zero(priv->service_gems, GPON_MAX_GEM_ID);
	mutex_unlock(&priv->link_state_lock);
	gpon_refresh_netdev_link(priv, true);
	dev_info(priv->dev, "GPON MAC stopped, state reset to %s\n",
		 gpon_state_name(ploam_get_state(priv->ploam)));
}

/* -----------------------------------------------------------------------
 * Timers
 * -------------------------------------------------------------------- */

static void gpon_ber_timer_fn(struct timer_list *t)
{
	struct xpon_priv *priv = timer_container_of(priv, t, ber_timer);
	bool ready, los;
	int ret;

	if (!READ_ONCE(priv->mac_enabled) || !priv->ber_interval_ms ||
	    ploam_get_state(priv->ploam) != GPON_O5_OPERATION)
		return;

	ret = airoha_xpon_phy_get_link_state(priv->phy, &ready, &los);
	if (!ret && (!ready || los)) {
		mod_delayed_work(priv->fsm_wq, &priv->phy_link_work, 0);
		return;
	}

	ploam_notify_ber(priv->ploam, 0);
	if (READ_ONCE(priv->mac_enabled) && priv->ber_interval_ms &&
	    ploam_get_state(priv->ploam) == GPON_O5_OPERATION)
		mod_timer(&priv->ber_timer,
			  jiffies +
			  msecs_to_jiffies(priv->ber_interval_ms));
}

static void airoha_xpon_phy_link_work_fn(struct work_struct *work)
{
	struct xpon_priv *priv =
		container_of(to_delayed_work(work), struct xpon_priv,
			     phy_link_work);
	bool ready, los, link, changed;
	int ret;

	if (!READ_ONCE(priv->phy_powered))
		return;

	ret = airoha_xpon_phy_get_link_state(priv->phy, &ready, &los);
	if (ret)
		goto rearm;

	link = ready && !los;
	changed = !READ_ONCE(priv->phy_link_known) ||
		  READ_ONCE(priv->phy_link_up) != link;
	WRITE_ONCE(priv->phy_link_known, true);
	WRITE_ONCE(priv->phy_link_up, link);
	if (priv->xpon)
		xpon_device_report_optical(priv->xpon, ready, los);

	if (changed)
		dev_info(priv->dev,
			 "%s digital PHY link %s: ready=%u LOS=%u\n",
			 airoha_xpon_mode_name(priv->mode),
			 link ? "up" : "down", ready, los);

	if (!link && priv->mode == AIROHA_XPON_MODE_GPON &&
	    ploam_get_state(priv->ploam) == GPON_O5_OPERATION) {
		dev_warn(priv->dev,
			 "GPON synchronization lost in O5, stopping upstream PLOAM\n");
		timer_delete(&priv->ber_timer);
		ploam_notify_los(priv->ploam);
	}

rearm:
	if (READ_ONCE(priv->phy_powered))
		mod_delayed_work(priv->fsm_wq, &priv->phy_link_work,
				 msecs_to_jiffies(XPON_LINK_POLL_MS));
}

/* TO1: O3/O4 timeout — no Ranging_Time received within 10 s → return to O2 */
static void gpon_to1_work_fn(struct work_struct *work)
{
	struct xpon_priv *priv =
		container_of(to_delayed_work(work), struct xpon_priv, to1_work);
	enum gpon_state st = ploam_get_state(priv->ploam);

	/*
	 * This work runs on the same ordered queue as downstream PLOAM
	 * processing, so Upstream_Overhead cannot re-enter O3 in the middle
	 * of the O3/O4 -> O2 transition.
	 */
	if (st != GPON_O3_SERIAL_NUMBER && st != GPON_O4_RANGING)
		return;

	gpon_dump_activation_regs(priv, "TO1 expired");
	priv->to1_failures++;

	/*
	 * A ranging cycle that does not complete is normal on a busy or
	 * misconfigured OLT, and the recovery for it is cheap: drop back to O2
	 * and answer the next Upstream_Overhead.  Restarting the MAC and the
	 * PHY here instead would cost an optical relock on every miss.  Only a
	 * long run of failures means the hardware itself is stuck, and that is
	 * what the vendor driver escalates on.
	 */
	if (priv->to1_failures < GPON_TO1_MAX_RETRIES) {
		dev_warn(priv->dev,
			 "GPON TO1 expired in O%d (%u/%u), returning to O2\n",
			 (int)st, priv->to1_failures, GPON_TO1_MAX_RETRIES);
		gpon_write(priv, GPON_ONU_ID, PLOAM_ONU_UNASSIGNED);
		ploam_reset(priv->ploam);
		ploam_start(priv->ploam);
		return;
	}

	dev_err(priv->dev,
		"GPON TO1 expired %u times without reaching O5, resetting MAC and PHY\n",
		priv->to1_failures);
	priv->to1_failures = 0;
	gpon_disable(priv);
	if (READ_ONCE(priv->started) && READ_ONCE(priv->optical_active))
		mod_delayed_work(priv->fsm_wq, &priv->restart_work,
				 msecs_to_jiffies(GPON_DEACTIVATE_RESTART_MS));
}

/* TO2: O6 timeout — no Popup received within 100 ms → full disable */
static void gpon_to2_work_fn(struct work_struct *work)
{
	struct xpon_priv *priv =
		container_of(to_delayed_work(work), struct xpon_priv, to2_work);

	bool ready, los;
	int ret;

	if (ploam_get_state(priv->ploam) != GPON_O6_POPUP)
		return;

	/*
	 * O6 is entered on LOS, so ask the PHY whether the fibre came back
	 * before scheduling anything.  While the light is still gone a restart
	 * cannot succeed: it relocks nothing, fails, and re-arms itself, which
	 * on a dirty connector turns one glitch into a restart storm.  Stop the
	 * MAC and wait instead -- gpon_sfp_link_up() restarts the session when
	 * the fibre is back.  The vendor TO2 handler makes the same check.
	 */
	ret = airoha_xpon_phy_get_link_state(priv->phy, &ready, &los);
	if (!ret && (los || !ready)) {
		dev_warn(priv->dev,
			 "GPON TO2 expired in O6 with the optical link still down, waiting for the fibre\n");
		gpon_disable(priv);
		return;
	}

	dev_warn(priv->dev, "GPON TO2 expired in O6, resetting\n");
	gpon_disable(priv);
	if (READ_ONCE(priv->started) && READ_ONCE(priv->optical_active))
		mod_delayed_work(priv->fsm_wq, &priv->restart_work,
				 msecs_to_jiffies(GPON_DEACTIVATE_RESTART_MS));
}

static bool
gpon_take_pending_identity(struct xpon_priv *priv,
			   struct omci_identity *identity, u16 *key)
{
	bool pending;

	mutex_lock(&priv->omci_config_lock);
	pending = priv->config_restart_pending;
	if (pending) {
		*identity = priv->pending_identity;
		*key = priv->config_restart_key;
		priv->config_restart_pending = false;
	}
	mutex_unlock(&priv->omci_config_lock);

	return pending;
}

static void
gpon_apply_runtime_identity(struct xpon_priv *priv,
			    const struct omci_identity *identity)
{
	priv->identity = *identity;
	memcpy(priv->hw_sn, identity->serial_number, sizeof(priv->hw_sn));
	memcpy(priv->hw_passwd, identity->password,
	       sizeof(priv->hw_passwd));
	ploam_set_identity(priv->ploam, priv->hw_sn, priv->hw_passwd);

	dev_info(priv->dev,
		 "applied runtime GPON identity (serial source %u, password source %u)\n",
		 identity->serial_source, identity->password_source);
}

static void gpon_restart_work_fn(struct work_struct *work)
{
	struct xpon_priv *priv =
		container_of(to_delayed_work(work), struct xpon_priv,
			     restart_work);
	struct omci_identity identity;
	bool config_restart;
	bool stopped = false;
	u16 key = OMCI_CONFIG_UNSPEC;
	int ret;

	config_restart = gpon_take_pending_identity(priv, &identity, &key);
	if (!READ_ONCE(priv->started) || !READ_ONCE(priv->optical_active)) {
		if (config_restart)
			gpon_apply_runtime_identity(priv, &identity);
		return;
	}

	if (config_restart) {
		dev_warn(priv->dev,
			 "restarting GPON after runtime OMCI configuration key %u changed\n",
			 key);
		if (READ_ONCE(priv->mac_enabled)) {
			gpon_disable(priv);
			stopped = true;
		}
		gpon_apply_runtime_identity(priv, &identity);
	} else if (ploam_get_state(priv->ploam) != GPON_O1_INITIAL) {
		dev_warn(priv->dev,
			 "restarting GPON after Deactivate_ONU-ID from the OLT\n");
		gpon_disable(priv);
		stopped = true;
	}

	if (stopped) {
		msleep(GPON_DEACTIVATE_RESTART_MS);
		if (!READ_ONCE(priv->started) ||
		    !READ_ONCE(priv->optical_active))
			return;
	}

	ret = airoha_xpon_tx_rearm(priv->dev, priv->frontend);
	if (ret) {
		dev_warn(priv->dev,
			 "retrying optical transmitter rearm in %u ms\n",
			 GPON_REARM_RETRY_MS);
		if (READ_ONCE(priv->started) &&
		    READ_ONCE(priv->optical_active))
			mod_delayed_work(priv->fsm_wq, &priv->restart_work,
					 msecs_to_jiffies(GPON_REARM_RETRY_MS));
		return;
	}

	ret = gpon_enable(priv);
	if (ret)
		dev_err(priv->dev, "failed to restart GPON: %d\n", ret);
}

int airoha_gpon_omci_hw_start(void *hw_priv)
{
	struct xpon_priv *priv = hw_priv;
	int ret = 0;

	if (READ_ONCE(priv->started))
		return 0;

	WRITE_ONCE(priv->started, true);
	gpon_refresh_netdev_link(priv, true);
	if (priv->sfp_bus) {
		if (!READ_ONCE(priv->optical_active))
			sfp_upstream_start(priv->sfp_bus);
	} else {
		ret = airoha_xpon_tx_rearm(priv->dev, priv->frontend);
		if (!ret) {
			WRITE_ONCE(priv->optical_active, true);
			ret = gpon_enable(priv);
		}
		if (ret)
			WRITE_ONCE(priv->optical_active, false);
	}

	if (ret) {
		WRITE_ONCE(priv->started, false);
		gpon_refresh_netdev_link(priv, true);
	}

	return ret;
}

void airoha_gpon_omci_hw_stop(void *hw_priv)
{
	struct xpon_priv *priv = hw_priv;

	if (!READ_ONCE(priv->started))
		return;

	WRITE_ONCE(priv->started, false);
	cancel_delayed_work_sync(&priv->restart_work);
	if (priv->sfp_bus && READ_ONCE(priv->optical_active)) {
		sfp_upstream_stop(priv->sfp_bus);
	} else {
		WRITE_ONCE(priv->optical_active, false);
		gpon_disable(priv);
	}
	gpon_refresh_netdev_link(priv, true);
}

/* -----------------------------------------------------------------------
 * Interrupt handler
 * -------------------------------------------------------------------- */

static void gpon_dump_error_counters(struct xpon_priv *priv, u32 errors)
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
	struct xpon_priv *priv =
		container_of(work, struct xpon_priv, irq_work);
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

		if (!READ_ONCE(priv->mac_enabled))
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
			dev_dbg(priv->dev,
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
			int ret;

			dev_warn(priv->dev, "GPON dying-gasp interrupt\n");
			ploam_notify_dying_gasp(priv->ploam);
			ret = airoha_gpon_omci_send_dying_gasp(&priv->omci);
			if (ret && ret != -EOPNOTSUPP && ret != -ENOLINK)
				dev_warn(priv->dev,
					 "failed to send OMCI dying gasp: %d\n",
					 ret);
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

	if (READ_ONCE(priv->mac_enabled))
		gpon_process_ploam_queue(priv);
}

static irqreturn_t gpon_isr(int irq, void *data)
{
	struct xpon_priv *priv = data;
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

static irqreturn_t gpon_dying_gasp_isr(int irq, void *data)
{
	struct xpon_priv *priv = data;

	if (!READ_ONCE(priv->mac_enabled))
		return IRQ_HANDLED;

	atomic_or(INT_DYING_GASP, &priv->pending_irqs);
	queue_work(priv->fsm_wq, &priv->irq_work);

	return IRQ_HANDLED;
}

/* -----------------------------------------------------------------------
 * SFP upstream ops
 * -------------------------------------------------------------------- */

static void gpon_sfp_attach(void *upstream, struct sfp_bus *bus)
{
	struct xpon_priv *priv = upstream;

	dev_info(priv->dev, "GPON SFP bus attached\n");
}

static void gpon_sfp_detach(void *upstream, struct sfp_bus *bus)
{
	struct xpon_priv *priv = upstream;

	dev_info(priv->dev, "GPON SFP bus detached\n");
}

static int gpon_sfp_module_insert(void *upstream,
				   const struct sfp_eeprom_id *id)
{
	struct xpon_priv *priv = upstream;

	dev_info(priv->dev, "GPON SFP module inserted\n");
	return 0;
}

static void gpon_sfp_module_remove(void *upstream)
{
	struct xpon_priv *priv = upstream;

	dev_info(priv->dev, "GPON SFP module removed\n");
	WRITE_ONCE(priv->optical_active, false);
	cancel_delayed_work_sync(&priv->restart_work);
	gpon_disable(priv);
}

static int gpon_sfp_module_start(void *upstream)
{
	struct xpon_priv *priv = upstream;
	enum gpon_state state;
	int ret = 0;

	state = ploam_get_state(priv->ploam);
	dev_info(priv->dev, "GPON SFP module start: state=%s\n",
		 gpon_state_name(state));
	WRITE_ONCE(priv->optical_active, true);

	/* PHY ready: if we were in emergency stop, stay there. */
	if (state == GPON_O1_INITIAL) {
		ret = airoha_xpon_tx_rearm(priv->dev, priv->frontend);
		if (ret)
			goto err_inactive;

		ret = gpon_enable(priv);
	}

	if (ret)
		goto err_inactive;

	return 0;

err_inactive:
	WRITE_ONCE(priv->optical_active, false);
	return ret;
}

static void gpon_sfp_module_stop(void *upstream)
{
	struct xpon_priv *priv = upstream;

	dev_info(priv->dev, "GPON SFP module stop\n");
	WRITE_ONCE(priv->optical_active, false);
	cancel_delayed_work_sync(&priv->restart_work);
	gpon_disable(priv);
}

static void gpon_sfp_link_down(void *upstream)
{
	struct xpon_priv *priv = upstream;

	dev_warn(priv->dev, "GPON optical link down / LOS\n");
	ploam_notify_los(priv->ploam);
}

static void gpon_sfp_link_up(void *upstream)
{
	struct xpon_priv *priv = upstream;

	dev_info(priv->dev, "GPON optical link up\n");
	if (READ_ONCE(priv->started) &&
	    READ_ONCE(priv->optical_active) &&
	    !READ_ONCE(priv->mac_enabled))
		mod_delayed_work(priv->fsm_wq, &priv->restart_work, 0);
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
	struct xpon_priv *priv = data;

	/* net/omci owns GPON activation; ndo_open only exposes saved carrier. */
	gpon_refresh_netdev_link(priv, true);
	return 0;
}

static void gpon_link_stop(void *data)
{
	struct xpon_priv *priv = data;

	/* net/omci keeps OMCC and activation alive across ndo_stop. */
	gpon_refresh_netdev_link(priv, true);
}

static void gpon_mac_irq(void *data)
{
	gpon_isr(0, data);
}

static const struct airoha_xpon_link_ops gpon_link_ops = {
	.start = gpon_link_start,
	.stop = gpon_link_stop,
	.mac_irq = gpon_mac_irq,
};

/* -------------------------------------------------------------------------
 * EPON implementation
 * ------------------------------------------------------------------------- */
static const char *epon_llid_state_name(enum airoha_epon_llid_state state)
{
	switch (state) {
	case AIROHA_EPON_LLID_WAIT:
		return "wait";
	case AIROHA_EPON_LLID_REGISTERING:
		return "registering";
	case AIROHA_EPON_LLID_REGISTERED:
		return "registered";
	default:
		return "unknown";
	}
}



static inline u32 epon_read(struct xpon_priv *priv, u32 reg)
{
	return readl(priv->epon_reg + reg);
}

static inline void epon_write(struct xpon_priv *priv, u32 reg, u32 val)
{
	writel(val, priv->epon_reg + reg);
}

/* --- LLID discovery status helpers --- */

static u32 epon_llid_sts(struct xpon_priv *priv, int idx)
{
	return epon_read(priv, EPON_LLID0_DSCVRY_STS + idx * 4);
}

static void epon_llid_sts_write(struct xpon_priv *priv, int idx, u32 val)
{
	epon_write(priv, EPON_LLID0_DSCVRY_STS + idx * 4, val);
}

/*
 * Set LLID HW discovery state to REGISTERING.
 * Pattern from ref eponMpcpDscvFsmWaitHandler / eponMpcpDiscvGateIntHandler:
 * clear bits[31:30] (set unregistered) 10 times, then set bits[31:30]=01
 * while setting all lower bits (0x7FFFFFFF gives bit30=1, rest all-ones).
 */
static void epon_llid_set_registering(struct xpon_priv *priv, int idx)
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

static int epon_wait_mac_cfg(struct xpon_priv *priv)
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
static int epon_program_mac_address(struct xpon_priv *priv, int llid_idx,
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
 * Submit one MPCP command. In interrupt mode mpcp_cmd_done is hardware
 * status; REG_REQ_DONE/REG_ACK_DONE drive the software state transition.
 */
static void epon_discv_cmd(struct xpon_priv *priv, u32 cmd, int llid_idx)
{
	u32 ctrl = cmd | (llid_idx & DSCVRY_TX_MPCP_LLID_MASK);

	dev_info(priv->dev,
		 "EPON discovery command: llid=%d cmd=%#08x ctrl=%#08x\n",
		 llid_idx, cmd, ctrl);
	epon_write(priv, EPON_LLID_DSCVRY_CTRL, ctrl);
}

static void epon_report_registration(struct xpon_priv *priv)
{
	if (!priv->xpon)
		return;

	xpon_device_report_registration(priv->xpon,
		priv->registered_llids ? XPON_REGISTRATION_OPERATIONAL :
		XPON_REGISTRATION_REGISTERING);
}

static void epon_llid_drop(struct xpon_priv *priv, int idx)
{
	if (priv->llid[idx].valid) {
		priv->llid[idx].valid = false;
		if (priv->registered_llids > 0)
			priv->registered_llids--;
	}
	if (priv->oam)
		xpon_oam_llid_unregistered(priv->oam, idx);
}

/* --- Security key --- */

static void epon_set_security_key(struct xpon_priv *priv, int llid_idx,
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

static void epon_sw_reset(struct xpon_priv *priv)
{
	u32 raw;

	dev_info(priv->dev, "resetting EPON MAC (external_reset=%u)\n",
		 !!priv->epon_reset_reg);
	/* Assert external system-level SW reset if mapped. */
	if (priv->epon_reset_reg) {
		raw = readl(priv->epon_reset_reg);
		writel(raw | EPON_EXT_SW_RST_BIT, priv->epon_reset_reg);
		udelay(EPON_RESET_DELAY_US);
		writel(raw & ~EPON_EXT_SW_RST_BIT, priv->epon_reset_reg);
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
static int epon_prepare_hardware(struct xpon_priv *priv)
{
	int ret;

	dev_info(priv->dev, "preparing EPON FE and WAN mux\n");
	ret = airoha_xpon_set_fe_mode(priv->dev, priv->gdm_dev,
				      AIROHA_XPON_MODE_EPON);
	if (ret)
		return ret;

	dev_info(priv->dev, "selecting EPON on SCU WAN mux\n");
	ret = airoha_xpon_select_wan(priv->scu, priv->match_data,
				     AIROHA_XPON_MODE_EPON);
	if (ret)
		return dev_err_probe(priv->dev, ret,
				     "failed to select EPON WAN mode\n");

	dev_info(priv->dev, "EPON FE and WAN mux prepared\n");
	return 0;
}

static void epon_hw_init(struct xpon_priv *priv)
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
	struct xpon_priv *priv = data;
	u32 enabled, raw, status;
	int idx;

	raw = epon_read(priv, EPON_INT_STATUS);
	if (!raw)
		return IRQ_NONE;

	enabled = epon_read(priv, EPON_INT_EN);
	status = raw & enabled;
	/* W1C: acknowledge the complete hardware snapshot. */
	epon_write(priv, EPON_INT_STATUS, raw);
	if (!status)
		return IRQ_HANDLED;

	dev_info(priv->dev,
		 "EPON IRQ: status=%#08x enabled=%#08x registered_llids=%d\n",
		 status, enabled, priv->registered_llids);

	if (status & EPON_INT_TIMEDRFT) {
		u32 drift = epon_read(priv, EPON_TIME_DRFT_STAT) & 0xff;

		dev_info(priv->dev, "EPON: time drift %u\n", drift);
		epon_write(priv, EPON_TIME_DRFT_STAT, 0);
	}

	if (status & EPON_INT_MPCP_TIMEOUT) {
		u32 tmout = epon_read(priv, EPON_RPT_MPCP_TIMEOUT);
		u8 llidmask = (tmout >> 16) & 0xff;

		for (idx = 0; idx < EPON_MAX_LLID; idx++) {
			if (!(llidmask & BIT(idx)))
				continue;

			dev_info(priv->dev, "EPON: MPCP timeout LLID%d\n", idx);
			epon_llid_drop(priv, idx);
			priv->llid[idx].state = AIROHA_EPON_LLID_REGISTERING;
			epon_llid_set_registering(priv, idx);
		}
		epon_write(priv, EPON_RPT_MPCP_TIMEOUT, tmout & 0x0000ff00);
		if (!priv->registered_llids)
			airoha_xpon_update_netdev_link(priv, false);
		epon_report_registration(priv);
	}

	/*
	 * A discovery GATE starts exactly one REGISTER_REQUEST. Completion is
	 * reported by REG_REQ_DONE; the command-done bit is not software-owned.
	 */
	if (status & EPON_INT_DISCV_GATE) {
		if (priv->xpon && !priv->registered_llids)
			xpon_device_report_registration(priv->xpon,
						XPON_REGISTRATION_DISCOVERY);
		dev_info(priv->dev, "EPON discovery GATE received\n");
		for (idx = 0; idx < EPON_MAX_LLID; idx++) {
			if (priv->llid[idx].state != AIROHA_EPON_LLID_REGISTERING)
				continue;
			epon_llid_set_registering(priv, idx);
			epon_discv_cmd(priv, DSCVRY_MPCP_REG_REQ, idx);
			break;
		}
	}

	if (status & EPON_INT_REG_REQ_DONE) {
		idx = epon_read(priv, EPON_LLID_DSCVRY_CTRL) &
			DSCVRY_TX_MPCP_LLID_MASK;
		if (idx < EPON_MAX_LLID &&
		    priv->llid[idx].state == AIROHA_EPON_LLID_REGISTERING) {
			priv->llid[idx].state = AIROHA_EPON_LLID_REGISTER_REQUEST;
			dev_info(priv->dev,
				 "EPON LLID%d REGISTER_REQUEST sent\n", idx);
			if (priv->xpon && !priv->registered_llids)
				xpon_device_report_registration(priv->xpon,
							XPON_REGISTRATION_REGISTERING);
		}
	}

	/* REGISTER frames are reported by bits 1..8, one bit per LLID slot. */
	for (idx = 0; idx < EPON_MAX_LLID; idx++) {
		u32 sts;
		int flag;
		u8 mac[ETH_ALEN];
		u32 mac_low;

		if (!(status & (EPON_INT_LLID0_RGST << idx)))
			continue;

		sts = epon_llid_sts(priv, idx);
		flag = (sts >> LLID_STS_RGST_FLG_SHIFT) & 3;
		dev_info(priv->dev,
			 "EPON LLID%d registration event: flag=%d status=%#08x state=%s valid=%u\n",
			 idx, flag, sts,
			 epon_llid_state_name(priv->llid[idx].state),
			 priv->llid[idx].valid);

		switch (flag) {
		case MPCP_REG_ACK:
			if (priv->llid[idx].state != AIROHA_EPON_LLID_REGISTER_REQUEST)
				break;
			if (!(sts & LLID_STS_VALID)) {
				dev_err(priv->dev,
					"EPON: LLID%d ACK without a valid LLID\n", idx);
				priv->llid[idx].state = AIROHA_EPON_LLID_REGISTERING;
				break;
			}

			priv->llid[idx].value = sts & LLID_STS_VALUE_MASK;
			priv->llid[idx].state = AIROHA_EPON_LLID_REGISTER_PENDING;

			ether_addr_copy(mac, priv->gdm_dev->dev_addr);
			mac_low = ((u32)mac[2] << 24) | ((u32)mac[3] << 16) |
				  ((u32)mac[4] << 8) | mac[5];
			mac_low += idx;
			mac[3] = (mac_low >> 16) & 0xff;
			mac[4] = (mac_low >> 8) & 0xff;
			mac[5] = mac_low & 0xff;
			epon_program_mac_address(priv, idx, mac);

			epon_discv_cmd(priv,
					DSCVRY_MPCP_ACK | DSCVRY_RGSTR_ACK_FLG,
					idx);
			priv->llid[idx].state = AIROHA_EPON_LLID_REGISTER_ACK;
			break;

		case MPCP_REG_NACK:
			dev_info(priv->dev, "EPON: LLID%d NACK, retrying\n", idx);
			epon_llid_drop(priv, idx);
			priv->llid[idx].state = AIROHA_EPON_LLID_REGISTERING;
			epon_llid_set_registering(priv, idx);
			if (!priv->registered_llids)
				airoha_xpon_update_netdev_link(priv, false);
			epon_report_registration(priv);
			break;

		case MPCP_REG_DE_REGISTER:
			dev_info(priv->dev, "EPON: LLID%d deregistered\n", idx);
			epon_llid_drop(priv, idx);
			priv->llid[idx].state = AIROHA_EPON_LLID_REGISTERING;
			epon_llid_set_registering(priv, idx);
			if (!priv->registered_llids)
				airoha_xpon_update_netdev_link(priv, false);
			epon_report_registration(priv);
			break;

		case MPCP_REG_RE_REGISTER:
			if (priv->llid[idx].state != AIROHA_EPON_LLID_REGISTERED)
				break;
			dev_info(priv->dev, "EPON: LLID%d re-register\n", idx);
			epon_llid_drop(priv, idx);
			priv->llid[idx].state = AIROHA_EPON_LLID_REGISTER_PENDING;
			epon_discv_cmd(priv,
					DSCVRY_MPCP_ACK | DSCVRY_RGSTR_ACK_FLG,
					idx);
			priv->llid[idx].state = AIROHA_EPON_LLID_REGISTER_ACK;
			if (!priv->registered_llids)
				airoha_xpon_update_netdev_link(priv, false);
			epon_report_registration(priv);
			break;
		}
	}

	/*
	 * REGISTER_ACK completion is the point at which the LLID becomes
	 * operational. This follows the vendor interrupt-mode MPCP FSM.
	 */
	if (status & EPON_INT_REG_ACK_DONE) {
		u32 sts;

		idx = epon_read(priv, EPON_LLID_DSCVRY_CTRL) &
			DSCVRY_TX_MPCP_LLID_MASK;
		if (idx < EPON_MAX_LLID &&
		    priv->llid[idx].state == AIROHA_EPON_LLID_REGISTER_ACK) {
			sts = epon_llid_sts(priv, idx);
			sts &= 0x3fffffff;
			sts |= 2U << LLID_STS_DSCVRY_SHIFT;
			epon_llid_sts_write(priv, idx, sts);

			priv->llid[idx].state = AIROHA_EPON_LLID_REGISTERED;
			if (!priv->llid[idx].valid) {
				priv->llid[idx].valid = true;
				priv->registered_llids++;
			}
			if (priv->oam)
				xpon_oam_llid_registered(priv->oam, idx,
						 priv->llid[idx].value);
			dev_info(priv->dev,
				 "EPON LLID%d registered: 0x%04x\n",
				 idx, priv->llid[idx].value);
			airoha_xpon_update_netdev_link(priv, true);
			epon_report_registration(priv);
		}
	}

	if (status & EPON_INT_RPT_OVRFLW) {
		u32 tmout = epon_read(priv, EPON_RPT_MPCP_TIMEOUT);

		epon_write(priv, EPON_RPT_MPCP_TIMEOUT, tmout & 0x000000ff);
	}

	return IRQ_HANDLED;
}

/* --- Enable / Disable --- */

static int epon_enable(struct xpon_priv *priv)
{
	int idx, ret;
	u32 int_en;

	if (READ_ONCE(priv->mac_enabled))
		return 0;

	dev_info(priv->dev, "starting EPON MAC\n");
	ret = airoha_xpon_reset_mac(priv);
	if (ret)
		goto err_disable_frontend;

	ret = airoha_xpon_phy_start(priv->dev, priv->phy,
				     AIROHA_XPON_MODE_EPON,
				     &priv->phy_initialized,
				     &priv->phy_powered);
	if (ret)
		goto err_disable_frontend;

	ret = epon_prepare_hardware(priv);
	if (ret)
		goto err_stop_phy;

	epon_hw_init(priv);

	/* Put all LLIDs into REGISTERING state */
	for (idx = 0; idx < EPON_MAX_LLID; idx++) {
		priv->llid[idx].state = AIROHA_EPON_LLID_REGISTERING;
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

	epon_write(priv, EPON_INT_EN, 0);
	epon_write(priv, EPON_INT_STATUS, ~0U);

	ret = airoha_xpon_set_fe_datapath(priv->dev, priv->gdm_dev,
					  AIROHA_XPON_MODE_EPON, true);
	if (ret) {
		epon_write(priv, EPON_INT_EN, 0);
		goto err_stop_phy;
	}

	ret = airoha_xpon_tx_enable(priv->dev, priv->frontend, true);
	if (ret)
		goto err_disable_datapath;

	/* Release EPON TX/RX MBI only after GDM2/CDM2 channels are ready. */
	epon_write(priv, EPON_GLB_CFG,
		   epon_read(priv, EPON_GLB_CFG) &
		   ~(GLB_CFG_TXMBI_STOP | GLB_CFG_RXMBI_STOP));
	usleep_range(50, 100);

	WRITE_ONCE(priv->mac_enabled, true);
	WRITE_ONCE(priv->phy_link_known, false);
	mod_delayed_work(priv->fsm_wq, &priv->phy_link_work, 0);
	epon_write(priv, EPON_INT_STATUS, ~0U);
	epon_write(priv, EPON_INT_EN, int_en);
	dev_info(priv->dev, "EPON interrupt mask=%#08x irq=%d\n",
		 int_en, priv->irq);
	dev_info(priv->dev,
		 "EPON MAC started: glb_cfg=%#08x int_enable=%#08x\n",
		 epon_read(priv, EPON_GLB_CFG), epon_read(priv, EPON_INT_EN));

	return 0;

err_disable_datapath:
	airoha_xpon_tx_enable(priv->dev, priv->frontend, false);
	airoha_xpon_set_fe_datapath(priv->dev, priv->gdm_dev,
				    AIROHA_XPON_MODE_EPON, false);
	goto err_stop_phy_only;
err_stop_phy:
	airoha_xpon_tx_enable(priv->dev, priv->frontend, false);
err_stop_phy_only:
	airoha_xpon_phy_stop(priv->dev, priv->phy,
			     AIROHA_XPON_MODE_EPON,
			     &priv->phy_initialized,
			     &priv->phy_powered);
	return ret;
err_disable_frontend:
	airoha_xpon_tx_enable(priv->dev, priv->frontend, false);
	return ret;
}

static void epon_disable(struct xpon_priv *priv)
{
	int idx, ret;

	airoha_xpon_tx_enable(priv->dev, priv->frontend, false);

	if (!READ_ONCE(priv->mac_enabled))
		return;

	dev_info(priv->dev,
		 "stopping EPON MAC: registered_llids=%d glb_cfg=%#08x\n",
		 priv->registered_llids, epon_read(priv, EPON_GLB_CFG));
	WRITE_ONCE(priv->mac_enabled, false);
	cancel_delayed_work_sync(&priv->phy_link_work);
	WRITE_ONCE(priv->phy_link_known, false);
	WRITE_ONCE(priv->phy_link_up, false);
	epon_write(priv, EPON_INT_EN, 0);
	epon_write(priv, EPON_INT_STATUS, ~0U);
	synchronize_irq(priv->irq);
	epon_write(priv, EPON_GLB_CFG,
		   epon_read(priv, EPON_GLB_CFG) |
		   GLB_CFG_TXMBI_STOP | GLB_CFG_RXMBI_STOP);
	ret = airoha_xpon_set_fe_datapath(priv->dev, priv->gdm_dev,
					  AIROHA_XPON_MODE_EPON, false);
	if (ret)
		dev_warn(priv->dev, "failed to disable EPON datapath: %d\n", ret);

	airoha_xpon_phy_stop(priv->dev, priv->phy,
			      AIROHA_XPON_MODE_EPON,
			      &priv->phy_initialized,
			      &priv->phy_powered);

	for (idx = 0; idx < EPON_MAX_LLID; idx++) {
		priv->llid[idx].state = AIROHA_EPON_LLID_WAIT;
		priv->llid[idx].valid = false;
	}
	priv->registered_llids = 0;
	airoha_xpon_update_netdev_link(priv, false);
	dev_info(priv->dev, "EPON MAC stopped: glb_cfg=%#08x\n",
		 epon_read(priv, EPON_GLB_CFG));
}

/* ---------- SFP upstream ops ---------- */

static void epon_sfp_attach(void *upstream, struct sfp_bus *bus)
{
	struct xpon_priv *priv = upstream;

	dev_info(priv->dev, "EPON SFP bus attached\n");
}

static void epon_sfp_detach(void *upstream, struct sfp_bus *bus)
{
	struct xpon_priv *priv = upstream;

	dev_info(priv->dev, "EPON SFP bus detached\n");
}

static int epon_sfp_module_insert(void *upstream,
				  const struct sfp_eeprom_id *id)
{
	struct xpon_priv *priv = upstream;

	dev_info(priv->dev, "EPON SFP module inserted\n");
	return 0;
}

static void epon_sfp_module_remove(void *upstream)
{
	struct xpon_priv *priv = upstream;

	dev_info(priv->dev, "EPON SFP module removed\n");
	WRITE_ONCE(priv->optical_active, false);
	epon_disable(priv);
}

static int epon_sfp_module_start(void *upstream)
{
	struct xpon_priv *priv = upstream;
	int ret;

	dev_info(priv->dev, "EPON SFP module start\n");

	ret = airoha_xpon_tx_rearm(priv->dev, priv->frontend);
	if (ret)
		return ret;

	ret = epon_enable(priv);
	if (!ret)
		WRITE_ONCE(priv->optical_active, true);

	return ret;
}

static void epon_sfp_module_stop(void *upstream)
{
	struct xpon_priv *priv = upstream;

	dev_info(priv->dev, "EPON SFP module stop\n");
	WRITE_ONCE(priv->optical_active, false);
	epon_disable(priv);
}

static void epon_sfp_link_down(void *upstream)
{
	struct xpon_priv *priv = upstream;

	dev_warn(priv->dev, "EPON optical link down / LOS\n");
	airoha_xpon_update_netdev_link(priv, false);
}

static void epon_sfp_link_up(void *upstream)
{
	struct xpon_priv *priv = upstream;

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
	struct xpon_priv *priv = data;
	int ret = 0;

	if (READ_ONCE(priv->started))
		return 0;
	WRITE_ONCE(priv->started, true);
	airoha_xpon_update_netdev_link(priv, false);
	if (priv->sfp_bus) {
		if (!READ_ONCE(priv->optical_active))
			sfp_upstream_start(priv->sfp_bus);
	} else {
		ret = airoha_xpon_tx_rearm(priv->dev, priv->frontend);
		if (!ret)
			ret = epon_enable(priv);
		if (!ret)
			WRITE_ONCE(priv->optical_active, true);
	}

	if (ret)
		WRITE_ONCE(priv->started, false);

	return ret;
}

static void epon_link_stop(void *data)
{
	struct xpon_priv *priv = data;

	if (!READ_ONCE(priv->started))
		return;
	WRITE_ONCE(priv->started, false);
	airoha_xpon_update_netdev_link(priv, false);
	/* Keep optical activation alive while userspace cycles pon0. */
}

static void epon_mac_irq(void *data)
{
	epon_isr(0, data);
}

static const struct airoha_xpon_link_ops epon_link_ops = {
	.start = epon_link_start,
	.stop = epon_link_stop,
	.mac_irq = epon_mac_irq,
};

/* -------------------------------------------------------------------------
 * Unified platform driver
 * ------------------------------------------------------------------------- */

static const struct airoha_xpon_match_data en7523_xpon_data = {
	.mode = AIROHA_XPON_MODE_GPON,
	.mode_from_dt = true,
	.wan_mode_mask = EN7523_SCU_WAN_MODE_MASK,
	.gpon_fine_delay = DBG_DLY_FINE_INT_DEFAULT,
	.gpon_rsp_time_activation = GPON_RSP_TIME_ACT_EN7523,
	.en7523_gpon_defaults = true,
	.gpon_reset_on_start = true,
};

static const struct airoha_xpon_match_data en7523_gpon_data = {
	.mode = AIROHA_XPON_MODE_GPON,
	.wan_mode_mask = EN7523_SCU_WAN_MODE_MASK,
	.gpon_fine_delay = DBG_DLY_FINE_INT_DEFAULT,
	.gpon_rsp_time_activation = GPON_RSP_TIME_ACT_EN7523,
	.en7523_gpon_defaults = true,
	.gpon_reset_on_start = true,
};

static const struct airoha_xpon_match_data en7523_epon_data = {
	.mode = AIROHA_XPON_MODE_EPON,
	.wan_mode_mask = EN7523_SCU_WAN_MODE_MASK,
	.gpon_fine_delay = DBG_DLY_FINE_INT_DEFAULT,
	.gpon_rsp_time_activation = GPON_RSP_TIME_ACT_EN7523,
	.en7523_gpon_defaults = true,
};

static const struct airoha_xpon_match_data en7528_xpon_data = {
	.mode = AIROHA_XPON_MODE_GPON,
	.mode_from_dt = true,
	.wan_mode_mask = EN7528_SCU_WAN_MODE_MASK,
	.gpon_fine_delay = 0x1c,
	.gpon_rsp_time_activation = GPON_RSP_TIME_ACT_EN7528,
	.gpon_reset_on_start = true,
};

static const struct airoha_xpon_match_data en751221_xpon_data = {
	.mode = AIROHA_XPON_MODE_GPON,
	.mode_from_dt = true,
	.wan_mode_mask = EN751221_SCU_WAN_MODE_MASK,
	.gpon_fine_delay = 0x1c,
	.gpon_rsp_time_activation = GPON_RSP_TIME_ACT_EN751221,
	.mac_irq_via_eth = true,
	.prepare_before_mmio = true,
};

static const struct airoha_xpon_match_data en751221_gpon_data = {
	.mode = AIROHA_XPON_MODE_GPON,
	.wan_mode_mask = EN751221_SCU_WAN_MODE_MASK,
	.gpon_fine_delay = 0x1c,
	.gpon_rsp_time_activation = GPON_RSP_TIME_ACT_EN751221,
	.mac_irq_via_eth = true,
	.prepare_before_mmio = true,
};

static const struct airoha_xpon_match_data en751221_epon_data = {
	.mode = AIROHA_XPON_MODE_EPON,
	.wan_mode_mask = EN751221_SCU_WAN_MODE_MASK,
	.gpon_fine_delay = 0x1c,
	.gpon_rsp_time_activation = GPON_RSP_TIME_ACT_EN751221,
	.mac_irq_via_eth = true,
	.prepare_before_mmio = true,
};

static bool airoha_xpon_is_gpon(struct xpon_priv *priv)
{
	return priv->mode == AIROHA_XPON_MODE_GPON;
}

static const struct sfp_upstream_ops *
airoha_xpon_get_sfp_ops(struct xpon_priv *priv)
{
	if (airoha_xpon_is_gpon(priv))
		return &gpon_sfp_ops;

	return &epon_sfp_ops;
}

static const struct airoha_xpon_link_ops *
airoha_xpon_get_link_ops(struct xpon_priv *priv)
{
	if (airoha_xpon_is_gpon(priv))
		return &gpon_link_ops;

	return &epon_link_ops;
}

static int airoha_xpon_request_mac_irq(struct platform_device *pdev,
				       struct xpon_priv *priv,
				       irq_handler_t handler)
{
	struct device *dev = &pdev->dev;
	int ret;

	if (priv->match_data->mac_irq_via_eth) {
		priv->irq = -1;
		dev_info(dev,
			 "%s MAC interrupt is routed through the EN751221 QDMA1 aggregator\n",
			 airoha_xpon_mode_name(priv->mode));
		return 0;
	}

	priv->irq = platform_get_irq_byname(pdev, "mac");
	if (priv->irq < 0)
		priv->irq = platform_get_irq(pdev, 0);
	if (priv->irq < 0)
		return dev_err_probe(dev, priv->irq, "needs mac irq\n");

	ret = devm_request_irq(dev, priv->irq, handler, 0, dev_name(dev), priv);
	if (ret)
		return ret;

	dev_info(dev, "%s IRQ %d requested; MAC interrupt mask controls delivery\n",
		 airoha_xpon_mode_name(priv->mode), priv->irq);

	return 0;
}

static int
airoha_xpon_request_dying_gasp_irq(struct platform_device *pdev,
				   struct xpon_priv *priv)
{
	struct device *dev = &pdev->dev;
	int ret;

	priv->dying_gasp_irq =
		platform_get_irq_byname_optional(pdev, "dying-gasp");
	if (priv->dying_gasp_irq == -ENXIO) {
		priv->dying_gasp_irq = -1;
		dev_info(dev, "no dedicated dying-gasp IRQ, using GPON MAC event\n");
		return 0;
	}
	if (priv->dying_gasp_irq < 0)
		return dev_err_probe(dev, priv->dying_gasp_irq,
				     "failed to get dying-gasp IRQ\n");

	ret = devm_request_irq(dev, priv->dying_gasp_irq,
			       gpon_dying_gasp_isr, 0,
			       "airoha-xpon-dying-gasp", priv);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to request dying-gasp IRQ\n");

	dev_info(dev, "dedicated dying-gasp IRQ %d requested\n",
		 priv->dying_gasp_irq);
	return 0;
}

static int airoha_xpon_init_gpon(struct platform_device *pdev,
				 struct xpon_priv *priv)
{
	struct device *dev = &pdev->dev;
	int ret;

	if (!priv->gpon_reg)
		return dev_err_probe(dev, -EINVAL,
				     "missing GPON register window\n");

	/* The FSM synchronously drains OMCI work queued on system_wq during
	 * session teardown. It is not part of the memory-reclaim path, so do
	 * not mark it WQ_MEM_RECLAIM: a reclaim worker must not flush a
	 * non-reclaim workqueue.
	 */
	priv->fsm_wq = alloc_ordered_workqueue("%s-gpon-fsm", 0,
					       dev_name(dev));
	if (!priv->fsm_wq)
		return -ENOMEM;

	mutex_init(&priv->tcont_lock);
	mutex_init(&priv->omci_profile_lock);
	mutex_init(&priv->omci_config_lock);
	mutex_init(&priv->link_state_lock);
	bitmap_zero(priv->service_gems, GPON_MAX_GEM_ID);
	memset(priv->tcont_alloc_id, 0xff, sizeof(priv->tcont_alloc_id));
	memset(priv->tcont_entity_id, 0xff, sizeof(priv->tcont_entity_id));

	INIT_WORK(&priv->irq_work, gpon_irq_work_fn);
	INIT_DELAYED_WORK(&priv->to1_work, gpon_to1_work_fn);
	INIT_DELAYED_WORK(&priv->to2_work, gpon_to2_work_fn);
	INIT_DELAYED_WORK(&priv->restart_work, gpon_restart_work_fn);
	atomic_set(&priv->pending_irqs, 0);

	/* Keep the Linux IRQ line enabled and quiesce the MAC at its source. */
	gpon_write(priv, GPON_INT_ENABLE, 0);
	gpon_write(priv, GPON_INT_STATUS, ~0U);
	ret = airoha_xpon_request_mac_irq(pdev, priv, gpon_isr);
	if (ret)
		goto err_destroy_fsm_wq;

	ret = airoha_xpon_request_dying_gasp_irq(pdev, priv);
	if (ret)
		goto err_destroy_fsm_wq;

	ret = gpon_load_credentials(priv);
	if (ret)
		goto err_destroy_fsm_wq;

	priv->ber_interval_ms = 1000;
	timer_setup(&priv->ber_timer, gpon_ber_timer_fn, 0);

	priv->ploam = ploam_alloc(&gpon_ploam_ops, priv, priv->hw_sn,
				  priv->hw_passwd);
	if (!priv->ploam) {
		ret = -ENOMEM;
		goto err_destroy_fsm_wq;
	}

	return 0;

err_destroy_fsm_wq:
	destroy_workqueue(priv->fsm_wq);
	priv->fsm_wq = NULL;
	return ret;
}

static void airoha_xpon_cleanup_gpon(struct xpon_priv *priv)
{
	ploam_free(priv->ploam);
	priv->ploam = NULL;

	if (priv->fsm_wq) {
		destroy_workqueue(priv->fsm_wq);
		priv->fsm_wq = NULL;
	}
}

static int airoha_xpon_init_epon(struct platform_device *pdev,
				 struct xpon_priv *priv)
{
	int ret;

	if (!priv->epon_reg)
		return dev_err_probe(&pdev->dev, -EINVAL,
				     "missing EPON register window\n");

	priv->fsm_wq = alloc_ordered_workqueue("airoha-epon", WQ_MEM_RECLAIM);
	if (!priv->fsm_wq)
		return -ENOMEM;

	/* Keep the Linux IRQ line enabled and quiesce the MAC at its source. */
	epon_write(priv, EPON_INT_EN, 0);
	epon_write(priv, EPON_INT_STATUS, ~0U);
	ret = airoha_xpon_request_mac_irq(pdev, priv, epon_isr);
	if (ret) {
		destroy_workqueue(priv->fsm_wq);
		priv->fsm_wq = NULL;
	}

	return ret;
}

static void airoha_xpon_cleanup_epon(struct xpon_priv *priv)
{
	if (!priv->fsm_wq)
		return;
	destroy_workqueue(priv->fsm_wq);
	priv->fsm_wq = NULL;
}

static int airoha_xpon_register_gpon_omci(struct xpon_priv *priv)
{
	int ret;

	priv->omci_handler.rx = airoha_gpon_omci_receive;
	priv->omci_handler.priv = &priv->omci;
	ret = airoha_eth_register_xpon_oam(priv->gdm_dev,
					   &priv->omci_handler);
	if (ret)
		return ret;

	ret = airoha_gpon_omci_register(&priv->omci, priv->xpon, priv->gdm_dev,
					priv, &priv->identity);
	if (ret)
		goto err_unregister_oam;

	/*
	 * The OMCI device is zero-initialized. Publish O1 before the first
	 * PLOAM state callback so userspace never observes an invalid O0.
	 */
	airoha_gpon_omci_set_state(&priv->omci, GPON_O1_INITIAL);
	ret = airoha_gpon_omci_start(&priv->omci);
	if (ret)
		goto err_unregister_omci;

	return 0;

err_unregister_omci:
	airoha_gpon_omci_stop(&priv->omci);
err_unregister_oam:
	airoha_eth_unregister_xpon_oam(priv->gdm_dev, &priv->omci_handler);
	airoha_gpon_omci_unregister(&priv->omci);
	return ret;
}

static void airoha_xpon_unregister_gpon_omci(struct xpon_priv *priv)
{
	/* Drain the transport before detaching its RX handler. */
	airoha_gpon_omci_stop(&priv->omci);
	airoha_eth_unregister_xpon_oam(priv->gdm_dev, &priv->omci_handler);
	airoha_gpon_omci_unregister(&priv->omci);
}

static int airoha_xpon_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	const struct airoha_xpon_match_data *data;
	const struct airoha_xpon_link_ops *link_ops;
	const struct sfp_upstream_ops *sfp_ops;
	struct device_node *eth_node;
	struct xpon_device_desc xpon_desc = {};
	struct xpon_priv *priv;
	struct resource *res;
	int ret;

	data = device_get_match_data(&pdev->dev);
	if (!data)
		return -EINVAL;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;
	priv->dev = dev;
	priv->match_data = data;
	ret = airoha_xpon_get_mode(dev, data, &priv->mode);
	if (ret)
		return ret;
	INIT_DELAYED_WORK(&priv->phy_link_work,
			  airoha_xpon_phy_link_work_fn);

	dev_info(dev, "xPON initial mode: %s\n",
		 airoha_xpon_mode_name(priv->mode));

	priv->scu = syscon_regmap_lookup_by_phandle(dev->of_node,
					    "airoha,scu");
	if (IS_ERR(priv->scu)) {
		ret = dev_err_probe(dev, PTR_ERR(priv->scu),
				    "failed to get SCU regmap\n");
		goto err_put_gdm;
	}

	priv->mac_reset = devm_reset_control_get_optional_exclusive(dev, "mac");
	if (IS_ERR(priv->mac_reset)) {
		ret = dev_err_probe(dev, PTR_ERR(priv->mac_reset),
				    "failed to get xPON MAC reset\n");
		goto err_put_gdm;
	}
	if (!priv->mac_reset)
		dev_warn(dev,
			 "missing xPON MAC reset; session restarts cannot clear all hardware state\n");

	priv->phy = devm_phy_get(dev, "xpon");
	if (IS_ERR(priv->phy)) {
		ret = dev_err_probe(dev, PTR_ERR(priv->phy),
				    "failed to get digital xPON PHY\n");
		goto err_put_gdm;
	}

	eth_node = of_parse_phandle(dev->of_node, "ethernet", 0);
	if (eth_node) {
		priv->gdm_dev = of_find_net_device_by_node(eth_node);
		dev_info(dev, "%s datapath phandle: %pOF\n",
			 airoha_xpon_mode_name(priv->mode), eth_node);
		of_node_put(eth_node);
	} else {
		priv->gdm_dev = airoha_eth_get_xpon_netdev();
		dev_info(dev, "%s datapath discovered automatically\n",
			 airoha_xpon_mode_name(priv->mode));
	}
	if (!priv->gdm_dev) {
		ret = dev_err_probe(dev, -EPROBE_DEFER,
				    "GDM2 netdev is not registered yet\n");
		goto err_put_gdm;
	}
	dev_info(dev, "resolved xPON datapath to %s\n",
		 priv->gdm_dev->name);

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "mac");
	if (!res)
		res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res) {
		ret = dev_err_probe(dev, -EINVAL, "missing mac resource\n");
		goto err_put_gdm;
	}

	priv->base = devm_ioremap_resource(dev, res);
	if (IS_ERR(priv->base)) {
		ret = dev_err_probe(dev, PTR_ERR(priv->base),
				    "needs xPON mac base\n");
		goto err_put_gdm;
	}

	/*
	 * All supported xPON MAC blocks expose GPON and EPON at fixed offsets.
	 * Add a larger resource-size case here when XGSPON support lands.
	 */
	if (resource_size(res) < V1_XPON_REGION_SIZE) {
		ret = dev_err_probe(dev, -EINVAL,
				    "unsupported xPON MAC resource size %#llx: %pR\n",
				    (unsigned long long)resource_size(res),
				    res);
		goto err_put_gdm;
	}

	/*
	 * The EN751221 xPON_1g SDK selects the shared WAN mux before the
	 * first GPON/EPON MAC register initialization.  Keep that ordering:
	 * RST_CTRL1[31] is a runtime MAC reset, not a prerequisite for mapping
	 * or quiescing the interrupt registers during probe.
	 */
	if (data->prepare_before_mmio) {
		dev_info(dev, "selecting %s WAN mode before first MAC access\n",
			 airoha_xpon_mode_name(priv->mode));
		ret = airoha_xpon_select_wan(priv->scu, data, priv->mode);
		if (ret) {
			ret = dev_err_probe(dev, ret,
					    "failed to prepare EN751221 xPON WAN mux\n");
			goto err_put_gdm;
		}

		dev_info(dev, "%s WAN mode selected before first MAC access\n",
			 airoha_xpon_mode_name(priv->mode));
	}

	priv->gpon_reg = priv->base + GPON_REG_OFFSET;
	priv->epon_reg = priv->base + EPON_REG_OFFSET;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM,
					   "epon-reset");
	if (res) {
		priv->epon_reset_reg = devm_ioremap_resource(dev, res);
		if (IS_ERR(priv->epon_reset_reg)) {
			ret = dev_err_probe(dev, PTR_ERR(priv->epon_reset_reg),
					    "failed to map EPON reset register\n");
			goto err_put_gdm;
		}
	}

	if (airoha_xpon_is_gpon(priv))
		ret = airoha_xpon_init_gpon(pdev, priv);
	else
		ret = airoha_xpon_init_epon(pdev, priv);
	if (ret)
		goto err_put_gdm;

	priv->frontend = devm_optical_frontend_get_optional(dev, "pon");
	if (IS_ERR(priv->frontend)) {
		ret = dev_err_probe(dev, PTR_ERR(priv->frontend),
				    "failed to get optical frontend\n");
		priv->frontend = NULL;
		goto err_cleanup_mode;
	}

	ret = airoha_xpon_frontend_set_mode(priv);
	if (ret) {
		ret = dev_err_probe(dev, ret,
				    "failed to configure optical frontend mode\n");
		goto err_cleanup_mode;
	}

	xpon_desc.netdev = priv->gdm_dev;
	xpon_desc.optical = priv->frontend ?
		optical_frontend_get_device(priv->frontend) : NULL;
	xpon_desc.modes = XPON_MODE_CAP(XPON_MODE_GPON) |
			  XPON_MODE_CAP(XPON_MODE_EPON);
	xpon_desc.mode = airoha_xpon_core_mode(priv->mode);
	xpon_desc.priv = priv;
	priv->xpon = xpon_device_register(dev, &xpon_desc);
	if (IS_ERR(priv->xpon)) {
		ret = dev_err_probe(dev, PTR_ERR(priv->xpon),
				    "failed to register generic xPON device\n");
		priv->xpon = NULL;
		goto err_cleanup_mode;
	}

	priv->sfp_bus = sfp_bus_find_fwnode(dev->fwnode);
	if (IS_ERR(priv->sfp_bus)) {
		ret = PTR_ERR(priv->sfp_bus);
		dev_err(dev, "failed to find SFP bus: %d\n", ret);
		priv->sfp_bus = NULL;
		goto err_unregister_core;
	}
	if (!priv->sfp_bus && !priv->frontend) {
		ret = -ENODEV;
		dev_err(dev, "missing SFP or optical frontend reference\n");
		goto err_unregister_core;
	}

	if (priv->sfp_bus) {
		sfp_ops = airoha_xpon_get_sfp_ops(priv);
		ret = sfp_bus_add_upstream(priv->sfp_bus, priv, sfp_ops);
		if (ret)
			goto err_put_sfp;
	}

	link_ops = airoha_xpon_get_link_ops(priv);
	ret = airoha_eth_register_xpon(priv->gdm_dev, priv->mode, link_ops,
				       priv);
	if (ret)
		goto err_del_upstream;

	airoha_xpon_update_netdev_link(priv, false);

	if (airoha_xpon_is_gpon(priv)) {
		ret = airoha_xpon_register_gpon_omci(priv);
		if (ret)
			goto err_unregister_xpon;
	}

	platform_set_drvdata(pdev, priv);
	if (airoha_xpon_is_gpon(priv))
		dev_info(dev,
			 "GPON probe complete: datapath=%s omci-genl=%u irq=%d dying-gasp-irq=%d mac-reset=%u default_state=%s\n",
			 priv->gdm_dev->name, omci_device_id(priv->omci.odev),
			 priv->irq, priv->dying_gasp_irq, !!priv->mac_reset,
			 gpon_state_name(ploam_get_state(priv->ploam)));
	else
		dev_info(dev, "EPON probe complete: datapath=%s irq=%d\n",
			 priv->gdm_dev->name, priv->irq);
	return 0;

err_unregister_xpon:
	airoha_eth_unregister_xpon(priv->gdm_dev, link_ops, priv);
err_del_upstream:
	if (priv->sfp_bus)
		sfp_bus_del_upstream(priv->sfp_bus);
err_put_sfp:
	if (priv->sfp_bus)
		sfp_bus_put(priv->sfp_bus);
err_unregister_core:
	xpon_device_unregister(priv->xpon);
	priv->xpon = NULL;
err_cleanup_mode:
	if (airoha_xpon_is_gpon(priv))
		airoha_xpon_cleanup_gpon(priv);
	else
		airoha_xpon_cleanup_epon(priv);
err_put_gdm:
	if (priv->gdm_dev) {
		dev_put(priv->gdm_dev);
		priv->gdm_dev = NULL;
	}
	return ret;
}

static void airoha_xpon_remove(struct platform_device *pdev)
{
	const struct airoha_xpon_link_ops *link_ops;
	struct xpon_priv *priv;

	priv = platform_get_drvdata(pdev);
	if (!priv)
		return;

	dev_info(priv->dev, "removing xPON mode %s\n",
		 airoha_xpon_mode_name(priv->mode));

	if (airoha_xpon_is_gpon(priv))
		cancel_delayed_work_sync(&priv->restart_work);

	/* net/omci owns and stops the GPON transport before it is detached. */
	if (airoha_xpon_is_gpon(priv))
		airoha_xpon_unregister_gpon_omci(priv);

	link_ops = airoha_xpon_get_link_ops(priv);
	airoha_eth_unregister_xpon(priv->gdm_dev, link_ops, priv);

	if (!airoha_xpon_is_gpon(priv) && priv->sfp_bus &&
	    READ_ONCE(priv->optical_active))
		sfp_upstream_stop(priv->sfp_bus);

	if (!airoha_xpon_is_gpon(priv)) {
		epon_disable(priv);
	} else if (airoha_xpon_is_gpon(priv) &&
		   (READ_ONCE(priv->mac_enabled) || priv->phy_powered)) {
		/* Defensive fallback for a provider that failed to stop cleanly. */
		gpon_disable(priv);
	} else if (airoha_xpon_is_gpon(priv)) {
		airoha_xpon_tx_enable(priv->dev, priv->frontend, false);
		cancel_work_sync(&priv->irq_work);
		cancel_delayed_work_sync(&priv->to1_work);
		cancel_delayed_work_sync(&priv->to2_work);
	}

	xpon_device_unregister(priv->xpon);
	priv->xpon = NULL;

	if (priv->sfp_bus) {
		sfp_bus_del_upstream(priv->sfp_bus);
		sfp_bus_put(priv->sfp_bus);
	}
	if (airoha_xpon_is_gpon(priv))
		airoha_xpon_cleanup_gpon(priv);
	else
		airoha_xpon_cleanup_epon(priv);
	dev_put(priv->gdm_dev);
	priv->gdm_dev = NULL;
}

static const struct of_device_id airoha_xpon_of_match[] = {
	{ .compatible = "airoha,en7523-xpon", .data = &en7523_xpon_data },
	{ .compatible = "airoha,en7528-xpon", .data = &en7528_xpon_data },
	{ .compatible = "econet,en751221-xpon", .data = &en751221_xpon_data },
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

MODULE_DESCRIPTION("Airoha PON MAC driver");
MODULE_AUTHOR("Matheus Sampaio Queiroga <srherobrine20@gmail.com>");
MODULE_AUTHOR("Benjamin Larsson <benjamin.larsson@genexis.eu>");
MODULE_LICENSE("GPL");
