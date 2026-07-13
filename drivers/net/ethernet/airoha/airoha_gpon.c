// SPDX-License-Identifier: GPL-2.0-only
/*
 * EcoNet EN7521/EN7526 GPON MAC driver
 *
 * Hardware driver for the GPON MAC on EcoNet SoCs.  All PLOAM protocol
 * logic lives in ploam.c; this file owns register access, interrupt
 * handling, the PON WAN netdev, and the OMCI management netdev.
 *
 * Two network interfaces are created:
 *   pon%d  — PON WAN data path (downstream/upstream GEM traffic)
 *   omci%d — OMCI management channel (raw PDUs to/from userspace daemon)
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/io.h>
#include <linux/interrupt.h>
#include <linux/iopoll.h>
#include <linux/sched.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/sfp.h>
#include <linux/spinlock.h>
#include <linux/timer.h>
#include <linux/random.h>
#include <linux/unaligned.h>
#include "airoha_ploam.h"
#include "airoha_gpon.h"

/* -----------------------------------------------------------------------
 * Register offsets from the GPON register window at physical 0x1fb64000.
 *
 * The vendor driver maps the encompassing region at 0x1fb60000 and the
 * generated register structure reserves the first 0x4000 bytes.  Some DTs
 * instead expose the register window directly at 0x1fb64000.  Probe accepts
 * both layouts and sets priv->regs to the actual GPON register window.
 * -------------------------------------------------------------------- */

#define EN7523_GPON_REGION_BASE	0x1fb60000
#define EN7523_GPON_REG_BASE		0x1fb64000
#define EN7523_GPON_REG_OFFSET		0x00004000

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
/* GEM / MIB table init */
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

/* G_INT_STATUS / G_INT_ENABLE */
#define INT_PLOAMD_RECV		BIT(0)
#define INT_PLOAMU_SEND		BIT(1)
#define INT_SN_REQ_RECV		BIT(2)
#define INT_SN_ONU_SEND_O3	BIT(3)
#define INT_RANGING_REQ_RECV	BIT(4)
#define INT_SN_ONU_SEND_O4	BIT(5)
#define INT_SN_REQ_CRS		BIT(6)
#define INT_LOS_GEM_DEL		BIT(7)
#define INT_AES_KEY_SWITCH_DONE	BIT(8)
#define INT_TOD_UPDATE_DONE	BIT(9)
#define INT_TOD_1PPS		BIT(10)
#define INT_DYING_GASP		BIT(11)
#define INT_TX_LATE_START	BIT(19)
#define INT_RX_EOF_ERR		BIT(20)

/* G_GEM_PORT_CFG */
#define GEM_CMD_WRITE		BIT(31)
#define GEM_ENCRYPT		BIT(17)
#define GEM_VALID		BIT(16)

/* G_GEM_PORT_STS */
#define GEM_CMD_DONE		BIT(31)

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

/* G_DBG_TX_SYNC_OFFSET bits[4:0] = internal byte delay */
#define DBG_TX_SYNC_OFFSET_MASK	0x1F

/* Default ONU response time per G.984.3 (~35 µs at 1.244 Gbps) */
#define GPON_RSP_TIME_DEFAULT	0x058B

/* TO1 timer: 10 seconds in O3/O4 without Ranging_Time → return to O2 */
#define GPON_TO1_MS		10000
/* TO2 timer: 100 ms in O6 without Popup/Swift_Popup → reset to O1 */
#define GPON_TO2_MS		100

#define GPON_MAX_GEM_ID		4096
#define GPON_MAX_TCONT		32

#define GPON_CMD_TIMEOUT_US		10000
#define GPON_TABLE_TIMEOUT_US		100000

/* PHY_TX_EN_BIT_LEN_CONST: fixed guard-bit value written to G_PLOu_GUARD_BIT */
#define GPON_GUARD_BIT_LEN	24

/* -----------------------------------------------------------------------
 * OMCI network device
 * -------------------------------------------------------------------- */

struct omci_priv {
	struct net_device	*netdev;
	struct gpon_priv	*gpon;

	u16			gem_port_id;
	bool			gem_port_valid;

	u64			rx_packets;
	u64			rx_bytes;
	u64			tx_packets;
	u64			tx_bytes;
};

/* -----------------------------------------------------------------------
 * GPON private data
 * -------------------------------------------------------------------- */

struct gpon_priv {
	void __iomem		*base;
	void __iomem		*regs;
	struct device		*dev;
	struct net_device	*netdev;
	struct net_device	*omci_dev;
	struct sfp_bus		*sfp_bus;
	spinlock_t		lock;
	int			irq;

	struct ploam_priv	*ploam;

	u8			sn[8];
	u8			passwd[10];
	u8			aes_key[16];

	/* G.984.3 activation timers */
	struct timer_list	to1_timer;	/* O3/O4: 10 s → O2 */
	struct timer_list	to2_timer;	/* O6: 100 ms → O1 */

	/* BER measurement timer */
	struct timer_list	ber_timer;
	u32			ber_interval_ms;

	/* EqD state for O5 incremental adjustment */
	u32			byte_delay;
	u32			bit_delay;

	u64			rx_packets;
	u64			rx_bytes;
	u64			tx_packets;
	u64			tx_bytes;
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

static inline void gpon_set_bits(struct gpon_priv *priv, u32 reg, u32 bits)
{
	gpon_write(priv, reg, gpon_read(priv, reg) | bits);
}

static inline void gpon_clear_bits(struct gpon_priv *priv, u32 reg, u32 bits)
{
	gpon_write(priv, reg, gpon_read(priv, reg) & ~bits);
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

static int gpon_reset_gem_ports(struct gpon_priv *priv)
{
	int id, ret;

	for (id = 0; id < GPON_MAX_GEM_ID; id++) {
		gpon_write(priv, GPON_GEM_PORT_CFG, GEM_CMD_WRITE | id);
		ret = gpon_wait_bits(priv, GPON_GEM_PORT_STS, GEM_CMD_DONE,
				     GPON_CMD_TIMEOUT_US);
		if (ret) {
			dev_err(priv->dev,
				"GEM reset timed out at port %d: cfg=%#x sts=%#x\n",
				id, gpon_read(priv, GPON_GEM_PORT_CFG),
				gpon_read(priv, GPON_GEM_PORT_STS));
			return ret;
		}

		if (!(id & 0x3f))
			cond_resched();
	}

	return 0;
}

static int gpon_reset_alloc_ids(struct gpon_priv *priv)
{
	int i, ret;

	for (i = 0; i < 8; i++)
		gpon_write(priv, GPON_TCONT_ID_0_1 + i * 4, TCONT_PAIR_INVALID);

	for (i = 16; i < GPON_MAX_TCONT; i++) {
		u32 cfg = TCONT16_CMD_EXEC |
			  ((i - 16) << TCONT16_IDX_SHIFT);

		gpon_write(priv, GPON_TCONT_ID_16_31_CFG, cfg);
		ret = gpon_wait_bits(priv, GPON_TCONT_ID_16_31_STS,
				     TCONT16_CMD_DONE, GPON_CMD_TIMEOUT_US);
		if (ret) {
			dev_err(priv->dev,
				"TCONT reset timed out at index %d: cfg=%#x sts=%#x\n",
				i, gpon_read(priv, GPON_TCONT_ID_16_31_CFG),
				gpon_read(priv, GPON_TCONT_ID_16_31_STS));
			return ret;
		}
	}

	return 0;
}

static int gpon_init_tables(struct gpon_priv *priv)
{
	int ret;

	gpon_write(priv, GPON_GEM_TBL_INIT, GEM_TBL_INIT_START);
	ret = gpon_wait_bits(priv, GPON_GEM_TBL_INIT, GEM_TBL_INIT_DONE,
			     GPON_TABLE_TIMEOUT_US);
	if (ret) {
		dev_err(priv->dev, "GEM table init timed out: reg=%#x\n",
			gpon_read(priv, GPON_GEM_TBL_INIT));
		return ret;
	}

	gpon_write(priv, GPON_MIB_TBL_INIT, MIB_TBL_INIT_START);
	ret = gpon_wait_bits(priv, GPON_MIB_TBL_INIT, MIB_TBL_INIT_DONE,
			     GPON_TABLE_TIMEOUT_US);
	if (ret) {
		dev_err(priv->dev, "MIB table init timed out: reg=%#x\n",
			gpon_read(priv, GPON_MIB_TBL_INIT));
		return ret;
	}

	return 0;
}

static int gpon_dev_init(struct gpon_priv *priv)
{
	int ret;

	dev_info(priv->dev, "GPON init: resetting GEM ports\n");
	ret = gpon_reset_gem_ports(priv);
	if (ret)
		return ret;

	dev_info(priv->dev, "GPON init: resetting T-CONTs\n");
	ret = gpon_reset_alloc_ids(priv);
	if (ret)
		return ret;

	dev_info(priv->dev, "GPON init: initializing GEM/MIB tables\n");
	ret = gpon_init_tables(priv);
	if (ret)
		return ret;

	gpon_write(priv, GPON_ONU_ID, PLOAM_ONU_UNASSIGNED);
	gpon_write(priv, GPON_RSP_TIME, GPON_RSP_TIME_DEFAULT);
	return 0;
}

static void gpon_set_serial_number_regs(struct gpon_priv *priv)
{
	u32 vendor = get_unaligned_be32(priv->sn);
	u32 vs_sn  = get_unaligned_be32(priv->sn + 4);

	gpon_write(priv, GPON_VENDOR_ID, vendor);
	gpon_write(priv, GPON_VS_SN,     vs_sn);
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

static void gpon_set_alloc_id_hw(struct gpon_priv *priv, u16 alloc_id,
				  bool allocate)
{
	if (alloc_id < 16) {
		int reg_idx = alloc_id / 2;
		u32 val = gpon_read(priv, GPON_TCONT_ID_0_1 + reg_idx * 4);

		if (alloc_id & 1) {
			val &= ~((TCONT_ID_MASK << TCONT1_ID_SHIFT) |
				 TCONT1_VALID);
			if (allocate)
				val |= ((alloc_id & TCONT_ID_MASK) <<
					TCONT1_ID_SHIFT) | TCONT1_VALID;
		} else {
			val &= ~(TCONT_ID_MASK | TCONT0_VALID);
			if (allocate)
				val |= (alloc_id & TCONT_ID_MASK) |
				       TCONT0_VALID;
		}
		gpon_write(priv, GPON_TCONT_ID_0_1 + reg_idx * 4, val);
	} else if (alloc_id < GPON_MAX_TCONT) {
		u32 cfg = TCONT16_CMD_EXEC |
			  ((alloc_id - 16) << TCONT16_IDX_SHIFT);

		if (allocate)
			cfg |= TCONT16_VALID |
			       (alloc_id & TCONT16_ALLOC_MASK);

		gpon_write(priv, GPON_TCONT_ID_16_31_CFG, cfg);
		if (gpon_wait_bits(priv, GPON_TCONT_ID_16_31_STS,
				   TCONT16_CMD_DONE, GPON_CMD_TIMEOUT_US))
			dev_err_ratelimited(priv->dev,
				"TCONT command timed out for alloc-id %u\n",
				alloc_id);
	}
}

/* -----------------------------------------------------------------------
 * Hardware-side PLOAM FIFO access
 * -------------------------------------------------------------------- */

static void gpon_hw_send_ploam(struct gpon_priv *priv,
			       const struct ploam_msg *msg, int times)
{
	int t;

	for (t = 0; t < times; t++) {
		if (!(gpon_read(priv, GPON_PLOAMu_FIFO_STS) &
		      PLOAMu_FIFO_AVAIL_MASK))
			break;
		gpon_write(priv, GPON_PLOAMu_WDATA, msg->value[0]);
		gpon_write(priv, GPON_PLOAMu_WDATA, msg->value[1]);
		gpon_write(priv, GPON_PLOAMu_WDATA, msg->value[2]);
	}
}

static void gpon_drain_ploam_fifo(struct gpon_priv *priv)
{
	u32 depth = gpon_read(priv, GPON_PLOAMd_FIFO_STS) &
		    PLOAMd_FIFO_USED_MASK;
	int i;

	for (i = 0; i < (int)depth; i++) {
		struct ploam_msg msg;

		msg.value[0] = gpon_read(priv, GPON_PLOAMd_RDATA);
		msg.value[1] = gpon_read(priv, GPON_PLOAMd_RDATA);
		msg.value[2] = gpon_read(priv, GPON_PLOAMd_RDATA);

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

	gpon_write(priv, GPON_ONU_ID, ONU_ID_VLD | (onu_id & ONU_ID_MASK));
}

static void gpon_cb_set_eqd_o4(void *hw_priv, u32 byte_delay, u32 bit_delay)
{
	struct gpon_priv *priv = hw_priv;

	priv->byte_delay = byte_delay;
	priv->bit_delay  = bit_delay;
	/* Write byte delay to G_EQD; bit delay goes to PHY (not abstracted) */
	gpon_write(priv, GPON_EQD, byte_delay);
}

static void gpon_cb_adjust_eqd_o5(void *hw_priv, u32 new_eqd)
{
	struct gpon_priv *priv = hw_priv;
	u32 curr_eqd = ploam_get_eqd(priv->ploam);
	int delta = (int)new_eqd - (int)curr_eqd;
	u32 sync_raw;
	int int_byte_delay;
	int K, A, B, a;

	if (delta == 0)
		return;

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
}

static void gpon_cb_enable_us_fec(void *hw_priv)
{
	struct gpon_priv *priv = hw_priv;

	gpon_set_bits(priv, GPON_GBL_CFG, GBL_CFG_US_FEC_EN);
}

static void gpon_cb_set_overhead(void *hw_priv,
				  u8 guard_bits, u8 t1_pbits, u8 t2_pbits,
				  u8 t3_pbits, const u8 delim[3],
				  bool delay_mode, u16 delay_time)
{
	struct gpon_priv *priv = hw_priv;
	u32 prmbl, pre_dly;

	/* G_PLOu_GUARD_BIT always gets the fixed PHY_TX_EN_BIT_LEN_CONST value */
	gpon_write(priv, GPON_PLOu_GUARD_BIT, GPON_GUARD_BIT_LEN);

	/* G_PLOu_PRMBL_TYPE1_2: t1 in upper 16 bits, t2 in lower 16 bits */
	prmbl = ((u32)t1_pbits << 16) | t2_pbits;
	gpon_write(priv, GPON_PLOu_PRMBL_TYPE1_2, prmbl);

	/* G_PRE_ASSIGNED_DLY */
	pre_dly = (delay_mode ? PRE_DLY_EN : 0) | (delay_time & PRE_DLY_MASK);
	gpon_write(priv, GPON_PRE_ASSIGNED_DLY, pre_dly);

	/* PHY delimiter and preamble T3 pattern are not abstracted */
	(void)guard_bits;
	(void)t3_pbits;
	(void)delim;
}

static void gpon_cb_set_t3_preamble(void *hw_priv, u8 o3_t3, u8 o5_t3)
{
	struct gpon_priv *priv = hw_priv;
	u32 val;

	/* G_PLOu_PRMBL_TYPE3: ebl_en=1, o5 in [15:8], o3/o4 in [7:0] */
	val = BIT(16) | ((u32)o5_t3 << 8) | o3_t3;
	gpon_write(priv, GPON_PLOu_PRMBL_TYPE3, val);
}

static void gpon_cb_set_key_switch_time(void *hw_priv, u32 superframe)
{
	struct gpon_priv *priv = hw_priv;

	gpon_write(priv, GPON_AES_CFG, superframe & AES_KEY_SWITCH_CNT_MASK);
}

static void gpon_cb_request_new_key(void *hw_priv)
{
	struct gpon_priv *priv = hw_priv;

	get_random_bytes(priv->aes_key, 16);
	/* Load into shadow registers; switch time set later by Key_Switching_Time */
	gpon_load_aes_shadow(priv, priv->aes_key, 0);
	/* Inform PLOAM layer of the key so it can transmit Encryption_Key */
	ploam_set_aes_key(priv->ploam, priv->aes_key);
}

static void gpon_cb_set_ber_interval(void *hw_priv, u32 interval_ms)
{
	struct gpon_priv *priv = hw_priv;

	priv->ber_interval_ms = interval_ms;
	if (interval_ms)
		mod_timer(&priv->ber_timer,
			  jiffies + msecs_to_jiffies(interval_ms));
	else
		timer_delete(&priv->ber_timer);
}

static void gpon_cb_set_omci_gem(void *hw_priv, u16 gem_port_id, bool valid)
{
	struct gpon_priv *priv = hw_priv;
	struct omci_priv *omci;
	u32 reg_val = valid ? (OMCI_PORT_VLD | (gem_port_id & OMCI_GPID_MASK))
			    : 0;

	gpon_write(priv, GPON_OMCI_ID, reg_val);

	if (!priv->omci_dev)
		return;

	omci = netdev_priv(priv->omci_dev);
	omci->gem_port_id    = gem_port_id;
	omci->gem_port_valid = valid;

	if (valid)
		netif_carrier_on(priv->omci_dev);
	else
		netif_carrier_off(priv->omci_dev);
}

static void gpon_cb_set_gem_encryption(void *hw_priv, u16 port_id,
					u8 encrypt_mode)
{
	struct gpon_priv *priv = hw_priv;
	u32 cfg = GEM_CMD_WRITE | (port_id & (GPON_MAX_GEM_ID - 1));

	if (encrypt_mode == 3)
		cfg |= GEM_ENCRYPT | GEM_VALID;
	else
		cfg |= GEM_VALID;

	gpon_write(priv, GPON_GEM_PORT_CFG, cfg);
	if (gpon_wait_bits(priv, GPON_GEM_PORT_STS, GEM_CMD_DONE,
			   GPON_CMD_TIMEOUT_US))
		dev_err_ratelimited(priv->dev,
			"GEM command timed out for port %u\n", port_id);
}

static void gpon_cb_set_alloc_id(void *hw_priv, u16 alloc_id, bool allocate)
{
	gpon_set_alloc_id_hw(hw_priv, alloc_id, allocate);
}

/* Forward declaration needed by gpon_disable */
static void gpon_disable(struct gpon_priv *priv);

static void gpon_cb_state_changed(void *hw_priv, enum gpon_state state)
{
	struct gpon_priv *priv = hw_priv;
	dev_info(priv->dev, "xpon changed: %d", state);

	switch (state) {
	case GPON_O2_STANDBY:
		/* On entering O2 reset response time */
		gpon_write(priv, GPON_RSP_TIME, GPON_RSP_TIME_DEFAULT);
		fallthrough;
	case GPON_O3_SERIAL_NUMBER:
	case GPON_O4_RANGING:
		/* Start / restart TO1 timer */
		mod_timer(&priv->to1_timer,
			  jiffies + msecs_to_jiffies(GPON_TO1_MS));
		break;
	case GPON_O5_OPERATION:
		/* Cancel TO1 */
		timer_delete(&priv->to1_timer);
		dev_info(priv->dev, "GPON O5: operational, ONU-ID=%u\n",
			 ploam_get_onu_id(priv->ploam));
		netif_carrier_on(priv->netdev);
		break;
	case GPON_O6_POPUP:
		/* Cancel TO1, start TO2 */
		timer_delete(&priv->to1_timer);
		mod_timer(&priv->to2_timer,
			  jiffies + msecs_to_jiffies(GPON_TO2_MS));
		netif_carrier_off(priv->netdev);
		if (priv->omci_dev)
			netif_carrier_off(priv->omci_dev);
		break;
	case GPON_O7_EMERGENCY_STOP:
		timer_delete(&priv->to1_timer);
		timer_delete(&priv->to2_timer);
		netif_carrier_off(priv->netdev);
		if (priv->omci_dev)
			netif_carrier_off(priv->omci_dev);
		break;
	case GPON_O1_INITIAL:
		timer_delete(&priv->to1_timer);
		timer_delete(&priv->to2_timer);
		netif_carrier_off(priv->netdev);
		if (priv->omci_dev)
			netif_carrier_off(priv->omci_dev);
		break;
	default:
		break;
	}
}

static void gpon_cb_deactivate(void *hw_priv)
{
	struct gpon_priv *priv = hw_priv;

	gpon_disable(priv);
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
	int ret;

	ret = gpon_dev_init(priv);
	if (ret) {
		dev_err(priv->dev, "GPON hardware init failed: %d\n", ret);
		return ret;
	}

	gpon_set_serial_number_regs(priv);
	gpon_load_aes_shadow(priv, priv->aes_key, 0);

	gpon_write(priv, GPON_INT_ENABLE, INT_PLOAMD_RECV | INT_DYING_GASP |
		   INT_LOS_GEM_DEL | INT_TOD_1PPS);

	enable_irq(priv->irq);
	if (priv->ber_interval_ms)
		mod_timer(&priv->ber_timer,
			  jiffies + msecs_to_jiffies(priv->ber_interval_ms));

	/* The receiver is ready; enter O2 and accept Upstream_Overhead. */
	ploam_start(priv->ploam);

	return 0;
}

static void gpon_disable(struct gpon_priv *priv)
{
	timer_delete_sync(&priv->ber_timer);
	timer_delete_sync(&priv->to1_timer);
	timer_delete_sync(&priv->to2_timer);
	disable_irq(priv->irq);
	gpon_write(priv, GPON_INT_ENABLE, 0);

	gpon_clear_bits(priv, GPON_GBL_CFG, GBL_CFG_US_FEC_EN);
	gpon_write(priv, GPON_ONU_ID, PLOAM_ONU_UNASSIGNED);
	gpon_write(priv, GPON_OMCI_ID, 0);

	gpon_reset_alloc_ids(priv);
	gpon_reset_gem_ports(priv);

	ploam_reset(priv->ploam);
	netif_carrier_off(priv->netdev);
	if (priv->omci_dev)
		netif_carrier_off(priv->omci_dev);
}

/* -----------------------------------------------------------------------
 * Timers
 * -------------------------------------------------------------------- */

static void gpon_ber_timer_fn(struct timer_list *t)
{
	struct gpon_priv *priv = timer_container_of(priv, t, ber_timer);

	ploam_notify_ber(priv->ploam, 0);
	mod_timer(&priv->ber_timer,
		  jiffies + msecs_to_jiffies(priv->ber_interval_ms));
}

/* TO1: O3/O4 timeout — no Ranging_Time received within 10 s → return to O2 */
static void gpon_to1_timer_fn(struct timer_list *t)
{
	struct gpon_priv *priv = timer_container_of(priv, t, to1_timer);
	enum gpon_state st     = ploam_get_state(priv->ploam);

	if (st == GPON_O3_SERIAL_NUMBER || st == GPON_O4_RANGING) {
		dev_warn(priv->dev,
			 "GPON TO1 expired in O%d, returning to O2\n", (int)st);
		/* Drive state to O2 through PLOAM layer is not directly possible;
		 * call state_changed callback with O2 to perform the transition */
		gpon_cb_state_changed(priv, GPON_O2_STANDBY);
		ploam_reset(priv->ploam);
	}
}

/* TO2: O6 timeout — no Popup received within 100 ms → full disable */
static void gpon_to2_timer_fn(struct timer_list *t)
{
	struct gpon_priv *priv = timer_container_of(priv, t, to2_timer);

	if (ploam_get_state(priv->ploam) == GPON_O6_POPUP) {
		dev_warn(priv->dev, "GPON TO2 expired in O6, resetting\n");
		gpon_disable(priv);
	}
}

/* -----------------------------------------------------------------------
 * Interrupt handler
 * -------------------------------------------------------------------- */

static irqreturn_t gpon_isr(int irq, void *data)
{
	struct gpon_priv *priv = data;
	u32 status, enabled;

	status  = gpon_read(priv, GPON_INT_STATUS);
	enabled = gpon_read(priv, GPON_INT_ENABLE);
	status &= enabled;
	gpon_write(priv, GPON_INT_STATUS, status);		/* W1C */

	if (status & INT_DYING_GASP)
		ploam_notify_dying_gasp(priv->ploam);

	if (status & INT_PLOAMD_RECV)
		gpon_drain_ploam_fifo(priv);

	if (status & INT_LOS_GEM_DEL)
		ploam_notify_los(priv->ploam);

	return IRQ_HANDLED;
}

/* -----------------------------------------------------------------------
 * OMCI network device
 * -------------------------------------------------------------------- */

/**
 * gpon_omci_rx_frame() - deliver a received OMCI frame to the netdev
 * @gpon_dev: the GPON netdev
 * @skb: received raw OMCI PDU; ownership transfers to network stack
 */
void gpon_omci_rx_frame(struct net_device *gpon_dev, struct sk_buff *skb)
{
	struct gpon_priv *priv  = netdev_priv(gpon_dev);
	struct net_device *odev = priv->omci_dev;
	struct omci_priv *omci;

	if (!odev || !netif_running(odev)) {
		dev_kfree_skb_any(skb);
		return;
	}

	omci = netdev_priv(odev);
	skb->dev      = odev;
	skb->protocol = htons(ETH_P_802_3);
	skb_reset_mac_header(skb);

	omci->rx_packets++;
	omci->rx_bytes += skb->len;

	netif_rx(skb);
}
EXPORT_SYMBOL_GPL(gpon_omci_rx_frame);

static int omci_ndo_open(struct net_device *dev)
{
	struct omci_priv *omci = netdev_priv(dev);

	if (omci->gem_port_valid) {
		struct gpon_priv *priv = omci->gpon;
		u32 reg = OMCI_PORT_VLD | (omci->gem_port_id & OMCI_GPID_MASK);

		gpon_write(priv, GPON_OMCI_ID, reg);
		netif_carrier_on(dev);
	}
	netif_start_queue(dev);
	return 0;
}

static int omci_ndo_stop(struct net_device *dev)
{
	struct omci_priv *omci = netdev_priv(dev);

	netif_stop_queue(dev);
	netif_carrier_off(dev);
	gpon_write(omci->gpon, GPON_OMCI_ID, 0);
	return 0;
}

static netdev_tx_t omci_ndo_start_xmit(struct sk_buff *skb,
					struct net_device *dev)
{
	struct omci_priv *omci = netdev_priv(dev);

	if (!omci->gem_port_valid) {
		dev_kfree_skb_any(skb);
		dev->stats.tx_dropped++;
		return NETDEV_TX_OK;
	}

	omci->tx_packets++;
	omci->tx_bytes += skb->len;
	dev_kfree_skb_any(skb);
	return NETDEV_TX_OK;
}

static void omci_ndo_get_stats64(struct net_device *dev,
				  struct rtnl_link_stats64 *stats)
{
	struct omci_priv *omci = netdev_priv(dev);

	stats->rx_packets = omci->rx_packets;
	stats->rx_bytes   = omci->rx_bytes;
	stats->tx_packets = omci->tx_packets;
	stats->tx_bytes   = omci->tx_bytes;
}

static const struct net_device_ops omci_netdev_ops = {
	.ndo_open	 = omci_ndo_open,
	.ndo_stop	 = omci_ndo_stop,
	.ndo_start_xmit	 = omci_ndo_start_xmit,
	.ndo_get_stats64 = omci_ndo_get_stats64,
};

static int gpon_omci_netdev_create(struct gpon_priv *priv)
{
	struct net_device *dev;
	struct omci_priv *omci;
	int ret;

	dev = alloc_netdev(sizeof(*omci), "omci%d", NET_NAME_PREDICTABLE,
			   ether_setup);
	if (!dev)
		return -ENOMEM;

	omci           = netdev_priv(dev);
	omci->netdev   = dev;
	omci->gpon     = priv;
	omci->gem_port_id    = 0xFFFF;
	omci->gem_port_valid = false;

	dev->netdev_ops = &omci_netdev_ops;
	dev->flags     |= IFF_NOARP;
	dev->flags     &= ~IFF_MULTICAST;
	eth_hw_addr_inherit(dev, priv->netdev);

	SET_NETDEV_DEV(dev, priv->dev);
	netif_carrier_off(dev);

	ret = register_netdev(dev);
	if (ret) {
		free_netdev(dev);
		return ret;
	}

	priv->omci_dev = dev;
	return 0;
}

static void gpon_omci_netdev_destroy(struct gpon_priv *priv)
{
	if (!priv->omci_dev)
		return;
	unregister_netdev(priv->omci_dev);
	free_netdev(priv->omci_dev);
	priv->omci_dev = NULL;
}

/* -----------------------------------------------------------------------
 * SFP upstream ops
 * -------------------------------------------------------------------- */

static void gpon_sfp_attach(void *upstream, struct sfp_bus *bus) {}
static void gpon_sfp_detach(void *upstream, struct sfp_bus *bus) {}

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
	if (ploam_get_state(priv->ploam) > GPON_O1_INITIAL)
		gpon_disable(priv);
}

static int gpon_sfp_module_start(void *upstream)
{
	struct gpon_priv *priv = upstream;
	enum gpon_state state;

	state = ploam_get_state(priv->ploam);
	dev_info(priv->dev, "Module state: %d", state);

	/* PHY ready: if we were in emergency stop, stay there */
	if (state == GPON_O1_INITIAL)
		return gpon_enable(priv);

	return 0;
}

static void gpon_sfp_module_stop(void *upstream)
{
	gpon_disable(upstream);
}

static void gpon_sfp_link_down(void *upstream)
{
	struct gpon_priv *priv = upstream;

	ploam_notify_los(priv->ploam);
}

static void gpon_sfp_link_up(void *upstream) {}

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
 * PON WAN net_device ops
 * -------------------------------------------------------------------- */

static int gpon_ndo_open(struct net_device *dev)
{
	struct gpon_priv *priv = netdev_priv(dev);

	netif_start_queue(dev);
	if (priv->sfp_bus)
		sfp_upstream_start(priv->sfp_bus);

	return 0;
}

static int gpon_ndo_stop(struct net_device *dev)
{
	struct gpon_priv *priv = netdev_priv(dev);

	if (priv->sfp_bus)
		sfp_upstream_stop(priv->sfp_bus);
	netif_stop_queue(dev);

	return 0;
}

static netdev_tx_t gpon_ndo_start_xmit(struct sk_buff *skb,
					struct net_device *dev)
{
	struct gpon_priv *priv = netdev_priv(dev);

	priv->tx_packets++;
	priv->tx_bytes += skb->len;
	dev_kfree_skb_any(skb);
	return NETDEV_TX_OK;
}

static void gpon_ndo_get_stats64(struct net_device *dev,
				  struct rtnl_link_stats64 *stats)
{
	struct gpon_priv *priv = netdev_priv(dev);

	stats->rx_packets = priv->rx_packets;
	stats->rx_bytes   = priv->rx_bytes;
	stats->tx_packets = priv->tx_packets;
	stats->tx_bytes   = priv->tx_bytes;
}

static const struct net_device_ops gpon_netdev_ops = {
	.ndo_open	  = gpon_ndo_open,
	.ndo_stop	  = gpon_ndo_stop,
	.ndo_start_xmit	  = gpon_ndo_start_xmit,
	.ndo_get_stats64  = gpon_ndo_get_stats64,
};

/* -----------------------------------------------------------------------
 * Platform driver
 * -------------------------------------------------------------------- */

static int gpon_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct net_device *netdev;
	struct gpon_priv *priv;
	struct resource *res;
	u32 reg_offset;
	int ret;

	netdev = alloc_etherdev(sizeof(*priv));
	if (!netdev)
		return dev_err_probe(dev, -ENOMEM, "cannot alloc netdev for xPON mac");

	SET_NETDEV_DEV(netdev, dev);
	priv = netdev_priv(netdev);
	priv->dev    = dev;
	priv->netdev = netdev;

	spin_lock_init(&priv->lock);

	dev_info(dev, "getting MAC base!!");
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "mac");
	if (!res) {
		ret = dev_err_probe(dev, -EINVAL, "missing mac resource\n");
		goto err_free_netdev;
	}

	priv->base = devm_ioremap_resource(dev, res);
	if (IS_ERR(priv->base)) {
		ret = dev_err_probe(dev, PTR_ERR(priv->base), "needs xPON mac base");
		goto err_free_netdev;
	}

	if (res->start == EN7523_GPON_REGION_BASE) {
		if (resource_size(res) < EN7523_GPON_REG_OFFSET +
					 GPON_SLEEP_CNT + sizeof(u32)) {
			ret = dev_err_probe(dev, -EINVAL,
				"GPON region is too small: %pR\n", res);
			goto err_free_netdev;
		}
		reg_offset = EN7523_GPON_REG_OFFSET;
	} else if (res->start == EN7523_GPON_REG_BASE) {
		if (resource_size(res) < GPON_SLEEP_CNT + sizeof(u32)) {
			ret = dev_err_probe(dev, -EINVAL,
				"GPON register window is too small: %pR\n", res);
			goto err_free_netdev;
		}
		reg_offset = 0;
	} else {
		ret = dev_err_probe(dev, -EINVAL,
			"unsupported GPON MAC resource: %pR\n", res);
		goto err_free_netdev;
	}

	priv->regs = priv->base + reg_offset;
	dev_info(dev, "GPON resource %pR, register offset %#x\n",
		 res, reg_offset);

	priv->irq = platform_get_irq(pdev, 0);
	if (priv->irq < 0) {
		ret = dev_err_probe(dev, priv->irq, "needs mac irq");
		goto err_free_netdev;
	}

	ret = devm_request_irq(dev, priv->irq, gpon_isr, 0,
			       dev_name(dev), priv);
	if (ret)
		goto err_free_netdev;
	disable_irq(priv->irq);

	/* Default serial number */
	memcpy(priv->sn, "MTKG\x00\x00\x00\x01", 8);
	priv->ber_interval_ms = 1000;

	timer_setup(&priv->ber_timer, gpon_ber_timer_fn, 0);
	timer_setup(&priv->to1_timer, gpon_to1_timer_fn, 0);
	timer_setup(&priv->to2_timer, gpon_to2_timer_fn, 0);

	priv->ploam = ploam_alloc(&gpon_ploam_ops, priv, priv->sn, priv->passwd);
	if (!priv->ploam) {
		ret = -ENOMEM;
		goto err_free_netdev;
	}

	priv->sfp_bus = sfp_bus_find_fwnode(dev->fwnode);
	if (!priv->sfp_bus) {
		ret = -ENODEV;
		dev_err(dev, "missing SFP reference\n");
		goto err_free_ploam;
	}
	if (IS_ERR(priv->sfp_bus)) {
		ret = PTR_ERR(priv->sfp_bus);
		dev_err(dev, "failed to find SFP bus: %d\n", ret);
		goto err_free_ploam;
	}

	ret = sfp_bus_add_upstream(priv->sfp_bus, priv, &gpon_sfp_ops);
	if (ret)
		goto err_put_sfp;

	eth_hw_addr_random(netdev);
	netdev->netdev_ops = &gpon_netdev_ops;
	strscpy(netdev->name, "pon%d", IFNAMSIZ);
	netif_carrier_off(netdev);

	ret = register_netdev(netdev);
	if (ret) {
		dev_err(dev, "failed to register PON netdev: %d\n", ret);
		goto err_del_upstream;
	}

	ret = gpon_omci_netdev_create(priv);
	if (ret) {
		dev_err(dev, "failed to create OMCI netdev: %d\n", ret);
		goto err_unreg_netdev;
	}

	platform_set_drvdata(pdev, priv);
	dev_info(dev, "EcoNet GPON MAC driver probed (pon: %s, omci: %s)\n",
		 netdev->name, priv->omci_dev->name);
	return 0;

err_unreg_netdev:
	dev_err(dev, "unregister pon");
	unregister_netdev(netdev);
err_del_upstream:
	dev_err(dev, "clean sfp bus");
	sfp_bus_del_upstream(priv->sfp_bus);
err_put_sfp:
	dev_err(dev, "clean sfp");
	sfp_bus_put(priv->sfp_bus);
err_free_ploam:
	dev_err(dev, "clean pload");
	ploam_free(priv->ploam);
err_free_netdev:
	dev_err(dev, "clean netdev");
	free_netdev(netdev);
	return ret;
}

static void gpon_remove(struct platform_device *pdev)
{
	struct gpon_priv *priv = platform_get_drvdata(pdev);

	gpon_omci_netdev_destroy(priv);
	unregister_netdev(priv->netdev);
	sfp_bus_del_upstream(priv->sfp_bus);
	sfp_bus_put(priv->sfp_bus);
	ploam_free(priv->ploam);
	free_netdev(priv->netdev);
}

static const struct of_device_id gpon_of_match[] = {
	{ .compatible = "airoha,en7523-gpon" },
	{ .compatible = "econet,en7521-gpon" },
	{ .compatible = "econet,en7526-gpon" },
	{ .compatible = "econet,en751221-gpon" },
	{}
};
MODULE_DEVICE_TABLE(of, gpon_of_match);

static struct platform_driver gpon_driver = {
	.probe  = gpon_probe,
	.remove = gpon_remove,
	.driver = {
		.name		= "econet-gpon",
		.of_match_table	= gpon_of_match,
	},
};
module_platform_driver(gpon_driver);

MODULE_DESCRIPTION("EcoNet EN7521/EN7526 GPON MAC driver");
MODULE_AUTHOR("Benjamin Larsson <benjamin.larsson@genexis.eu>");
MODULE_LICENSE("GPL");
