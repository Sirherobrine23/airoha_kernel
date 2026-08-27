/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _AIROHA_XPON_H
#define _AIROHA_XPON_H

#include <linux/bitmap.h>
#include <linux/io.h>
#include <linux/mutex.h>
#include <linux/netdevice.h>
#include <linux/optical_frontend.h>
#include <linux/sfp.h>
#include <linux/timer.h>
#include <linux/workqueue.h>
#include <net/xpon.h>
#include <net/xpon/omci.h>
#include <net/xpon/oam.h>

#include "airoha_eth.h"
#include "airoha_gpon_omci.h"
#include "airoha_ploam.h"

#define V1_XPON_REGION_SIZE		0x00010000
#define GPON_REG_OFFSET			0x00004000
#define EPON_REG_OFFSET			0x00006000

#define XPON_SCU_WAN_CONF              0x070
#define EN7523_SCU_WAN_MODE_MASK       GENMASK(7, 0)
#define EN7528_SCU_WAN_MODE_MASK       GENMASK(2, 0)
#define EN751221_SCU_WAN_MODE_MASK     GENMASK(2, 0)
#define XPON_SCU_WAN_MODE_GPON         0x00
#define XPON_SCU_WAN_MODE_EPON         0x01

struct airoha_xpon_match_data {
	enum airoha_xpon_mode mode;
	bool mode_from_dt;
	u32 wan_mode_mask;
	u8 gpon_fine_delay;
	u16 gpon_rsp_time_activation;
	bool en7523_gpon_defaults;
	bool mac_irq_via_eth;
	bool prepare_before_mmio;
	bool gpon_reset_on_start;
};


/* GPON MAC register layout and protocol constants. */
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
#define GBL_CFG_SR_BLK_SIZE_MASK	GENMASK(7, 0)

/*
 * The GPON MAC stores the reciprocal of the DBRu block size with the bit
 * order reversed.  The vendor SDK programs a 48-byte block, encoded as
 * bitrev8(round(2048 / 48)) = bitrev8(43) = 0xd4.
 */
#define GPON_DBRU_BLOCK_SIZE_48B	0xd4

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
				 INT_TX_LATE_START)

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
 * G_RSP_TIME carries the ONU response time in units of 32 bits, so one unit is
 * 25.72ns at the 1.24416Gbit/s upstream rate and the 0x0551 reset value is the
 * 35us of G.984.3.  The MAC is held at 0x058b while it is reset or in O1.
 *
 * The activation value is generation-specific.  The EN7523 vendor driver
 * switches to 0x0577 before serial-number activation in O2, while the EN751221
 * driver programs its configured response time of 0x058b there and leaves the
 * FEC-adjusted variant disabled.  Responding 0x14 units early on EN751221 does
 * not match the timing the OLT ranges against.
 */
#define GPON_RSP_TIME_RESET		0x058b
#define GPON_RSP_TIME_ACT_EN7523	0x0577
#define GPON_RSP_TIME_ACT_EN7528	0x0577
#define GPON_RSP_TIME_ACT_EN751221	0x058b
#define GPON_IDLE_GEM_THLD_DEFAULT	0x001A

/* TO1 timer: 10 seconds in O3/O4 without Ranging_Time → return to O2 */
#define GPON_TO1_MS		10000
/*
 * Consecutive TO1 expiries tolerated before the MAC and the PHY are reset.
 * Matches the vendor driver: a normal ranging failure only costs a return to
 * O2, and only a run of them is treated as wedged hardware.
 */
#define GPON_TO1_MAX_RETRIES	20
/* TO2 timer: 100 ms in O6 without Popup/Swift_Popup → reset to O1 */
#define GPON_TO2_MS		100
/* Restart delay after an OLT Deactivate_ONU-ID request. */
#define GPON_DEACTIVATE_RESTART_MS	500
/* Coalesce a net/omci apply batch into one complete MAC/PHY restart. */
#define GPON_CONFIG_RESTART_DEBOUNCE_MS	250
#define XPON_LINK_POLL_MS		250
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
 * Fallback TX-enable guard, used only when an OLT announces none.  The guard
 * time is the OLT's to choose: it sizes the quiet period between upstream
 * bursts, and every ONU on the PON is told the same value in
 * Upstream_Overhead.
 */
#define GPON_PHY_GUARD_BIT_NUM	20


#define EPON_MAX_LLID		8

/* LLID registration states */
enum airoha_epon_llid_state {
	AIROHA_EPON_LLID_WAIT = 0,
	AIROHA_EPON_LLID_REGISTERING,
	AIROHA_EPON_LLID_REGISTER_REQUEST,
	AIROHA_EPON_LLID_REGISTER_PENDING,
	AIROHA_EPON_LLID_REGISTER_ACK,
	AIROHA_EPON_LLID_REGISTERED,
};

struct epon_llid {
	enum airoha_epon_llid_state state;
	bool		valid;
	u16		value;
};

struct xpon_priv {
	void __iomem		*base;
	void __iomem		*gpon_reg;
	void __iomem		*epon_reg;
	void __iomem		*epon_reset_reg;
	struct device		*dev;
	struct regmap		*scu;
	const struct airoha_xpon_match_data *match_data;
	struct reset_control	*mac_reset;
	struct phy		*phy;
	enum airoha_xpon_mode	mode;
	bool			phy_initialized;
	bool			phy_powered;
	struct net_device	*gdm_dev;
	bool			started;
	bool			optical_active;
	bool			mac_enabled;
	bool			phy_link_known;
	bool			phy_link_up;
	struct xpon_device	*xpon;
	struct xpon_oam		*oam;
	struct airoha_gpon_omci omci;
	struct airoha_xpon_oam_handler omci_handler;
	struct sfp_bus		*sfp_bus;
	struct optical_frontend	*frontend;
	int			irq;
	int			dying_gasp_irq;

	struct epon_llid	llid[EPON_MAX_LLID];

	struct ploam_priv	*ploam;
	int			registered_llids;

	u8			hw_sn[8];
	u8			hw_passwd[10];
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
	struct delayed_work	phy_link_work;	/* Digital PHY link monitor */
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

	/* Protects the active OMCI OLT interoperability policy. */
	struct mutex		omci_profile_lock;
	struct omci_olt_profile_state omci_profile;

	/* Protects pending runtime identity updates from OMCI. */
	struct mutex		omci_config_lock;
	struct omci_identity	pending_identity;
	bool			config_restart_pending;
	u16			config_restart_key;

	/* Protects the GPON carrier readiness inputs. */
	struct mutex		link_state_lock;
	DECLARE_BITMAP(service_gems, GPON_MAX_GEM_ID);
	bool			gpon_o5;
	bool			omci_operational;
	bool			netdev_link;
	u16			tcont_alloc_id[GPON_MAX_TCONT];
	u16			tcont_entity_id[GPON_MAX_TCONT];

	/* EqD state for O5 incremental adjustment */
	u32			byte_delay;
	u32			bit_delay;

	/* Consecutive TO1 expiries; cleared on O5. */
	unsigned int		to1_failures;

};


/* EPON MAC register layout and MPCP constants. */
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


#endif /* _AIROHA_XPON_H */
