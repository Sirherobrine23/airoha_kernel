// SPDX-License-Identifier: GPL-2.0-only
#include <kunit/test.h>
#include <linux/module.h>

#include "airoha_xdsl_internal.h"

static struct airoha_xdsl_line_state airoha_xdsl_test_showtime(void)
{
	return (struct airoha_xdsl_line_state) {
		.status = AIROHA_XDSL_LINE_SHOWTIME,
		.transport = AIROHA_XDSL_TRANSPORT_PTM,
		.bearer_mask = BIT(0),
		.downstream_bps = 100000000,
		.upstream_bps = 20000000,
	};
}

static void airoha_xdsl_test_bearers(struct kunit *test)
{
	struct airoha_xdsl_line_state state = airoha_xdsl_test_showtime();

	KUNIT_EXPECT_EQ(test, airoha_xdsl_validate_line(&state), 0);
	KUNIT_EXPECT_EQ(test, airoha_xdsl_path_mask(state.bearer_mask), (u8)0x3);
	state.bearer_mask = BIT(1);
	state.tx_bearer = 1;
	KUNIT_EXPECT_EQ(test, airoha_xdsl_validate_line(&state), 0);
	KUNIT_EXPECT_EQ(test, airoha_xdsl_path_mask(state.bearer_mask), (u8)0xc);
	state.bearer_mask = GENMASK(1, 0);
	KUNIT_EXPECT_EQ(test, airoha_xdsl_validate_line(&state), 0);
	KUNIT_EXPECT_EQ(test, airoha_xdsl_path_mask(state.bearer_mask), (u8)0xf);
}

static void airoha_xdsl_test_invalid_bearers(struct kunit *test)
{
	struct airoha_xdsl_line_state state = airoha_xdsl_test_showtime();

	state.bearer_mask = 0;
	KUNIT_EXPECT_EQ(test, airoha_xdsl_validate_line(&state), -EINVAL);
	state.bearer_mask = BIT(2);
	KUNIT_EXPECT_EQ(test, airoha_xdsl_validate_line(&state), -EINVAL);
	state.bearer_mask = BIT(0);
	state.tx_bearer = 1;
	KUNIT_EXPECT_EQ(test, airoha_xdsl_validate_line(&state), -EINVAL);
	state.tx_bearer = 8;
	KUNIT_EXPECT_EQ(test, airoha_xdsl_validate_line(&state), -EINVAL);
}

static void airoha_xdsl_test_atm(struct kunit *test)
{
	struct airoha_xdsl_line_state state = airoha_xdsl_test_showtime();

	state.transport = AIROHA_XDSL_TRANSPORT_ATM;
	KUNIT_EXPECT_EQ(test, airoha_xdsl_validate_line(&state), -EOPNOTSUPP);
}

static void airoha_xdsl_test_rates(struct kunit *test)
{
	struct airoha_xdsl_line_state state = airoha_xdsl_test_showtime();

	state.downstream_bps = 0;
	KUNIT_EXPECT_EQ(test, airoha_xdsl_validate_line(&state), -EINVAL);
	state.downstream_bps = 500000;
	KUNIT_EXPECT_EQ(test, airoha_xdsl_validate_line(&state), 0);
	state.upstream_bps = 0;
	KUNIT_EXPECT_EQ(test, airoha_xdsl_validate_line(&state), -EINVAL);
	state.upstream_bps = 20000000;
	state.downstream_bps = (u64)(u32)SPEED_UNKNOWN * 1000000;
	KUNIT_EXPECT_EQ(test, airoha_xdsl_validate_line(&state), -EINVAL);
}

static void airoha_xdsl_test_status(struct kunit *test)
{
	struct airoha_xdsl_line_state state = {};

	KUNIT_EXPECT_EQ(test, airoha_xdsl_validate_line(&state), 0);
	state.status = AIROHA_XDSL_LINE_TRAINING;
	KUNIT_EXPECT_EQ(test, airoha_xdsl_validate_line(&state), 0);
	state.status = AIROHA_XDSL_LINE_FAILED;
	state.error = -EIO;
	KUNIT_EXPECT_EQ(test, airoha_xdsl_validate_line(&state), 0);
	state.error = EIO;
	KUNIT_EXPECT_EQ(test, airoha_xdsl_validate_line(&state), -EINVAL);
	state.error = 0;
	state.status = AIROHA_XDSL_LINE_FAILED + 1;
	KUNIT_EXPECT_EQ(test, airoha_xdsl_validate_line(&state), -EINVAL);
}

static void airoha_xdsl_test_duplicate(struct kunit *test)
{
	struct airoha_xdsl_line_state a = airoha_xdsl_test_showtime();
	struct airoha_xdsl_line_state b = a;

	KUNIT_EXPECT_TRUE(test, airoha_xdsl_same_line(&a, &b));
	b.upstream_bps++;
	KUNIT_EXPECT_FALSE(test, airoha_xdsl_same_line(&a, &b));
	b = a;
	b.status = AIROHA_XDSL_LINE_DOWN;
	KUNIT_EXPECT_FALSE(test, airoha_xdsl_same_line(&a, &b));
	b = a;
	b.tx_bearer = 1;
	KUNIT_EXPECT_FALSE(test, airoha_xdsl_same_line(&a, &b));
}

static struct kunit_case airoha_xdsl_cases[] = {
	KUNIT_CASE(airoha_xdsl_test_bearers),
	KUNIT_CASE(airoha_xdsl_test_invalid_bearers),
	KUNIT_CASE(airoha_xdsl_test_atm),
	KUNIT_CASE(airoha_xdsl_test_rates),
	KUNIT_CASE(airoha_xdsl_test_status),
	KUNIT_CASE(airoha_xdsl_test_duplicate),
	{}
};

static struct kunit_suite airoha_xdsl_suite = {
	.name = "airoha-xdsl",
	.test_cases = airoha_xdsl_cases,
};

kunit_test_suite(airoha_xdsl_suite);

MODULE_LICENSE("GPL");
