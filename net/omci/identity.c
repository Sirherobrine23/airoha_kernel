// SPDX-License-Identifier: GPL-2.0-only
/*
 * OMCI identity loading and normalization
 */

#include <linux/ctype.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/export.h>
#include <linux/kernel.h>
#include <linux/nvmem-consumer.h>
#include <linux/property.h>
#include <linux/slab.h>
#include <linux/string.h>

#include <net/omci.h>

#define OMCI_IDENTITY_MAX_INPUT_LEN	64

static bool omci_source_can_replace(u8 current, u8 source)
{
	return source >= current;
}

static size_t omci_trim_input(const u8 **data, size_t len)
{
	while (len && isspace(**data)) {
		(*data)++;
		len--;
	}
	while (len && (data[0][len - 1] == '\0' ||
		       data[0][len - 1] == 0xff ||
		       isspace(data[0][len - 1])))
		len--;

	return len;
}

static int omci_hex_compact(const u8 *data, size_t len, u8 *output,
			    size_t output_len)
{
	size_t digits = 0;
	size_t i;
	int high = -1;

	if (len >= 4 && !strncasecmp((const char *)data, "hex:", 4)) {
		data += 4;
		len -= 4;
	} else if (len >= 2 && data[0] == '0' &&
		   (data[1] == 'x' || data[1] == 'X')) {
		data += 2;
		len -= 2;
	}

	memset(output, 0, output_len);
	for (i = 0; i < len; i++) {
		int value;

		if (data[i] == ':' || data[i] == '-' || data[i] == '.' ||
		    data[i] == '_' || isspace(data[i]))
			continue;
		value = hex_to_bin(data[i]);
		if (value < 0)
			return -EINVAL;
		if (high < 0) {
			high = value;
		} else {
			if (digits / 2 >= output_len)
				return -E2BIG;
			output[digits / 2] = (high << 4) | value;
			high = -1;
		}
		digits++;
	}

	if (high >= 0 || digits != output_len * 2)
		return -EINVAL;

	return 0;
}

static bool omci_all_hex(const u8 *data, size_t len)
{
	size_t i;

	for (i = 0; i < len; i++)
		if (hex_to_bin(data[i]) < 0)
			return false;
	return true;
}

static int omci_normalize_serial(const u8 *data, size_t len, u8 serial[8])
{
	const u8 *trimmed = data;
	u8 suffix[4];
	int ret;

	if (len == 8) {
		memcpy(serial, data, 8);
		return 0;
	}
	len = omci_trim_input(&trimmed, len);
	if (len == 12 && omci_all_hex(trimmed + 4, 8)) {
		ret = omci_hex_compact(trimmed + 4, 8, suffix, sizeof(suffix));
		if (ret)
			return ret;
		memcpy(serial, trimmed, 4);
		memcpy(serial + 4, suffix, sizeof(suffix));
		return 0;
	}

	return omci_hex_compact(trimmed, len, serial, 8);
}

static int omci_normalize_vendor(const u8 *data, size_t len, u8 vendor[4])
{
	const u8 *trimmed = data;

	if (len == 4) {
		memcpy(vendor, data, 4);
		return 0;
	}
	len = omci_trim_input(&trimmed, len);
	return omci_hex_compact(trimmed, len, vendor, 4);
}

static int omci_normalize_password(const u8 *data, size_t len,
				   u8 password[10])
{
	const u8 *trimmed = data;

	if (len == 10) {
		memcpy(password, data, 10);
		return 0;
	}
	len = omci_trim_input(&trimmed, len);
	if (!len)
		return -EINVAL;
	if (len == 20 || (len > 4 && !strncasecmp((const char *)trimmed, "hex:", 4)))
		return omci_hex_compact(trimmed, len, password, 10);
	if (len > 10)
		return -E2BIG;

	memset(password, 0, 10);
	memcpy(password, trimmed, len);
	return 0;
}

static int omci_normalize_string(const u8 *data, size_t len, u8 *output,
				 size_t output_len)
{
	const u8 *trimmed = data;

	len = omci_trim_input(&trimmed, len);
	if (!len || len > output_len)
		return -EINVAL;
	memset(output, 0, output_len);
	memcpy(output, trimmed, len);
	return 0;
}

static int omci_identity_set_serial(struct omci_identity *identity,
				    const u8 *data, size_t len, u8 source)
{
	u8 serial[8];
	int ret;

	ret = omci_normalize_serial(data, len, serial);
	if (ret)
		return ret;
	if (omci_source_can_replace(identity->serial_source, source)) {
		memcpy(identity->serial_number, serial, sizeof(serial));
		identity->serial_source = source;
		identity->valid |= OMCI_IDENTITY_F_SERIAL_NUMBER;
	}
	if (omci_source_can_replace(identity->vendor_source, source)) {
		memcpy(identity->vendor_id, serial, sizeof(identity->vendor_id));
		identity->vendor_source = source;
		identity->valid |= OMCI_IDENTITY_F_VENDOR_ID;
	}
	return 0;
}

static int omci_identity_set_vendor(struct omci_identity *identity,
				    const u8 *data, size_t len, u8 source)
{
	u8 vendor[4];
	int ret;

	ret = omci_normalize_vendor(data, len, vendor);
	if (ret)
		return ret;
	if (!omci_source_can_replace(identity->vendor_source, source))
		return 0;

	memcpy(identity->vendor_id, vendor, sizeof(vendor));
	identity->vendor_source = source;
	identity->valid |= OMCI_IDENTITY_F_VENDOR_ID;
	if (identity->valid & OMCI_IDENTITY_F_SERIAL_NUMBER)
		memcpy(identity->serial_number, vendor, sizeof(vendor));
	return 0;
}

static int omci_identity_set_password(struct omci_identity *identity,
				      const u8 *data, size_t len, u8 source)
{
	u8 password[10];
	int ret;

	ret = omci_normalize_password(data, len, password);
	if (ret)
		return ret;
	if (!omci_source_can_replace(identity->password_source, source))
		return 0;

	memcpy(identity->password, password, sizeof(password));
	identity->password_source = source;
	identity->valid |= OMCI_IDENTITY_F_PASSWORD;
	return 0;
}

static int omci_identity_set_version(struct omci_identity *identity,
				     const u8 *data, size_t len, u8 source)
{
	u8 version[OMCI_OLT_VERSION_LEN];
	int ret;

	ret = omci_normalize_string(data, len, version, sizeof(version));
	if (ret)
		return ret;
	if (!omci_source_can_replace(identity->version_source, source))
		return 0;

	memcpy(identity->version, version, sizeof(version));
	identity->version_source = source;
	identity->valid |= OMCI_IDENTITY_F_VERSION;
	return 0;
}

static int omci_identity_set_equipment(struct omci_identity *identity,
				       const u8 *data, size_t len, u8 source)
{
	u8 equipment[OMCI_OLT_EQUIPMENT_ID_LEN];
	int ret;

	ret = omci_normalize_string(data, len, equipment, sizeof(equipment));
	if (ret)
		return ret;
	if (!omci_source_can_replace(identity->equipment_source, source))
		return 0;

	memcpy(identity->equipment_id, equipment, sizeof(equipment));
	identity->equipment_source = source;
	identity->valid |= OMCI_IDENTITY_F_EQUIPMENT_ID;
	return 0;
}

int omci_identity_normalize_config(u16 key, const void *value, size_t len,
				   u8 *output, size_t *output_len)
{
	size_t required;
	int ret;

	if (!value || !output || !output_len)
		return -EINVAL;
	switch (key) {
	case OMCI_CONFIG_SERIAL_NUMBER:
		required = 8;
		if (*output_len < required)
			return -ENOSPC;
		ret = omci_normalize_serial(value, len, output);
		break;
	case OMCI_CONFIG_VENDOR_ID:
		required = 4;
		if (*output_len < required)
			return -ENOSPC;
		ret = omci_normalize_vendor(value, len, output);
		break;
	case OMCI_CONFIG_PASSWORD:
		required = 10;
		if (*output_len < required)
			return -ENOSPC;
		ret = omci_normalize_password(value, len, output);
		break;
	case OMCI_CONFIG_VERSION:
		required = OMCI_OLT_VERSION_LEN;
		if (*output_len < required)
			return -ENOSPC;
		ret = omci_normalize_string(value, len, output, required);
		break;
	case OMCI_CONFIG_EQUIPMENT_ID:
		required = OMCI_OLT_EQUIPMENT_ID_LEN;
		if (*output_len < required)
			return -ENOSPC;
		ret = omci_normalize_string(value, len, output, required);
		break;
	default:
		return -EOPNOTSUPP;
	}
	if (ret)
		return ret;
	*output_len = required;
	return 0;
}

typedef int (*omci_identity_setter_t)(struct omci_identity *identity,
				      const u8 *data, size_t len, u8 source);

static int omci_identity_read_property(struct device *dev,
				       struct omci_identity *identity,
				       const char *name,
				       omci_identity_setter_t setter)
{
	u8 data[OMCI_IDENTITY_MAX_INPUT_LEN];
	const char *string;
	int count;
	int ret;

	ret = device_property_read_string(dev, name, &string);
	if (!ret)
		return setter(identity, string, strlen(string),
			      OMCI_CONFIG_SOURCE_DEVICE_TREE);

	count = device_property_count_u8(dev, name);
	if (count < 0)
		return count == -EINVAL || count == -ENODATA ? 0 : count;
	if (!count || count > sizeof(data))
		return -EINVAL;
	ret = device_property_read_u8_array(dev, name, data, count);
	if (ret)
		return ret;
	return setter(identity, data, count, OMCI_CONFIG_SOURCE_DEVICE_TREE);
}

static int omci_identity_read_cell(struct device *dev,
				   struct omci_identity *identity,
				   const char *name,
				   omci_identity_setter_t setter)
{
	struct nvmem_cell *cell;
	void *data;
	size_t len;
	int ret;

	cell = nvmem_cell_get(dev, name);
	if (IS_ERR(cell)) {
		ret = PTR_ERR(cell);
		if (ret == -ENOENT || ret == -ENODEV || ret == -EOPNOTSUPP)
			return 0;
		return ret;
	}
	data = nvmem_cell_read(cell, &len);
	nvmem_cell_put(cell);
	if (IS_ERR(data))
		return PTR_ERR(data);
	ret = setter(identity, data, len, OMCI_CONFIG_SOURCE_NVMEM);
	kfree(data);
	return ret;
}

static int omci_identity_load_properties(struct device *dev,
					 struct omci_identity *identity)
{
	static const struct {
		const char *name;
		omci_identity_setter_t setter;
	} properties[] = {
		{ "airoha,gpon-serial-number", omci_identity_set_serial },
		{ "airoha,gpon-password", omci_identity_set_password },
		{ "omci-serial-number", omci_identity_set_serial },
		{ "omci-vendor-id", omci_identity_set_vendor },
		{ "omci-password", omci_identity_set_password },
		{ "omci-version", omci_identity_set_version },
		{ "omci-equipment-id", omci_identity_set_equipment },
	};
	unsigned int i;
	int ret;

	for (i = 0; i < ARRAY_SIZE(properties); i++) {
		ret = omci_identity_read_property(dev, identity,
						  properties[i].name,
						  properties[i].setter);
		if (ret)
			return dev_err_probe(dev, ret,
					     "invalid %s identity property\n",
					     properties[i].name);
	}
	return 0;
}

static int omci_identity_load_nvmem(struct device *dev,
				    struct omci_identity *identity)
{
	static const struct {
		const char *name;
		omci_identity_setter_t setter;
	} cells[] = {
		{ "gpon-serial-number", omci_identity_set_serial },
		{ "omci-vendor-id", omci_identity_set_vendor },
		{ "gpon-password", omci_identity_set_password },
		{ "omci-version", omci_identity_set_version },
		{ "omci-equipment-id", omci_identity_set_equipment },
	};
	unsigned int i;
	int ret;

	for (i = 0; i < ARRAY_SIZE(cells); i++) {
		ret = omci_identity_read_cell(dev, identity, cells[i].name,
					      cells[i].setter);
		if (ret)
			return dev_err_probe(dev, ret,
					     "failed to read %s NVMEM cell\n",
					     cells[i].name);
	}
	return 0;
}

int omci_identity_load(struct device *dev, struct omci_identity *identity)
{
	int ret;

	if (!dev || !identity)
		return -EINVAL;
	memset(identity, 0, sizeof(*identity));

	ret = omci_identity_load_properties(dev, identity);
	if (ret)
		return ret;
	return omci_identity_load_nvmem(dev, identity);
}
EXPORT_SYMBOL_GPL(omci_identity_load);
