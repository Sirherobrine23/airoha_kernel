// SPDX-License-Identifier: GPL-2.0-only
/* EN7512/EN7516 PTM word-0 validation, without a DMT or DMA engine. */

#include <kunit/test.h>

#include "airoha_eth.h"

static void ptm_rx_bearer0_test(struct kunit *test)
{
	/* Numeric fixtures also catch an accidental change in field placement. */
	KUNIT_EXPECT_TRUE(test, econet_ptm_rx_supported(0x00000000));
	KUNIT_EXPECT_TRUE(test, econet_ptm_rx_supported(0x00000008));
}

static void ptm_rx_other_paths_test(struct kunit *test)
{
	unsigned int path;

	/* Path = line[2] | bearer[1] | preemption[0]. Do not merge bearer 1
	 * or a bonded second line into the single bearer-0 Ethernet interface.
	 */
	for (path = 2; path < 256; path++)
		KUNIT_EXPECT_FALSE(test, econet_ptm_rx_supported(path << 3));
}

static void ptm_rx_mac_errors_test(struct kunit *test)
{
	unsigned int bit;

	for (bit = 0; bit < 3; bit++) {
		KUNIT_EXPECT_FALSE(test, econet_ptm_rx_supported(BIT(bit)));
		KUNIT_EXPECT_FALSE(test, econet_ptm_rx_supported(BIT(bit) | 8));
	}
}

static void ptm_rx_control_test(struct kunit *test)
{
	KUNIT_EXPECT_FALSE(test, econet_ptm_rx_supported(0x00000800));
	KUNIT_EXPECT_FALSE(test, econet_ptm_rx_supported(0x00001000));
	KUNIT_EXPECT_FALSE(test, econet_ptm_rx_supported(0x00001808));
}

static void ptm_rx_reserved_test(struct kunit *test)
{
	/* Unlike PON GEM metadata, PTM word-0 bits 31:13 are reserved. */
	KUNIT_EXPECT_TRUE(test, econet_ptm_rx_supported(0xffffe000));
	KUNIT_EXPECT_TRUE(test, econet_ptm_rx_supported(0xffffe008));
}

static struct kunit_case ptm_test_cases[] = {
	KUNIT_CASE(ptm_rx_bearer0_test),
	KUNIT_CASE(ptm_rx_other_paths_test),
	KUNIT_CASE(ptm_rx_mac_errors_test),
	KUNIT_CASE(ptm_rx_control_test),
	KUNIT_CASE(ptm_rx_reserved_test),
	{}
};

static struct kunit_suite ptm_test_suite = {
	.name = "airoha-ptm-metadata",
	.test_cases = ptm_test_cases,
};

kunit_test_suite(ptm_test_suite);

MODULE_LICENSE("GPL");
