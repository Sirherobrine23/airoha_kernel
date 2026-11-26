// SPDX-License-Identifier: GPL-2.0-only
/* Huawei layouts, large attributes and the actual OMCI transaction path. */

#include <kunit/test.h>
#include <linux/etherdevice.h>
#include <linux/unaligned.h>

#include "internal.h"
#include "wire.h"

struct omci_huawei_test_context {
	struct omci_device odev;
	struct net_device *netdev;
	u16 transaction;
};

static const struct omci_device_ops omci_huawei_test_ops;
static const struct xpon_device_ops omci_huawei_test_xpon_ops;

static int omci_huawei_test_init(struct kunit *test)
{
	struct omci_huawei_test_context *ctx;
	u8 profile = OMCI_OLT_PROFILE_HUAWEI;
	int ret;

	ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;
	test->priv = ctx;
	ctx->odev.parent = root_device_register("omci-huawei-kunit");
	if (IS_ERR(ctx->odev.parent))
		return PTR_ERR(ctx->odev.parent);
	ctx->netdev = alloc_etherdev(0);
	if (!ctx->netdev) {
		ret = -ENOMEM;
		goto parent;
	}
	ctx->odev.xpon = xpon_device_alloc(ctx->odev.parent, 0);
	if (IS_ERR(ctx->odev.xpon)) {
		ret = PTR_ERR(ctx->odev.xpon);
		goto netdev;
	}
	ret = xpon_device_setup(ctx->odev.xpon, ctx->netdev,
				&omci_huawei_test_xpon_ops, NULL,
				XPON_MODE_GPON, XPON_MODE_GPON);
	if (ret)
		goto xpon;
	ctx->odev.ops = &omci_huawei_test_ops;
	spin_lock_init(&ctx->odev.state_lock);
	mutex_init(&ctx->odev.owner_lock);
	ret = omci_agent_init(&ctx->odev);
	if (ret) {
		omci_agent_cleanup(&ctx->odev);
		goto xpon;
	}
	ret = omci_agent_config_set_source(&ctx->odev, OMCI_CONFIG_OLT_PROFILE,
					   &profile, sizeof(profile),
					   OMCI_CONFIG_SOURCE_DRIVER);
	if (!ret)
		return 0;
	omci_agent_cleanup(&ctx->odev);
xpon:
	xpon_device_free(ctx->odev.xpon);
netdev:
	free_netdev(ctx->netdev);
parent:
	root_device_unregister(ctx->odev.parent);
	return ret;
}

static void omci_huawei_test_exit(struct kunit *test)
{
	struct omci_huawei_test_context *ctx = test->priv;

	omci_agent_cleanup(&ctx->odev);
	xpon_device_free(ctx->odev.xpon);
	free_netdev(ctx->netdev);
	root_device_unregister(ctx->odev.parent);
}

/* The response is cached before TX; no optical transport is needed here. */
static const u8 *omci_huawei_exchange(struct kunit *test, u8 action, u16 class,
				      u16 entity, const void *payload,
				      size_t len, bool extended)
{
	struct omci_huawei_test_context *ctx = test->priv;
	struct sk_buff *skb;
	u8 *pdu;
	size_t header = extended ? 10 : 8;
	size_t size = extended ? header + len : OMCI_BASELINE_LEN_NO_MIC;

	if (len > (extended ? OMCI_MAX_PDU_LEN - 10 : 32))
		return NULL;
	skb = alloc_skb(size, GFP_KERNEL);
	if (!skb)
		return NULL;
	pdu = skb_put_zero(skb, size);
	put_unaligned_be16(++ctx->transaction, pdu);
	pdu[2] = 0x40 | action;
	pdu[3] = extended ? OMCI_EXTENDED_DEV_ID : OMCI_BASELINE_DEV_ID;
	put_unaligned_be16(class, pdu + 4);
	put_unaligned_be16(entity, pdu + 6);
	if (extended)
		put_unaligned_be16(len, pdu + 8);
	else
		put_unaligned_be32(40, pdu + 40);
	if (len)
		memcpy(pdu + header, payload, len);
	omci_agent_receive(&ctx->odev, skb);
	kfree_skb(skb);
	if (!ctx->odev.agent.last_response ||
	    get_unaligned_be16(ctx->odev.agent.last_response->data) != ctx->transaction)
		return NULL;
	return ctx->odev.agent.last_response->data + header;
}

static void omci_huawei_layout_test(struct kunit *test)
{
	static const u8 widths[] = { 4, 2, 2, 2, 2, 2, 1, 4, 3, 1, 1, 1, 1, 4, 6, 2 };
	const struct omci_me_desc *desc;
	unsigned int i, offset = 0;

	desc = omci_me_lookup_profile(OMCI_OLT_PROFILE_HUAWEI, 350);
	KUNIT_ASSERT_NOT_NULL(test, desc);
	KUNIT_EXPECT_EQ(test, desc->num_attrs, 16U);
	KUNIT_EXPECT_EQ(test, desc->data_len, 38);
	for (i = 0; i < ARRAY_SIZE(widths); i++) {
		KUNIT_EXPECT_EQ(test, desc->attrs[i].len, widths[i]);
		KUNIT_EXPECT_EQ(test, desc->attrs[i].offset, offset);
		offset += widths[i];
	}
	desc = omci_me_lookup_profile(OMCI_OLT_PROFILE_HUAWEI, 370);
	KUNIT_ASSERT_NOT_NULL(test, desc);
	KUNIT_EXPECT_EQ(test, desc->num_attrs, 3U);
	KUNIT_EXPECT_EQ(test, desc->data_len, 10);
	KUNIT_EXPECT_EQ(test, desc->attrs[0].len, 2);
	KUNIT_EXPECT_EQ(test, desc->attrs[1].len, 4);
	KUNIT_EXPECT_EQ(test, desc->attrs[2].len, 4);
	desc = omci_me_lookup_profile(OMCI_OLT_PROFILE_HUAWEI, 373);
	KUNIT_ASSERT_NOT_NULL(test, desc);
	KUNIT_EXPECT_EQ(test, desc->data_len, 376);
	KUNIT_EXPECT_EQ(test, desc->attrs[15].offset, 351);
	KUNIT_EXPECT_EQ(test, desc->attrs[15].len, 25);
	KUNIT_EXPECT_EQ(test, OMCI_MAX_ATTR_DATA, 64);
	KUNIT_EXPECT_GT(test, OMCI_ATTR_ATTR_DATA_EXT, OMCI_ATTR_LINE_CAPABILITIES);
}

static void omci_huawei_unknown_classes_test(struct kunit *test)
{
	static const u16 unverified[] = { 352, 367, 65408, 65414, 65425 };
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(unverified); i++)
		KUNIT_EXPECT_PTR_EQ(test,
				    omci_me_lookup_profile(OMCI_OLT_PROFILE_HUAWEI, unverified[i]),
				    NULL);
	KUNIT_EXPECT_PTR_EQ(test, omci_me_lookup_profile(OMCI_OLT_PROFILE_GENERIC, 373),
			    NULL);
}

static void omci_huawei_product_tail_test(struct kunit *test)
{
	struct omci_huawei_test_context *ctx = test->priv;
	struct omci_mib_object *object;
	u8 payload[27], mask[2];
	const u8 *response;
	unsigned int i;

	put_unaligned_be16(1, payload); /* Part 15, offset 351. */
	for (i = 2; i < sizeof(payload); i++)
		payload[i] = 'A' + i - 2;
	response = omci_huawei_exchange(test, 8, 373, 0, payload, sizeof(payload), false);
	KUNIT_ASSERT_NOT_NULL(test, response);
	KUNIT_ASSERT_EQ(test, response[0], 0);
	put_unaligned_be16(1, mask);
	response = omci_huawei_exchange(test, 9, 373, 0, mask, sizeof(mask), false);
	KUNIT_ASSERT_NOT_NULL(test, response);
	KUNIT_EXPECT_EQ(test, response[0], 0);
	KUNIT_EXPECT_EQ(test, get_unaligned_be16(response + 1), 1);
	KUNIT_EXPECT_MEMEQ(test, response + 3, payload + 2, 25);

	object = kunit_kzalloc(test, sizeof(*object), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, object);
	KUNIT_ASSERT_EQ(test, omci_agent_mib_get(&ctx->odev, 373, 0, object), 0);
	KUNIT_EXPECT_EQ(test, object->data_len, 376);
	KUNIT_EXPECT_MEMEQ(test, object->huawei_product + 351, payload + 2, 25);
	KUNIT_EXPECT_EQ(test, object->huawei_product[0], 1);
	KUNIT_EXPECT_MEMEQ(test, object->huawei_product + 1,
			   ctx->odev.agent.config.equipment_id,
			   strlen(ctx->odev.agent.config.equipment_id));

	put_unaligned_be16(0xffff, mask);
	response = omci_huawei_exchange(test, 9, 373, 0, mask, sizeof(mask), true);
	KUNIT_ASSERT_NOT_NULL(test, response);
	KUNIT_EXPECT_EQ(test, response[0], 0);
	KUNIT_EXPECT_MEMEQ(test, response + 3, object->huawei_product, 376);
	KUNIT_EXPECT_EQ(test, get_unaligned_be16(ctx->odev.agent.last_response->data + 8),
			379);
}

static void omci_huawei_product_rollback_test(struct kunit *test)
{
	struct omci_huawei_test_context *ctx = test->priv;
	struct omci_mib_object *object;
	u8 payload[] = { 0x80, 0, 16 };
	const u8 *response;

	response = omci_huawei_exchange(test, 8, 373, 0, payload, sizeof(payload), false);
	KUNIT_ASSERT_NOT_NULL(test, response);
	KUNIT_EXPECT_NE(test, response[0], 0);
	object = kunit_kzalloc(test, sizeof(*object), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, object);
	KUNIT_ASSERT_EQ(test, omci_agent_mib_get(&ctx->odev, 373, 0, object), 0);
	KUNIT_EXPECT_EQ(test, object->huawei_product[0], 1);
	/* Local import/export also keeps the tail and the complete length. */
	object->huawei_product[375] = 0xa5;
	KUNIT_ASSERT_EQ(test, omci_agent_mib_set(&ctx->odev, object), 0);
	memset(object, 0, sizeof(*object));
	KUNIT_ASSERT_EQ(test, omci_agent_mib_get(&ctx->odev, 373, 0, object), 0);
	KUNIT_EXPECT_EQ(test, object->huawei_product[375], 0xa5);
	KUNIT_EXPECT_EQ(test, object->data_len, 376);
}

static void omci_huawei_management_codec_test(struct kunit *test)
{
	u8 payload[14] = { 0, 7 }; /* Last three attributes, 4 + 6 + 2 bytes. */
	const u8 *response;
	unsigned int i;

	for (i = 2; i < sizeof(payload); i++)
		payload[i] = i;
	response = omci_huawei_exchange(test, 8, 350, 0, payload, sizeof(payload), false);
	KUNIT_ASSERT_NOT_NULL(test, response);
	KUNIT_ASSERT_EQ(test, response[0], 0);
	response = omci_huawei_exchange(test, 9, 350, 0, payload, 2, false);
	KUNIT_ASSERT_NOT_NULL(test, response);
	KUNIT_EXPECT_EQ(test, response[0], 0);
	KUNIT_EXPECT_EQ(test, get_unaligned_be16(response + 1), 7);
	KUNIT_EXPECT_MEMEQ(test, response + 3, payload + 2, 12);
}

static void omci_huawei_controls_test(struct kunit *test)
{
	struct omci_huawei_test_context *ctx = test->priv;
	u8 qos[] = { 0x40, 0, 3, 0, 0, 0 };
	u8 date[] = { 1, 0, 0x07, 0xea, 11, 20 };
	u8 get_qos[] = { 0xe0, 0 };
	const u8 *response;
	unsigned int action;

	ctx->odev.agent.permissive = true;
	ctx->odev.agent.fake_omci = true;
	response = omci_huawei_exchange(test, 8, 370, 0x101, qos, sizeof(qos), false);
	KUNIT_ASSERT_NOT_NULL(test, response);
	KUNIT_EXPECT_NE(test, response[0], 0);
	response = omci_huawei_exchange(test, 9, 370, 0x101, get_qos, 2, false);
	KUNIT_ASSERT_NOT_NULL(test, response);
	KUNIT_EXPECT_EQ(test, response[0], 0);
	KUNIT_EXPECT_EQ(test, get_unaligned_be16(response + 3), 0x0201);
	KUNIT_EXPECT_EQ(test, get_unaligned_be32(response + 5), 0);
	response = omci_huawei_exchange(test, 8, 350, 0, date, sizeof(date), false);
	KUNIT_ASSERT_NOT_NULL(test, response);
	KUNIT_EXPECT_NE(test, response[0], 0);
	/* Firmware operations must never be faked for the read-only VoIP image. */
	for (action = 19; action <= 23; action++) {
		response = omci_huawei_exchange(test, action, 353, 0, NULL, 0, false);
		KUNIT_ASSERT_NOT_NULL(test, response);
		KUNIT_EXPECT_NE(test, response[0], 0);
		KUNIT_EXPECT_FALSE(test, ctx->odev.agent.last_response_fake);
	}
}

static void omci_huawei_upload_test(struct kunit *test)
{
	struct omci_mib_object *copy;
	const struct omci_me_desc *desc;
	const u8 *response;
	u16 total, sequence, product_mask = 0, management_mask = 0;
	unsigned int product_records = 0, management_records = 0;
	u8 request[2];

	copy = kunit_kzalloc(test, sizeof(*copy), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, copy);
	copy->class_id = 373;
	desc = omci_me_lookup_profile(OMCI_OLT_PROFILE_HUAWEI, 373);
	KUNIT_ASSERT_NOT_NULL(test, desc);
	response = omci_huawei_exchange(test, 13, 2, 0, NULL, 0, false);
	KUNIT_ASSERT_NOT_NULL(test, response);
	total = get_unaligned_be16(response);
	KUNIT_ASSERT_GT(test, total, 0);
	for (sequence = 0; sequence < total; sequence++) {
		u16 class, mask;

		put_unaligned_be16(sequence, request);
		response = omci_huawei_exchange(test, 14, 2, 0, request, 2, false);
		KUNIT_ASSERT_NOT_NULL(test, response);
		class = get_unaligned_be16(response);
		mask = get_unaligned_be16(response + 4);
		KUNIT_EXPECT_NE(test, class, 0);
		if (class == 373) {
			KUNIT_EXPECT_EQ(test, product_mask & mask, 0);
			product_mask |= mask;
			product_records++;
			KUNIT_ASSERT_EQ(test, omci_me_decode_set(desc, copy, mask,
								 response + 6, 26), 0);
		} else if (class == 350) {
			KUNIT_EXPECT_EQ(test, management_mask & mask, 0);
			management_mask |= mask;
			management_records++;
		}
	}
	KUNIT_EXPECT_EQ(test, product_mask, 0xffff);
	KUNIT_EXPECT_EQ(test, product_records, 15U);
	KUNIT_EXPECT_EQ(test, management_mask, 0xffff);
	KUNIT_EXPECT_EQ(test, management_records, 2U);
	KUNIT_EXPECT_EQ(test, copy->huawei_product[0], 1);
	KUNIT_EXPECT_MEMEQ(test, copy->huawei_product + 1, "Airoha EN7523", 13);
}

static struct kunit_case omci_huawei_test_cases[] = {
	KUNIT_CASE(omci_huawei_layout_test),
	KUNIT_CASE(omci_huawei_unknown_classes_test),
	KUNIT_CASE(omci_huawei_product_tail_test),
	KUNIT_CASE(omci_huawei_product_rollback_test),
	KUNIT_CASE(omci_huawei_management_codec_test),
	KUNIT_CASE(omci_huawei_controls_test),
	KUNIT_CASE(omci_huawei_upload_test),
	{}
};

static struct kunit_suite omci_huawei_test_suite = {
	.name = "omci-huawei",
	.init = omci_huawei_test_init,
	.exit = omci_huawei_test_exit,
	.test_cases = omci_huawei_test_cases,
};

kunit_test_suite(omci_huawei_test_suite);
