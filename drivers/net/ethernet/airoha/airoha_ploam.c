// SPDX-License-Identifier: GPL-2.0-only
/*
 * EcoNet GPON PLOAM message processing — hardware-independent layer
 *
 * Implements the ITU-T G.984.3 ONU activation state machine (O1–O7) and
 * all downstream PLOAM message decode / upstream PLOAM message encode
 * logic.  Hardware effects (register writes, carrier changes) are
 * delegated entirely through struct ploam_ops callbacks.
 *
 * Includes the EN7521 PLOAM deduplication filter: every 3rd consecutive
 * identical message is passed through; intermediate duplicates are
 * suppressed.  For RANGING_TIME only the first 5 content bytes are
 * compared (delay bytes excluded) per ref driver behaviour.
 */

#include <linux/slab.h>
#include <linux/random.h>
#include <linux/string.h>
#include <linux/unaligned.h>
#include "airoha_ploam.h"

/* Number of copies used by the EN7523 vendor activation path. */
#define PLOAM_REPEATED_MSG_REPS	3

/* PHY_TX_EN_BIT_LEN_CONST used for G_PLOu_GUARD_BIT */
#define PLOAM_GUARD_BIT_LEN	24

struct ploam_priv {
	const struct ploam_ops	*ops;
	void			*hw_priv;

	enum gpon_state		state;
	u8			onu_id;
	bool			emergency_state;

	u8			sn[8];
	u8			passwd[10];

	/* AES key management */
	u8			aes_key[16];
	u8			key_idx;
	bool			key_exchange_pending;

	/* BER REI sequence number (4-bit, wraps at 16) */
	u8			rei_seq;

	/* Current EqD for O5 incremental adjustment */
	u32			eqd;

	/* EN7521 downstream PLOAM deduplication filter */
	struct ploam_msg	dedup_prev;
	bool			dedup_prev_valid;
	u32			dedup_same_cnt;
};

/* -----------------------------------------------------------------------
 * Message pack / unpack
 *
 * Wire format (big-endian, 12 bytes):
 *   byte 0: ONU-ID (dest_id)
 *   byte 1: message type (msg_id)
 *   bytes 2-11: payload (content[0..9])
 * -------------------------------------------------------------------- */

static void ploam_unpack(const struct ploam_msg *msg,
			 u8 *onu_id, u8 *type,
			 u8 content[PLOAM_CONTENT_LEN])
{
	*onu_id    = (msg->value[0] >> 24) & 0xFF;
	*type      = (msg->value[0] >> 16) & 0xFF;
	content[0] = (msg->value[0] >>  8) & 0xFF;
	content[1] =  msg->value[0]        & 0xFF;
	content[2] = (msg->value[1] >> 24) & 0xFF;
	content[3] = (msg->value[1] >> 16) & 0xFF;
	content[4] = (msg->value[1] >>  8) & 0xFF;
	content[5] =  msg->value[1]        & 0xFF;
	content[6] = (msg->value[2] >> 24) & 0xFF;
	content[7] = (msg->value[2] >> 16) & 0xFF;
	content[8] = (msg->value[2] >>  8) & 0xFF;
	content[9] =  msg->value[2]        & 0xFF;
}

static void ploam_pack(struct ploam_msg *msg, u8 onu_id, u8 type,
		       const u8 content[PLOAM_CONTENT_LEN])
{
	msg->value[0] = ((u32)onu_id     << 24) | ((u32)type      << 16) |
			((u32)content[0] <<  8) | content[1];
	msg->value[1] = ((u32)content[2] << 24) | ((u32)content[3] << 16) |
			((u32)content[4] <<  8) | content[5];
	msg->value[2] = ((u32)content[6] << 24) | ((u32)content[7] << 16) |
			((u32)content[8] <<  8) | content[9];
}

/* -----------------------------------------------------------------------
 * EN7521 downstream PLOAM deduplication filter
 *
 * Every 3rd consecutive identical message is passed through; duplicates
 * 1 and 2 are suppressed.  For RANGING_TIME only the first 5 content
 * bytes (excluding the 4-byte delay field) are compared.
 * -------------------------------------------------------------------- */

static bool ploam_filter_suppress(struct ploam_priv *pp,
				  const struct ploam_msg *msg, u8 type)
{
	bool same;

	if (!pp->dedup_prev_valid) {
		pp->dedup_prev       = *msg;
		pp->dedup_prev_valid = true;
		pp->dedup_same_cnt   = 1;
		return false;
	}

	if (type == PLOAM_DOWN_RANGING_TIME) {
		/* Compare only ONU-ID + type + content[0..4] (5 bytes);
		 * ignore content[1..4] = delay field which may legitimately change */
		same = (msg->value[0] == pp->dedup_prev.value[0]) &&
		       ((msg->value[1] >> 16) == (pp->dedup_prev.value[1] >> 16));
	} else {
		same = (msg->value[0] == pp->dedup_prev.value[0]) &&
		       (msg->value[1] == pp->dedup_prev.value[1]) &&
		       (msg->value[2] == pp->dedup_prev.value[2]);
	}

	pp->dedup_prev = *msg;

	if (!same) {
		pp->dedup_same_cnt = 1;
		return false;
	}

	pp->dedup_same_cnt++;
	/* Pass every 3rd (same_cnt==1,3,6,...), suppress 2nd and 3rd of each triple */
	return (pp->dedup_same_cnt % 3) != 1;
}

/* -----------------------------------------------------------------------
 * Upstream message helpers
 * -------------------------------------------------------------------- */

static void ploam_send(struct ploam_priv *pp, u8 type,
		       const u8 content[PLOAM_CONTENT_LEN], int times)
{
	struct ploam_msg msg;

	ploam_pack(&msg, pp->onu_id, type, content);
	pp->ops->send_upstream(pp->hw_priv, &msg, times);
}

static void ploam_send_password(struct ploam_priv *pp)
{
	u8 content[PLOAM_CONTENT_LEN] = {};

	memcpy(content, pp->passwd, 10);
	ploam_send(pp, PLOAM_UP_PASSWORD, content, PLOAM_REPEATED_MSG_REPS);
}

static void ploam_send_dying_gasp(struct ploam_priv *pp)
{
	u8 content[PLOAM_CONTENT_LEN] = {};

	ploam_send(pp, PLOAM_UP_DYING_GASP, content,
		   PLOAM_REPEATED_MSG_REPS);
}

static void ploam_send_rei(struct ploam_priv *pp, u32 bip_count)
{
	u8 content[PLOAM_CONTENT_LEN] = {};

	put_unaligned_be32(bip_count, &content[0]);
	/* content[4] bits[3:0] = 4-bit sequence number */
	content[4] = pp->rei_seq & 0x0F;
	pp->rei_seq = (pp->rei_seq + 1) & 0x0F;
	ploam_send(pp, PLOAM_UP_REI, content, 1);
}

/* dm_id = downstream message type being acknowledged;
 * dm_bytes = first 9 bytes of the downstream message (ONU-ID .. content[6]) */
static void ploam_send_ack(struct ploam_priv *pp, u8 dm_id,
			   const u8 dm_bytes[9])
{
	u8 content[PLOAM_CONTENT_LEN] = {};

	content[0] = dm_id;
	memcpy(&content[1], dm_bytes, 9 > (PLOAM_CONTENT_LEN - 1) ?
	       (PLOAM_CONTENT_LEN - 1) : 9);
	ploam_send(pp, PLOAM_UP_ACK, content, PLOAM_REPEATED_MSG_REPS);
}

/* Helper: build the 9-byte dm_bytes from a received message's onu_id,
 * type and content[0..6] for use in Acknowledge */
static void ploam_make_dm_bytes(u8 dm_bytes[9], u8 onu_id, u8 type,
				const u8 content[PLOAM_CONTENT_LEN])
{
	dm_bytes[0] = onu_id;
	dm_bytes[1] = type;
	memcpy(&dm_bytes[2], content, 7);
}

static void ploam_send_encrypt_key(struct ploam_priv *pp)
{
	u8 content[PLOAM_CONTENT_LEN] = {};
	int i;

	/* Two fragments of 8 key bytes each.
	 * content[0] = key_idx, content[1] = frag_idx, content[2..9] = key[8] */
	for (i = 0; i < 2; i++) {
		content[0] = pp->key_idx;
		content[1] = i;
		memcpy(&content[2], pp->aes_key + i * 8, 8);
		ploam_send(pp, PLOAM_UP_ENCRYPT_KEY, content,
			   PLOAM_REPEATED_MSG_REPS);
	}
}

/* -----------------------------------------------------------------------
 * State machine helpers
 * -------------------------------------------------------------------- */

static void ploam_set_state(struct ploam_priv *pp, enum gpon_state new_state)
{
	if (pp->state == new_state)
		return;

	pp->state = new_state;
	pp->ops->state_changed(pp->hw_priv, new_state);

	/*
	 * Do not enqueue Serial_Number_ONU when entering O3.  The EN7523 GPON
	 * MAC builds the message from G_VENDOR_ID, G_VS_SN and G_SN_MSG_CFG
	 * and transmits it in the OLT-provided serial-number grant.  Sending a
	 * second software-generated message here occurs outside that grant and
	 * also omits the hardware random-delay and power-level fields.
	 */
}

/* -----------------------------------------------------------------------
 * Downstream message handlers
 * Each handler is called after ONU-ID and filter checks pass.
 *
 * Field extraction conventions:
 *   content[N] corresponds to raw message byte (N+2) in big-endian order.
 *   Bit-field positions follow the __BIG_ENDIAN layout in gpon_ploam_raw.h.
 * -------------------------------------------------------------------- */

/* PLOAM_DOWN_UPSTREAM_OVERHEAD (0x01) — broadcast only */
static void handle_upstream_overhead(struct ploam_priv *pp,
				     u8 onu_id,
				     const u8 c[PLOAM_CONTENT_LEN])
{
	u8 delim[3];
	bool delay_mode;
	u16 delay_time;

	if (onu_id != PLOAM_ONU_BCAST)
		return;
	if (pp->state != GPON_O2_STANDBY)
		return;

	/* c[0]=gbits, c[1]=t1_pbits, c[2]=t2_pbits, c[3]=t3_pbits
	 * c[4..6]=delimiter[0..2]
	 * c[7]: BE layout bits[7:6]=resv, [5]=delay_mode, [4]=sn_mask,
	 *        [3:2]=sn_tran_num, [1:0]=tx_power
	 * c[8]=delay_time[0], c[9]=delay_time[1] */
	delim[0]   = c[4];
	delim[1]   = c[5];
	delim[2]   = c[6];
	delay_mode = (c[7] >> 5) & 1;
	delay_time = ((u16)c[8] << 8) | c[9];

	pp->ops->set_overhead(pp->hw_priv, c[0], c[1], c[2], c[3],
			      delim, delay_mode, delay_time);
	ploam_set_state(pp, GPON_O3_SERIAL_NUMBER);
}

/* PLOAM_DOWN_ASSIGN_ONU_ID (0x03) — broadcast only */
static void handle_assign_onu_id(struct ploam_priv *pp,
				 u8 onu_id,
				 const u8 c[PLOAM_CONTENT_LEN])
{
	if (onu_id != PLOAM_ONU_BCAST)
		return;
	if (pp->state != GPON_O3_SERIAL_NUMBER)
		return;

	/* c[0]=new ONU-ID, c[1..8]=SN for verification */
	if (memcmp(&c[1], pp->sn, 8) != 0)
		return;

	/* Transition to O4 first, then program the ONU-ID register */
	ploam_set_state(pp, GPON_O4_RANGING);
	pp->onu_id = c[0];
	pp->ops->set_onu_id(pp->hw_priv, pp->onu_id);
}

/* PLOAM_DOWN_RANGING_TIME (0x04) — unicast */
static void handle_ranging_time(struct ploam_priv *pp,
				const u8 c[PLOAM_CONTENT_LEN])
{
	u32 new_eqd;
	u8 eqd_type;

	if (pp->state != GPON_O4_RANGING && pp->state != GPON_O5_OPERATION)
		return;

	/* c[0] BE: bits[7:1]=resv, bit[0]=eqd_type */
	eqd_type = c[0] & 1;

	if (eqd_type == 1) {
		/* Protection path EqD — ignore */
		return;
	}

	/* c[1..4] = delay[0..3] big-endian EqD value */
	new_eqd = get_unaligned_be32(&c[1]);

	if (pp->state == GPON_O4_RANGING) {
		u32 byte_delay = new_eqd & ~7u;
		u32 bit_delay  = new_eqd &  7u;

		pp->eqd        = new_eqd;
		pp->ops->set_eqd_o4(pp->hw_priv, byte_delay, bit_delay);
		pp->ops->enable_us_fec(pp->hw_priv);
		ploam_set_state(pp, GPON_O5_OPERATION);
	} else {
		/* O5: incremental EqD adjustment via hardware */
		pp->ops->adjust_eqd_o5(pp->hw_priv, new_eqd);
		pp->eqd = new_eqd;
	}
}

/* PLOAM_DOWN_DEACTIVATE_ONU_ID (0x05) — broadcast or unicast */
static void handle_deactivate_onu(struct ploam_priv *pp, u8 onu_id)
{
	if (onu_id != pp->onu_id && onu_id != PLOAM_ONU_BCAST)
		return;
	if (pp->state != GPON_O4_RANGING &&
	    pp->state != GPON_O5_OPERATION &&
	    pp->state != GPON_O6_POPUP)
		return;

	pp->ops->deactivate(pp->hw_priv);
}

/* PLOAM_DOWN_DISABLE_SN (0x06) — broadcast only */
static void handle_disable_sn(struct ploam_priv *pp,
			       const u8 c[PLOAM_CONTENT_LEN])
{
	/* c[0]=mode, c[1..8]=SN */
	u8 mode = c[0];

	if (pp->state == GPON_O7_EMERGENCY_STOP) {
		if (mode == PLOAM_DISABLE_PARTICIPATE_ALL ||
		    (mode == PLOAM_DISABLE_PARTICIPATE &&
		     memcmp(&c[1], pp->sn, 8) == 0)) {
			pp->emergency_state = false;
			ploam_set_state(pp, GPON_O2_STANDBY);
		}
	} else if (pp->state != GPON_O1_INITIAL) {
		if (mode == PLOAM_DISABLE_DENIED_ALL ||
		    (mode == PLOAM_DISABLE_DENIED &&
		     memcmp(&c[1], pp->sn, 8) == 0)) {
			pp->emergency_state = true;
			ploam_set_state(pp, GPON_O7_EMERGENCY_STOP);
		}
	}
}

/* PLOAM_DOWN_ENCRYPTED_PORT_ID (0x08) — unicast */
static void handle_encrypted_port_id(struct ploam_priv *pp,
				      u8 onu_id, u8 type,
				      const u8 c[PLOAM_CONTENT_LEN])
{
	u16 port_id;
	u8 encrypt;
	u8 dm_bytes[9];

	if (onu_id != pp->onu_id)
		return;

	/* c[0] BE: bits[7:2]=resv, bits[1:0]=encrypt */
	encrypt = c[0] & 3;
	/* c[1]=port_id_m (8 bits), c[2] BE: bits[7:4]=port_id_l, bits[3:0]=resv */
	port_id = ((u16)c[1] << 4) | ((c[2] >> 4) & 0xF);

	pp->ops->set_gem_encryption(pp->hw_priv, port_id, encrypt);

	ploam_make_dm_bytes(dm_bytes, onu_id, type, c);
	ploam_send_ack(pp, type, dm_bytes);
}

/* PLOAM_DOWN_REQUEST_PASSWORD (0x09) — unicast */
static void handle_request_password(struct ploam_priv *pp, u8 onu_id)
{
	if (onu_id != pp->onu_id)
		return;
	if (pp->state == GPON_O5_OPERATION)
		ploam_send_password(pp);
}

/* PLOAM_DOWN_ASSIGN_ALLOC_ID (0x0A) — unicast */
static void handle_assign_alloc_id(struct ploam_priv *pp,
				    u8 onu_id, u8 type,
				    const u8 c[PLOAM_CONTENT_LEN])
{
	u16 alloc_id;
	bool allocate;
	u8 dm_bytes[9];

	if (onu_id != pp->onu_id)
		return;
	if (pp->state != GPON_O5_OPERATION)
		goto send_ack;

	/* c[0]=alloc_id_m, c[1] BE: bits[7:4]=alloc_id_l, [3:0]=resv */
	alloc_id = ((u16)c[0] << 4) | ((c[1] >> 4) & 0xF);
	/* c[2]=type: 0x01=allocate, 0xFF=deallocate */
	allocate = (c[2] == 0x01);

	if (alloc_id != pp->onu_id)
		pp->ops->set_alloc_id(pp->hw_priv, alloc_id, allocate);

send_ack:
	ploam_make_dm_bytes(dm_bytes, onu_id, type, c);
	ploam_send_ack(pp, type, dm_bytes);
}

/* PLOAM_DOWN_POPUP (0x0C) — broadcast or unicast */
static void handle_popup(struct ploam_priv *pp, u8 onu_id)
{
	if (onu_id != pp->onu_id && onu_id != PLOAM_ONU_BCAST)
		return;
	if (pp->state != GPON_O6_POPUP)
		return;

	if (onu_id == PLOAM_ONU_BCAST)
		ploam_set_state(pp, GPON_O4_RANGING);
	else
		ploam_set_state(pp, GPON_O5_OPERATION);
}

/* PLOAM_DOWN_REQUEST_KEY (0x0D) — unicast */
static void handle_request_key(struct ploam_priv *pp, u8 onu_id)
{
	if (onu_id != pp->onu_id)
		return;
	if (pp->state != GPON_O5_OPERATION)
		return;
	if (pp->key_exchange_pending)
		return;

	pp->key_exchange_pending = true;
	pp->key_idx ^= 1;
	/* Hardware generates a random key and loads shadow regs */
	pp->ops->request_new_key(pp->hw_priv);
	ploam_send_encrypt_key(pp);
}

/* PLOAM_DOWN_CONFIGURE_PORT_ID (0x0E) — unicast */
static void handle_configure_port_id(struct ploam_priv *pp,
				      u8 onu_id, u8 type,
				      const u8 c[PLOAM_CONTENT_LEN])
{
	u16 port_id;
	bool activate;
	u8 dm_bytes[9];

	if (onu_id != pp->onu_id)
		return;
	if (pp->state != GPON_O5_OPERATION)
		goto send_ack;

	/* c[0] BE: bits[7:1]=resv, bit[0]=activate */
	activate = c[0] & 1;
	/* c[1]=port_id_m, c[2] BE: bits[7:4]=port_id_l, [3:0]=resv */
	port_id = ((u16)c[1] << 4) | ((c[2] >> 4) & 0xF);

	/* Do not acknowledge an OMCC assignment until the hardware path is
	 * ready. The OLT will retry Configure-Port-ID after a transient
	 * command failure.
	 */
	if (pp->ops->set_omci_gem(pp->hw_priv, port_id, activate))
		return;

send_ack:
	ploam_make_dm_bytes(dm_bytes, onu_id, type, c);
	ploam_send_ack(pp, type, dm_bytes);
}

/* PLOAM_DOWN_BER_INTERVAL (0x12) — broadcast or unicast */
static void handle_ber_interval(struct ploam_priv *pp,
				u8 onu_id, u8 type,
				const u8 c[PLOAM_CONTENT_LEN])
{
	u32 interval_frames;
	u32 interval_ms;
	u8 dm_bytes[9];

	if (onu_id != pp->onu_id && onu_id != PLOAM_ONU_BCAST)
		return;
	if (pp->state == GPON_O5_OPERATION) {
		/* c[0..3] = interval in frames; convert to ms (125 µs/frame) */
		interval_frames = get_unaligned_be32(&c[0]);
		/* frames × 125 µs = interval_frames >> 3 ms (≈) */
		interval_ms = interval_frames >> 3;
		pp->ops->set_ber_interval(pp->hw_priv, interval_ms);
	}

	ploam_make_dm_bytes(dm_bytes, onu_id, type, c);
	ploam_send_ack(pp, type, dm_bytes);
}

/* PLOAM_DOWN_KEY_SWITCHING_TIME (0x13) — unicast */
static void handle_key_switching_time(struct ploam_priv *pp,
				       u8 onu_id, u8 type,
				       const u8 c[PLOAM_CONTENT_LEN])
{
	u32 superframe;
	u8 dm_bytes[9];

	if (onu_id != pp->onu_id)
		return;
	if (pp->state == GPON_O5_OPERATION) {
		superframe = get_unaligned_be32(&c[0]);
		pp->ops->set_key_switch_time(pp->hw_priv, superframe);
	}

	ploam_make_dm_bytes(dm_bytes, onu_id, type, c);
	ploam_send_ack(pp, type, dm_bytes);
}

/* PLOAM_DOWN_EXTENDED_BURST_LEN (0x14) — broadcast only, O3 only */
static void handle_extended_burst_length(struct ploam_priv *pp,
					  u8 onu_id,
					  const u8 c[PLOAM_CONTENT_LEN])
{
	if (onu_id != PLOAM_ONU_BCAST)
		return;
	if (pp->state != GPON_O3_SERIAL_NUMBER)
		return;

	/* c[0]=o3_t3_preamble, c[1]=o5_t3_preamble */
	pp->ops->set_t3_preamble(pp->hw_priv, c[0], c[1]);
}

/* PLOAM_DOWN_SWIFT_POPUP (0x16) — broadcast only */
static void handle_swift_popup(struct ploam_priv *pp, u8 onu_id)
{
	if (onu_id != PLOAM_ONU_BCAST)
		return;
	if (pp->state == GPON_O6_POPUP)
		ploam_set_state(pp, GPON_O5_OPERATION);
}

/* PLOAM_DOWN_RANGING_ADJUSTMENT (0x17) — unicast or broadcast */
static void handle_ranging_adjustment(struct ploam_priv *pp,
				       u8 onu_id, u8 type,
				       const u8 c[PLOAM_CONTENT_LEN])
{
	u32 eqd_offset;
	u8 s_bit;
	u8 dm_bytes[9];

	if (onu_id != pp->onu_id && onu_id != PLOAM_ONU_BCAST)
		return;
	if (pp->state == GPON_O5_OPERATION) {
		/* c[0] BE: bits[7:2]=resv, bit[1]=s_bit, bit[0]=resv */
		s_bit = (c[0] >> 1) & 1;
		eqd_offset = get_unaligned_be32(&c[1]);
		if (eqd_offset != 0) {
			u32 new_eqd = s_bit ? pp->eqd - eqd_offset
					    : pp->eqd + eqd_offset;
			pp->ops->adjust_eqd_o5(pp->hw_priv, new_eqd);
			pp->eqd = new_eqd;
		}
	}

	ploam_make_dm_bytes(dm_bytes, onu_id, type, c);
	ploam_send_ack(pp, type, dm_bytes);
}

/* -----------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------- */

struct ploam_priv *ploam_alloc(const struct ploam_ops *ops, void *hw_priv,
			       const u8 sn[8], const u8 passwd[10])
{
	struct ploam_priv *pp;

	pp = kzalloc(sizeof(*pp), GFP_KERNEL);
	if (!pp)
		return NULL;

	pp->ops     = ops;
	pp->hw_priv = hw_priv;
	pp->state   = GPON_O1_INITIAL;
	pp->onu_id  = PLOAM_ONU_UNASSIGNED;

	memcpy(pp->sn,     sn,     8);
	memcpy(pp->passwd, passwd, 10);

	return pp;
}

void ploam_free(struct ploam_priv *pp)
{
	kfree(pp);
}

void ploam_reset(struct ploam_priv *pp)
{
	pp->state                = GPON_O1_INITIAL;
	pp->onu_id               = PLOAM_ONU_UNASSIGNED;
	pp->key_exchange_pending = false;
	pp->rei_seq              = 0;
	pp->eqd                  = 0;
	pp->dedup_prev_valid     = false;
	pp->dedup_same_cnt       = 0;
	/* emergency_state is sticky: only cleared by Disable_SN PARTICIPATE */
}

void ploam_start(struct ploam_priv *pp)
{
	if (pp->state == GPON_O1_INITIAL && !pp->emergency_state)
		ploam_set_state(pp, GPON_O2_STANDBY);
}

void ploam_handle_downstream(struct ploam_priv *pp,
			     const struct ploam_msg *msg)
{
	u8 onu_id, type;
	u8 content[PLOAM_CONTENT_LEN];

	ploam_unpack(msg, &onu_id, &type, content);

	/* Drop messages not addressed to us */
	if (onu_id != PLOAM_ONU_BCAST && onu_id != pp->onu_id)
		return;

	/* EN7521 deduplication filter */
	if (ploam_filter_suppress(pp, msg, type))
		return;

	switch (type) {
	case PLOAM_DOWN_UPSTREAM_OVERHEAD:
		handle_upstream_overhead(pp, onu_id, content);
		break;
	case PLOAM_DOWN_ASSIGN_ONU_ID:
		handle_assign_onu_id(pp, onu_id, content);
		break;
	case PLOAM_DOWN_RANGING_TIME:
		handle_ranging_time(pp, content);
		break;
	case PLOAM_DOWN_DEACTIVATE_ONU_ID:
		handle_deactivate_onu(pp, onu_id);
		break;
	case PLOAM_DOWN_DISABLE_SN:
		handle_disable_sn(pp, content);
		break;
	case PLOAM_DOWN_ENCRYPTED_PORT_ID:
		handle_encrypted_port_id(pp, onu_id, type, content);
		break;
	case PLOAM_DOWN_REQUEST_PASSWORD:
		handle_request_password(pp, onu_id);
		break;
	case PLOAM_DOWN_ASSIGN_ALLOC_ID:
		handle_assign_alloc_id(pp, onu_id, type, content);
		break;
	case PLOAM_DOWN_POPUP:
		handle_popup(pp, onu_id);
		break;
	case PLOAM_DOWN_REQUEST_KEY:
		handle_request_key(pp, onu_id);
		break;
	case PLOAM_DOWN_CONFIGURE_PORT_ID:
		handle_configure_port_id(pp, onu_id, type, content);
		break;
	case PLOAM_DOWN_BER_INTERVAL:
		handle_ber_interval(pp, onu_id, type, content);
		break;
	case PLOAM_DOWN_KEY_SWITCHING_TIME:
		handle_key_switching_time(pp, onu_id, type, content);
		break;
	case PLOAM_DOWN_EXTENDED_BURST_LEN:
		handle_extended_burst_length(pp, onu_id, content);
		break;
	case PLOAM_DOWN_SWIFT_POPUP:
		handle_swift_popup(pp, onu_id);
		break;
	case PLOAM_DOWN_RANGING_ADJUSTMENT:
		handle_ranging_adjustment(pp, onu_id, type, content);
		break;
	case PLOAM_DOWN_PEE:
	case PLOAM_DOWN_PST:
	case PLOAM_DOWN_CHANGE_POWER_LEVEL:
	case PLOAM_DOWN_PON_ID:
	case PLOAM_DOWN_SLEEP_ALLOW:
		/* Stub: message acknowledged by presence in handler table */
		break;
	default:
		break;
	}
}

void ploam_notify_dying_gasp(struct ploam_priv *pp)
{
	if (pp->state >= GPON_O2_STANDBY)
		ploam_send_dying_gasp(pp);
}

void ploam_notify_ber(struct ploam_priv *pp, u32 bip_count)
{
	if (pp->state == GPON_O5_OPERATION)
		ploam_send_rei(pp, bip_count);
}

void ploam_notify_los(struct ploam_priv *pp)
{
	/* LOS while operational: transition to O6 (popup state).
	 * The TO2 timer in the hardware layer will reset to O1 if no
	 * Popup / Swift_Popup is received within 100 ms. */
	if (pp->state == GPON_O5_OPERATION)
		ploam_set_state(pp, GPON_O6_POPUP);
}

enum gpon_state ploam_get_state(const struct ploam_priv *pp)
{
	return pp->state;
}

u8 ploam_get_onu_id(const struct ploam_priv *pp)
{
	return pp->onu_id;
}

u32 ploam_get_eqd(const struct ploam_priv *pp)
{
	return pp->eqd;
}

void ploam_set_aes_key(struct ploam_priv *pp, const u8 key[16])
{
	memcpy(pp->aes_key, key, 16);
	pp->key_exchange_pending = false;
}
