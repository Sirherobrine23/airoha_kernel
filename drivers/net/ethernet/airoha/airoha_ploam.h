/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * EcoNet GPON PLOAM layer — public interface
 *
 * This header defines the hardware-independent PLOAM protocol types and
 * the callback interface through which the PLOAM state machine drives
 * the hardware layer in gpon.c.
 */

#ifndef _ECONET_PLOAM_H
#define _ECONET_PLOAM_H

#include <linux/types.h>

/* PLOAM message: 13 bytes carried in 3 × 32-bit FIFO words.
 * Layout: value[0][31:24]=ONU-ID, [23:16]=MsgType, [15:0]=content[0-1]
 *         value[1]                               = content[2-5]
 *         value[2]                               = content[6-9]
 * The BIP-8 checksum (byte 12) is computed/verified by hardware.
 */
#define PLOAM_WORDS		3
#define PLOAM_CONTENT_LEN	10
#define PLOAM_ONU_BCAST		0xFF
#define PLOAM_ONU_UNASSIGNED	0xFF

/* Downstream PLOAM message type IDs (from ITU-T G.984.3 / vendor ref) */
#define PLOAM_DOWN_UPSTREAM_OVERHEAD	0x01
#define PLOAM_DOWN_ASSIGN_ONU_ID	0x03
#define PLOAM_DOWN_RANGING_TIME		0x04
#define PLOAM_DOWN_DEACTIVATE_ONU_ID	0x05
#define PLOAM_DOWN_DISABLE_SN		0x06
#define PLOAM_DOWN_ENCRYPTED_PORT_ID	0x08
#define PLOAM_DOWN_REQUEST_PASSWORD	0x09
#define PLOAM_DOWN_ASSIGN_ALLOC_ID	0x0A
#define PLOAM_DOWN_POPUP		0x0C
#define PLOAM_DOWN_REQUEST_KEY		0x0D
#define PLOAM_DOWN_CONFIGURE_PORT_ID	0x0E
#define PLOAM_DOWN_PEE			0x0F
#define PLOAM_DOWN_CHANGE_POWER_LEVEL	0x10
#define PLOAM_DOWN_PST			0x11
#define PLOAM_DOWN_BER_INTERVAL		0x12
#define PLOAM_DOWN_KEY_SWITCHING_TIME	0x13
#define PLOAM_DOWN_EXTENDED_BURST_LEN	0x14
#define PLOAM_DOWN_PON_ID		0x15
#define PLOAM_DOWN_SWIFT_POPUP		0x16
#define PLOAM_DOWN_RANGING_ADJUSTMENT	0x17
#define PLOAM_DOWN_SLEEP_ALLOW		0x18
#define PLOAM_DOWN_MAX_TYPE		0x19

/* Disable_SN mode byte values */
#define PLOAM_DISABLE_DENIED		0xFF	/* unicast SN → go to O7  */
#define PLOAM_DISABLE_DENIED_ALL	0xF0	/* all ONUs → O7          */
#define PLOAM_DISABLE_PARTICIPATE	0x00	/* unicast SN → O2        */
#define PLOAM_DISABLE_PARTICIPATE_ALL	0x0F	/* all O7 ONUs → O2       */

/* Upstream PLOAM message type IDs */
#define PLOAM_UP_SERIAL_NUMBER_ONU	0x01
#define PLOAM_UP_PASSWORD		0x02
#define PLOAM_UP_DYING_GASP		0x03
#define PLOAM_UP_NO_MESSAGE		0x04
#define PLOAM_UP_ENCRYPT_KEY		0x05
#define PLOAM_UP_PEE			0x06
#define PLOAM_UP_PST			0x07
#define PLOAM_UP_REI			0x08
#define PLOAM_UP_ACK			0x09
#define PLOAM_UP_SLEEP_REQUEST		0x0A

/**
 * enum gpon_state - ONU activation states per ITU-T G.984.3
 */
enum gpon_state {
	GPON_O1_INITIAL		= 1,
	GPON_O2_STANDBY,
	GPON_O3_SERIAL_NUMBER,
	GPON_O4_RANGING,
	GPON_O5_OPERATION,
	GPON_O6_POPUP,
	GPON_O7_EMERGENCY_STOP,
};

/**
 * struct ploam_msg - raw PLOAM message as three 32-bit FIFO words
 */
struct ploam_msg {
	u32 value[PLOAM_WORDS];
};

/**
 * struct ploam_ops - callbacks from the PLOAM layer to the hardware layer
 *
 * @send_upstream: write @times copies of upstream PLOAM to the TX FIFO.
 * @set_onu_id: program the OLT-assigned ONU-ID into G_ONU_ID.
 * @set_eqd_o4: program equalization delay in O4; byteDelay = eqd & ~7,
 *   bitDelay = eqd & 7. Hardware writes G_EQD and programs PHY bit delay.
 * @adjust_eqd_o5: incremental EqD update in O5; hardware reads the current
 *   internal byte delay from DBG_TX_SYNC_OFFSET and adjusts G_EQD.
 * @enable_us_fec: set GBL_CFG_US_FEC_EN on O4→O5 transition.
 * @set_overhead: program burst overhead parameters from Upstream_Overhead
 *   PLOAM. guard_bits is the raw message value (PHY_TX_EN_BIT_LEN_CONST=24
 *   should be written to G_PLOu_GUARD_BIT). t1/t2/t3 go to G_PLOu_PRMBL_*,
 *   delim/delay_mode/delay_time go to the PHY and G_PRE_ASSIGNED_DLY.
 * @set_t3_preamble: program G_PLOu_PRMBL_TYPE3 from Extended_Burst_Length.
 * @set_key_switch_time: write the AES key-switch superframe counter to
 *   G_AES_CFG so the shadow key becomes active at the OLT-specified frame.
 * @request_new_key: OLT sent Request_Key; hardware generates a random key,
 *   loads it into shadow registers, returns the key via the aes_key array.
 * @set_ber_interval: update BER reporting timer to @interval_ms.
 * @set_omci_gem: configure or clear the OMCI GEM port in G_OMCI_ID;
 *   return zero only when the hardware channel is ready.
 * @set_gem_encryption: program per-GEM-port encryption mode in GEM port table.
 * @set_alloc_id: allocate or deallocate a T-CONT for the given alloc-ID.
 * @state_changed: ONU activation state has changed; start/stop TO1/TO2 timers,
 *   manage carrier, MBI interface, etc.
 * @deactivate: OLT sent Deactivate_ONU; trigger a full hardware disable/reset.
 */
struct ploam_ops {
	void (*send_upstream)(void *priv, const struct ploam_msg *msg, int times);
	void (*set_onu_id)(void *priv, u8 onu_id);
	void (*set_eqd_o4)(void *priv, u32 byte_delay, u32 bit_delay);
	void (*adjust_eqd_o5)(void *priv, u32 new_eqd);
	void (*enable_us_fec)(void *priv);
	void (*set_overhead)(void *priv, u8 guard_bits, u8 t1_pbits, u8 t2_pbits,
			     u8 t3_pbits, const u8 delim[3],
			     bool delay_mode, u16 delay_time);
	void (*set_t3_preamble)(void *priv, u8 o3_t3, u8 o5_t3);
	void (*set_key_switch_time)(void *priv, u32 superframe);
	void (*request_new_key)(void *priv);
	void (*set_ber_interval)(void *priv, u32 interval_ms);
	int (*set_omci_gem)(void *priv, u16 gem_port_id, bool valid);
	void (*set_gem_encryption)(void *priv, u16 port_id, u8 encrypt_mode);
	void (*set_alloc_id)(void *priv, u16 alloc_id, bool allocate);
	void (*state_changed)(void *priv, enum gpon_state state);
	void (*deactivate)(void *priv);
};

struct ploam_priv;

/* Lifecycle */
struct ploam_priv *ploam_alloc(const struct ploam_ops *ops, void *hw_priv,
			       const u8 sn[8], const u8 passwd[10]);
void ploam_free(struct ploam_priv *pp);
void ploam_set_identity(struct ploam_priv *pp, const u8 sn[8],
			const u8 passwd[10]);
void ploam_reset(struct ploam_priv *pp);
void ploam_start(struct ploam_priv *pp);

/* Downstream processing — call from the HW interrupt handler */
void ploam_handle_downstream(struct ploam_priv *pp,
			     const struct ploam_msg *msg);

/* Event notifications from hardware to PLOAM layer */
void ploam_notify_dying_gasp(struct ploam_priv *pp);
void ploam_notify_ber(struct ploam_priv *pp, u32 bip_count);
void ploam_notify_los(struct ploam_priv *pp);

/* State queries */
enum gpon_state ploam_get_state(const struct ploam_priv *pp);
u8 ploam_get_onu_id(const struct ploam_priv *pp);
u32 ploam_get_eqd(const struct ploam_priv *pp);

/* Key management — called by hardware after loading key into shadow regs */
void ploam_set_aes_key(struct ploam_priv *pp, const u8 key[16]);

#endif /* _ECONET_PLOAM_H */
