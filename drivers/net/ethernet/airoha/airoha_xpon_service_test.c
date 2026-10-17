// SPDX-License-Identifier: GPL-2.0-only
/* Cookie sharing and capacity regressions without frame-engine hardware. */

#include <kunit/test.h>

#include "airoha_eth.h"

static struct airoha_xpon_service_cfg airoha_service_cfg(u32 cookie, u16 gem)
{
	return (struct airoha_xpon_service_cfg){
		.cookie = cookie,
		.gem_port_id = gem,
		.tcont = 1,
		.queue = 0,
		.vlan_id = 100,
		.vlan_valid = true,
	};
}

static unsigned int airoha_service_count(struct airoha_xpon_service_table *table)
{
	unsigned int count = 0;
	int i;

	for (i = 0; i < AIROHA_XPON_MAX_SERVICES; i++)
		count += table->rules[i].valid;
	return count;
}

static int airoha_service_test_init(struct kunit *test)
{
	test->priv = kunit_kzalloc(test, sizeof(struct airoha_xpon_service_table),
				   GFP_KERNEL);
	return test->priv ? 0 : -ENOMEM;
}

static void airoha_service_share_test(struct kunit *test)
{
	struct airoha_xpon_service_table *table = test->priv;
	struct airoha_xpon_service_cfg cfg = airoha_service_cfg(1, 257);
	int slot;

	KUNIT_ASSERT_EQ(test, airoha_xpon_store_service(table, &cfg), 0);
	cfg.cookie = 2;
	KUNIT_ASSERT_EQ(test, airoha_xpon_store_service(table, &cfg), 0);
	KUNIT_EXPECT_EQ(test, airoha_service_count(table), 1U);
	airoha_xpon_forget_cookie(table, 1);
	slot = airoha_xpon_find_cookie(table, 2);
	KUNIT_ASSERT_GE(test, slot, 0);
	KUNIT_EXPECT_EQ(test, table->rules[slot].gem_port_id, (u16)257);
	airoha_xpon_forget_cookie(table, 2);
	KUNIT_EXPECT_EQ(test, airoha_service_count(table), 0U);
}

static void airoha_service_bucket_test(struct kunit *test)
{
	struct airoha_xpon_service_table *table = test->priv;
	struct airoha_xpon_service_cfg cfg = airoha_service_cfg(1, 257);
	unsigned int cookie;

	for (cookie = 1; cookie <= AIROHA_XPON_SVC_MAX_COOKIES; cookie++) {
		cfg.cookie = cookie;
		KUNIT_ASSERT_EQ(test, airoha_xpon_store_service(table, &cfg), 0);
	}
	/* Replaying an existing cookie in a full bucket must not spill. */
	KUNIT_ASSERT_EQ(test, airoha_xpon_store_service(table, &cfg), 0);
	KUNIT_EXPECT_EQ(test, airoha_service_count(table), 1U);
	cfg.cookie++;
	KUNIT_ASSERT_EQ(test, airoha_xpon_store_service(table, &cfg), 0);
	KUNIT_EXPECT_EQ(test, airoha_service_count(table), 2U);
	for (cookie = 1; cookie <= cfg.cookie; cookie++)
		airoha_xpon_forget_cookie(table, cookie);
	KUNIT_EXPECT_EQ(test, airoha_service_count(table), 0U);
}

static void airoha_service_full_replace_test(struct kunit *test)
{
	struct airoha_xpon_service_table *table = test->priv;
	struct airoha_xpon_service_cfg cfg;
	unsigned int i;
	int slot;

	for (i = 1; i <= AIROHA_XPON_MAX_SERVICES; i++) {
		cfg = airoha_service_cfg(i, i);
		KUNIT_ASSERT_EQ(test, airoha_xpon_store_service(table, &cfg), 0);
	}
	cfg = airoha_service_cfg(1, 1000);
	KUNIT_ASSERT_EQ(test, airoha_xpon_store_service(table, &cfg), 0);
	slot = airoha_xpon_find_cookie(table, 1);
	KUNIT_ASSERT_GE(test, slot, 0);
	KUNIT_EXPECT_EQ(test, table->rules[slot].gem_port_id, (u16)1000);
	KUNIT_EXPECT_EQ(test, airoha_service_count(table),
			AIROHA_XPON_MAX_SERVICES);
}

static void airoha_service_failed_replace_test(struct kunit *test)
{
	struct airoha_xpon_service_table *table = test->priv;
	struct airoha_xpon_service_cfg cfg;
	unsigned int i;
	int slot;

	for (i = 1; i <= AIROHA_XPON_MAX_SERVICES; i++) {
		cfg = airoha_service_cfg(i, i);
		cfg.default_service = i == 1;
		KUNIT_ASSERT_EQ(test, airoha_xpon_store_service(table, &cfg), 0);
	}
	cfg = airoha_service_cfg(1000, 1);
	cfg.default_service = true;
	KUNIT_ASSERT_EQ(test, airoha_xpon_store_service(table, &cfg), 0);
	/* Cookie 1 cannot reuse its old slot while cookie 1000 references it. */
	cfg = airoha_service_cfg(1, 1000);
	KUNIT_EXPECT_EQ(test, airoha_xpon_store_service(table, &cfg), -ENOSPC);
	slot = airoha_xpon_find_cookie(table, 1);
	KUNIT_ASSERT_GE(test, slot, 0);
	KUNIT_EXPECT_EQ(test, table->rules[slot].gem_port_id, (u16)1);
	KUNIT_EXPECT_TRUE(test, table->rules[slot].default_service);
	KUNIT_EXPECT_EQ(test, airoha_xpon_find_cookie(table, 1000), slot);
}

static void airoha_service_compact_test(struct kunit *test)
{
	struct airoha_xpon_service_table *table = test->priv;
	struct airoha_xpon_service_cfg cfg;
	unsigned int i;

	for (i = 1; i <= AIROHA_XPON_SVC_MAX_COOKIES + 1; i++) {
		cfg = airoha_service_cfg(i, 1000);
		KUNIT_ASSERT_EQ(test, airoha_xpon_store_service(table, &cfg), 0);
	}
	for (i = 1; i <= AIROHA_XPON_MAX_SERVICES - 2; i++) {
		cfg = airoha_service_cfg(100 + i, i);
		KUNIT_ASSERT_EQ(test, airoha_xpon_store_service(table, &cfg), 0);
	}
	airoha_xpon_forget_cookie(table, 1);
	cfg = airoha_service_cfg(10000, 2000);
	KUNIT_ASSERT_EQ(test, airoha_xpon_store_service(table, &cfg), 0);
	for (i = 2; i <= AIROHA_XPON_SVC_MAX_COOKIES + 1; i++) {
		int slot = airoha_xpon_find_cookie(table, i);

		KUNIT_ASSERT_GE(test, slot, 0);
		KUNIT_EXPECT_EQ(test, table->rules[slot].gem_port_id, (u16)1000);
	}
}

static void airoha_service_distinct_queue_test(struct kunit *test)
{
	struct airoha_xpon_service_table *table = test->priv;
	struct airoha_xpon_service_cfg cfg = airoha_service_cfg(1, 257);

	KUNIT_ASSERT_EQ(test, airoha_xpon_store_service(table, &cfg), 0);
	cfg.cookie = 2;
	cfg.queue = 1;
	KUNIT_ASSERT_EQ(test, airoha_xpon_store_service(table, &cfg), 0);
	KUNIT_EXPECT_EQ(test, airoha_service_count(table), 2U);
}

static void airoha_service_wildcard_test(struct kunit *test)
{
	struct airoha_xpon_service_table *table = test->priv;
	struct airoha_xpon_service_cfg cfg = airoha_service_cfg(1, 257);

	cfg.vlan_valid = false;
	KUNIT_ASSERT_EQ(test, airoha_xpon_store_service(table, &cfg), 0);
	cfg.cookie = 2;
	cfg.vlan_id = 200;
	cfg.pcp = 7;
	KUNIT_ASSERT_EQ(test, airoha_xpon_store_service(table, &cfg), 0);
	KUNIT_EXPECT_EQ(test, airoha_service_count(table), 1U);
}

static struct kunit_case airoha_service_test_cases[] = {
	KUNIT_CASE(airoha_service_share_test),
	KUNIT_CASE(airoha_service_bucket_test),
	KUNIT_CASE(airoha_service_full_replace_test),
	KUNIT_CASE(airoha_service_failed_replace_test),
	KUNIT_CASE(airoha_service_compact_test),
	KUNIT_CASE(airoha_service_distinct_queue_test),
	KUNIT_CASE(airoha_service_wildcard_test),
	{}
};

static struct kunit_suite airoha_service_test_suite = {
	.name = "airoha-xpon-services",
	.init = airoha_service_test_init,
	.test_cases = airoha_service_test_cases,
};

kunit_test_suite(airoha_service_test_suite);
