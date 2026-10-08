/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * EcoNet GPON PLOAM layer — public interface
 *
 * The protocol vocabulary and byte codec live in net/xpon. This header
 * defines the FIFO representation and the callbacks through which the
 * PLOAM state machine drives the MAC in airoha_xpon.c.
 */

#ifndef _ECONET_PLOAM_H
#define _ECONET_PLOAM_H

#include <linux/types.h>
#include <linux/unaligned.h>
#include <net/xpon/ploam_msg.h>

#define AIROHA_GPON_PLOAM_WORDS	3

/**
 * struct airoha_ploam_msg - raw PLOAM message as three 32-bit FIFO words
 * @value: numeric FIFO words, with the first octet in bits 31:24
 */
struct airoha_ploam_msg {
	u32 value[AIROHA_GPON_PLOAM_WORDS];
};

/* Numeric FIFO words have the first wire octet in bits 31:24 on every CPU.
 * Convert explicitly at the byte-codec boundary, never with be32_to_cpu()
 * on the register value itself. BIP-8 is generated/checked by the MAC.
 */
static inline void
airoha_ploam_decode(const struct airoha_ploam_msg *raw,
		    struct gpon_ploam_msg *msg)
{
	u8 body[GPON_PLOAM_BODY_LEN];
	int i;

	for (i = 0; i < AIROHA_GPON_PLOAM_WORDS; i++)
		put_unaligned_be32(raw->value[i], body + i * sizeof(u32));
	/* The local buffer always contains one complete standard body. */
	gpon_ploam_parse(body, sizeof(body), msg);
}

static inline void
airoha_ploam_encode(struct airoha_ploam_msg *raw,
		    const struct gpon_ploam_msg *msg)
{
	u8 body[GPON_PLOAM_BODY_LEN];
	int i;

	gpon_ploam_build(body, sizeof(body), msg);
	for (i = 0; i < AIROHA_GPON_PLOAM_WORDS; i++)
		raw->value[i] = get_unaligned_be32(body + i * sizeof(u32));
}

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
	void (*send_upstream)(void *priv, const struct airoha_ploam_msg *msg,
			      int times);
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
			     const struct airoha_ploam_msg *msg);

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
