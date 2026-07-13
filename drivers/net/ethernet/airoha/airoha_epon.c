// SPDX-License-Identifier: GPL-2.0-only
/*
 * EcoNet EN7521/EN7526 EPON MAC driver
 *
 * Implements IEEE 802.3 clause 64 EPON ONU MAC layer for EcoNet SoCs.
 * Supports MPCP-based LLID registration, AES-128 security, and dying gasp.
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/io.h>
#include <linux/interrupt.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/sfp.h>
#include <linux/spinlock.h>
#include <linux/delay.h>

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
#define DSCVRY_MPCP_REG_REQ	(1U << 30)	/* send REGISTER_REQUEST */
#define DSCVRY_MPCP_NORMAL	(2U << 30)
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
#define EPON_RST_LOOP_CNT	80000

#define EPON_MAX_LLID		8

/* LLID registration states */
enum llid_state {
	LLID_STATE_WAIT		= 0,
	LLID_STATE_REGISTERING	= 1,
	LLID_STATE_REGISTERED	= 2,
};

struct epon_llid {
	enum llid_state	state;
	bool		valid;
	u16		value;
};

struct epon_priv {
	void __iomem		*base;
	void __iomem		*rst_base;	/* optional: external SW reset reg */
	struct device		*dev;
	struct net_device	*netdev;
	struct sfp_bus		*sfp_bus;
	spinlock_t		lock;
	int			irq;

	struct epon_llid	llid[EPON_MAX_LLID];
	int			registered_llids;

	u64			rx_packets;
	u64			rx_bytes;
	u64			tx_packets;
	u64			tx_bytes;
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
	volatile u32 cnt;
	u32 raw;

	/* Assert external system-level SW reset if mapped */
	if (priv->rst_base) {
		raw = readl(priv->rst_base);
		writel(raw | EPON_EXT_SW_RST_BIT, priv->rst_base);
		for (cnt = 0; cnt < EPON_RST_LOOP_CNT; cnt++)
			;
		writel(raw & ~EPON_EXT_SW_RST_BIT, priv->rst_base);
	}

	/* Assert EPON MAC internal SW reset (GLB_CFG bit 4) */
	raw = epon_read(priv, EPON_GLB_CFG);
	epon_write(priv, EPON_GLB_CFG, raw | GLB_CFG_EPON_MAC_SW_RST);
	for (cnt = 0; cnt < EPON_RST_LOOP_CNT; cnt++)
		;
	raw &= ~GLB_CFG_EPON_MAC_SW_RST;
	epon_write(priv, EPON_GLB_CFG, raw);
	for (cnt = 0; cnt < EPON_RST_LOOP_CNT; cnt++)
		;

	/* Enable RPT_TXPRI_CTRL and write post-reset timing parameters */
	raw |= GLB_CFG_RPT_TXPRI_CTRL;
	epon_write(priv, EPON_GLB_CFG, raw);

	epon_write(priv, EPON_GRD_THRSHLD,      EPON_TIMEDRIFT_THRSHLD);
	epon_write(priv, EPON_TRX_ADJUST_TIME1, EPON_TRX_ADJUST_TIME1_DEF);
	epon_write(priv, EPON_TRX_ADJUST_TIME2, EPON_TRX_ADJUST_TIME2_DEF);
	epon_write(priv, EPON_TXFETCH_CFG,      EPON_TXFETCH_DEFAULT);
}

/* --- Hardware init --- */

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
}

/* --- ISR --- */

static irqreturn_t epon_isr(int irq, void *data)
{
	struct epon_priv *priv = data;
	u32 status;
	int idx;

	status = epon_read(priv, EPON_INT_STATUS);
	/* W1C: clear all at once */
	epon_write(priv, EPON_INT_STATUS, 0xFFFFFFFF);

	if (!status)
		return IRQ_NONE;

	/* Time drift: read stat, log, reset counter */
	if (status & EPON_INT_TIMEDRFT) {
		u32 drift = epon_read(priv, EPON_TIME_DRFT_STAT) & 0xFF;

		dev_dbg(priv->dev, "EPON: time drift %u\n", drift);
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
			dev_dbg(priv->dev, "EPON: MPCP timeout LLID%d\n", idx);
			if (priv->llid[idx].valid) {
				priv->llid[idx].valid = false;
				if (priv->registered_llids > 0)
					priv->registered_llids--;
			}
			priv->llid[idx].state = LLID_STATE_REGISTERING;
		}
		epon_write(priv, EPON_RPT_MPCP_TIMEOUT, tmout & 0x0000FF00);
		if (!priv->registered_llids)
			netif_carrier_off(priv->netdev);
	}

	/*
	 * Discovery gate: for each LLID in REGISTERING state, prepare HW
	 * state and send REGISTER_REQUEST.  Send one at a time (ref pattern).
	 */
	if (status & EPON_INT_DISCV_GATE) {
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

		switch (flag) {
		case MPCP_REG_ACK:
			if (!(sts & LLID_STS_VALID)) {
				dev_err(priv->dev,
					"EPON: LLID%d ACK but LLID invalid\n", idx);
				break;
			}
			priv->llid[idx].value = sts & LLID_STS_VALUE_MASK;

			/* Assign unique MAC per LLID (add llid_idx to low bytes) */
			ether_addr_copy(mac, priv->netdev->dev_addr);
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
			netif_carrier_on(priv->netdev);
			break;

		case MPCP_REG_NACK:
			dev_dbg(priv->dev, "EPON: LLID%d NACK, retrying\n", idx);
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
				netif_carrier_off(priv->netdev);
			break;

		case MPCP_REG_RE_REGISTER:
			dev_dbg(priv->dev, "EPON: LLID%d re-register\n", idx);
			epon_discv_cmd(priv, DSCVRY_MPCP_ACK | DSCVRY_RGSTR_ACK_FLG, idx);
			break;
		}
	}

	if (status & EPON_INT_REG_REQ_DONE)
		dev_dbg(priv->dev, "EPON: REGISTER_REQUEST sent\n");

	if (status & EPON_INT_REG_ACK_DONE)
		dev_dbg(priv->dev, "EPON: REGISTER_ACK sent\n");

	/* Report over-interval: clear bits[31:24] of e_rpt_mpcp_timeout */
	if (status & EPON_INT_RPT_OVRFLW) {
		u32 tmout = epon_read(priv, EPON_RPT_MPCP_TIMEOUT);

		epon_write(priv, EPON_RPT_MPCP_TIMEOUT, tmout & 0x000000FF);
	}

	return IRQ_HANDLED;
}

/* --- Enable / Disable --- */

static void epon_enable(struct epon_priv *priv)
{
	int idx;
	u32 int_en;

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

	epon_write(priv, EPON_INT_EN, int_en);
	enable_irq(priv->irq);
}

static void epon_disable(struct epon_priv *priv)
{
	int idx;

	disable_irq(priv->irq);
	epon_write(priv, EPON_INT_EN, 0);

	for (idx = 0; idx < EPON_MAX_LLID; idx++) {
		priv->llid[idx].state = LLID_STATE_WAIT;
		priv->llid[idx].valid = false;
	}
	priv->registered_llids = 0;
	netif_carrier_off(priv->netdev);
}

/* ---------- SFP upstream ops ---------- */

static void epon_sfp_attach(void *upstream, struct sfp_bus *bus) {}
static void epon_sfp_detach(void *upstream, struct sfp_bus *bus) {}

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

	epon_enable(priv);
	return 0;
}

static void epon_sfp_module_stop(void *upstream)
{
	epon_disable(upstream);
}

static void epon_sfp_link_down(void *upstream)
{
	struct epon_priv *priv = upstream;

	netif_carrier_off(priv->netdev);
}

static void epon_sfp_link_up(void *upstream) {}

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

/* ---------- net_device ops ---------- */

static int epon_ndo_open(struct net_device *dev)
{
	netif_start_queue(dev);
	return 0;
}

static int epon_ndo_stop(struct net_device *dev)
{
	netif_stop_queue(dev);
	return 0;
}

static netdev_tx_t epon_ndo_start_xmit(struct sk_buff *skb,
					struct net_device *dev)
{
	struct epon_priv *priv = netdev_priv(dev);

	priv->tx_packets++;
	priv->tx_bytes += skb->len;
	dev_kfree_skb_any(skb);
	return NETDEV_TX_OK;
}

static void epon_ndo_get_stats64(struct net_device *dev,
				  struct rtnl_link_stats64 *stats)
{
	struct epon_priv *priv = netdev_priv(dev);

	stats->rx_packets = priv->rx_packets;
	stats->rx_bytes   = priv->rx_bytes;
	stats->tx_packets = priv->tx_packets;
	stats->tx_bytes   = priv->tx_bytes;
}

static const struct net_device_ops epon_netdev_ops = {
	.ndo_open	 = epon_ndo_open,
	.ndo_stop	 = epon_ndo_stop,
	.ndo_start_xmit	 = epon_ndo_start_xmit,
	.ndo_get_stats64 = epon_ndo_get_stats64,
};

/* ---------- Platform driver ---------- */

static int epon_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct net_device *netdev;
	struct epon_priv *priv;
	struct resource *res;
	int ret;

	netdev = alloc_etherdev(sizeof(*priv));
	if (!netdev)
		return -ENOMEM;

	SET_NETDEV_DEV(netdev, dev);
	priv = netdev_priv(netdev);
	priv->dev    = dev;
	priv->netdev = netdev;

	spin_lock_init(&priv->lock);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	priv->base = devm_ioremap_resource(dev, res);
	if (IS_ERR(priv->base)) {
		ret = PTR_ERR(priv->base);
		goto err_free_netdev;
	}

	/* Optional second resource: external SW reset register */
	res = platform_get_resource(pdev, IORESOURCE_MEM, 1);
	if (res) {
		priv->rst_base = devm_ioremap_resource(dev, res);
		if (IS_ERR(priv->rst_base))
			priv->rst_base = NULL;
	}

	priv->irq = platform_get_irq(pdev, 0);
	if (priv->irq < 0) {
		ret = priv->irq;
		goto err_free_netdev;
	}

	ret = devm_request_irq(dev, priv->irq, epon_isr, 0,
			       dev_name(dev), priv);
	if (ret)
		goto err_free_netdev;

	/* IRQ starts disabled; enabled in epon_enable() when SFP is ready */
	disable_irq(priv->irq);

	priv->sfp_bus = sfp_bus_find_fwnode(dev->fwnode);
	if (IS_ERR(priv->sfp_bus)) {
		ret = PTR_ERR(priv->sfp_bus);
		dev_err(dev, "failed to find SFP bus: %d\n", ret);
		goto err_free_netdev;
	}

	ret = sfp_bus_add_upstream(priv->sfp_bus, priv, &epon_sfp_ops);
	if (ret)
		goto err_put_sfp;

	eth_hw_addr_random(netdev);
	netdev->netdev_ops = &epon_netdev_ops;
	strscpy(netdev->name, "pon%d", IFNAMSIZ);

	ret = register_netdev(netdev);
	if (ret) {
		dev_err(dev, "failed to register netdev: %d\n", ret);
		goto err_del_upstream;
	}

	platform_set_drvdata(pdev, priv);
	dev_info(dev, "EcoNet EPON MAC driver probed\n");
	return 0;

err_del_upstream:
	sfp_bus_del_upstream(priv->sfp_bus);
err_put_sfp:
	sfp_bus_put(priv->sfp_bus);
err_free_netdev:
	free_netdev(netdev);
	return ret;
}

static void epon_remove(struct platform_device *pdev)
{
	struct epon_priv *priv = platform_get_drvdata(pdev);

	unregister_netdev(priv->netdev);
	sfp_bus_del_upstream(priv->sfp_bus);
	sfp_bus_put(priv->sfp_bus);
	free_netdev(priv->netdev);
}

static const struct of_device_id epon_of_match[] = {
	{ .compatible = "econet,en7521-epon" },
	{ .compatible = "econet,en7526-epon" },
	{ .compatible = "econet,en751221-epon" },
	{}
};
MODULE_DEVICE_TABLE(of, epon_of_match);

static struct platform_driver epon_driver = {
	.probe  = epon_probe,
	.remove = epon_remove,
	.driver = {
		.name		= "econet-epon",
		.of_match_table	= epon_of_match,
	},
};
module_platform_driver(epon_driver);

MODULE_DESCRIPTION("EcoNet EN7521/EN7526 EPON MAC driver");
MODULE_AUTHOR("Benjamin Larsson <benjamin.larsson@genexis.eu>");
MODULE_LICENSE("GPL");
