// SPDX-License-Identifier: GPL-2.0-only
#include <kunit/test.h>
#include <linux/soc/airoha/pkgids.h>

#include "airoha_dmt_data.h"

static const struct airoha_dmt_data_spec dmt_specs[] = {
	{ .target = 0x7512, .digital_regs = 34, .digital_maps = 208 },
	{ .target = 0x7516, .digital_regs = 44, .digital_maps = 262 },
};

static void dmt_checksum(u8 *image, size_t size)
{
	put_unaligned_le32(crc32_le(~0U, image + 32, size - 32) ^ ~0U, image + 24);
}

static void dmt_test_map(u8 *map, unsigned int count, unsigned int regs,
			 unsigned int width)
{
	unsigned int i;

	for (i = 0; i < count; i++, map += AIROHA_DMT_MAP_SIZE) {
		put_unaligned_le16(i, map);
		map[2] = i < regs ? i : 0;
		map[3] = i < regs ? width : 1;
	}
}

static u8 *dmt_fixture(struct kunit *test, unsigned int target, size_t *size)
{
	const struct airoha_dmt_data_spec *spec = &dmt_specs[target];
	u8 *image, *map;

	*size = 32 + spec->digital_regs * 4 + AIROHA_DMT_ANALOG_REGS +
		(spec->digital_maps + AIROHA_DMT_ANALOG_MAPS) * AIROHA_DMT_MAP_SIZE;
	/* Extra byte permits checking an otherwise valid image with trailing data. */
	image = kunit_kzalloc(test, *size + 1, GFP_KERNEL);
	if (!image)
		return NULL;
	memcpy(image, "AIRODMT\0", 8);
	put_unaligned_le16(1, image + 8);
	put_unaligned_le16(spec->target, image + 10);
	put_unaligned_le16(spec->digital_regs, image + 12);
	put_unaligned_le16(AIROHA_DMT_ANALOG_REGS, image + 14);
	put_unaligned_le16(spec->digital_maps, image + 16);
	put_unaligned_le16(AIROHA_DMT_ANALOG_MAPS, image + 18);
	put_unaligned_le32(*size - 32, image + 20);
	map = image + 32 + spec->digital_regs * 4 + AIROHA_DMT_ANALOG_REGS;
	dmt_test_map(map, spec->digital_maps, spec->digital_regs, 32);
	map += spec->digital_maps * AIROHA_DMT_MAP_SIZE;
	dmt_test_map(map, AIROHA_DMT_ANALOG_MAPS, AIROHA_DMT_ANALOG_REGS, 8);
	dmt_checksum(image, *size);
	return image;
}

static void dmt_valid_targets_test(struct kunit *test)
{
	struct airoha_dmt_data parsed;
	unsigned int target;
	size_t size;
	u8 *image;

	for (target = 0; target < ARRAY_SIZE(dmt_specs); target++) {
		image = dmt_fixture(test, target, &size);
		KUNIT_ASSERT_NOT_NULL(test, image);
		KUNIT_ASSERT_EQ(test, airoha_dmt_data_parse(image, size,
							    &dmt_specs[target], &parsed), 0);
		KUNIT_EXPECT_PTR_EQ(test, parsed.digital, image + 32);
		KUNIT_EXPECT_EQ(test, airoha_dmt_data_parse(image, size,
							    &dmt_specs[!target], &parsed), -EINVAL);
		KUNIT_EXPECT_PTR_EQ(test, parsed.digital, NULL);
	}
}

static void dmt_lengths_test(struct kunit *test)
{
	struct airoha_dmt_data parsed;
	size_t size, len;
	u8 *image = dmt_fixture(test, 0, &size);

	KUNIT_ASSERT_NOT_NULL(test, image);
	for (len = 0; len < size; len++)
		KUNIT_EXPECT_EQ(test, airoha_dmt_data_parse(image, len,
							    &dmt_specs[0], &parsed), -EINVAL);
	KUNIT_EXPECT_EQ(test, airoha_dmt_data_parse(image, size + 1,
						    &dmt_specs[0], &parsed), -EINVAL);
	put_unaligned_le32(U32_MAX, image + 20);
	KUNIT_EXPECT_EQ(test, airoha_dmt_data_parse(image, size,
						    &dmt_specs[0], &parsed), -EINVAL);
}

static void dmt_header_test(struct kunit *test)
{
	static const unsigned int offsets[] = { 0, 8, 10, 12, 14, 16, 18, 28 };
	struct airoha_dmt_data parsed;
	unsigned int i;
	size_t size;
	u8 *image = dmt_fixture(test, 0, &size);

	KUNIT_ASSERT_NOT_NULL(test, image);
	for (i = 0; i < ARRAY_SIZE(offsets); i++) {
		image[offsets[i]] ^= 1;
		KUNIT_EXPECT_EQ(test, airoha_dmt_data_parse(image, size,
							    &dmt_specs[0], &parsed), -EINVAL);
		image[offsets[i]] ^= 1;
	}
}

static void dmt_checksum_test(struct kunit *test)
{
	struct airoha_dmt_data parsed;
	size_t size;
	u8 *image = dmt_fixture(test, 0, &size);

	KUNIT_ASSERT_NOT_NULL(test, image);
	image[32] ^= 1;
	KUNIT_EXPECT_EQ(test, airoha_dmt_data_parse(image, size,
						    &dmt_specs[0], &parsed), -EBADMSG);
	KUNIT_EXPECT_PTR_EQ(test, parsed.digital, NULL);
}

static void dmt_map_bounds_test(struct kunit *test)
{
	static const u8 invalid[][2] = {
		{ 2, 44 }, { 3, 0 }, { 3, 33 }, { 4, 32 }, { 5, 1 }, { 0, 255 },
	};
	struct airoha_dmt_data parsed;
	unsigned int i;
	size_t size;
	u8 *entry, old;
	u8 *image = dmt_fixture(test, 1, &size);

	KUNIT_ASSERT_NOT_NULL(test, image);
	entry = image + 32 + 44 * 4 + AIROHA_DMT_ANALOG_REGS;
	for (i = 0; i < ARRAY_SIZE(invalid); i++) {
		old = entry[invalid[i][0]];
		entry[invalid[i][0]] = invalid[i][1];
		dmt_checksum(image, size);
		KUNIT_EXPECT_EQ(test, airoha_dmt_data_parse(image, size,
							    &dmt_specs[1], &parsed), -EINVAL);
		entry[invalid[i][0]] = old;
	}
}

static void dmt_fields_test(struct kunit *test)
{
	/* Full 32-bit register and its top two bits. */
	const u8 map[] = { 0, 0, 0, 32, 0, 0, 1, 0, 0, 2, 30, 0 };
	u32 shadow = 0;

	KUNIT_ASSERT_EQ(test, airoha_dmt_map_validate(map, 2, 1, 32), 0);
	KUNIT_ASSERT_EQ(test, airoha_dmt_field_set(map, 2, 0, 0x12345678, &shadow), 0);
	KUNIT_EXPECT_EQ(test, shadow, 0x12345678U);
	KUNIT_ASSERT_EQ(test, airoha_dmt_field_set(map, 2, 1, 3, &shadow), 0);
	KUNIT_EXPECT_EQ(test, shadow, 0xd2345678U);
	KUNIT_EXPECT_EQ(test, airoha_dmt_field_set(map, 2, 1, 4, &shadow), -ERANGE);
	KUNIT_EXPECT_EQ(test, airoha_dmt_field_set(map, 2, 2, 0, &shadow), -EINVAL);
	KUNIT_EXPECT_EQ(test, shadow, 0xd2345678U);
}

static void dmt_vendor_duplicate_test(struct kunit *test)
{
	struct airoha_dmt_data parsed;
	u32 shadow[AIROHA_DMT_ANALOG_REGS] = {};
	size_t size;
	unsigned int i;
	u8 *map;
	u8 *image = dmt_fixture(test, 0, &size);

	KUNIT_ASSERT_NOT_NULL(test, image);
	map = image + 32 + 34 * 4 + AIROHA_DMT_ANALOG_REGS + 208 * 6;
	for (i = 149; i <= 151; i++)
		put_unaligned_le16(148, map + i * 6);
	dmt_checksum(image, size);
	KUNIT_ASSERT_EQ(test, airoha_dmt_data_parse(image, size,
						    &dmt_specs[0], &parsed), 0);
	for (i = 149; i <= 151; i++) {
		int ret;

		ret = airoha_dmt_field_set(parsed.analog_map,
					   AIROHA_DMT_ANALOG_MAPS, i, 1, shadow);
		KUNIT_EXPECT_EQ(test, ret, -EINVAL);
	}
}

static void dmt_pkgids_test(struct kunit *test)
{
	struct airoha_soc_id_regs regs = { .hir = EN751221_PKG };

	regs.efuse_data0 = AIROHA_EFUSE_EN7512;
	KUNIT_EXPECT_EQ(test, airoha_soc_variant_id(&regs), EN7512);
	regs.efuse_data0 = AIROHA_EFUSE_REMARK_BIT |
		(AIROHA_EFUSE_EN7513G << AIROHA_EFUSE_PKG_REMARK_SHIFT);
	KUNIT_EXPECT_EQ(test, airoha_soc_variant_id(&regs), EN7513G);
	regs.hir = 0;
	regs.pdidr = 0x7516;
	regs.efuse_data0 = AIROHA_EFUSE_EN7516G;
	KUNIT_EXPECT_EQ(test, airoha_soc_variant_id(&regs), EN7516G);
	regs.efuse_data0 = 0;
	KUNIT_EXPECT_NE(test, airoha_soc_variant_id(&regs), EN7516G);
}

static struct kunit_case dmt_cases[] = {
	KUNIT_CASE(dmt_valid_targets_test),
	KUNIT_CASE(dmt_lengths_test),
	KUNIT_CASE(dmt_header_test),
	KUNIT_CASE(dmt_checksum_test),
	KUNIT_CASE(dmt_map_bounds_test),
	KUNIT_CASE(dmt_fields_test),
	KUNIT_CASE(dmt_vendor_duplicate_test),
	KUNIT_CASE(dmt_pkgids_test),
	{}
};

static struct kunit_suite dmt_suite = {
	.name = "airoha-dmt-data",
	.test_cases = dmt_cases,
};

kunit_test_suite(dmt_suite);

MODULE_LICENSE("GPL");
