// SPDX-License-Identifier: GPL-2.0-only
/* OLT profile isolation and vendor ME transactions. */

#include <kunit/test.h>
#include <linux/etherdevice.h>
#include <linux/unaligned.h>

#include "internal.h"
#include "wire.h"

struct omci_profiles_test_context {
	struct omci_device odev;
	struct net_device *netdev;
	u16 transaction;
};

static const struct omci_device_ops omci_profiles_test_ops;
static const struct xpon_device_ops omci_profiles_test_xpon_ops;

static int omci_profiles_test_init(struct kunit *test)
{
	struct omci_profiles_test_context *ctx;
	u8 profile = OMCI_OLT_PROFILE_HUAWEI;
	int ret;

	ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;
	test->priv = ctx;
	ctx->odev.parent = root_device_register("omci-profiles-kunit");
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
				&omci_profiles_test_xpon_ops, NULL,
				XPON_MODE_GPON, XPON_MODE_GPON);
	if (ret)
		goto xpon;
	ctx->odev.ops = &omci_profiles_test_ops;
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

static void omci_profiles_test_exit(struct kunit *test)
{
	struct omci_profiles_test_context *ctx = test->priv;

	omci_agent_cleanup(&ctx->odev);
	xpon_device_free(ctx->odev.xpon);
	free_netdev(ctx->netdev);
	root_device_unregister(ctx->odev.parent);
}

/* The response is cached before TX; no optical transport is needed here. */
static const u8 *omci_profiles_exchange(struct kunit *test, u8 action, u16 class,
					u16 entity, const void *payload,
				      size_t len, bool extended)
{
	struct omci_profiles_test_context *ctx = test->priv;
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

/* The test agent is stopped, so no concurrent MIB writer can run. */
static struct omci_mib_object *omci_profiles_object(struct omci_agent *agent,
						    u16 class, u16 entity)
{
	return xa_load(&agent->mib, ((unsigned long)class << 16) | entity);
}

static void omci_profiles_select(struct kunit *test, u8 profile)
{
	struct omci_profiles_test_context *ctx = test->priv;
	int ret;

	ret = omci_agent_config_set_source(&ctx->odev, OMCI_CONFIG_OLT_PROFILE,
					   &profile, sizeof(profile),
					   OMCI_CONFIG_SOURCE_DRIVER);
	KUNIT_ASSERT_EQ(test, ret, 0);
}

static int omci_profiles_ctc(struct kunit *test, u8 enable)
{
	struct omci_profiles_test_context *ctx = test->priv;

	return omci_agent_config_set_source(&ctx->odev, OMCI_CONFIG_CTC_EXTENSION,
		&enable, sizeof(enable), OMCI_CONFIG_SOURCE_DRIVER);
}

static void omci_profiles_detection_test(struct kunit *test)
{
	static const struct {
		char vendor[4];
		u8 profile;
	} cases[] = {
		{ "ALCL", OMCI_OLT_PROFILE_NOKIA_ALCL },
		{ "HWTC", OMCI_OLT_PROFILE_HUAWEI },
		{ "ZTEG", OMCI_OLT_PROFILE_ZTE },
		{ "DSNW", OMCI_OLT_PROFILE_DASAN },
		{ "FHTT", OMCI_OLT_PROFILE_FIBERHOME },
		{ "ABCD", OMCI_OLT_PROFILE_GENERIC },
	};
	struct omci_olt_g olt = { .valid = true, .vendor_id_valid = true };
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(cases); i++) {
		memcpy(olt.vendor_id, cases[i].vendor, sizeof(olt.vendor_id));
		KUNIT_EXPECT_EQ(test, omci_profile_detect(&olt), cases[i].profile);
	}
	olt.valid = false;
	KUNIT_EXPECT_EQ(test, omci_profile_detect(&olt), OMCI_OLT_PROFILE_GENERIC);
	KUNIT_EXPECT_EQ(test,
			omci_profile_normalize_uni_entity(OMCI_OLT_PROFILE_FIBERHOME, 0x601),
		0x601);
	KUNIT_EXPECT_EQ(test,
			omci_profile_normalize_uni_entity(OMCI_OLT_PROFILE_DASAN, 0x601), 1);
}

static void omci_profiles_zte_layout_test(struct kunit *test)
{
	const struct omci_me_desc *desc;
	unsigned int i;

	desc = omci_me_lookup_profile(OMCI_OLT_PROFILE_ZTE, 247);
	KUNIT_ASSERT_NOT_NULL(test, desc);
	KUNIT_EXPECT_EQ(test, desc->num_attrs, 16U);
	KUNIT_EXPECT_EQ(test, desc->data_len, 43);
	KUNIT_EXPECT_EQ(test, desc->attrs[2].offset, 4);
	KUNIT_EXPECT_EQ(test, desc->attrs[2].len, 14);
	KUNIT_EXPECT_EQ(test, desc->attrs[2].access, OMCI_ATTR_ACCESS_READ);
	for (i = 3; i < 15; i++) {
		KUNIT_EXPECT_EQ(test, desc->attrs[i].offset, 18 + (i - 3) * 2);
		KUNIT_EXPECT_EQ(test, desc->attrs[i].len, 2);
		KUNIT_EXPECT_EQ(test, desc->attrs[i].access, OMCI_ATTR_ACCESS_WRITE);
	}
	KUNIT_EXPECT_EQ(test, desc->attrs[15].offset, 42);
	KUNIT_EXPECT_EQ(test, desc->attrs[15].len, 1);
	KUNIT_EXPECT_EQ(test, desc->mib_upload_mask, BIT(13));
	KUNIT_EXPECT_PTR_EQ(test,
			    omci_me_lookup_profile(OMCI_OLT_PROFILE_HUAWEI, 247), NULL);
	KUNIT_EXPECT_PTR_EQ(test,
			    omci_me_lookup_profile(OMCI_OLT_PROFILE_GENERIC, 247), NULL);
}

static void omci_profiles_zte_transactions_test(struct kunit *test)
{
	struct omci_profiles_test_context *ctx = test->priv;
	struct omci_mib_object *object;
	const struct omci_me_desc *desc;
	struct omci_me_class class;
	static const u8 model[] = "Test ZTE model";
	const u8 get[] = { 0x20, 0 };
	const u8 set[] = { 0xc0, 0, 0x12, 0x34, 0x56, 0x78 };
	u8 readonly[16] = { 0x20, 0 };
	const u8 *response;
	u8 data[26];
	u16 mask;
	size_t len;

	omci_profiles_select(test, OMCI_OLT_PROFILE_ZTE);
	KUNIT_ASSERT_EQ(test, omci_me_class_get(&ctx->odev, 247, &class), 0);
	KUNIT_EXPECT_STREQ(test, class.name, "ZTE extended ONU-G");
	KUNIT_EXPECT_TRUE(test, class.flags & OMCI_CLASS_F_VENDOR_SPECIFIC);
	KUNIT_ASSERT_EQ(test,
			omci_agent_config_set_source(&ctx->odev, OMCI_CONFIG_EQUIPMENT_ID,
						     model, sizeof(model) - 1,
						     OMCI_CONFIG_SOURCE_DRIVER), 0);
	response = omci_profiles_exchange(test, 9, 247, 0, get, sizeof(get), false);
	KUNIT_ASSERT_NOT_NULL(test, response);
	KUNIT_EXPECT_EQ(test, response[0], 0);
	KUNIT_EXPECT_EQ(test, get_unaligned_be16(response + 1), BIT(13));
	KUNIT_EXPECT_EQ(test, memcmp(response + 3, model, 14), 0);
	response = omci_profiles_exchange(test, 8, 247, 0, set, sizeof(set), false);
	KUNIT_ASSERT_NOT_NULL(test, response);
	KUNIT_EXPECT_EQ(test, response[0], 0);
	object = omci_profiles_object(&ctx->odev.agent, 247, 0);
	KUNIT_ASSERT_NOT_NULL(test, object);
	KUNIT_EXPECT_EQ(test, get_unaligned_be32(object->data), 0x12345678U);
	desc = omci_me_lookup(&ctx->odev.agent, 247);
	KUNIT_ASSERT_NOT_NULL(test, desc);
	KUNIT_EXPECT_EQ(test, omci_me_upload_records(desc, object), 1U);
	KUNIT_ASSERT_EQ(test,
			omci_me_encode_upload(desc, object, 0, data, &mask, &len), 0);
	KUNIT_EXPECT_EQ(test, mask, BIT(13));
	KUNIT_EXPECT_EQ(test, len, 14U);
	KUNIT_EXPECT_EQ(test, memcmp(data, model, 14), 0);
	response = omci_profiles_exchange(test, 8, 247, 0, readonly,
					  sizeof(readonly), false);
	KUNIT_ASSERT_NOT_NULL(test, response);
	KUNIT_EXPECT_NE(test, response[0], 0);
	response = omci_profiles_exchange(test, 8, 247, 1, set, sizeof(set), false);
	KUNIT_ASSERT_NOT_NULL(test, response);
	KUNIT_EXPECT_NE(test, response[0], 0);
}

static void omci_profiles_zte_reset_test(struct kunit *test)
{
	struct omci_profiles_test_context *ctx = test->priv;
	struct omci_mib_object *object;
	u8 reset[] = { 0, 1, 0 };
	const u8 *response;
	unsigned int value;

	omci_profiles_select(test, OMCI_OLT_PROFILE_ZTE);
	ctx->odev.agent.permissive = true;
	ctx->odev.agent.fake_omci = true;
	for (value = 0; value <= 1; value++) {
		reset[2] = value;
		response = omci_profiles_exchange(test, 8, 247, 0,
						  reset, sizeof(reset), false);
		KUNIT_ASSERT_NOT_NULL(test, response);
		KUNIT_EXPECT_NE(test, response[0], 0);
		object = omci_profiles_object(&ctx->odev.agent, 247, 0);
		KUNIT_ASSERT_NOT_NULL(test, object);
		KUNIT_EXPECT_EQ(test, object->data[42], 0);
	}
	{
		const u8 mixed[] = { 0x80, 1, 0x12, 0x34, 0 };

		response = omci_profiles_exchange(test, 8, 247, 0,
						  mixed, sizeof(mixed), false);
		KUNIT_ASSERT_NOT_NULL(test, response);
		KUNIT_EXPECT_NE(test, response[0], 0);
		object = omci_profiles_object(&ctx->odev.agent, 247, 0);
		KUNIT_ASSERT_NOT_NULL(test, object);
		KUNIT_EXPECT_EQ(test, get_unaligned_be16(object->data), 0);
	}
	KUNIT_EXPECT_EQ(test, atomic64_read(&ctx->odev.agent.fake_responses), 0);
}

static void omci_profiles_ctc_transactions_test(struct kunit *test)
{
	struct omci_profiles_test_context *ctx = test->priv;
	struct omci_mib_object *object;
	const struct omci_me_desc *desc;
	u8 set[] = { 0x81, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12 };
	u8 get[] = { 0xff, 0 };
	const u8 *response;
	u8 value, source;
	size_t len = sizeof(value);
	unsigned int attr_count = omci_me_attr_count(&ctx->odev.agent);

	KUNIT_EXPECT_PTR_EQ(test, omci_me_lookup(&ctx->odev.agent, 65527), NULL);
	KUNIT_EXPECT_EQ(test, omci_profiles_ctc(test, 2), -EINVAL);
	KUNIT_ASSERT_EQ(test, omci_profiles_ctc(test, 1), 0);
	KUNIT_EXPECT_EQ(test, omci_me_attr_count(&ctx->odev.agent), attr_count + 8);
	KUNIT_ASSERT_EQ(test, omci_agent_config_get(&ctx->odev,
						    OMCI_CONFIG_CTC_EXTENSION, &value, &len), 0);
	KUNIT_EXPECT_EQ(test, value, 1);
	KUNIT_ASSERT_EQ(test, omci_agent_config_source_get(&ctx->odev,
							   OMCI_CONFIG_CTC_EXTENSION, &source), 0);
	KUNIT_EXPECT_EQ(test, source, OMCI_CONFIG_SOURCE_DRIVER);
	desc = omci_me_lookup(&ctx->odev.agent, 65527);
	KUNIT_ASSERT_NOT_NULL(test, desc);
	KUNIT_EXPECT_EQ(test, desc->data_len, 34);
	KUNIT_EXPECT_EQ(test, desc->num_attrs, 8U);
	response = omci_profiles_exchange(test, 8, 65527, 0, set, sizeof(set), false);
	KUNIT_ASSERT_NOT_NULL(test, response);
	KUNIT_EXPECT_EQ(test, response[0], 0);
	response = omci_profiles_exchange(test, 9, 65527, 0, get, sizeof(get), true);
	KUNIT_ASSERT_NOT_NULL(test, response);
	KUNIT_EXPECT_EQ(test, response[0], 0);
	KUNIT_EXPECT_EQ(test, get_unaligned_be16(response + 1), 0xff00);
	KUNIT_EXPECT_EQ(test, memcmp(response + 3, set + 2, 4), 0);
	KUNIT_EXPECT_EQ(test, memcmp(response + 29, set + 6, 8), 0);
	object = omci_profiles_object(&ctx->odev.agent, 65527, 0);
	KUNIT_ASSERT_NOT_NULL(test, object);
	KUNIT_EXPECT_EQ(test, omci_me_upload_records(desc, object), 0U);
	KUNIT_ASSERT_EQ(test, omci_profiles_ctc(test, 1), 0);
	object = omci_profiles_object(&ctx->odev.agent, 65527, 0);
	KUNIT_ASSERT_NOT_NULL(test, object);
	KUNIT_EXPECT_EQ(test, object->data[0], 1);
	omci_profiles_select(test, OMCI_OLT_PROFILE_ZTE);
	KUNIT_EXPECT_NOT_NULL(test, omci_me_lookup(&ctx->odev.agent, 65527));
	omci_agent_mib_reset(&ctx->odev, false);
	object = omci_profiles_object(&ctx->odev.agent, 65527, 0);
	KUNIT_ASSERT_NOT_NULL(test, object);
	KUNIT_EXPECT_EQ(test, object->data[0], 1);
	omci_agent_mib_reset(&ctx->odev, true);
	KUNIT_EXPECT_NOT_NULL(test, omci_profiles_object(&ctx->odev.agent, 65527, 0));
	KUNIT_ASSERT_EQ(test, omci_profiles_ctc(test, 0), 0);
	KUNIT_EXPECT_PTR_EQ(test, omci_me_lookup(&ctx->odev.agent, 65527), NULL);
	KUNIT_EXPECT_PTR_EQ(test, omci_profiles_object(&ctx->odev.agent, 65527, 0), NULL);
	KUNIT_EXPECT_PTR_EQ(test, omci_me_lookup(&ctx->odev.agent, 65408), NULL);
}

static void omci_profiles_switch_test(struct kunit *test)
{
	struct omci_profiles_test_context *ctx = test->priv;
	struct omci_mib_object *object;
	const u8 identify[] = { 0x80, 0, 'H', 'W', 'T', 'C' };
	const u8 *response;

	omci_profiles_select(test, OMCI_OLT_PROFILE_ZTE);
	KUNIT_EXPECT_NOT_NULL(test, omci_profiles_object(&ctx->odev.agent, 247, 0));
	KUNIT_EXPECT_PTR_EQ(test, omci_profiles_object(&ctx->odev.agent, 373, 0), NULL);
	omci_profiles_select(test, OMCI_OLT_PROFILE_NOKIA_ALCL);
	KUNIT_EXPECT_PTR_EQ(test, omci_profiles_object(&ctx->odev.agent, 247, 0), NULL);
	object = omci_profiles_object(&ctx->odev.agent, OMCI_CLASS_OLT_G, 0);
	KUNIT_ASSERT_NOT_NULL(test, object);
	KUNIT_EXPECT_FALSE(test, object->olt_g.valid);
	KUNIT_EXPECT_EQ(test, memcmp(object->data, "\0\0\0\0", 4), 0);
	omci_profiles_select(test, OMCI_OLT_PROFILE_AUTO);
	KUNIT_EXPECT_EQ(test, ctx->odev.agent.profile_effective,
			OMCI_OLT_PROFILE_GENERIC);
	response = omci_profiles_exchange(test, 8, 131, 0, identify,
					  sizeof(identify), false);
	KUNIT_ASSERT_NOT_NULL(test, response);
	KUNIT_EXPECT_EQ(test, response[0], 0);
	KUNIT_EXPECT_EQ(test, ctx->odev.agent.profile_effective,
			OMCI_OLT_PROFILE_HUAWEI);
	omci_profiles_select(test, OMCI_OLT_PROFILE_NOKIA_ALCL);
	object = omci_profiles_object(&ctx->odev.agent, OMCI_CLASS_OLT_G, 0);
	KUNIT_ASSERT_NOT_NULL(test, object);
	KUNIT_EXPECT_EQ(test, memcmp(object->olt_g.vendor_id, "HWTC", 4), 0);
	omci_profiles_select(test, OMCI_OLT_PROFILE_AUTO);
	KUNIT_EXPECT_EQ(test, ctx->odev.agent.profile_effective,
			OMCI_OLT_PROFILE_HUAWEI);
}

static void omci_profiles_huawei_standard_qos_test(struct kunit *test)
{
	struct omci_profiles_test_context *ctx = test->priv;
	struct omci_mib_object *object;
	u8 mode[] = { 0x40, 0, 1, 0, 0, 0 };
	const u8 *response;

	response = omci_profiles_exchange(test, 8, 370, 0x101,
					  mode, sizeof(mode), false);
	KUNIT_ASSERT_NOT_NULL(test, response);
	KUNIT_EXPECT_EQ(test, response[0], 0);
	mode[2] = 3;
	response = omci_profiles_exchange(test, 8, 370, 0x101,
					  mode, sizeof(mode), false);
	KUNIT_ASSERT_NOT_NULL(test, response);
	KUNIT_EXPECT_NE(test, response[0], 0);
	object = omci_profiles_object(&ctx->odev.agent, 370, 0x101);
	KUNIT_ASSERT_NOT_NULL(test, object);
	KUNIT_EXPECT_EQ(test, get_unaligned_be32(object->data + 2), 0x01000000U);
}

static struct kunit_case omci_profiles_test_cases[] = {
	KUNIT_CASE(omci_profiles_detection_test),
	KUNIT_CASE(omci_profiles_zte_layout_test),
	KUNIT_CASE(omci_profiles_zte_transactions_test),
	KUNIT_CASE(omci_profiles_zte_reset_test),
	KUNIT_CASE(omci_profiles_ctc_transactions_test),
	KUNIT_CASE(omci_profiles_switch_test),
	KUNIT_CASE(omci_profiles_huawei_standard_qos_test),
	{}
};

static struct kunit_suite omci_profiles_test_suite = {
	.name = "omci-profiles",
	.init = omci_profiles_test_init,
	.exit = omci_profiles_test_exit,
	.test_cases = omci_profiles_test_cases,
};

kunit_test_suite(omci_profiles_test_suite);
