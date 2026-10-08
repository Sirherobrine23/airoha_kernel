// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (C) 2026 John Crispin <john@phrozen.org> */
/* Tests for the PLOAM message codec.
 *
 * The codec is a pure function of an ITU-T wire format, so it is the one part
 * of the subsystem whose correctness can be established without a PON. Every
 * offset below is the offset the standard gives, so a regression here is a
 * regression on the wire.
 */

#include <kunit/test.h>
#include <linux/module.h>
#include <net/xpon/ploam_msg.h>

static void gpon_ploam_wire_test(struct kunit *test)
{
	static const u8 wire[GPON_PLOAM_BODY_LEN] = {
		0xff, 0x03, 0x42, 0x4d, 0x54, 0x4b,
		0x47, 0x01, 0x23, 0x45, 0x67, 0x00,
	};
	struct gpon_ploam_msg msg;
	u8 storage[GPON_PLOAM_BODY_LEN + 2];

	KUNIT_ASSERT_EQ(test, gpon_ploam_parse(wire, sizeof(wire), &msg), 0);
	KUNIT_EXPECT_EQ(test, msg.onu_id, GPON_PLOAM_ONU_BCAST);
	KUNIT_EXPECT_EQ(test, msg.msg_id, GPON_PLOAM_DOWN_ASSIGN_ONU_ID);
	KUNIT_EXPECT_EQ(test, msg.content[0], 0x42);
	KUNIT_EXPECT_MEMEQ(test, msg.content + 1, "MTKG\x01\x23\x45\x67", 8);

	memset(storage, 0xa5, sizeof(storage));
	KUNIT_ASSERT_EQ(test,
			gpon_ploam_build(storage + 1, GPON_PLOAM_BODY_LEN, &msg),
			GPON_PLOAM_BODY_LEN);
	KUNIT_EXPECT_MEMEQ(test, storage + 1, wire, sizeof(wire));
	KUNIT_EXPECT_EQ(test, storage[0], 0xa5);
	KUNIT_EXPECT_EQ(test, storage[sizeof(storage) - 1], 0xa5);
	KUNIT_ASSERT_EQ(test,
			gpon_ploam_parse(storage + 1, GPON_PLOAM_BODY_LEN, &msg), 0);
	KUNIT_EXPECT_EQ(test, msg.content[0], 0x42);
}

static void gpon_ploam_short_buffer_test(struct kunit *test)
{
	struct gpon_ploam_msg msg, original;
	u8 body[GPON_PLOAM_BODY_LEN], original_body[GPON_PLOAM_BODY_LEN];

	memset(&msg, 0xa5, sizeof(msg));
	original = msg;
	memset(body, 0x5a, sizeof(body));
	memcpy(original_body, body, sizeof(body));

	KUNIT_EXPECT_EQ(test, gpon_ploam_build(body, sizeof(body) - 1, &msg),
			-ENOSPC);
	KUNIT_EXPECT_MEMEQ(test, body, original_body, sizeof(body));
	KUNIT_EXPECT_EQ(test, gpon_ploam_parse(body, sizeof(body) - 1, &msg),
			-ENOSPC);
	KUNIT_EXPECT_MEMEQ(test, &msg, &original, sizeof(msg));
}

static void xgspon_ploam_unaligned_test(struct kunit *test)
{
	struct xgspon_ploam_up up = {
		.onu_id = 0x102,
		.msg_id = XGSPON_PLOAM_UP_ACKNOWLEDGE,
		.seq_no = 0x77,
		.ack.code = XGSPON_PLOAM_ACK_OK,
	};
	struct xgspon_ploam_down down;
	u8 body[XGSPON_PLOAM_BODY_LEN + 2];

	memset(body, 0xa5, sizeof(body));
	KUNIT_ASSERT_EQ(test,
			xgspon_ploam_up_build(body + 1, XGSPON_PLOAM_BODY_LEN, &up),
			XGSPON_PLOAM_BODY_LEN);
	KUNIT_EXPECT_EQ(test, body[1], 0x01);
	KUNIT_EXPECT_EQ(test, body[2], 0x02);
	KUNIT_EXPECT_EQ(test, body[0], 0xa5);
	KUNIT_EXPECT_EQ(test, body[sizeof(body) - 1], 0xa5);

	body[3] = XGSPON_PLOAM_DOWN_RANGING_TIME;
	body[5] = 1;
	body[6] = 0x11;
	body[7] = 0x22;
	body[8] = 0x33;
	body[9] = 0x44;
	KUNIT_ASSERT_EQ(test,
			xgspon_ploam_down_parse(body + 1, XGSPON_PLOAM_BODY_LEN,
						&down), 0);
	KUNIT_EXPECT_EQ(test, down.onu_id, 0x102);
	KUNIT_EXPECT_EQ(test, down.seq_no, 0x77);
	KUNIT_EXPECT_EQ(test, down.ranging_time.eqd, 0x11223344);
}

static void xgspon_ploam_refusal_preserves_buffer_test(struct kunit *test)
{
	struct xgspon_ploam_up up = {
		.msg_id = XGSPON_PLOAM_UP_KEY_REPORT,
		.key_report.len = XGSPON_PLOAM_KEY_FRAGMENT_LEN + 1,
	};
	struct xgspon_ploam_down down, original;
	u8 body[XGSPON_PLOAM_BODY_LEN], original_body[XGSPON_PLOAM_BODY_LEN];

	memset(body, 0x5a, sizeof(body));
	memcpy(original_body, body, sizeof(body));
	memset(&down, 0xa5, sizeof(down));
	memcpy(&original, &down, sizeof(down));
	KUNIT_EXPECT_EQ(test, xgspon_ploam_up_build(body, sizeof(body), &up),
			-ERANGE);
	KUNIT_EXPECT_MEMEQ(test, body, original_body, sizeof(body));
	KUNIT_EXPECT_EQ(test,
			xgspon_ploam_down_parse(body, sizeof(body) - 1, &down),
			-ENOSPC);
	KUNIT_EXPECT_MEMEQ(test, &down, &original, sizeof(down));
}

/* The buffer a MAC hands the codec. The Airoha MAC prefixes four bytes, so
 * the numbers in the comments are offsets into that 44 byte buffer and the
 * codec writes from byte 4 on.
 */
#define T_PREFIX	4
#define T_BUF		(T_PREFIX + XGSPON_PLOAM_BODY_LEN)
#define T_OCTET(n)	(T_PREFIX + (n) - 1)
#define T_DIRT		0xee
#define T_FRAGMENT	(T_BUF - XGSPON_PLOAM_KEY_FRAGMENT_LEN)
#define T_KEY_PARTIAL_LEN	5

static const u8 t_zero[XGSPON_PLOAM_CONTENT_LEN];

static void ploam_sn_build_test(struct kunit *test)
{
	static const u8 sn[XGSPON_PLOAM_SN_LEN] = {
		0x4d, 0x54, 0x4b, 0x47, 0x00, 0x00, 0x00, 0x01,
	};
	struct xgspon_ploam_up up = {
		.onu_id = XGSPON_PLOAM_ONU_ID_UNASSIGNED,
		.msg_id = XGSPON_PLOAM_UP_SERIAL_NUMBER,
	};
	u8 body[T_BUF];

	memset(body, T_DIRT, sizeof(body));
	memcpy(up.sn.sn, sn, sizeof(sn));
	up.sn.random_delay = 0xaabbccdd;

	KUNIT_EXPECT_EQ(test,
			xgspon_ploam_up_build(body + T_PREFIX, XGSPON_PLOAM_BODY_LEN,
					      &up),
			XGSPON_PLOAM_BODY_LEN);

	KUNIT_EXPECT_EQ(test, body[4], 0x03);	/* ONU-ID high */
	KUNIT_EXPECT_EQ(test, body[5], 0xff);	/* ONU-ID low */
	KUNIT_EXPECT_EQ(test, body[6], XGSPON_PLOAM_UP_SERIAL_NUMBER);
	KUNIT_EXPECT_EQ(test, body[7], 0);		/* sequence number */
	KUNIT_EXPECT_MEMEQ(test, &body[8], sn, sizeof(sn));
	KUNIT_EXPECT_EQ(test, body[16], 0xaa);	/* random delay, big endian */
	KUNIT_EXPECT_EQ(test, body[17], 0xbb);
	KUNIT_EXPECT_EQ(test, body[18], 0xcc);
	KUNIT_EXPECT_EQ(test, body[19], 0xdd);
	KUNIT_EXPECT_EQ(test, body[T_OCTET(37)], 0x03);	/* capability */
	KUNIT_EXPECT_MEMEQ(test, &body[T_OCTET(17)], t_zero, 36 - 17 + 1);
	KUNIT_EXPECT_MEMEQ(test, &body[T_OCTET(38)], t_zero, 40 - 38 + 1);
	KUNIT_EXPECT_EQ(test, body[0], T_DIRT);
	KUNIT_EXPECT_EQ(test, body[T_PREFIX - 1], T_DIRT);
}

static void ploam_sn_capability_c_11_24_test(struct kunit *test)
{
	struct xgspon_ploam_up up = {
		.onu_id = XGSPON_PLOAM_ONU_ID_UNASSIGNED,
		.msg_id = XGSPON_PLOAM_UP_SERIAL_NUMBER,
	};
	u8 body[T_BUF] = {};

	KUNIT_EXPECT_EQ(test, XGSPON_PLOAM_SN_RATE_10G, 0x02);
	KUNIT_EXPECT_EQ(test, XGSPON_PLOAM_SN_RATE_NO_2G5, 0x01);
	KUNIT_ASSERT_EQ(test,
			xgspon_ploam_up_build(body + T_PREFIX, XGSPON_PLOAM_BODY_LEN,
					      &up),
			XGSPON_PLOAM_BODY_LEN);
	KUNIT_EXPECT_EQ(test, body[T_OCTET(37)],
			XGSPON_PLOAM_SN_RATE_10G | XGSPON_PLOAM_SN_RATE_NO_2G5);
}

static void ploam_registration_build_test(struct kunit *test)
{
	struct xgspon_ploam_up up = {
		.onu_id = 0x101,
		.msg_id = XGSPON_PLOAM_UP_REGISTRATION,
		.seq_no = 7,
	};
	u8 body[T_BUF] = {};

	memset(up.registration.reg_id, 0x5a, XGSPON_PLOAM_REG_ID_LEN);

	KUNIT_EXPECT_EQ(test,
			xgspon_ploam_up_build(body + T_PREFIX, XGSPON_PLOAM_BODY_LEN,
					      &up),
			XGSPON_PLOAM_BODY_LEN);

	KUNIT_EXPECT_EQ(test, body[4], 0x01);
	KUNIT_EXPECT_EQ(test, body[5], 0x01);
	KUNIT_EXPECT_EQ(test, body[6], XGSPON_PLOAM_UP_REGISTRATION);
	KUNIT_EXPECT_EQ(test, body[7], 7);
	/* The registration id fills the content to its last byte. */
	KUNIT_EXPECT_EQ(test, body[8], 0x5a);
	KUNIT_EXPECT_EQ(test, body[43], 0x5a);
}

static void ploam_key_report_build_test(struct kunit *test)
{
	struct xgspon_ploam_up up = {
		.onu_id = 0x101,
		.msg_id = XGSPON_PLOAM_UP_KEY_REPORT,
		.seq_no = 3,
	};
	u8 body[T_BUF] = {};

	up.key_report.type = XGSPON_PLOAM_KEY_REPORT_TYPE_EXISTING;
	up.key_report.index = XGSPON_PLOAM_KEY_INDEX_SECOND;
	up.key_report.num = 5;
	up.key_report.len = XGSPON_PLOAM_KEY_FRAGMENT_LEN;
	memset(up.key_report.key, 0xc3, XGSPON_PLOAM_KEY_FRAGMENT_LEN);

	KUNIT_EXPECT_EQ(test,
			xgspon_ploam_up_build(body + T_PREFIX, XGSPON_PLOAM_BODY_LEN,
					      &up),
			XGSPON_PLOAM_BODY_LEN);

	KUNIT_EXPECT_EQ(test, body[8], 1);		/* type */
	KUNIT_EXPECT_EQ(test, body[9], 2);		/* key index */
	KUNIT_EXPECT_EQ(test, XGSPON_PLOAM_KEY_REPORT_TYPE_NEW, 0);
	KUNIT_EXPECT_EQ(test, body[10], 5);	/* fragment number */
	KUNIT_EXPECT_EQ(test, body[12], 0xc3);	/* fragment starts at 12 */
	KUNIT_EXPECT_EQ(test, body[43], 0xc3);
	/* byte 11 is between the header and the fragment and stays clear */
	KUNIT_EXPECT_EQ(test, body[11], 0);
}

/* The three narrowed fields are masked, not truncated by the caller. */
static void ploam_key_report_mask_test(struct kunit *test)
{
	struct xgspon_ploam_up up = {
		.onu_id = 0x101,
		.msg_id = XGSPON_PLOAM_UP_KEY_REPORT,
	};
	u8 body[T_BUF] = {};

	up.key_report.type = 0xff;
	up.key_report.index = 0xff;
	up.key_report.num = 0xff;

	KUNIT_EXPECT_EQ(test,
			xgspon_ploam_up_build(body + T_PREFIX, XGSPON_PLOAM_BODY_LEN,
					      &up),
			XGSPON_PLOAM_BODY_LEN);

	KUNIT_EXPECT_EQ(test, body[8], 1);		/* one bit */
	KUNIT_EXPECT_EQ(test, body[9], 3);		/* two bits */
	KUNIT_EXPECT_EQ(test, body[10], 7);	/* three bits */
}

static void ploam_ack_build_test(struct kunit *test)
{
	struct xgspon_ploam_up up = {
		.onu_id = 0x101,
		.msg_id = XGSPON_PLOAM_UP_ACKNOWLEDGE,
		.seq_no = 9,
		.ack.code = XGSPON_PLOAM_ACK_PROCESS_ERR,
	};
	u8 body[T_BUF];

	memset(body, T_DIRT, sizeof(body));
	KUNIT_EXPECT_EQ(test,
			xgspon_ploam_up_build(body + T_PREFIX, XGSPON_PLOAM_BODY_LEN,
					      &up),
			XGSPON_PLOAM_BODY_LEN);

	KUNIT_EXPECT_EQ(test, body[6], XGSPON_PLOAM_UP_ACKNOWLEDGE);
	KUNIT_EXPECT_EQ(test, body[7], 9);
	KUNIT_EXPECT_EQ(test, body[8], XGSPON_PLOAM_ACK_PROCESS_ERR);
	KUNIT_EXPECT_MEMEQ(test, &body[T_OCTET(6)], t_zero, 40 - 6 + 1);
}

static void ploam_build_refusal_test(struct kunit *test)
{
	struct xgspon_ploam_up up = {
		.onu_id = 0x101,
		.msg_id = XGSPON_PLOAM_UP_ACKNOWLEDGE,
	};
	u8 body[T_BUF] = {};

	KUNIT_EXPECT_EQ(test,
			xgspon_ploam_up_build(body, XGSPON_PLOAM_BODY_LEN - 1, &up),
			-ENOSPC);

	up.msg_id = 0x7f;
	KUNIT_EXPECT_EQ(test, xgspon_ploam_up_build(body, XGSPON_PLOAM_BODY_LEN, &up),
			-EINVAL);

	up.msg_id = XGSPON_PLOAM_UP_KEY_REPORT;
	up.key_report.len = XGSPON_PLOAM_KEY_FRAGMENT_LEN + 1;
	KUNIT_EXPECT_EQ(test, xgspon_ploam_up_build(body, XGSPON_PLOAM_BODY_LEN, &up),
			-ERANGE);
}

static void ploam_build_onu_id_range_test(struct kunit *test)
{
	struct xgspon_ploam_up up = {
		.onu_id = 0xffff,
		.msg_id = XGSPON_PLOAM_UP_ACKNOWLEDGE,
	};
	u8 body[T_BUF];

	memset(body, T_DIRT, sizeof(body));
	KUNIT_EXPECT_EQ(test, xgspon_ploam_up_build(body, XGSPON_PLOAM_BODY_LEN, &up),
			-EINVAL);
	KUNIT_EXPECT_EQ(test, body[0], T_DIRT);

	up.onu_id = XGSPON_PLOAM_ONU_ID_MASK + 1;
	KUNIT_EXPECT_EQ(test, xgspon_ploam_up_build(body, XGSPON_PLOAM_BODY_LEN, &up),
			-EINVAL);

	up.msg_id = XGSPON_PLOAM_UP_SERIAL_NUMBER;
	KUNIT_EXPECT_EQ(test, xgspon_ploam_up_build(body, XGSPON_PLOAM_BODY_LEN, &up),
			-EINVAL);

	up.onu_id = XGSPON_PLOAM_ONU_ID_MASK;
	up.msg_id = XGSPON_PLOAM_UP_ACKNOWLEDGE;
	KUNIT_EXPECT_EQ(test, xgspon_ploam_up_build(body, XGSPON_PLOAM_BODY_LEN, &up),
			XGSPON_PLOAM_BODY_LEN);
	KUNIT_EXPECT_EQ(test, body[0], 0x03);
	KUNIT_EXPECT_EQ(test, body[1], 0xff);
}

static void ploam_sn_header_c_11_24_test(struct kunit *test)
{
	struct xgspon_ploam_up up = {
		.onu_id = 0x101,
		.msg_id = XGSPON_PLOAM_UP_SERIAL_NUMBER,
		.seq_no = 7,
	};
	u8 body[T_BUF] = {};

	KUNIT_ASSERT_EQ(test,
			xgspon_ploam_up_build(body + T_PREFIX, XGSPON_PLOAM_BODY_LEN,
					      &up),
			XGSPON_PLOAM_BODY_LEN);
	KUNIT_EXPECT_EQ(test, body[T_OCTET(1)], 0x03);
	KUNIT_EXPECT_EQ(test, body[T_OCTET(2)], 0xff);
	KUNIT_EXPECT_EQ(test, body[T_OCTET(3)], XGSPON_PLOAM_UP_SERIAL_NUMBER);
	KUNIT_EXPECT_EQ(test, body[T_OCTET(4)], 0);
}

static void ploam_broadcast_test(struct kunit *test)
{
	KUNIT_EXPECT_TRUE(test,
			  xgspon_ploam_is_broadcast(XGSPON_PLOAM_ONU_ID_BROADCAST));
	KUNIT_EXPECT_FALSE(test, xgspon_ploam_is_broadcast(0));
	KUNIT_EXPECT_FALSE(test, xgspon_ploam_is_broadcast(0x101));
}

static void ploam_profile_broadcast_test(struct kunit *test)
{
	u16 profile = XGSPON_PLOAM_ONU_ID_PROFILE_BCAST_10G;
	u16 bcast = XGSPON_PLOAM_ONU_ID_BROADCAST;

	KUNIT_EXPECT_EQ(test, profile, 0x3fe);
	KUNIT_EXPECT_TRUE(test, xgspon_ploam_profile_is_broadcast(bcast));
	KUNIT_EXPECT_TRUE(test, xgspon_ploam_profile_is_broadcast(profile));
	KUNIT_EXPECT_FALSE(test, xgspon_ploam_profile_is_broadcast(0x3fd));
	KUNIT_EXPECT_FALSE(test, xgspon_ploam_profile_is_broadcast(0));
	KUNIT_EXPECT_FALSE(test, xgspon_ploam_profile_is_broadcast(0x101));
	KUNIT_EXPECT_FALSE(test, xgspon_ploam_is_broadcast(profile));
}

static void ploam_alloc_assignable_c_6_5_test(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, XGSPON_PLOAM_ALLOC_ID_SN_GRANT_10G, 1022);
	KUNIT_EXPECT_FALSE(test, xgspon_ploam_alloc_is_assignable(0));
	KUNIT_EXPECT_FALSE(test, xgspon_ploam_alloc_is_assignable(1020));
	KUNIT_EXPECT_FALSE(test, xgspon_ploam_alloc_is_assignable(1021));
	KUNIT_EXPECT_FALSE(test, xgspon_ploam_alloc_is_assignable(1022));
	KUNIT_EXPECT_FALSE(test, xgspon_ploam_alloc_is_assignable(1023));
	KUNIT_EXPECT_TRUE(test, xgspon_ploam_alloc_is_assignable(1024));
	KUNIT_EXPECT_TRUE(test, xgspon_ploam_alloc_is_assignable(16383));
	KUNIT_EXPECT_FALSE(test, xgspon_ploam_alloc_is_assignable(16384));
}

static void ploam_assignable_test(struct kunit *test)
{
	u16 reserved = XGSPON_PLOAM_ONU_ID_MAX + 1;
	u16 profile = XGSPON_PLOAM_ONU_ID_PROFILE_BCAST_10G;
	u16 bcast = XGSPON_PLOAM_ONU_ID_BROADCAST;

	KUNIT_EXPECT_TRUE(test, xgspon_ploam_is_assignable(0));
	KUNIT_EXPECT_TRUE(test, xgspon_ploam_is_assignable(XGSPON_PLOAM_ONU_ID_MAX));
	KUNIT_EXPECT_FALSE(test, xgspon_ploam_is_assignable(reserved));
	KUNIT_EXPECT_FALSE(test, xgspon_ploam_is_assignable(profile));
	KUNIT_EXPECT_FALSE(test, xgspon_ploam_is_assignable(bcast));
}

/* The downstream body has no vendor prefix: the ONU-ID is at 0, then the
 * message identifier, the sequence number and 36 bytes of content from 4.
 * Every offset asserted below is the offset the driver's own handlers read.
 */
static void ploam_down_header_test(struct kunit *test)
{
	struct xgspon_ploam_down msg;
	u8 body[XGSPON_PLOAM_BODY_LEN] = {};

	body[0] = 0x01; body[1] = 0x02;
	body[2] = XGSPON_PLOAM_DOWN_DEACTIVATE;
	body[3] = 0x2a;

	KUNIT_EXPECT_EQ(test, xgspon_ploam_down_parse(body, sizeof(body), &msg),
			0);
	KUNIT_EXPECT_EQ(test, msg.onu_id, 0x0102);
	KUNIT_EXPECT_EQ(test, msg.msg_id, XGSPON_PLOAM_DOWN_DEACTIVATE);
	KUNIT_EXPECT_EQ(test, msg.seq_no, 0x2a);

	/* the six high bits of the ONU-ID field are reserved and ignored */
	body[0] = 0xff;
	KUNIT_EXPECT_EQ(test, xgspon_ploam_down_parse(body, sizeof(body), &msg),
			0);
	KUNIT_EXPECT_EQ(test, msg.onu_id, 0x0302);
}

static void ploam_down_burst_profile_test(struct kunit *test)
{
	struct xgspon_ploam_down msg;
	u8 body[XGSPON_PLOAM_BODY_LEN] = {};

	body[2] = XGSPON_PLOAM_DOWN_BURST_PROFILE;
	body[4] = 0x56;		/* index 2, line rate 1, version 5 */
	body[5] = 0x01;		/* FEC on */
	body[6] = 0x08;		/* delimiter length */
	memset(&body[7], 0xd1, 8);	/* delimiter */
	body[15] = 0x07;		/* preamble length */
	body[16] = 0x20;		/* preamble repeat */
	memset(&body[17], 0xb1, 8);/* preamble */
	memset(&body[25], 0x7a, 8);/* PON tag */

	KUNIT_EXPECT_EQ(test, xgspon_ploam_down_parse(body, sizeof(body), &msg),
			0);
	KUNIT_EXPECT_EQ(test, msg.burst_profile.index, 2);
	KUNIT_EXPECT_EQ(test, msg.burst_profile.line_rate,
			XGSPON_PLOAM_LINE_RATE_XGSPON);
	KUNIT_EXPECT_EQ(test, msg.burst_profile.version, 5);
	KUNIT_EXPECT_EQ(test, msg.burst_profile.fec, 1);
	KUNIT_EXPECT_EQ(test, msg.burst_profile.delimiter_len, 8);
	KUNIT_EXPECT_EQ(test, msg.burst_profile.delimiter[0], 0xd1);
	KUNIT_EXPECT_EQ(test, msg.burst_profile.delimiter[7], 0xd1);
	KUNIT_EXPECT_EQ(test, msg.burst_profile.preamble_len, 7);
	KUNIT_EXPECT_EQ(test, msg.burst_profile.preamble_repeat, 0x20);
	KUNIT_EXPECT_EQ(test, msg.burst_profile.pon_tag[0], 0x7a);
	KUNIT_EXPECT_EQ(test, msg.burst_profile.pon_tag[7], 0x7a);
}

/* G.9807.1 Table C.11.4: delimiter length 0..8, preamble length 1..8. */
static void ploam_down_burst_profile_range_test(struct kunit *test)
{
	struct xgspon_ploam_down msg;
	u8 body[XGSPON_PLOAM_BODY_LEN] = {};

	body[2] = XGSPON_PLOAM_DOWN_BURST_PROFILE;
	body[6] = 0;
	body[15] = 1;
	KUNIT_EXPECT_EQ(test, xgspon_ploam_down_parse(body, sizeof(body), &msg),
			0);

	body[6] = 9;
	KUNIT_EXPECT_EQ(test, xgspon_ploam_down_parse(body, sizeof(body), &msg),
			-EINVAL);

	body[6] = 8;
	body[15] = 0;
	KUNIT_EXPECT_EQ(test, xgspon_ploam_down_parse(body, sizeof(body), &msg),
			-EINVAL);

	body[15] = 9;
	KUNIT_EXPECT_EQ(test, xgspon_ploam_down_parse(body, sizeof(body), &msg),
			-EINVAL);

	body[15] = 8;
	KUNIT_EXPECT_EQ(test, xgspon_ploam_down_parse(body, sizeof(body), &msg),
			0);
}

static void ploam_down_assign_onu_id_test(struct kunit *test)
{
	static const u8 sn[XGSPON_PLOAM_SN_LEN] = {
		0x4d, 0x54, 0x4b, 0x47, 0x00, 0x00, 0x00, 0x01,
	};
	struct xgspon_ploam_down msg;
	u8 body[XGSPON_PLOAM_BODY_LEN] = {};

	body[2] = XGSPON_PLOAM_DOWN_ASSIGN_ONU_ID;
	body[4] = 0x03;		/* only two bits are the ONU-ID */
	body[5] = 0x05;
	memcpy(&body[6], sn, sizeof(sn));

	KUNIT_EXPECT_EQ(test, xgspon_ploam_down_parse(body, sizeof(body), &msg),
			0);
	KUNIT_EXPECT_EQ(test, msg.assign_onu_id.onu_id, 0x305);
	KUNIT_EXPECT_MEMEQ(test, msg.assign_onu_id.sn, sn, sizeof(sn));
	KUNIT_EXPECT_EQ(test, msg.assign_onu_id.line_rate,
			XGSPON_PLOAM_LINE_RATE_XGPON);

	/* the upper bits of the high byte are not part of the id */
	body[4] = 0xff;
	KUNIT_EXPECT_EQ(test, xgspon_ploam_down_parse(body, sizeof(body), &msg),
			0);
	KUNIT_EXPECT_EQ(test, msg.assign_onu_id.onu_id, 0x305);
}

static void ploam_down_assign_onu_id_c_11_6_rate_test(struct kunit *test)
{
	struct xgspon_ploam_down msg;
	u8 body[XGSPON_PLOAM_BODY_LEN] = {};

	body[2] = XGSPON_PLOAM_DOWN_ASSIGN_ONU_ID;

	body[14] = 0xff;
	KUNIT_EXPECT_EQ(test, xgspon_ploam_down_parse(body, sizeof(body), &msg),
			0);
	KUNIT_EXPECT_EQ(test, msg.assign_onu_id.line_rate,
			XGSPON_PLOAM_LINE_RATE_XGSPON);

	body[14] = 0xfe;
	KUNIT_EXPECT_EQ(test, xgspon_ploam_down_parse(body, sizeof(body), &msg),
			0);
	KUNIT_EXPECT_EQ(test, msg.assign_onu_id.line_rate,
			XGSPON_PLOAM_LINE_RATE_XGPON);
}

static void ploam_down_ranging_time_test(struct kunit *test)
{
	struct xgspon_ploam_down msg;
	u8 body[XGSPON_PLOAM_BODY_LEN] = {};

	body[2] = XGSPON_PLOAM_DOWN_RANGING_TIME;
	body[4] = 0x01;		/* absolute, positive */
	body[5] = 0x11; body[6] = 0x22; body[7] = 0x33; body[8] = 0x44;

	KUNIT_EXPECT_EQ(test, xgspon_ploam_down_parse(body, sizeof(body), &msg),
			0);
	KUNIT_EXPECT_TRUE(test, msg.ranging_time.absolute);
	KUNIT_EXPECT_TRUE(test, msg.ranging_time.positive);
	KUNIT_EXPECT_EQ(test, msg.ranging_time.eqd, 0x11223344);

	body[4] = 0x02;		/* relative, negative */
	KUNIT_EXPECT_EQ(test, xgspon_ploam_down_parse(body, sizeof(body), &msg),
			0);
	KUNIT_EXPECT_FALSE(test, msg.ranging_time.absolute);
	KUNIT_EXPECT_FALSE(test, msg.ranging_time.positive);
}

static void t_ranging_time_parse(struct kunit *test, u8 control,
				 bool absolute, bool positive)
{
	struct xgspon_ploam_down msg;
	u8 body[XGSPON_PLOAM_BODY_LEN] = {};

	body[2] = XGSPON_PLOAM_DOWN_RANGING_TIME;
	body[4] = control;
	KUNIT_EXPECT_EQ(test, xgspon_ploam_down_parse(body, sizeof(body), &msg),
			0);
	KUNIT_EXPECT_EQ(test, msg.ranging_time.absolute, absolute);
	KUNIT_EXPECT_EQ(test, msg.ranging_time.positive, positive);
}

static void ploam_down_ranging_time_c_11_7_control_test(struct kunit *test)
{
	t_ranging_time_parse(test, 0x00, false, true);
	t_ranging_time_parse(test, 0x03, true, false);
	t_ranging_time_parse(test, 0x04, false, true);
}

static void ploam_down_disable_sn_test(struct kunit *test)
{
	static const u8 sn[XGSPON_PLOAM_SN_LEN] = {
		0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff, 0x00, 0x11,
	};
	struct xgspon_ploam_down msg;
	u8 body[XGSPON_PLOAM_BODY_LEN] = {};

	body[2] = XGSPON_PLOAM_DOWN_DISABLE_SN;
	body[4] = XGSPON_PLOAM_DISABLE_DENY_ONE;
	memcpy(&body[5], sn, sizeof(sn));

	KUNIT_EXPECT_EQ(test, xgspon_ploam_down_parse(body, sizeof(body), &msg),
			0);
	KUNIT_EXPECT_EQ(test, msg.disable_sn.mode, XGSPON_PLOAM_DISABLE_DENY_ONE);
	KUNIT_EXPECT_MEMEQ(test, msg.disable_sn.sn, sn, sizeof(sn));
}

static void ploam_down_disable_sn_c_11_9_modes_test(struct kunit *test)
{
	static const u8 modes[] = {
		XGSPON_PLOAM_DISABLE_ALLOW_ONE,
		XGSPON_PLOAM_DISABLE_DENY_ALL,
		XGSPON_PLOAM_DISABLE_ALLOW_ALL,
		XGSPON_PLOAM_DISABLE_DENY_ONE,
	};
	struct xgspon_ploam_down msg;
	u8 body[XGSPON_PLOAM_BODY_LEN] = {};
	int i;

	KUNIT_EXPECT_EQ(test, XGSPON_PLOAM_DISABLE_ALLOW_ONE, 0x00);
	KUNIT_EXPECT_EQ(test, XGSPON_PLOAM_DISABLE_DENY_ALL, 0x0f);
	KUNIT_EXPECT_EQ(test, XGSPON_PLOAM_DISABLE_ALLOW_ALL, 0xf0);
	KUNIT_EXPECT_EQ(test, XGSPON_PLOAM_DISABLE_DENY_ONE, 0xff);

	body[2] = XGSPON_PLOAM_DOWN_DISABLE_SN;
	for (i = 0; i < ARRAY_SIZE(modes); i++) {
		body[4] = modes[i];
		KUNIT_EXPECT_EQ(test,
				xgspon_ploam_down_parse(body, sizeof(body), &msg),
				0);
		KUNIT_EXPECT_EQ(test, msg.disable_sn.mode, modes[i]);
	}
}

static void ploam_down_assign_alloc_id_test(struct kunit *test)
{
	struct xgspon_ploam_down msg;
	u8 body[XGSPON_PLOAM_BODY_LEN] = {};

	body[2] = XGSPON_PLOAM_DOWN_ASSIGN_ALLOC_ID;
	body[4] = 0xff;		/* only six bits are the alloc-id */
	body[5] = 0x03;
	body[6] = XGSPON_PLOAM_ALLOC_DEALLOCATE;

	KUNIT_EXPECT_EQ(test, xgspon_ploam_down_parse(body, sizeof(body), &msg),
			0);
	KUNIT_EXPECT_EQ(test, msg.assign_alloc_id.alloc_id, 0x3f03);
	KUNIT_EXPECT_EQ(test, msg.assign_alloc_id.type,
			XGSPON_PLOAM_ALLOC_DEALLOCATE);
}

/* Key_Control is Table C.11.12 of G.9807.1: octet 5 reserved, 6 the control
 * flag, 7 the key index, 8 the key length.
 */
static void ploam_down_key_control_test(struct kunit *test)
{
	struct xgspon_ploam_down msg;
	u8 body[XGSPON_PLOAM_BODY_LEN] = {};

	body[2] = XGSPON_PLOAM_DOWN_KEY_CONTROL;
	body[5] = 0x01;		/* control flag: confirm */
	body[6] = 0x02;		/* key index: second */
	body[7] = 16;		/* key length */

	KUNIT_EXPECT_EQ(test, xgspon_ploam_down_parse(body, sizeof(body), &msg),
			0);
	KUNIT_EXPECT_EQ(test, msg.key_control.control,
			XGSPON_PLOAM_KEY_CONTROL_CONFIRM);
	KUNIT_EXPECT_EQ(test, msg.key_control.key_index,
			XGSPON_PLOAM_KEY_INDEX_SECOND);
	KUNIT_EXPECT_EQ(test, msg.key_control.key_length,
			XGSPON_PLOAM_KEY_LEN_AES128);

	body[5] = 0x00;
	KUNIT_EXPECT_EQ(test, xgspon_ploam_down_parse(body, sizeof(body), &msg),
			0);
	KUNIT_EXPECT_EQ(test, msg.key_control.control,
			XGSPON_PLOAM_KEY_CONTROL_GENERATE);

	body[7] = 0;
	KUNIT_EXPECT_EQ(test, xgspon_ploam_down_parse(body, sizeof(body), &msg),
			0);
	KUNIT_EXPECT_EQ(test, msg.key_control.key_length, 0);
}

/* Reboot_ONU is Table C.11.23A: octets 5 to 12 the serial number a broadcast
 * message names, 13 the depth, 14 the image, 15 the state, 16 the flags.
 */
static void ploam_down_reboot_test(struct kunit *test)
{
	static const u8 sn[XGSPON_PLOAM_SN_LEN] = {
		0x4d, 0x54, 0x4b, 0x47, 0x00, 0x00, 0x00, 0x01,
	};
	struct xgspon_ploam_down msg;
	u8 body[XGSPON_PLOAM_BODY_LEN] = {};

	body[2] = XGSPON_PLOAM_DOWN_REBOOT_ONU;
	memcpy(&body[4], sn, sizeof(sn));
	body[12] = 2;		/* depth */
	body[13] = 1;		/* image */
	body[14] = 0;		/* state */
	body[15] = 2;		/* flags */

	KUNIT_EXPECT_EQ(test, xgspon_ploam_down_parse(body, sizeof(body), &msg),
			0);
	KUNIT_EXPECT_MEMEQ(test, msg.reboot.sn, sn, sizeof(sn));
	KUNIT_EXPECT_EQ(test, msg.reboot.depth, 2);
	KUNIT_EXPECT_EQ(test, msg.reboot.image, 1);
	KUNIT_EXPECT_EQ(test, msg.reboot.state, 0);
	KUNIT_EXPECT_EQ(test, msg.reboot.flags, 2);
}

static void ploam_down_reboot_c_11_23a_padding_test(struct kunit *test)
{
	struct xgspon_ploam_down msg;
	u8 body[XGSPON_PLOAM_BODY_LEN];

	memset(body, T_DIRT, sizeof(body));
	body[0] = 0x03;
	body[1] = 0xff;
	body[2] = XGSPON_PLOAM_DOWN_REBOOT_ONU;
	body[3] = 0x05;
	memset(&body[4], 0, XGSPON_PLOAM_SN_LEN);
	body[12] = XGSPON_PLOAM_REBOOT_DEPTH_MAX;
	body[13] = 0;
	body[14] = XGSPON_PLOAM_REBOOT_STATE_INACTIVE_ONLY;
	body[15] = 1;

	KUNIT_EXPECT_EQ(test, xgspon_ploam_down_parse(body, sizeof(body), &msg),
			0);
	KUNIT_EXPECT_EQ(test, msg.onu_id, XGSPON_PLOAM_ONU_ID_BROADCAST);
	KUNIT_EXPECT_EQ(test, msg.seq_no, 0x05);
	KUNIT_EXPECT_MEMEQ(test, msg.reboot.sn, t_zero, XGSPON_PLOAM_SN_LEN);
	KUNIT_EXPECT_EQ(test, msg.reboot.depth, XGSPON_PLOAM_REBOOT_DEPTH_MAX);
	KUNIT_EXPECT_EQ(test, msg.reboot.image, 0);
	KUNIT_EXPECT_EQ(test, msg.reboot.state,
			XGSPON_PLOAM_REBOOT_STATE_INACTIVE_ONLY);
	KUNIT_EXPECT_EQ(test, msg.reboot.flags, 1);
}

static void ploam_down_sleep_allow_header_test(struct kunit *test)
{
	struct xgspon_ploam_down msg;
	u8 body[XGSPON_PLOAM_BODY_LEN] = {};

	body[0] = 0x01;
	body[1] = 0x02;
	body[2] = XGSPON_PLOAM_DOWN_SLEEP_ALLOW;
	body[3] = 0x33;
	body[4] = 0x01;

	KUNIT_EXPECT_EQ(test, xgspon_ploam_down_parse(body, sizeof(body), &msg),
			-EOPNOTSUPP);
	KUNIT_EXPECT_EQ(test, msg.onu_id, 0x0102);
	KUNIT_EXPECT_EQ(test, msg.msg_id, XGSPON_PLOAM_DOWN_SLEEP_ALLOW);
	KUNIT_EXPECT_EQ(test, msg.seq_no, 0x33);
}

static void ploam_down_burst_profile_einval_header_test(struct kunit *test)
{
	struct xgspon_ploam_down msg;
	u8 body[XGSPON_PLOAM_BODY_LEN] = {};

	body[0] = 0x01;
	body[1] = 0x05;
	body[2] = XGSPON_PLOAM_DOWN_BURST_PROFILE;
	body[3] = 0x44;
	body[6] = XGSPON_PLOAM_BURST_DELIMITER_LEN_MAX + 1;
	body[15] = XGSPON_PLOAM_BURST_PREAMBLE_LEN_MIN;

	KUNIT_EXPECT_EQ(test, xgspon_ploam_down_parse(body, sizeof(body), &msg),
			-EINVAL);
	KUNIT_EXPECT_EQ(test, msg.onu_id, 0x0105);
	KUNIT_EXPECT_EQ(test, msg.msg_id, XGSPON_PLOAM_DOWN_BURST_PROFILE);
	KUNIT_EXPECT_EQ(test, msg.seq_no, 0x44);
}

static void ploam_down_refusal_test(struct kunit *test)
{
	struct xgspon_ploam_down msg;
	u8 body[XGSPON_PLOAM_BODY_LEN] = {};

	body[2] = 0x15;
	KUNIT_EXPECT_EQ(test, xgspon_ploam_down_parse(body, sizeof(body), &msg),
			-EOPNOTSUPP);
	/* the header is still filled in, so a caller can count it */
	KUNIT_EXPECT_EQ(test, msg.msg_id, 0x15);

	KUNIT_EXPECT_EQ(test, xgspon_ploam_down_parse(body, XGSPON_PLOAM_BODY_LEN - 1,
						      &msg), -ENOSPC);
}

static void t_key_report_build(struct kunit *test, u8 len, u8 *body)
{
	struct xgspon_ploam_up up = {
		.onu_id = 0x101,
		.msg_id = XGSPON_PLOAM_UP_KEY_REPORT,
	};

	memset(body, T_DIRT, T_BUF);
	memset(up.key_report.key, 0xc3, XGSPON_PLOAM_KEY_FRAGMENT_LEN);
	up.key_report.len = len;

	KUNIT_EXPECT_EQ(test,
			xgspon_ploam_up_build(body + T_PREFIX, XGSPON_PLOAM_BODY_LEN,
					      &up),
			XGSPON_PLOAM_BODY_LEN);
}

static void ploam_key_report_c_11_26_partial_test(struct kunit *test)
{
	static const u8 key[T_KEY_PARTIAL_LEN] = {
		0xc3, 0xc3, 0xc3, 0xc3, 0xc3,
	};
	u8 body[T_BUF];

	t_key_report_build(test, T_KEY_PARTIAL_LEN, body);
	KUNIT_EXPECT_MEMEQ(test, &body[T_FRAGMENT], key, sizeof(key));
	KUNIT_EXPECT_MEMEQ(test, &body[T_FRAGMENT + T_KEY_PARTIAL_LEN], t_zero,
			   XGSPON_PLOAM_KEY_FRAGMENT_LEN - T_KEY_PARTIAL_LEN);

	t_key_report_build(test, 0, body);
	KUNIT_EXPECT_MEMEQ(test, &body[T_FRAGMENT], t_zero,
			   XGSPON_PLOAM_KEY_FRAGMENT_LEN);
}

static void ploam_down_request_registration_c_11_10_test(struct kunit *test)
{
	struct xgspon_ploam_down msg;
	u8 body[XGSPON_PLOAM_BODY_LEN];

	memset(body, T_DIRT, sizeof(body));
	body[0] = 0x01;
	body[1] = 0x02;
	body[2] = XGSPON_PLOAM_DOWN_REQUEST_REG;
	body[3] = 0x11;

	KUNIT_EXPECT_EQ(test, xgspon_ploam_down_parse(body, sizeof(body), &msg),
			0);
	KUNIT_EXPECT_EQ(test, msg.onu_id, 0x0102);
	KUNIT_EXPECT_EQ(test, msg.msg_id, XGSPON_PLOAM_DOWN_REQUEST_REG);
	KUNIT_EXPECT_EQ(test, msg.seq_no, 0x11);
}

static void ploam_down_burst_profile_c_11_4_fields_test(struct kunit *test)
{
	static const u8 delimiter[XGSPON_PLOAM_BURST_PATTERN_LEN] = {
		0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
	};
	static const u8 preamble[XGSPON_PLOAM_BURST_PATTERN_LEN] = {
		0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27,
	};
	static const u8 pon_tag[XGSPON_PLOAM_PON_TAG_LEN] = {
		0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37,
	};
	struct xgspon_ploam_down msg;
	u8 body[XGSPON_PLOAM_BODY_LEN] = {};

	body[2] = XGSPON_PLOAM_DOWN_BURST_PROFILE;
	body[4] = 0x5b;
	body[5] = 0xfe;
	body[6] = 0xf8;
	memcpy(&body[7], delimiter, sizeof(delimiter));
	body[15] = 0xf7;
	body[16] = 0x40;
	memcpy(&body[17], preamble, sizeof(preamble));
	memcpy(&body[25], pon_tag, sizeof(pon_tag));

	KUNIT_EXPECT_EQ(test, xgspon_ploam_down_parse(body, sizeof(body), &msg),
			0);
	KUNIT_EXPECT_EQ(test, msg.burst_profile.index, 3);
	KUNIT_EXPECT_EQ(test, msg.burst_profile.line_rate,
			XGSPON_PLOAM_LINE_RATE_XGPON);
	KUNIT_EXPECT_EQ(test, msg.burst_profile.version, 5);
	KUNIT_EXPECT_EQ(test, msg.burst_profile.fec, 0);
	KUNIT_EXPECT_EQ(test, msg.burst_profile.delimiter_len, 8);
	KUNIT_EXPECT_MEMEQ(test, msg.burst_profile.delimiter, delimiter,
			   sizeof(delimiter));
	KUNIT_EXPECT_EQ(test, msg.burst_profile.preamble_len, 7);
	KUNIT_EXPECT_EQ(test, msg.burst_profile.preamble_repeat, 0);
	KUNIT_EXPECT_MEMEQ(test, msg.burst_profile.preamble, preamble,
			   sizeof(preamble));
	KUNIT_EXPECT_MEMEQ(test, msg.burst_profile.pon_tag, pon_tag,
			   sizeof(pon_tag));

	body[5] = 0xff;
	KUNIT_EXPECT_EQ(test, xgspon_ploam_down_parse(body, sizeof(body), &msg),
			0);
	KUNIT_EXPECT_EQ(test, msg.burst_profile.fec, 1);
}

static void ploam_down_burst_profile_c_11_4_repeat_test(struct kunit *test)
{
	struct xgspon_ploam_down msg;
	u8 body[XGSPON_PLOAM_BODY_LEN] = {};

	body[2] = XGSPON_PLOAM_DOWN_BURST_PROFILE;
	body[15] = 1;
	body[16] = 0xff;

	body[4] = 0x00;
	KUNIT_EXPECT_EQ(test, xgspon_ploam_down_parse(body, sizeof(body), &msg),
			0);
	KUNIT_EXPECT_EQ(test, msg.burst_profile.line_rate,
			XGSPON_PLOAM_LINE_RATE_XGPON);
	KUNIT_EXPECT_EQ(test, msg.burst_profile.preamble_repeat,
			XGSPON_PLOAM_PREAMBLE_MASK_XGPON);

	body[4] = 0x04;
	KUNIT_EXPECT_EQ(test, xgspon_ploam_down_parse(body, sizeof(body), &msg),
			0);
	KUNIT_EXPECT_EQ(test, msg.burst_profile.line_rate,
			XGSPON_PLOAM_LINE_RATE_XGSPON);
	KUNIT_EXPECT_EQ(test, msg.burst_profile.preamble_repeat, 0xff);
}

static void ploam_down_key_control_c_11_12_mask_test(struct kunit *test)
{
	struct xgspon_ploam_down msg;
	u8 body[XGSPON_PLOAM_BODY_LEN] = {};

	body[2] = XGSPON_PLOAM_DOWN_KEY_CONTROL;
	body[5] = 0xff;
	body[6] = 0xff;
	body[7] = 0xff;

	KUNIT_EXPECT_EQ(test, xgspon_ploam_down_parse(body, sizeof(body), &msg),
			0);
	KUNIT_EXPECT_EQ(test, msg.key_control.control, 1);
	KUNIT_EXPECT_EQ(test, msg.key_control.key_index, 3);
	KUNIT_EXPECT_EQ(test, msg.key_control.key_length, 0xff);
}

static void ploam_down_assign_alloc_id_c_11_11_assign_test(struct kunit *test)
{
	struct xgspon_ploam_down msg;
	u8 body[XGSPON_PLOAM_BODY_LEN] = {};

	body[2] = XGSPON_PLOAM_DOWN_ASSIGN_ALLOC_ID;
	body[4] = 0x04;
	body[5] = 0x03;
	body[6] = XGSPON_PLOAM_ALLOC_ASSIGN;

	KUNIT_EXPECT_EQ(test, xgspon_ploam_down_parse(body, sizeof(body), &msg),
			0);
	KUNIT_EXPECT_EQ(test, msg.assign_alloc_id.alloc_id, 0x0403);
	KUNIT_EXPECT_EQ(test, msg.assign_alloc_id.type, XGSPON_PLOAM_ALLOC_ASSIGN);
}

static void t_down_parse_dirty(struct kunit *test, const u8 *body,
			       struct xgspon_ploam_down *msg)
{
	memset(msg, T_DIRT, sizeof(*msg));
	KUNIT_EXPECT_EQ(test,
			xgspon_ploam_down_parse(body, XGSPON_PLOAM_BODY_LEN, msg), 0);
}

static void ploam_down_parse_clears_test(struct kunit *test)
{
	struct xgspon_ploam_down msg, want;
	u8 body[XGSPON_PLOAM_BODY_LEN];

	memset(body, T_DIRT, sizeof(body));
	body[0] = 0x00;
	body[1] = 0x07;
	body[3] = 0x01;

	body[2] = XGSPON_PLOAM_DOWN_DEACTIVATE;
	t_down_parse_dirty(test, body, &msg);
	memset(&want, 0, sizeof(want));
	want.onu_id = 0x07;
	want.msg_id = XGSPON_PLOAM_DOWN_DEACTIVATE;
	want.seq_no = 0x01;
	KUNIT_EXPECT_MEMEQ(test, &msg, &want, sizeof(want));

	body[2] = XGSPON_PLOAM_DOWN_REQUEST_REG;
	t_down_parse_dirty(test, body, &msg);
	want.msg_id = XGSPON_PLOAM_DOWN_REQUEST_REG;
	KUNIT_EXPECT_MEMEQ(test, &msg, &want, sizeof(want));

	body[2] = XGSPON_PLOAM_DOWN_ASSIGN_ALLOC_ID;
	t_down_parse_dirty(test, body, &msg);
	want.msg_id = XGSPON_PLOAM_DOWN_ASSIGN_ALLOC_ID;
	want.assign_alloc_id.alloc_id = 0x2eee;
	want.assign_alloc_id.type = T_DIRT;
	KUNIT_EXPECT_MEMEQ(test, &msg, &want, sizeof(want));
}

static void ploam_key_constants_c_11_26_test(struct kunit *test)
{
	static const u8 key_name[] = {
		0x33, 0x31, 0x34, 0x31, 0x35, 0x39, 0x32, 0x36,
		0x35, 0x33, 0x35, 0x38, 0x39, 0x37, 0x39, 0x33,
	};
	static const u8 msk[] = XGSPON_PLOAM_DEFAULT_MSK;

	KUNIT_EXPECT_EQ(test, sizeof(XGSPON_PLOAM_KEY_NAME_CONSTANT) - 1,
			XGSPON_PLOAM_KEY_NAME_CONSTANT_LEN);
	KUNIT_EXPECT_EQ(test, sizeof(key_name),
			XGSPON_PLOAM_KEY_NAME_CONSTANT_LEN);
	KUNIT_EXPECT_MEMEQ(test, XGSPON_PLOAM_KEY_NAME_CONSTANT, key_name,
			   sizeof(key_name));
	KUNIT_EXPECT_EQ(test, sizeof(msk), XGSPON_PLOAM_KEY_LEN_AES128);
	KUNIT_EXPECT_EQ(test, XGSPON_PLOAM_DEFAULT_PLOAM_IK_BYTE, 0x55);
	KUNIT_EXPECT_EQ(test, XGSPON_PLOAM_KEY_FRAGMENT_FIRST, 0);
}

static struct kunit_case xgspon_ploam_msg_test_cases[] = {
	KUNIT_CASE(gpon_ploam_wire_test),
	KUNIT_CASE(gpon_ploam_short_buffer_test),
	KUNIT_CASE(xgspon_ploam_unaligned_test),
	KUNIT_CASE(xgspon_ploam_refusal_preserves_buffer_test),
	KUNIT_CASE(ploam_sn_build_test),
	KUNIT_CASE(ploam_sn_capability_c_11_24_test),
	KUNIT_CASE(ploam_registration_build_test),
	KUNIT_CASE(ploam_key_report_build_test),
	KUNIT_CASE(ploam_key_report_mask_test),
	KUNIT_CASE(ploam_ack_build_test),
	KUNIT_CASE(ploam_build_refusal_test),
	KUNIT_CASE(ploam_build_onu_id_range_test),
	KUNIT_CASE(ploam_sn_header_c_11_24_test),
	KUNIT_CASE(ploam_broadcast_test),
	KUNIT_CASE(ploam_profile_broadcast_test),
	KUNIT_CASE(ploam_alloc_assignable_c_6_5_test),
	KUNIT_CASE(ploam_assignable_test),
	KUNIT_CASE(ploam_down_header_test),
	KUNIT_CASE(ploam_down_burst_profile_test),
	KUNIT_CASE(ploam_down_burst_profile_range_test),
	KUNIT_CASE(ploam_down_assign_onu_id_test),
	KUNIT_CASE(ploam_down_assign_onu_id_c_11_6_rate_test),
	KUNIT_CASE(ploam_down_ranging_time_test),
	KUNIT_CASE(ploam_down_ranging_time_c_11_7_control_test),
	KUNIT_CASE(ploam_down_disable_sn_test),
	KUNIT_CASE(ploam_down_disable_sn_c_11_9_modes_test),
	KUNIT_CASE(ploam_down_assign_alloc_id_test),
	KUNIT_CASE(ploam_down_key_control_test),
	KUNIT_CASE(ploam_down_reboot_test),
	KUNIT_CASE(ploam_down_reboot_c_11_23a_padding_test),
	KUNIT_CASE(ploam_down_sleep_allow_header_test),
	KUNIT_CASE(ploam_down_burst_profile_einval_header_test),
	KUNIT_CASE(ploam_down_refusal_test),
	KUNIT_CASE(ploam_key_report_c_11_26_partial_test),
	KUNIT_CASE(ploam_down_request_registration_c_11_10_test),
	KUNIT_CASE(ploam_down_burst_profile_c_11_4_fields_test),
	KUNIT_CASE(ploam_down_burst_profile_c_11_4_repeat_test),
	KUNIT_CASE(ploam_down_key_control_c_11_12_mask_test),
	KUNIT_CASE(ploam_down_assign_alloc_id_c_11_11_assign_test),
	KUNIT_CASE(ploam_down_parse_clears_test),
	KUNIT_CASE(ploam_key_constants_c_11_26_test),
	{}
};

static struct kunit_suite xgspon_ploam_msg_suite = {
	.name = "xpon_ploam_msg",
	.test_cases = xgspon_ploam_msg_test_cases,
};

kunit_test_suite(xgspon_ploam_msg_suite);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("xPON PLOAM codec tests");
