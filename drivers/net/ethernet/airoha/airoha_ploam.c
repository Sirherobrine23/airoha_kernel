// SPDX-License-Identifier: GPL-2.0-only
/*
 * Airoha GPON and XGS-PON PLOAM protocol engines
 *
 * The GPON engine implements the ITU-T G.984.3 state machine (O1–O7) and
 * all downstream PLOAM message decode / upstream PLOAM message encode
 * logic.  Hardware effects (register writes, carrier changes) are
 * delegated entirely through struct ploam_ops callbacks.
 *
 * XGS-PON uses the same protocol library with its own state and callbacks
 * below. Its 40-byte bodies, ten-bit ONU-ID and combined O2/O3 state must
 * retain their protocol-specific processing.
 *
 * GPON includes the EN7521 PLOAM deduplication filter: every 3rd consecutive
 * identical message is passed through; intermediate duplicates are
 * suppressed. For RANGING_TIME the ONU-ID, message type and first five
 * content bytes are compared, matching the seven-byte vendor comparison.
 */

#include <linux/err.h>
#include <linux/module.h>
#include <linux/overflow.h>
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
	struct airoha_ploam_msg	dedup_prev;
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

static void ploam_unpack(const struct airoha_ploam_msg *msg,
			 u8 *onu_id, u8 *type,
			 u8 content[GPON_PLOAM_CONTENT_LEN])
{
	struct gpon_ploam_msg decoded;

	airoha_ploam_decode(msg, &decoded);
	*onu_id = decoded.onu_id;
	*type = decoded.msg_id;
	memcpy(content, decoded.content, GPON_PLOAM_CONTENT_LEN);
}

static void ploam_pack(struct airoha_ploam_msg *msg, u8 onu_id, u8 type,
		       const u8 content[GPON_PLOAM_CONTENT_LEN])
{
	struct gpon_ploam_msg decoded = {
		.onu_id = onu_id,
		.msg_id = type,
	};

	memcpy(decoded.content, content, GPON_PLOAM_CONTENT_LEN);
	airoha_ploam_encode(msg, &decoded);
}

/* -----------------------------------------------------------------------
 * EN7521 downstream PLOAM deduplication filter
 *
 * Every 3rd consecutive identical message is passed through; duplicates
 * 1 and 2 are suppressed. For RANGING_TIME the first seven wire bytes
 * are compared: ONU-ID, message type and content[0..4].
 * -------------------------------------------------------------------- */

static bool ploam_filter_suppress(struct ploam_priv *pp,
				  const struct airoha_ploam_msg *msg, u8 type)
{
	bool same;

	if (!pp->dedup_prev_valid) {
		pp->dedup_prev       = *msg;
		pp->dedup_prev_valid = true;
		pp->dedup_same_cnt   = 1;
		return false;
	}

	if (type == GPON_PLOAM_DOWN_RANGING_TIME) {
		/* Match the vendor seven-byte memcmp without depending on
		 * struct layout or host endianness.
		 */
		same = msg->value[0] == pp->dedup_prev.value[0] &&
		       (msg->value[1] >> 8) ==
		       (pp->dedup_prev.value[1] >> 8);
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
		       const u8 content[GPON_PLOAM_CONTENT_LEN], int times)
{
	struct airoha_ploam_msg msg;

	ploam_pack(&msg, pp->onu_id, type, content);
	pp->ops->send_upstream(pp->hw_priv, &msg, times);
}

static void ploam_send_password(struct ploam_priv *pp)
{
	u8 content[GPON_PLOAM_CONTENT_LEN] = {};

	memcpy(content, pp->passwd, 10);
	ploam_send(pp, GPON_PLOAM_UP_PASSWORD, content, PLOAM_REPEATED_MSG_REPS);
}

static void ploam_send_dying_gasp(struct ploam_priv *pp)
{
	u8 content[GPON_PLOAM_CONTENT_LEN] = {};

	ploam_send(pp, GPON_PLOAM_UP_DYING_GASP, content,
		   PLOAM_REPEATED_MSG_REPS);
}

static void ploam_send_rei(struct ploam_priv *pp, u32 bip_count)
{
	u8 content[GPON_PLOAM_CONTENT_LEN] = {};

	put_unaligned_be32(bip_count, &content[0]);
	/* content[4] bits[3:0] = 4-bit sequence number */
	content[4] = pp->rei_seq & 0x0F;
	pp->rei_seq = (pp->rei_seq + 1) & 0x0F;
	ploam_send(pp, GPON_PLOAM_UP_REI, content, 1);
}

/* dm_id = downstream message type being acknowledged;
 * dm_bytes = first 9 bytes of the downstream message (ONU-ID .. content[6]) */
static void ploam_send_ack(struct ploam_priv *pp, u8 dm_id,
			   const u8 dm_bytes[9])
{
	u8 content[GPON_PLOAM_CONTENT_LEN] = {};

	content[0] = dm_id;
	memcpy(&content[1], dm_bytes, 9 > (GPON_PLOAM_CONTENT_LEN - 1) ?
	       (GPON_PLOAM_CONTENT_LEN - 1) : 9);
	ploam_send(pp, GPON_PLOAM_UP_ACK, content, PLOAM_REPEATED_MSG_REPS);
}

/* Helper: build the 9-byte dm_bytes from a received message's onu_id,
 * type and content[0..6] for use in Acknowledge */
static void ploam_make_dm_bytes(u8 dm_bytes[9], u8 onu_id, u8 type,
				const u8 content[GPON_PLOAM_CONTENT_LEN])
{
	dm_bytes[0] = onu_id;
	dm_bytes[1] = type;
	memcpy(&dm_bytes[2], content, 7);
}

static void ploam_send_encrypt_key(struct ploam_priv *pp)
{
	u8 content[GPON_PLOAM_CONTENT_LEN] = {};
	int i;

	/* Two fragments of 8 key bytes each, as the Nokia/ALCL OLT expects:
	 * octet 3 (content[0]) = key_idx (0 or 1)
	 * octet 4 (content[1]) = frag_idx (0 or 1)
	 * octets 5..12 (content[2..9]) = 8 key bytes */
	for (i = 0; i < 2; i++) {
		content[0] = pp->key_idx;
		content[1] = i;
		memcpy(&content[2], pp->aes_key + i * 8, 8);
		ploam_send(pp, GPON_PLOAM_UP_ENCRYPT_KEY, content,
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

/* GPON_PLOAM_DOWN_UPSTREAM_OVERHEAD (0x01) — broadcast only */
static void handle_upstream_overhead(struct ploam_priv *pp,
				     u8 onu_id,
				     const u8 c[GPON_PLOAM_CONTENT_LEN])
{
	u8 delim[3];
	bool delay_mode;
	u16 delay_time;

	if (onu_id != GPON_PLOAM_ONU_BCAST)
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

/* GPON_PLOAM_DOWN_ASSIGN_ONU_ID (0x03) — broadcast only */
static void handle_assign_onu_id(struct ploam_priv *pp,
				 u8 onu_id,
				 const u8 c[GPON_PLOAM_CONTENT_LEN])
{
	if (onu_id != GPON_PLOAM_ONU_BCAST)
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

/* GPON_PLOAM_DOWN_RANGING_TIME (0x04) — unicast */
static void handle_ranging_time(struct ploam_priv *pp,
				const u8 c[GPON_PLOAM_CONTENT_LEN])
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

/* GPON_PLOAM_DOWN_DEACTIVATE_ONU_ID (0x05) — broadcast or unicast */
static void handle_deactivate_onu(struct ploam_priv *pp, u8 onu_id)
{
	if (onu_id != pp->onu_id && onu_id != GPON_PLOAM_ONU_BCAST)
		return;
	if (pp->state != GPON_O4_RANGING &&
	    pp->state != GPON_O5_OPERATION &&
	    pp->state != GPON_O6_POPUP)
		return;

	pp->ops->deactivate(pp->hw_priv);
}

/* GPON_PLOAM_DOWN_DISABLE_SN (0x06) — broadcast only */
static void handle_disable_sn(struct ploam_priv *pp,
			       const u8 c[GPON_PLOAM_CONTENT_LEN])
{
	/* c[0]=mode, c[1..8]=SN */
	u8 mode = c[0];

	if (pp->state == GPON_O7_EMERGENCY_STOP) {
		if (mode == GPON_PLOAM_DISABLE_PARTICIPATE_ALL ||
		    (mode == GPON_PLOAM_DISABLE_PARTICIPATE &&
		     memcmp(&c[1], pp->sn, 8) == 0)) {
			pp->emergency_state = false;
			/*
			 * Activation starts over from O2, without the ONU-ID,
			 * the equalization delay or the PLOAM history of the
			 * session the OLT stopped.
			 */
			ploam_reset(pp);
			ploam_start(pp);
		}
	} else if (pp->state != GPON_O1_INITIAL) {
		if (mode == GPON_PLOAM_DISABLE_DENIED_ALL ||
		    (mode == GPON_PLOAM_DISABLE_DENIED &&
		     memcmp(&c[1], pp->sn, 8) == 0)) {
			pp->emergency_state = true;
			ploam_set_state(pp, GPON_O7_EMERGENCY_STOP);
		}
	}
}

/* GPON_PLOAM_DOWN_ENCRYPTED_PORT_ID (0x08) — unicast */
static void handle_encrypted_port_id(struct ploam_priv *pp,
				      u8 onu_id, u8 type,
				      const u8 c[GPON_PLOAM_CONTENT_LEN])
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

/* GPON_PLOAM_DOWN_REQUEST_PASSWORD (0x09) — unicast */
static void handle_request_password(struct ploam_priv *pp, u8 onu_id)
{
	if (onu_id != pp->onu_id)
		return;
	if (pp->state == GPON_O5_OPERATION)
		ploam_send_password(pp);
}

/* GPON_PLOAM_DOWN_ASSIGN_ALLOC_ID (0x0A) — unicast */
static void handle_assign_alloc_id(struct ploam_priv *pp,
				    u8 onu_id, u8 type,
				    const u8 c[GPON_PLOAM_CONTENT_LEN])
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

/* GPON_PLOAM_DOWN_POPUP (0x0C) — broadcast or unicast */
static void handle_popup(struct ploam_priv *pp, u8 onu_id)
{
	if (onu_id != pp->onu_id && onu_id != GPON_PLOAM_ONU_BCAST)
		return;
	if (pp->state != GPON_O6_POPUP)
		return;

	if (onu_id == GPON_PLOAM_ONU_BCAST)
		ploam_set_state(pp, GPON_O4_RANGING);
	else
		ploam_set_state(pp, GPON_O5_OPERATION);
}

/* GPON_PLOAM_DOWN_REQUEST_KEY (0x0D) — unicast */
static void handle_request_key(struct ploam_priv *pp, u8 onu_id)
{
	if (onu_id != pp->onu_id)
		return;
	if (pp->state != GPON_O5_OPERATION)
		return;
	if (pp->key_exchange_pending)
		return;

	pp->key_exchange_pending = true;
	/* Hardware generates a random key and loads shadow regs */
	pp->ops->request_new_key(pp->hw_priv);
	/*
	 * Announce the key under the index the OLT is still using and only
	 * then flip: toggling first sends Encryption_Key with the index of a
	 * key the OLT has not been given yet.
	 */
	ploam_send_encrypt_key(pp);
	pp->key_idx ^= 1;
}

/* GPON_PLOAM_DOWN_CONFIGURE_PORT_ID (0x0E) — unicast */
static void handle_configure_port_id(struct ploam_priv *pp,
				      u8 onu_id, u8 type,
				      const u8 c[GPON_PLOAM_CONTENT_LEN])
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

/* GPON_PLOAM_DOWN_BER_INTERVAL (0x12) — broadcast or unicast */
static void handle_ber_interval(struct ploam_priv *pp,
				u8 onu_id, u8 type,
				const u8 c[GPON_PLOAM_CONTENT_LEN])
{
	u32 interval_frames;
	u32 interval_ms;
	u8 dm_bytes[9];

	if (onu_id != pp->onu_id && onu_id != GPON_PLOAM_ONU_BCAST)
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

/* GPON_PLOAM_DOWN_KEY_SWITCHING_TIME (0x13) — unicast */
static void handle_key_switching_time(struct ploam_priv *pp,
				       u8 onu_id, u8 type,
				       const u8 c[GPON_PLOAM_CONTENT_LEN])
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

/* GPON_PLOAM_DOWN_EXTENDED_BURST_LEN (0x14) — broadcast only, O3 only */
static void handle_extended_burst_length(struct ploam_priv *pp,
					  u8 onu_id,
					  const u8 c[GPON_PLOAM_CONTENT_LEN])
{
	if (onu_id != GPON_PLOAM_ONU_BCAST)
		return;
	if (pp->state != GPON_O3_SERIAL_NUMBER)
		return;

	/* c[0]=o3_t3_preamble, c[1]=o5_t3_preamble */
	pp->ops->set_t3_preamble(pp->hw_priv, c[0], c[1]);
}

/* GPON_PLOAM_DOWN_SWIFT_POPUP (0x16) — broadcast only */
static void handle_swift_popup(struct ploam_priv *pp, u8 onu_id)
{
	if (onu_id != GPON_PLOAM_ONU_BCAST)
		return;
	if (pp->state == GPON_O6_POPUP)
		ploam_set_state(pp, GPON_O5_OPERATION);
}

/* GPON_PLOAM_DOWN_RANGING_ADJUSTMENT (0x17) — unicast or broadcast */
static void handle_ranging_adjustment(struct ploam_priv *pp,
				       u8 onu_id, u8 type,
				       const u8 c[GPON_PLOAM_CONTENT_LEN])
{
	u32 eqd_offset;
	u8 s_bit;
	u8 dm_bytes[9];

	if (onu_id != pp->onu_id && onu_id != GPON_PLOAM_ONU_BCAST)
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
	pp->onu_id  = GPON_PLOAM_ONU_UNASSIGNED;

	memcpy(pp->sn, sn, sizeof(pp->sn));
	memcpy(pp->passwd, passwd, sizeof(pp->passwd));

	return pp;
}

void ploam_free(struct ploam_priv *pp)
{
	kfree(pp);
}

void ploam_set_identity(struct ploam_priv *pp, const u8 sn[8],
			const u8 passwd[10])
{
	memcpy(pp->sn, sn, sizeof(pp->sn));
	memcpy(pp->passwd, passwd, sizeof(pp->passwd));
}

void ploam_reset(struct ploam_priv *pp)
{
	pp->state                = GPON_O1_INITIAL;
	pp->onu_id               = GPON_PLOAM_ONU_UNASSIGNED;
	pp->key_exchange_pending = false;
	pp->rei_seq              = 0;
	pp->eqd                  = 0;
	pp->dedup_prev_valid     = false;
	pp->dedup_same_cnt       = 0;
	/* emergency_state is sticky: only cleared by Disable_SN PARTICIPATE */
}

void ploam_start(struct ploam_priv *pp)
{
	if (pp->state != GPON_O1_INITIAL)
		return;

	/*
	 * A session restart must not release an ONU that the OLT disabled:
	 * go back to O7, where Disable_Serial_Number can enable it again.
	 * Staying in O1 would ignore that message until the driver reloads.
	 */
	ploam_set_state(pp, pp->emergency_state ? GPON_O7_EMERGENCY_STOP :
						  GPON_O2_STANDBY);
}

void ploam_handle_downstream(struct ploam_priv *pp,
			     const struct airoha_ploam_msg *msg)
{
	u8 onu_id, type;
	u8 content[GPON_PLOAM_CONTENT_LEN];

	ploam_unpack(msg, &onu_id, &type, content);

	/* Drop messages not addressed to us */
	if (onu_id != GPON_PLOAM_ONU_BCAST && onu_id != pp->onu_id)
		return;

	/* EN7521 deduplication filter */
	if (ploam_filter_suppress(pp, msg, type))
		return;

	switch (type) {
	case GPON_PLOAM_DOWN_UPSTREAM_OVERHEAD:
		handle_upstream_overhead(pp, onu_id, content);
		break;
	case GPON_PLOAM_DOWN_ASSIGN_ONU_ID:
		handle_assign_onu_id(pp, onu_id, content);
		break;
	case GPON_PLOAM_DOWN_RANGING_TIME:
		handle_ranging_time(pp, content);
		break;
	case GPON_PLOAM_DOWN_DEACTIVATE_ONU_ID:
		handle_deactivate_onu(pp, onu_id);
		break;
	case GPON_PLOAM_DOWN_DISABLE_SN:
		handle_disable_sn(pp, content);
		break;
	case GPON_PLOAM_DOWN_ENCRYPTED_PORT_ID:
		handle_encrypted_port_id(pp, onu_id, type, content);
		break;
	case GPON_PLOAM_DOWN_REQUEST_PASSWORD:
		handle_request_password(pp, onu_id);
		break;
	case GPON_PLOAM_DOWN_ASSIGN_ALLOC_ID:
		handle_assign_alloc_id(pp, onu_id, type, content);
		break;
	case GPON_PLOAM_DOWN_POPUP:
		handle_popup(pp, onu_id);
		break;
	case GPON_PLOAM_DOWN_REQUEST_KEY:
		handle_request_key(pp, onu_id);
		break;
	case GPON_PLOAM_DOWN_CONFIGURE_PORT_ID:
		handle_configure_port_id(pp, onu_id, type, content);
		break;
	case GPON_PLOAM_DOWN_BER_INTERVAL:
		handle_ber_interval(pp, onu_id, type, content);
		break;
	case GPON_PLOAM_DOWN_KEY_SWITCHING_TIME:
		handle_key_switching_time(pp, onu_id, type, content);
		break;
	case GPON_PLOAM_DOWN_EXTENDED_BURST_LEN:
		handle_extended_burst_length(pp, onu_id, content);
		break;
	case GPON_PLOAM_DOWN_SWIFT_POPUP:
		handle_swift_popup(pp, onu_id);
		break;
	case GPON_PLOAM_DOWN_RANGING_ADJUSTMENT:
		handle_ranging_adjustment(pp, onu_id, type, content);
		break;
	case GPON_PLOAM_DOWN_PEE:
	case GPON_PLOAM_DOWN_PST:
	case GPON_PLOAM_DOWN_CHANGE_POWER_LEVEL:
	case GPON_PLOAM_DOWN_PON_ID:
	case GPON_PLOAM_DOWN_SLEEP_ALLOW:
		/* Stub: message acknowledged by presence in handler table */
		break;
	default:
		break;
	}
}

void ploam_notify_dying_gasp(struct ploam_priv *pp)
{
	/* An ONU in emergency stop must not transmit at all. */
	if (pp->state >= GPON_O2_STANDBY &&
	    pp->state != GPON_O7_EMERGENCY_STOP)
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

/* XGS-PON activation and PLOAM policy. */

/* Cache directed requests separately so interleaved broadcasts cannot cause
 * a retransmitted relative Ranging_Time to be applied for a second time.
 */
struct airoha_xgspon_reply {
	u8 request[XGSPON_PLOAM_BODY_LEN];
	struct xgspon_ploam_up reply;
	bool valid;
	bool has_reply;
	bool registered;
};

struct airoha_xgspon_ploam {
	const struct airoha_xgspon_ploam_ops *ops;
	void *priv;
	enum airoha_xgspon_state state;
	u16 onu_id;
	u32 eqd;
	u8 sn[XGSPON_PLOAM_SN_LEN];
	u8 reg_id[XGSPON_PLOAM_REG_ID_LEN];
	u8 profiles;
	bool running;
	bool ready;
	bool denied;
	bool registered;
	bool omcc_up;
	struct airoha_xgspon_reply cache[XGSPON_PLOAM_DOWN_MAX];
	struct airoha_xgspon_reply *reply_entry;
};

static void airoha_xgspon_publish(struct airoha_xgspon_ploam *pp)
{
	if (pp->ops->state_changed)
		pp->ops->state_changed(pp->priv, pp->state, pp->onu_id);
}

static int airoha_xgspon_state_set(struct airoha_xgspon_ploam *pp,
				   enum airoha_xgspon_state state)
{
	unsigned int timeout = 0;
	int ret;

	ret = pp->ops->set_state(pp->priv, state);
	if (ret)
		return ret;
	pp->state = state;
	if (state == AIROHA_XGSPON_O4)
		timeout = XGSPON_PLOAM_TO1_MS;
	else if (state == AIROHA_XGSPON_O6)
		timeout = AIROHA_XGSPON_TO2_MS;
	if (pp->ops->timer_changed)
		pp->ops->timer_changed(pp->priv, timeout);
	airoha_xgspon_publish(pp);
	return 0;
}

static int airoha_xgspon_reset_session(struct airoha_xgspon_ploam *pp)
{
	int ret;

	/* reset() owns all hardware teardown, including a partially set OMCC. */
	ret = pp->ops->reset(pp->priv);
	pp->onu_id = XGSPON_PLOAM_ONU_ID_UNASSIGNED;
	pp->eqd = 0;
	pp->profiles = 0;
	pp->registered = false;
	pp->omcc_up = false;
	memset(pp->cache, 0, sizeof(pp->cache));
	pp->reply_entry = NULL;
	pp->state = pp->denied ? AIROHA_XGSPON_O7 : AIROHA_XGSPON_O1;
	if (pp->ops->timer_changed)
		pp->ops->timer_changed(pp->priv, 0);
	if (ret)
		pp->running = false;
	else
		ret = pp->ops->set_state(pp->priv, pp->state);
	if (ret)
		pp->running = false;
	airoha_xgspon_publish(pp);
	return ret;
}

static int airoha_xgspon_send(struct airoha_xgspon_ploam *pp,
			      const struct xgspon_ploam_up *msg, bool registered)
{
	u8 body[XGSPON_PLOAM_BODY_LEN];
	int ret;

	ret = xgspon_ploam_up_build(body, sizeof(body), msg);
	if (ret < 0)
		return ret;
	return pp->ops->send_upstream(pp->priv, body, registered);
}

static int airoha_xgspon_reply(struct airoha_xgspon_ploam *pp,
			       const struct xgspon_ploam_up *msg, bool registered)
{
	/* Keep the response even when the TX queue is temporarily full. The
	 * next identical request must resend it without repeating side effects.
	 */
	if (pp->reply_entry) {
		pp->reply_entry->reply = *msg;
		pp->reply_entry->has_reply = true;
		pp->reply_entry->registered = registered;
	}
	return airoha_xgspon_send(pp, msg, registered);
}

static int airoha_xgspon_ack(struct airoha_xgspon_ploam *pp,
			     const struct xgspon_ploam_down *msg, u8 code)
{
	struct xgspon_ploam_up reply = {
		.onu_id = pp->onu_id,
		.msg_id = XGSPON_PLOAM_UP_ACKNOWLEDGE,
		.seq_no = msg->seq_no,
		.ack.code = code,
	};

	if (pp->denied || msg->onu_id != pp->onu_id ||
	    !xgspon_ploam_is_assignable(pp->onu_id))
		return 0;
	return airoha_xgspon_reply(pp, &reply, pp->registered);
}

static int airoha_xgspon_result(struct airoha_xgspon_ploam *pp,
				const struct xgspon_ploam_down *msg, int result)
{
	u8 code = XGSPON_PLOAM_ACK_OK;

	/* A busy/failed provider has not applied the operation. Let the OLT
	 * retry the same request after that condition clears.
	 */
	if (result)
		pp->reply_entry = NULL;
	if (result == -EINVAL || result == -ERANGE)
		code = XGSPON_PLOAM_ACK_PARAM_ERR;
	else if (result == -EBUSY || result == -EAGAIN)
		code = XGSPON_PLOAM_ACK_BUSY;
	else if (result == -EOPNOTSUPP)
		code = XGSPON_PLOAM_ACK_UNKNOWN_TYPE;
	else if (result)
		code = XGSPON_PLOAM_ACK_PROCESS_ERR;
	return airoha_xgspon_ack(pp, msg, code);
}

static int airoha_xgspon_profile(struct airoha_xgspon_ploam *pp,
				 const struct xgspon_ploam_down *msg)
{
	const struct xgspon_ploam_burst_profile *profile = &msg->burst_profile;
	int ret;

	if (pp->state != AIROHA_XGSPON_O2_3 && pp->state != AIROHA_XGSPON_O4 &&
	    pp->state != AIROHA_XGSPON_O5)
		return -EAGAIN;
	if (pp->state == AIROHA_XGSPON_O2_3 &&
	    !xgspon_ploam_profile_is_broadcast(msg->onu_id))
		return -EADDRNOTAVAIL;
	if (profile->line_rate != XGSPON_PLOAM_LINE_RATE_XGSPON)
		return -EOPNOTSUPP;
	ret = pp->ops->set_profile(pp->priv, profile);
	if (!ret)
		pp->profiles |= BIT(profile->index);
	return airoha_xgspon_result(pp, msg, ret);
}

static int airoha_xgspon_assign_onu(struct airoha_xgspon_ploam *pp,
				    const struct xgspon_ploam_down *msg)
{
	const struct xgspon_ploam_assign_onu_id *assign = &msg->assign_onu_id;
	int ret;

	if (!xgspon_ploam_is_broadcast(msg->onu_id) ||
	    memcmp(assign->sn, pp->sn, sizeof(pp->sn)))
		return -EADDRNOTAVAIL;
	if (!xgspon_ploam_is_assignable(assign->onu_id))
		return -EINVAL;
	if (pp->state == AIROHA_XGSPON_O4 || pp->state == AIROHA_XGSPON_O5) {
		if (assign->onu_id == pp->onu_id)
			return 0;
		/* A conflicting assignment invalidates the previous registration. */
		return airoha_xgspon_reset_session(pp);
	}
	if (pp->state != AIROHA_XGSPON_O2_3 || !pp->profiles)
		return -EAGAIN;
	ret = pp->ops->set_onu_id(pp->priv, assign->onu_id);
	if (ret)
		goto reset;
	ret = pp->ops->set_alloc_id(pp->priv, assign->onu_id, true);
	if (ret)
		goto reset;
	pp->onu_id = assign->onu_id;
	ret = airoha_xgspon_state_set(pp, AIROHA_XGSPON_O4);
	if (!ret)
		return 0;
reset:
	airoha_xgspon_reset_session(pp);
	return ret;
}

static int airoha_xgspon_operational(struct airoha_xgspon_ploam *pp)
{
	int ret;

	/* Apply activation and the default OMCC before publishing O5. */
	ret = pp->ops->set_state(pp->priv, AIROHA_XGSPON_O5);
	if (ret)
		return ret;
	ret = pp->ops->set_omcc(pp->priv, pp->onu_id, true);
	if (ret) {
		airoha_xgspon_reset_session(pp);
		return ret;
	}
	pp->omcc_up = true;
	pp->state = AIROHA_XGSPON_O5;
	if (pp->ops->timer_changed)
		pp->ops->timer_changed(pp->priv, 0);
	airoha_xgspon_publish(pp);
	return 0;
}

static int airoha_xgspon_ranging(struct airoha_xgspon_ploam *pp,
				 const struct xgspon_ploam_down *msg)
{
	const struct xgspon_ploam_ranging_time *range = &msg->ranging_time;
	u32 eqd;
	int ret;

	if (pp->state != AIROHA_XGSPON_O4 && pp->state != AIROHA_XGSPON_O5)
		return -EAGAIN;
	if (range->absolute) {
		if (msg->onu_id != pp->onu_id)
			return -EADDRNOTAVAIL;
		eqd = range->eqd;
	} else {
		if (pp->state != AIROHA_XGSPON_O5)
			return -EAGAIN;
		if (range->positive) {
			if (check_add_overflow(pp->eqd, range->eqd, &eqd))
				return airoha_xgspon_result(pp, msg, -ERANGE);
		} else if (check_sub_overflow(pp->eqd, range->eqd, &eqd)) {
			return airoha_xgspon_result(pp, msg, -ERANGE);
		}
	}
	ret = pp->ops->set_eqd(pp->priv, eqd);
	if (ret)
		return airoha_xgspon_result(pp, msg, ret);
	pp->eqd = eqd;
	if (pp->state == AIROHA_XGSPON_O4) {
		ret = airoha_xgspon_operational(pp);
		if (ret)
			return ret;
	}
	return airoha_xgspon_ack(pp, msg, XGSPON_PLOAM_ACK_OK);
}

static int airoha_xgspon_disable(struct airoha_xgspon_ploam *pp,
				 const struct xgspon_ploam_down *msg)
{
	const struct xgspon_ploam_disable_sn *disable = &msg->disable_sn;
	bool match = !memcmp(disable->sn, pp->sn, sizeof(pp->sn));

	switch (disable->mode) {
	case XGSPON_PLOAM_DISABLE_DENY_ONE:
		if (!match)
			return -EADDRNOTAVAIL;
		fallthrough;
	case XGSPON_PLOAM_DISABLE_DENY_ALL:
		pp->denied = true;
		if (pp->omcc_up) {
			int ret = pp->ops->set_omcc(pp->priv, pp->onu_id, false);

			if (ret)
				return airoha_xgspon_reset_session(pp);
			pp->omcc_up = false;
		}
		/* Retain the ONU-ID to accept a directed enable while in O7. */
		if (airoha_xgspon_state_set(pp, AIROHA_XGSPON_O7))
			return airoha_xgspon_reset_session(pp);
		return 0;
	case XGSPON_PLOAM_DISABLE_ALLOW_ONE:
		if (!match)
			return -EADDRNOTAVAIL;
		fallthrough;
	case XGSPON_PLOAM_DISABLE_ALLOW_ALL:
		if (!pp->denied)
			return 0;
		pp->denied = false;
		return airoha_xgspon_reset_session(pp);
	default:
		return airoha_xgspon_result(pp, msg, -EINVAL);
	}
}

static int airoha_xgspon_alloc_id(struct airoha_xgspon_ploam *pp,
				  const struct xgspon_ploam_down *msg)
{
	const struct xgspon_ploam_assign_alloc_id *alloc = &msg->assign_alloc_id;
	bool valid = alloc->type == XGSPON_PLOAM_ALLOC_ASSIGN;
	int ret;

	if (pp->state != AIROHA_XGSPON_O5 || msg->onu_id != pp->onu_id)
		return -EAGAIN;
	if (alloc->alloc_id == pp->onu_id && valid)
		return airoha_xgspon_ack(pp, msg, XGSPON_PLOAM_ACK_OK);
	if (!xgspon_ploam_alloc_is_assignable(alloc->alloc_id) ||
	    (!valid && alloc->type != XGSPON_PLOAM_ALLOC_DEALLOCATE))
		return airoha_xgspon_result(pp, msg, -EINVAL);
	ret = pp->ops->set_alloc_id(pp->priv, alloc->alloc_id, valid);
	return airoha_xgspon_result(pp, msg, ret);
}

static int airoha_xgspon_key(struct airoha_xgspon_ploam *pp,
			     const struct xgspon_ploam_down *msg)
{
	const struct xgspon_ploam_key_control *key = &msg->key_control;
	struct xgspon_ploam_up reply = {
		.onu_id = pp->onu_id,
		.msg_id = XGSPON_PLOAM_UP_KEY_REPORT,
		.seq_no = msg->seq_no,
	};
	int ret;

	if (pp->state != AIROHA_XGSPON_O5)
		return -EAGAIN;
	if (key->key_length != XGSPON_PLOAM_KEY_LEN_AES128 ||
	    key->key_index < XGSPON_PLOAM_KEY_INDEX_FIRST ||
	    key->key_index > XGSPON_PLOAM_KEY_INDEX_SECOND)
		return airoha_xgspon_result(pp, msg, -EINVAL);
	if (!pp->ops->key_report)
		return airoha_xgspon_result(pp, msg, -EOPNOTSUPP);
	ret = pp->ops->key_report(pp->priv, key, &reply.key_report);
	if (ret)
		return airoha_xgspon_result(pp, msg, ret);
	/* A new AES-128 key is wrapped into 16 octets; a key name is also 16. */
	if (reply.key_report.len != XGSPON_PLOAM_KEY_LEN_AES128 ||
	    reply.key_report.index != key->key_index ||
	    reply.key_report.num != XGSPON_PLOAM_KEY_FRAGMENT_FIRST ||
	    reply.key_report.type != key->control)
		return airoha_xgspon_result(pp, msg, -EINVAL);
	return airoha_xgspon_reply(pp, &reply, pp->registered);
}

static int airoha_xgspon_reboot(struct airoha_xgspon_ploam *pp,
				const struct xgspon_ploam_down *msg)
{
	const struct xgspon_ploam_reboot *reboot = &msg->reboot;
	static const u8 zero_sn[XGSPON_PLOAM_SN_LEN];

	if (xgspon_ploam_is_broadcast(msg->onu_id) &&
	    memcmp(reboot->sn, pp->sn, sizeof(pp->sn)) &&
	    memcmp(reboot->sn, zero_sn, sizeof(zero_sn)))
		return -EADDRNOTAVAIL;
	if (reboot->depth > XGSPON_PLOAM_REBOOT_DEPTH_MAX ||
	    reboot->image > XGSPON_PLOAM_REBOOT_IMAGE_MAX ||
	    reboot->state > XGSPON_PLOAM_REBOOT_STATE_INACTIVE_ONLY ||
	    reboot->flags > XGSPON_PLOAM_REBOOT_CALLS_MASK)
		return airoha_xgspon_result(pp, msg, -EINVAL);
	if (reboot->state && pp->state == AIROHA_XGSPON_O5)
		return 0;
	if (!pp->ops->reboot)
		return airoha_xgspon_result(pp, msg, -EOPNOTSUPP);
	return pp->ops->reboot(pp->priv, reboot);
}

static int airoha_xgspon_dispatch(struct airoha_xgspon_ploam *pp,
				  const struct xgspon_ploam_down *msg)
{
	switch (msg->msg_id) {
	case XGSPON_PLOAM_DOWN_BURST_PROFILE:
		return airoha_xgspon_profile(pp, msg);
	case XGSPON_PLOAM_DOWN_ASSIGN_ONU_ID:
		return airoha_xgspon_assign_onu(pp, msg);
	case XGSPON_PLOAM_DOWN_RANGING_TIME:
		return airoha_xgspon_ranging(pp, msg);
	case XGSPON_PLOAM_DOWN_DEACTIVATE:
		return airoha_xgspon_reset_session(pp);
	case XGSPON_PLOAM_DOWN_DISABLE_SN:
		return airoha_xgspon_disable(pp, msg);
	case XGSPON_PLOAM_DOWN_REQUEST_REG:
		if (pp->state != AIROHA_XGSPON_O5 || msg->onu_id != pp->onu_id)
			return -EAGAIN;
		return airoha_xgspon_ploam_registration(pp, msg->seq_no);
	case XGSPON_PLOAM_DOWN_ASSIGN_ALLOC_ID:
		return airoha_xgspon_alloc_id(pp, msg);
	case XGSPON_PLOAM_DOWN_KEY_CONTROL:
		return airoha_xgspon_key(pp, msg);
	case XGSPON_PLOAM_DOWN_REBOOT_ONU:
		return airoha_xgspon_reboot(pp, msg);
	default:
		return airoha_xgspon_result(pp, msg, -EOPNOTSUPP);
	}
}

int airoha_xgspon_ploam_receive(struct airoha_xgspon_ploam *pp,
				const void *body, size_t len, bool mic_valid)
{
	struct airoha_xgspon_reply *entry = NULL;
	struct xgspon_ploam_down msg;
	bool directed;
	int ret;

	if (!body || len != XGSPON_PLOAM_BODY_LEN)
		return -EMSGSIZE;
	if (!mic_valid)
		return -EBADMSG;
	if (!pp->running || !pp->ready || pp->state == AIROHA_XGSPON_O1)
		return -ENOLINK;
	ret = xgspon_ploam_down_parse(body, len, &msg);
	directed = xgspon_ploam_is_assignable(pp->onu_id) && msg.onu_id == pp->onu_id;
	if (!directed && !xgspon_ploam_is_broadcast(msg.onu_id) &&
	    !(msg.msg_id == XGSPON_PLOAM_DOWN_BURST_PROFILE &&
	      xgspon_ploam_profile_is_broadcast(msg.onu_id)))
		return -EADDRNOTAVAIL;
	if (pp->state == AIROHA_XGSPON_O7 &&
	    msg.msg_id != XGSPON_PLOAM_DOWN_DISABLE_SN)
		return -EACCES;
	if (ret)
		return airoha_xgspon_result(pp, &msg, ret);

	if (directed && msg.seq_no && msg.msg_id < ARRAY_SIZE(pp->cache)) {
		entry = &pp->cache[msg.msg_id];
		if (entry->valid && !memcmp(entry->request, body, len)) {
			if (entry->has_reply) {
				ret = airoha_xgspon_send(pp, &entry->reply, entry->registered);
				if (!ret && entry->reply.msg_id == XGSPON_PLOAM_UP_REGISTRATION)
					pp->registered = true;
				return ret;
			}
			return 0;
		}
		memset(entry, 0, sizeof(*entry));
		pp->reply_entry = entry;
	}
	ret = airoha_xgspon_dispatch(pp, &msg);
	/* A reset may have invalidated the cache while dispatching. */
	if (entry && pp->reply_entry == entry && (!ret || entry->has_reply)) {
		memcpy(entry->request, body, len);
		entry->valid = true;
	}
	pp->reply_entry = NULL;
	return ret;
}

int airoha_xgspon_ploam_serial_number(struct airoha_xgspon_ploam *pp,
				      u32 random_delay)
{
	struct xgspon_ploam_up msg = {
		.onu_id = XGSPON_PLOAM_ONU_ID_UNASSIGNED,
		.msg_id = XGSPON_PLOAM_UP_SERIAL_NUMBER,
		.sn.random_delay = random_delay,
	};

	if (!pp->running || !pp->ready || pp->state != AIROHA_XGSPON_O2_3 ||
	    !pp->profiles)
		return -ENOLINK;
	memcpy(msg.sn.sn, pp->sn, sizeof(pp->sn));
	return airoha_xgspon_send(pp, &msg, false);
}

int airoha_xgspon_ploam_registration(struct airoha_xgspon_ploam *pp, u8 seq_no)
{
	struct xgspon_ploam_up msg = {
		.onu_id = pp->onu_id,
		.msg_id = XGSPON_PLOAM_UP_REGISTRATION,
		.seq_no = seq_no,
	};
	int ret;

	if (!pp->running || !pp->ready ||
	    (pp->state != AIROHA_XGSPON_O4 && pp->state != AIROHA_XGSPON_O5))
		return -ENOLINK;
	memcpy(msg.registration.reg_id, pp->reg_id, sizeof(pp->reg_id));
	ret = airoha_xgspon_reply(pp, &msg, true);
	if (!ret)
		pp->registered = true;
	return ret;
}

int airoha_xgspon_ploam_reset(struct airoha_xgspon_ploam *pp)
{
	if (!pp->running)
		return -ENETDOWN;
	return airoha_xgspon_reset_session(pp);
}

int airoha_xgspon_ploam_registration_sent(struct airoha_xgspon_ploam *pp)
{
	if (!pp->running || !pp->ready || pp->denied ||
	    (pp->state != AIROHA_XGSPON_O4 && pp->state != AIROHA_XGSPON_O5))
		return -ENOLINK;
	pp->registered = true;
	return 0;
}

struct airoha_xgspon_ploam *
airoha_xgspon_ploam_alloc(const struct airoha_xgspon_ploam_ops *ops, void *priv,
			  const u8 sn[XGSPON_PLOAM_SN_LEN],
			 const u8 reg_id[XGSPON_PLOAM_REG_ID_LEN])
{
	struct airoha_xgspon_ploam *pp;

	if (!ops || !sn || !ops->reset || !ops->set_state || !ops->set_profile ||
	    !ops->set_onu_id || !ops->set_eqd || !ops->set_alloc_id ||
	    !ops->set_omcc || !ops->send_upstream)
		return ERR_PTR(-EINVAL);
	pp = kzalloc(sizeof(*pp), GFP_KERNEL);
	if (!pp)
		return ERR_PTR(-ENOMEM);
	pp->ops = ops;
	pp->priv = priv;
	pp->state = AIROHA_XGSPON_O1;
	pp->onu_id = XGSPON_PLOAM_ONU_ID_UNASSIGNED;
	memcpy(pp->sn, sn, sizeof(pp->sn));
	if (reg_id)
		memcpy(pp->reg_id, reg_id, sizeof(pp->reg_id));
	return pp;
}

void airoha_xgspon_ploam_free(struct airoha_xgspon_ploam *pp)
{
	kfree_sensitive(pp);
}

int airoha_xgspon_ploam_start(struct airoha_xgspon_ploam *pp)
{
	if (pp->running)
		return 0;
	pp->running = true;
	pp->ready = false;
	return airoha_xgspon_reset_session(pp);
}

int airoha_xgspon_ploam_stop(struct airoha_xgspon_ploam *pp)
{
	pp->running = false;
	pp->ready = false;
	return airoha_xgspon_reset_session(pp);
}

int airoha_xgspon_ploam_link(struct airoha_xgspon_ploam *pp, bool ready)
{
	int ret;

	if (!pp->running)
		return -ENETDOWN;
	pp->ready = ready;
	if (pp->denied)
		return 0;
	if (ready) {
		if (pp->state == AIROHA_XGSPON_O1)
			return airoha_xgspon_state_set(pp, AIROHA_XGSPON_O2_3);
		if (pp->state == AIROHA_XGSPON_O6)
			return airoha_xgspon_operational(pp);
		return 0;
	}
	if (pp->state == AIROHA_XGSPON_O5) {
		ret = pp->ops->set_omcc(pp->priv, pp->onu_id, false);
		if (ret)
			return airoha_xgspon_reset_session(pp);
		pp->omcc_up = false;
		ret = airoha_xgspon_state_set(pp, AIROHA_XGSPON_O6);
		if (ret)
			airoha_xgspon_reset_session(pp);
		return ret;
	}
	if (pp->state == AIROHA_XGSPON_O6 || pp->state == AIROHA_XGSPON_O1)
		return 0;
	return airoha_xgspon_reset_session(pp);
}

int airoha_xgspon_ploam_timeout(struct airoha_xgspon_ploam *pp)
{
	if (!pp->running)
		return -ENETDOWN;
	if (pp->state != AIROHA_XGSPON_O4 && pp->state != AIROHA_XGSPON_O6)
		return 0;
	return airoha_xgspon_reset_session(pp);
}

enum airoha_xgspon_state
airoha_xgspon_ploam_state(const struct airoha_xgspon_ploam *pp)
{
	return pp->state;
}

u16 airoha_xgspon_ploam_onu_id(const struct airoha_xgspon_ploam *pp)
{
	return pp->onu_id;
}

u32 airoha_xgspon_ploam_eqd(const struct airoha_xgspon_ploam *pp)
{
	return pp->eqd;
}

MODULE_DESCRIPTION("Airoha GPON and XGS-PON PLOAM protocol engines");
MODULE_LICENSE("GPL");
