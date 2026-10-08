// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (C) 2026 John Crispin <john@phrozen.org> */

#include <linux/errno.h>
#include <linux/export.h>
#include <linux/string.h>
#include <linux/unaligned.h>
#include <net/xpon/ploam_msg.h>

/**
 * gpon_ploam_build() - lay out a GPON body without its BIP-8
 * @buf: destination for the twelve standard body octets
 * @len: available space
 * @msg: header and content to send
 *
 * The caller owns FIFO ordering, BIP-8 and protocol policy.
 *
 * Return: GPON_PLOAM_BODY_LEN, or -ENOSPC without modifying @buf.
 */
int gpon_ploam_build(void *buf, size_t len, const struct gpon_ploam_msg *msg)
{
	u8 *body = buf;

	if (len < GPON_PLOAM_BODY_LEN)
		return -ENOSPC;

	body[0] = msg->onu_id;
	body[1] = msg->msg_id;
	memcpy(body + 2, msg->content, GPON_PLOAM_CONTENT_LEN);

	return GPON_PLOAM_BODY_LEN;
}
EXPORT_SYMBOL_GPL(gpon_ploam_build);

/**
 * gpon_ploam_parse() - read a GPON body without its BIP-8
 * @buf: standard body bytes, after the MAC checked BIP-8
 * @len: available bytes
 * @msg: decoded header and content
 *
 * Return: 0, or -ENOSPC without modifying @msg.
 */
int gpon_ploam_parse(const void *buf, size_t len, struct gpon_ploam_msg *msg)
{
	const u8 *body = buf;

	if (len < GPON_PLOAM_BODY_LEN)
		return -ENOSPC;

	msg->onu_id = body[0];
	msg->msg_id = body[1];
	memcpy(msg->content, body + 2, GPON_PLOAM_CONTENT_LEN);

	return 0;
}
EXPORT_SYMBOL_GPL(gpon_ploam_parse);

/* Offsets within the standard body. The ONU-ID is big endian and the content
 * follows the four byte header. G.9807.1 Table C.11.1.
 */
#define XGSPON_PLOAM_OFF_ONU_ID	0
#define XGSPON_PLOAM_OFF_MSG_ID	2
#define XGSPON_PLOAM_OFF_SEQ_NO	3
#define XGSPON_PLOAM_OFF_CONTENT	4

/* Serial_Number_ONU content, G.9807.1 Table C.11.24. */
#define XGSPON_PLOAM_SN_OFF_SN		0
#define XGSPON_PLOAM_SN_OFF_DELAY		8
#define XGSPON_PLOAM_SN_OFF_CAPABILITY	32
#define XGSPON_PLOAM_SN_CAPABILITY		(XGSPON_PLOAM_SN_RATE_10G | \
					 XGSPON_PLOAM_SN_RATE_NO_2G5)

/* Registration content, G.9807.1 Table C.11.25. */
#define XGSPON_PLOAM_REG_OFF_ID		0

/* Key_Report content, G.9807.1 Table C.11.26. */
#define XGSPON_PLOAM_KR_OFF_TYPE		0
#define XGSPON_PLOAM_KR_OFF_INDEX		1
#define XGSPON_PLOAM_KR_OFF_NUM		2
#define XGSPON_PLOAM_KR_OFF_FRAGMENT	4

/* Acknowledgment content, G.9807.1 Table C.11.27. */
#define XGSPON_PLOAM_ACK_OFF_CODE		0

/* Downstream content offsets, checked against the message tables of
 * G.9807.1 clause C.11.3.3: Key_Control is Table C.11.12 and Reboot_ONU is
 * Table C.11.23A.
 */
/* Burst_Profile, G.9807.1 Table C.11.4. */
#define XGSPON_PLOAM_BP_OFF_FLAGS		0
#define XGSPON_PLOAM_BP_OFF_FEC		1
#define XGSPON_PLOAM_BP_OFF_DELIM_LEN	2
#define XGSPON_PLOAM_BP_OFF_DELIM		3
#define XGSPON_PLOAM_BP_OFF_PRE_LEN	11
#define XGSPON_PLOAM_BP_OFF_PRE_REPEAT	12
#define XGSPON_PLOAM_BP_OFF_PRE		13
#define XGSPON_PLOAM_BP_OFF_PON_TAG	21

/* Assign_ONU-ID, G.9807.1 Table C.11.6. */
#define XGSPON_PLOAM_AOI_OFF_ID		0
#define XGSPON_PLOAM_AOI_OFF_SN		2
#define XGSPON_PLOAM_AOI_OFF_RATE		10

/* Ranging_Time, G.9807.1 Table C.11.7. */
#define XGSPON_PLOAM_RT_OFF_FLAGS		0
#define XGSPON_PLOAM_RT_OFF_EQD		1

/* Disable_Serial_Number, G.9807.1 Table C.11.9. */
#define XGSPON_PLOAM_DSN_OFF_MODE		0
#define XGSPON_PLOAM_DSN_OFF_SN		1

/* Assign_Alloc-ID, G.9807.1 Table C.11.11. */
#define XGSPON_PLOAM_AAI_OFF_ID		0
#define XGSPON_PLOAM_AAI_OFF_TYPE		2

/* Key_Control, G.9807.1 Table C.11.12. */
#define XGSPON_PLOAM_KC_OFF_TYPE		1
#define XGSPON_PLOAM_KC_OFF_INDEX		2
#define XGSPON_PLOAM_KC_OFF_LENGTH		3

/* Reboot_ONU, G.9807.1 Table C.11.23A. */
#define XGSPON_PLOAM_RB_OFF_SN		0
#define XGSPON_PLOAM_RB_OFF_DEPTH		8
#define XGSPON_PLOAM_RB_OFF_IMAGE		9
#define XGSPON_PLOAM_RB_OFF_STATE		10
#define XGSPON_PLOAM_RB_OFF_FLAGS		11

/**
 * xgspon_ploam_up_build() - lay out one upstream PLOAM message
 * @buf:	where to write the standard body, at least XGSPON_PLOAM_BODY_LEN
 * @len:	the space available
 * @msg:	what to send
 *
 * Writes the ONU-ID, the message identifier, the sequence number and the
 * content. The caller owns everything around the body: any vendor prefix or
 * trailer, the message integrity check and the order the bytes reach the
 * hardware.
 *
 * The whole 36 byte content is cleared before the fields are written, so
 * every octet a message does not use goes out as 0x00. The key report's type,
 * index and fragment number are masked to their field widths.
 *
 * Serial_Number_ONU reports an ONU that supports the 9.95328 Gbit/s upstream
 * rate only: XGSPON_PLOAM_SN_RATE_10G and XGSPON_PLOAM_SN_RATE_NO_2G5 are set. It
 * always goes out with ONU-ID XGSPON_PLOAM_ONU_ID_UNASSIGNED and sequence number
 * 0, as Table C.11.24 fixes both. @msg->onu_id and @msg->seq_no are not used
 * for it.
 *
 * The layouts are ITU-T G.9807.1 Table C.11.24 for Serial_Number_ONU,
 * Table C.11.25 for Registration, Table C.11.26 for Key_Report and
 * Table C.11.27 for Acknowledgment.
 *
 * Return: the number of bytes written, -EINVAL for a message identifier the
 * codec does not build or an ONU-ID above XGSPON_PLOAM_ONU_ID_MASK, -ENOSPC for
 * a buffer that is too small, or -ERANGE for a key fragment longer than
 * XGSPON_PLOAM_KEY_FRAGMENT_LEN.
 */
int xgspon_ploam_up_build(void *buf, size_t len, const struct xgspon_ploam_up *msg)
{
	u8 *body = buf;
	u8 *content = body + XGSPON_PLOAM_OFF_CONTENT;
	u16 onu_id = msg->onu_id;
	u8 seq_no = msg->seq_no;

	if (len < XGSPON_PLOAM_BODY_LEN)
		return -ENOSPC;

	if (msg->onu_id > XGSPON_PLOAM_ONU_ID_MASK)
		return -EINVAL;

	switch (msg->msg_id) {
	case XGSPON_PLOAM_UP_SERIAL_NUMBER:
		onu_id = XGSPON_PLOAM_ONU_ID_UNASSIGNED;
		seq_no = 0;
		break;
	case XGSPON_PLOAM_UP_REGISTRATION:
	case XGSPON_PLOAM_UP_ACKNOWLEDGE:
		break;
	case XGSPON_PLOAM_UP_KEY_REPORT:
		if (msg->key_report.len > XGSPON_PLOAM_KEY_FRAGMENT_LEN)
			return -ERANGE;
		break;
	default:
		return -EINVAL;
	}

	put_unaligned_be16(onu_id, &body[XGSPON_PLOAM_OFF_ONU_ID]);
	body[XGSPON_PLOAM_OFF_MSG_ID] = msg->msg_id;
	body[XGSPON_PLOAM_OFF_SEQ_NO] = seq_no;
	memset(content, 0, XGSPON_PLOAM_CONTENT_LEN);

	switch (msg->msg_id) {
	case XGSPON_PLOAM_UP_SERIAL_NUMBER:
		memcpy(&content[XGSPON_PLOAM_SN_OFF_SN], msg->sn.sn,
		       XGSPON_PLOAM_SN_LEN);
		put_unaligned_be32(msg->sn.random_delay,
				   &content[XGSPON_PLOAM_SN_OFF_DELAY]);
		content[XGSPON_PLOAM_SN_OFF_CAPABILITY] = XGSPON_PLOAM_SN_CAPABILITY;
		break;

	case XGSPON_PLOAM_UP_REGISTRATION:
		memcpy(&content[XGSPON_PLOAM_REG_OFF_ID], msg->registration.reg_id,
		       XGSPON_PLOAM_REG_ID_LEN);
		break;

	case XGSPON_PLOAM_UP_KEY_REPORT:
		content[XGSPON_PLOAM_KR_OFF_TYPE] = msg->key_report.type & 1;
		content[XGSPON_PLOAM_KR_OFF_INDEX] = msg->key_report.index & 3;
		content[XGSPON_PLOAM_KR_OFF_NUM] = msg->key_report.num & 7;
		memcpy(&content[XGSPON_PLOAM_KR_OFF_FRAGMENT], msg->key_report.key,
		       msg->key_report.len);
		break;

	case XGSPON_PLOAM_UP_ACKNOWLEDGE:
		content[XGSPON_PLOAM_ACK_OFF_CODE] = msg->ack.code;
		break;
	}

	return XGSPON_PLOAM_BODY_LEN;
}
EXPORT_SYMBOL_GPL(xgspon_ploam_up_build);

/**
 * xgspon_ploam_down_parse() - read one downstream PLOAM message
 * @buf:	the standard body, at least XGSPON_PLOAM_BODY_LEN bytes
 * @len:	how much is there
 * @msg:	filled in on success
 *
 * Reads the header and, for the messages it knows, the content into the
 * matching member. It decides nothing: whether the message is addressed to
 * this ONU, whether the current state allows it and whether to acknowledge
 * it are all the caller's.
 *
 * The layouts are ITU-T G.9807.1 Table C.11.4 for Burst_Profile, Table C.11.6
 * for Assign_ONU-ID, Table C.11.7 for Ranging_Time, Table C.11.8 for
 * Deactivate_ONU-ID, Table C.11.9 for Disable_Serial_Number, Table C.11.10 for
 * Request_Registration, Table C.11.11 for Assign_Alloc-ID, Table C.11.12 for
 * Key_Control and Table C.11.23A for Reboot_ONU.
 *
 * The codec checks one range only: the delimiter and preamble lengths of
 * Burst_Profile. Every other field with a range in the Recommendation, such
 * as the Reboot_ONU depth or the key index, is the caller's to check.
 *
 * Return: 0, -ENOSPC for a short buffer, -EINVAL for a Burst_Profile whose
 * delimiter or preamble length is outside Table C.11.4, or -EOPNOTSUPP for a
 * message the codec does not decode, which a caller may count and ignore.
 * After -EINVAL and -EOPNOTSUPP, @msg->onu_id, @msg->msg_id and
 * @msg->seq_no are valid, so that the caller can acknowledge the message.
 * After -ENOSPC, @msg is not written.
 */
int xgspon_ploam_down_parse(const void *buf, size_t len,
			    struct xgspon_ploam_down *msg)
{
	const u8 *body = buf;
	const u8 *content = body + XGSPON_PLOAM_OFF_CONTENT;

	if (len < XGSPON_PLOAM_BODY_LEN)
		return -ENOSPC;

	memset(msg, 0, sizeof(*msg));
	msg->onu_id = get_unaligned_be16(&body[XGSPON_PLOAM_OFF_ONU_ID]) &
		      XGSPON_PLOAM_ONU_ID_MASK;
	msg->msg_id = body[XGSPON_PLOAM_OFF_MSG_ID];
	msg->seq_no = body[XGSPON_PLOAM_OFF_SEQ_NO];

	switch (msg->msg_id) {
	case XGSPON_PLOAM_DOWN_BURST_PROFILE: {
		struct xgspon_ploam_burst_profile *profile = &msg->burst_profile;
		u8 flags = content[XGSPON_PLOAM_BP_OFF_FLAGS];
		u8 repeat_mask;

		profile->index = flags & 3;
		profile->line_rate = (flags >> 2) & 1;
		if (profile->line_rate == XGSPON_PLOAM_LINE_RATE_XGSPON)
			repeat_mask = XGSPON_PLOAM_PREAMBLE_MASK_XGSPON;
		else
			repeat_mask = XGSPON_PLOAM_PREAMBLE_MASK_XGPON;
		profile->version = flags >> 4;
		profile->fec = content[XGSPON_PLOAM_BP_OFF_FEC] & 1;
		profile->delimiter_len =
			content[XGSPON_PLOAM_BP_OFF_DELIM_LEN] & 0xf;
		memcpy(profile->delimiter, &content[XGSPON_PLOAM_BP_OFF_DELIM],
		       XGSPON_PLOAM_BURST_PATTERN_LEN);
		profile->preamble_len = content[XGSPON_PLOAM_BP_OFF_PRE_LEN] & 0xf;
		profile->preamble_repeat =
			content[XGSPON_PLOAM_BP_OFF_PRE_REPEAT] & repeat_mask;
		memcpy(profile->preamble, &content[XGSPON_PLOAM_BP_OFF_PRE],
		       XGSPON_PLOAM_BURST_PATTERN_LEN);
		memcpy(profile->pon_tag, &content[XGSPON_PLOAM_BP_OFF_PON_TAG],
		       XGSPON_PLOAM_PON_TAG_LEN);
		if (profile->delimiter_len > XGSPON_PLOAM_BURST_DELIMITER_LEN_MAX)
			return -EINVAL;
		if (profile->preamble_len < XGSPON_PLOAM_BURST_PREAMBLE_LEN_MIN ||
		    profile->preamble_len > XGSPON_PLOAM_BURST_PREAMBLE_LEN_MAX)
			return -EINVAL;
		return 0;
	}

	case XGSPON_PLOAM_DOWN_ASSIGN_ONU_ID:
		msg->assign_onu_id.onu_id =
			get_unaligned_be16(&content[XGSPON_PLOAM_AOI_OFF_ID]) &
			XGSPON_PLOAM_ONU_ID_MASK;
		memcpy(msg->assign_onu_id.sn, &content[XGSPON_PLOAM_AOI_OFF_SN],
		       XGSPON_PLOAM_SN_LEN);
		msg->assign_onu_id.line_rate =
			content[XGSPON_PLOAM_AOI_OFF_RATE] & 1;
		return 0;

	case XGSPON_PLOAM_DOWN_RANGING_TIME: {
		u8 flags = content[XGSPON_PLOAM_RT_OFF_FLAGS];

		msg->ranging_time.absolute =
			(flags & 1) == XGSPON_PLOAM_EQD_ABSOLUTE;
		msg->ranging_time.positive =
			((flags >> 1) & 1) == XGSPON_PLOAM_EQD_POSITIVE;
		msg->ranging_time.eqd =
			get_unaligned_be32(&content[XGSPON_PLOAM_RT_OFF_EQD]);
		return 0;
	}

	case XGSPON_PLOAM_DOWN_DEACTIVATE:
		return 0;

	case XGSPON_PLOAM_DOWN_DISABLE_SN:
		msg->disable_sn.mode = content[XGSPON_PLOAM_DSN_OFF_MODE];
		memcpy(msg->disable_sn.sn, &content[XGSPON_PLOAM_DSN_OFF_SN],
		       XGSPON_PLOAM_SN_LEN);
		return 0;

	case XGSPON_PLOAM_DOWN_REQUEST_REG:
		return 0;

	case XGSPON_PLOAM_DOWN_ASSIGN_ALLOC_ID:
		msg->assign_alloc_id.alloc_id =
			get_unaligned_be16(&content[XGSPON_PLOAM_AAI_OFF_ID]) &
			0x3fff;
		msg->assign_alloc_id.type = content[XGSPON_PLOAM_AAI_OFF_TYPE];
		return 0;

	case XGSPON_PLOAM_DOWN_KEY_CONTROL:
		msg->key_control.control = content[XGSPON_PLOAM_KC_OFF_TYPE] & 1;
		msg->key_control.key_index =
			content[XGSPON_PLOAM_KC_OFF_INDEX] & 3;
		msg->key_control.key_length = content[XGSPON_PLOAM_KC_OFF_LENGTH];
		return 0;

	case XGSPON_PLOAM_DOWN_REBOOT_ONU:
		memcpy(msg->reboot.sn, &content[XGSPON_PLOAM_RB_OFF_SN],
		       XGSPON_PLOAM_SN_LEN);
		msg->reboot.depth = content[XGSPON_PLOAM_RB_OFF_DEPTH];
		msg->reboot.image = content[XGSPON_PLOAM_RB_OFF_IMAGE];
		msg->reboot.state = content[XGSPON_PLOAM_RB_OFF_STATE];
		msg->reboot.flags = content[XGSPON_PLOAM_RB_OFF_FLAGS];
		return 0;

	default:
		return -EOPNOTSUPP;
	}
}
EXPORT_SYMBOL_GPL(xgspon_ploam_down_parse);
