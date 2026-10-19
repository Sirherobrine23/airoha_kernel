/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _AIROHA_DMT_DATA_H
#define _AIROHA_DMT_DATA_H

#include <linux/crc32.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/unaligned.h>

#define AIROHA_DMT_DATA_HEADER_SIZE	32
#define AIROHA_DMT_MAP_SIZE		6
#define AIROHA_DMT_ANALOG_REGS		52
#define AIROHA_DMT_ANALOG_MAPS		201
#define AIROHA_DMT_MAX_DIGITAL_REGS	44

struct airoha_dmt_data_spec {
	u16 target;
	u16 digital_regs;
	u16 digital_maps;
};

struct airoha_dmt_data {
	const u8 *digital;
	const u8 *analog;
	const u8 *digital_map;
	const u8 *analog_map;
};

/* Canonical LE data, never an ELF image or a list of executable operations. */
static inline int
airoha_dmt_map_validate(const u8 *map, unsigned int count,
			unsigned int regs, unsigned int bits)
{
	unsigned int i;

	for (i = 0; i < count; i++, map += AIROHA_DMT_MAP_SIZE) {
		unsigned int id = get_unaligned_le16(map);
		unsigned int reg = map[2], width = map[3], shift = map[4];

		if (id >= count || reg >= regs || !width || width > bits ||
		    shift + width > bits || map[5])
			return -EINVAL;
		/* Full-register entries must be an identity mapping. */
		if (i < regs && (id != i || reg != i || width != bits || shift))
			return -EINVAL;
		/* Three vendor EN7556 entries repeat ID 148. Retain, don't fix. */
		if (id != i && !(bits == 8 && i >= 149 && i <= 151 && id == 148))
			return -EINVAL;
	}
	return 0;
}

static inline int
airoha_dmt_data_parse(const u8 *buf, size_t size,
		      const struct airoha_dmt_data_spec *spec,
		      struct airoha_dmt_data *out)
{
	struct airoha_dmt_data data;
	size_t payload;
	int ret;

	memset(out, 0, sizeof(*out));
	if (!buf || size < AIROHA_DMT_DATA_HEADER_SIZE)
		return -EINVAL;
	if (memcmp(buf, "AIRODMT\0", 8) || get_unaligned_le16(buf + 8) != 1 ||
	    get_unaligned_le16(buf + 10) != spec->target ||
	    get_unaligned_le16(buf + 12) != spec->digital_regs ||
	    get_unaligned_le16(buf + 14) != AIROHA_DMT_ANALOG_REGS ||
	    get_unaligned_le16(buf + 16) != spec->digital_maps ||
	    get_unaligned_le16(buf + 18) != AIROHA_DMT_ANALOG_MAPS ||
	    get_unaligned_le32(buf + 28))
		return -EINVAL;

	payload = spec->digital_regs * sizeof(u32) + AIROHA_DMT_ANALOG_REGS +
		  (spec->digital_maps + AIROHA_DMT_ANALOG_MAPS) * AIROHA_DMT_MAP_SIZE;
	if (size != AIROHA_DMT_DATA_HEADER_SIZE + payload ||
	    get_unaligned_le32(buf + 20) != payload)
		return -EINVAL;
	if ((crc32_le(~0U, buf + AIROHA_DMT_DATA_HEADER_SIZE, payload) ^ ~0U) !=
	    get_unaligned_le32(buf + 24))
		return -EBADMSG;

	data.digital = buf + AIROHA_DMT_DATA_HEADER_SIZE;
	data.analog = data.digital + spec->digital_regs * sizeof(u32);
	data.digital_map = data.analog + AIROHA_DMT_ANALOG_REGS;
	data.analog_map = data.digital_map + spec->digital_maps * AIROHA_DMT_MAP_SIZE;
	ret = airoha_dmt_map_validate(data.digital_map, spec->digital_maps,
				      spec->digital_regs, 32);
	if (ret)
		return ret;
	ret = airoha_dmt_map_validate(data.analog_map, AIROHA_DMT_ANALOG_MAPS,
				      AIROHA_DMT_ANALOG_REGS, 8);
	if (ret)
		return ret;
	*out = data;
	return 0;
}

/* Only a previously validated map is accepted; no arbitrary MMIO addresses. */
static inline int
airoha_dmt_field_set(const u8 *map, unsigned int count, unsigned int id,
		     u32 value, u32 *shadow)
{
	const u8 *entry;
	u32 mask;

	if (id >= count)
		return -EINVAL;
	entry = map + id * AIROHA_DMT_MAP_SIZE;
	if (get_unaligned_le16(entry) != id)
		return -EINVAL;
	mask = U32_MAX >> (32 - entry[3]);
	if (value & ~mask)
		return -ERANGE;
	shadow[entry[2]] &= ~(mask << entry[4]);
	shadow[entry[2]] |= value << entry[4];
	return 0;
}

#endif /* _AIROHA_DMT_DATA_H */
