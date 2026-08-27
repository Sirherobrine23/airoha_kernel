// SPDX-License-Identifier: GPL-2.0-only
/*
 * OMCI sysfs interface below the owning xPON device.
 *
 * Generic Netlink remains the structured management API. The sysfs group
 * exports the small, stable subset useful to shell scripts and early userspace.
 */

#include <linux/ctype.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "../internal.h"
#include "internal.h"

static struct omci_device *omci_dev_from_dev(struct device *dev)
{
	struct xpon_device *xpon = dev_get_drvdata(dev);

	return xpon ? xpon->omci : NULL;
}

static int omci_sysfs_config_set(struct omci_device *odev, u16 key,
				 const void *value, size_t len)
{
	u8 event = OMCI_EVENT_CONFIG_CHANGE;
	int ret;

	ret = omci_agent_config_set_userspace(odev, key, value, len,
					      OMCI_CONFIG_SOURCE_SYSFS);
	if (ret)
		return ret;

	if (key == OMCI_CONFIG_OLT_PROFILE ||
	    key == OMCI_CONFIG_OLT_PROFILE_FORCE)
		event = OMCI_EVENT_PROFILE_CHANGE;
	omci_device_notify(odev, event);
	return 0;
}

static int omci_sysfs_config_get_u8(struct omci_device *odev, u16 key,
				    u8 *value)
{
	size_t len = sizeof(*value);

	return omci_agent_config_get(odev, key, value, &len);
}

static ssize_t dev_id_show(struct device *dev, struct device_attribute *attr,
			   char *buf)
{
	struct omci_device *odev = omci_dev_from_dev(dev);

	return sysfs_emit(buf, "%u\n", odev->id);
}
static DEVICE_ATTR_RO(dev_id);

static ssize_t ifindex_show(struct device *dev, struct device_attribute *attr,
			    char *buf)
{
	struct omci_device *odev = omci_dev_from_dev(dev);

	return sysfs_emit(buf, "%u\n", odev->ifindex);
}
static DEVICE_ATTR_RO(ifindex);

static void omci_sysfs_channel_snapshot(struct omci_device *odev, u16 *onu_id,
					u16 *gem_port_id, u8 *state,
					bool *channel_up)
{
	spin_lock_bh(&odev->state_lock);
	if (onu_id)
		*onu_id = odev->onu_id;
	if (gem_port_id)
		*gem_port_id = odev->gem_port_id;
	if (state)
		*state = odev->state;
	if (channel_up)
		*channel_up = odev->channel_up;
	spin_unlock_bh(&odev->state_lock);
}

static ssize_t onu_id_show(struct device *dev, struct device_attribute *attr,
			   char *buf)
{
	struct omci_device *odev = omci_dev_from_dev(dev);
	u16 value;

	omci_sysfs_channel_snapshot(odev, &value, NULL, NULL, NULL);
	return sysfs_emit(buf, "%u\n", value);
}
static DEVICE_ATTR_RO(onu_id);

static ssize_t gem_port_id_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct omci_device *odev = omci_dev_from_dev(dev);
	u16 value;

	omci_sysfs_channel_snapshot(odev, NULL, &value, NULL, NULL);
	return sysfs_emit(buf, "%u\n", value);
}
static DEVICE_ATTR_RO(gem_port_id);

static ssize_t state_show(struct device *dev, struct device_attribute *attr,
			  char *buf)
{
	struct omci_device *odev = omci_dev_from_dev(dev);
	u8 value;

	omci_sysfs_channel_snapshot(odev, NULL, NULL, &value, NULL);
	return sysfs_emit(buf, "%u\n", value);
}
static DEVICE_ATTR_RO(state);

static ssize_t channel_up_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct omci_device *odev = omci_dev_from_dev(dev);
	bool value;

	omci_sysfs_channel_snapshot(odev, NULL, NULL, NULL, &value);
	return sysfs_emit(buf, "%u\n", value);
}
static DEVICE_ATTR_RO(channel_up);

static ssize_t capabilities_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct omci_device *odev = omci_dev_from_dev(dev);

	return sysfs_emit(buf, "0x%08x\n", odev->capabilities);
}
static DEVICE_ATTR_RO(capabilities);

#define OMCI_ATOMIC64_ATTR_RO(_name, _member) \
static ssize_t _name##_show(struct device *dev, \
			    struct device_attribute *attr, char *buf) \
{ \
	struct omci_device *odev = omci_dev_from_dev(dev); \
	return sysfs_emit(buf, "%lld\n", \
			  (long long)atomic64_read(&odev->_member)); \
} \
static DEVICE_ATTR_RO(_name)

OMCI_ATOMIC64_ATTR_RO(rx_packets, rx_packets);
OMCI_ATOMIC64_ATTR_RO(rx_bytes, rx_bytes);
OMCI_ATOMIC64_ATTR_RO(rx_dropped, rx_dropped);
OMCI_ATOMIC64_ATTR_RO(tx_packets, tx_packets);
OMCI_ATOMIC64_ATTR_RO(tx_bytes, tx_bytes);
OMCI_ATOMIC64_ATTR_RO(tx_errors, tx_errors);

#define OMCI_AGENT_ATOMIC64_ATTR_RO(_name, _member) \
static ssize_t _name##_show(struct device *dev, \
			    struct device_attribute *attr, char *buf) \
{ \
	struct omci_device *odev = omci_dev_from_dev(dev); \
	return sysfs_emit(buf, "%lld\n", \
			  (long long)atomic64_read(&odev->agent._member)); \
} \
static DEVICE_ATTR_RO(_name)

OMCI_AGENT_ATOMIC64_ATTR_RO(agent_responses, responses);
OMCI_AGENT_ATOMIC64_ATTR_RO(agent_duplicates, duplicates);
OMCI_AGENT_ATOMIC64_ATTR_RO(agent_unsupported, unsupported);
OMCI_AGENT_ATOMIC64_ATTR_RO(agent_fake_responses, fake_responses);

static ssize_t mib_sync_show(struct device *dev, struct device_attribute *attr,
			     char *buf)
{
	struct omci_device *odev = omci_dev_from_dev(dev);
	u16 value;

	mutex_lock(&odev->agent.lock);
	value = odev->agent.mib_sync;
	mutex_unlock(&odev->agent.lock);
	return sysfs_emit(buf, "%u\n", value);
}
static DEVICE_ATTR_RO(mib_sync);

static ssize_t agent_operational_show(struct device *dev,
				      struct device_attribute *attr, char *buf)
{
	struct omci_device *odev = omci_dev_from_dev(dev);
	bool value;

	mutex_lock(&odev->agent.lock);
	value = odev->agent.operational;
	mutex_unlock(&odev->agent.lock);
	return sysfs_emit(buf, "%u\n", value);
}
static DEVICE_ATTR_RO(agent_operational);

#define OMCI_CONFIG_U8_ATTR_RW(_name, _key) \
static ssize_t _name##_show(struct device *dev, \
			    struct device_attribute *attr, char *buf) \
{ \
	struct omci_device *odev = omci_dev_from_dev(dev); \
	u8 value; \
	int ret = omci_sysfs_config_get_u8(odev, _key, &value); \
	if (ret) \
		return ret; \
	return sysfs_emit(buf, "%u\n", value); \
} \
static ssize_t _name##_store(struct device *dev, \
			     struct device_attribute *attr, \
			     const char *buf, size_t count) \
{ \
	struct omci_device *odev = omci_dev_from_dev(dev); \
	u8 value; \
	int ret = kstrtou8(buf, 0, &value); \
	if (ret) \
		return ret; \
	ret = omci_sysfs_config_set(odev, _key, &value, sizeof(value)); \
	return ret ? ret : count; \
} \
static DEVICE_ATTR_RW(_name)

#define OMCI_CONFIG_BOOL_ATTR_RW(_name, _key) \
static ssize_t _name##_show(struct device *dev, \
			    struct device_attribute *attr, char *buf) \
{ \
	struct omci_device *odev = omci_dev_from_dev(dev); \
	u8 value; \
	int ret = omci_sysfs_config_get_u8(odev, _key, &value); \
	if (ret) \
		return ret; \
	return sysfs_emit(buf, "%u\n", !!value); \
} \
static ssize_t _name##_store(struct device *dev, \
			     struct device_attribute *attr, \
			     const char *buf, size_t count) \
{ \
	struct omci_device *odev = omci_dev_from_dev(dev); \
	bool enabled; \
	u8 value; \
	int ret = kstrtobool(buf, &enabled); \
	if (ret) \
		return ret; \
	value = enabled; \
	ret = omci_sysfs_config_set(odev, _key, &value, sizeof(value)); \
	return ret ? ret : count; \
} \
static DEVICE_ATTR_RW(_name)

OMCI_CONFIG_U8_ATTR_RW(traffic_management_option, OMCI_CONFIG_TRAFFIC_MGMT_OPTION);
OMCI_CONFIG_U8_ATTR_RW(uni_count, OMCI_CONFIG_UNI_COUNT);
OMCI_CONFIG_U8_ATTR_RW(omcc_version, OMCI_CONFIG_OMCC_VERSION);
OMCI_CONFIG_BOOL_ATTR_RW(agent_enabled, OMCI_CONFIG_AGENT_ENABLED);
OMCI_CONFIG_BOOL_ATTR_RW(agent_permissive, OMCI_CONFIG_AGENT_PERMISSIVE);
OMCI_CONFIG_BOOL_ATTR_RW(agent_fake_omci, OMCI_CONFIG_AGENT_FAKE_OMCI);
OMCI_CONFIG_BOOL_ATTR_RW(agent_dying_gasp, OMCI_CONFIG_AGENT_DYING_GASP);

static ssize_t omci_sysfs_string_show(struct omci_device *odev, u16 key,
				      char *buf)
{
	u8 value[OMCI_MAX_CONFIG_VALUE];
	size_t len = sizeof(value);
	int ret;

	ret = omci_agent_config_get(odev, key, value, &len);
	if (ret)
		return ret;
	return sysfs_emit(buf, "%.*s\n", (int)len, value);
}

static ssize_t omci_sysfs_string_store(struct omci_device *odev, u16 key,
				       const char *buf, size_t count)
{
	size_t input_count = count;
	int ret;

	while (count && (buf[count - 1] == '\n' || buf[count - 1] == '\r'))
		count--;
	if (!count)
		return -EINVAL;

	ret = omci_sysfs_config_set(odev, key, buf, count);
	return ret ? ret : input_count;
}

#define OMCI_CONFIG_STRING_ATTR_RW(_name, _key) \
static ssize_t _name##_show(struct device *dev, \
			    struct device_attribute *attr, char *buf) \
{ \
	return omci_sysfs_string_show(omci_dev_from_dev(dev), _key, buf); \
} \
static ssize_t _name##_store(struct device *dev, \
			     struct device_attribute *attr, \
			     const char *buf, size_t count) \
{ \
	return omci_sysfs_string_store(omci_dev_from_dev(dev), _key, buf, count); \
} \
static DEVICE_ATTR_RW(_name)

OMCI_CONFIG_STRING_ATTR_RW(equipment_id, OMCI_CONFIG_EQUIPMENT_ID);
OMCI_CONFIG_STRING_ATTR_RW(hardware_version, OMCI_CONFIG_HARDWARE_VERSION);
OMCI_CONFIG_STRING_ATTR_RW(software_version_0, OMCI_CONFIG_SOFTWARE_VERSION_0);
OMCI_CONFIG_STRING_ATTR_RW(software_version_1, OMCI_CONFIG_SOFTWARE_VERSION_1);

static ssize_t vendor_id_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	struct omci_device *odev = omci_dev_from_dev(dev);
	u8 value[4];
	size_t len = sizeof(value);
	unsigned int i;
	int ret;

	ret = omci_agent_config_get(odev, OMCI_CONFIG_VENDOR_ID, value, &len);
	if (ret)
		return ret;
	for (i = 0; i < sizeof(value); i++)
		if (!isprint(value[i]))
			return sysfs_emit(buf, "%*phN\n", (int)sizeof(value), value);
	return sysfs_emit(buf, "%.*s\n", (int)sizeof(value), value);
}

static ssize_t vendor_id_store(struct device *dev,
			       struct device_attribute *attr,
			       const char *buf, size_t count)
{
	return omci_sysfs_string_store(omci_dev_from_dev(dev),
				       OMCI_CONFIG_VENDOR_ID, buf, count);
}
static DEVICE_ATTR_RW(vendor_id);

static ssize_t serial_number_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct omci_device *odev = omci_dev_from_dev(dev);
	u8 value[8];
	size_t len = sizeof(value);
	unsigned int i;
	int ret;

	ret = omci_agent_config_get(odev, OMCI_CONFIG_SERIAL_NUMBER,
				    value, &len);
	if (ret)
		return ret;
	for (i = 0; i < 4; i++)
		if (!isprint(value[i]))
			return sysfs_emit(buf, "%*phN\n", (int)sizeof(value), value);

	return sysfs_emit(buf, "%c%c%c%c%02X%02X%02X%02X\n",
			  value[0], value[1], value[2], value[3],
			  value[4], value[5], value[6], value[7]);
}

static ssize_t serial_number_store(struct device *dev,
				   struct device_attribute *attr,
				   const char *buf, size_t count)
{
	return omci_sysfs_string_store(omci_dev_from_dev(dev),
				       OMCI_CONFIG_SERIAL_NUMBER, buf, count);
}
static DEVICE_ATTR_RW(serial_number);

static ssize_t password_store(struct device *dev,
			      struct device_attribute *attr,
			      const char *buf, size_t count)
{
	return omci_sysfs_string_store(omci_dev_from_dev(dev), OMCI_CONFIG_PASSWORD,
				       buf, count);
}
static DEVICE_ATTR_WO(password);

struct omci_onu_type_name {
	u8 type;
	const char *name;
	const char *description;
};

static const struct omci_onu_type_name omci_onu_types[] = {
	{ OMCI_ONU_TYPE_OTHER, "Other", "Other" },
	{ OMCI_ONU_TYPE_SFU, "SFU", "Single Family Unit" },
	{ OMCI_ONU_TYPE_HGU, "HGU", "Home Gateway Unit" },
	{ OMCI_ONU_TYPE_MDU, "MDU", "Multi-Dwelling Unit" },
	{ OMCI_ONU_TYPE_SBU, "SBU", "Single Business Unit" },
	{ OMCI_ONU_TYPE_MTU, "MTU", "Multi-Tenant Unit" },
	{ OMCI_ONU_TYPE_CBU, "CBU", "Cellular Backhaul Unit" },
};

static const char *omci_sysfs_onu_type_name(u8 type)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(omci_onu_types); i++)
		if (omci_onu_types[i].type == type)
			return omci_onu_types[i].name;
	return "Other";
}

static int omci_sysfs_parse_onu_type(const char *buf, size_t count, u8 *type)
{
	char input[32];
	char *name;
	unsigned int i;
	int ret;

	if (!count || count >= sizeof(input))
		return -EINVAL;
	memcpy(input, buf, count);
	input[count] = '\0';
	name = strim(input);

	ret = kstrtou8(name, 0, type);
	if (!ret)
		return *type <= OMCI_ONU_TYPE_CBU ? 0 : -EINVAL;

	for (i = 0; i < ARRAY_SIZE(omci_onu_types); i++)
		if (!strcasecmp(name, omci_onu_types[i].name) ||
		    !strcasecmp(name, omci_onu_types[i].description)) {
			*type = omci_onu_types[i].type;
			return 0;
		}
	return -EINVAL;
}

static ssize_t onu_type_show(struct device *dev,
			     struct device_attribute *attr, char *buf)
{
	struct omci_device *odev = omci_dev_from_dev(dev);
	u8 type;
	int ret;

	ret = omci_sysfs_config_get_u8(odev, OMCI_CONFIG_ONU_TYPE, &type);
	if (ret)
		return ret;
	return sysfs_emit(buf, "%s\n", omci_sysfs_onu_type_name(type));
}

static ssize_t onu_type_store(struct device *dev,
			      struct device_attribute *attr,
			      const char *buf, size_t count)
{
	struct omci_device *odev = omci_dev_from_dev(dev);
	u8 type;
	int ret;

	ret = omci_sysfs_parse_onu_type(buf, count, &type);
	if (ret)
		return ret;
	ret = omci_sysfs_config_set(odev, OMCI_CONFIG_ONU_TYPE,
				    &type, sizeof(type));
	return ret ? ret : count;
}
static DEVICE_ATTR_RW(onu_type);

static ssize_t onu_type_id_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct omci_device *odev = omci_dev_from_dev(dev);
	u8 type;
	int ret;

	ret = omci_sysfs_config_get_u8(odev, OMCI_CONFIG_ONU_TYPE, &type);
	if (ret)
		return ret;
	return sysfs_emit(buf, "%u\n", type);
}
static DEVICE_ATTR_RO(onu_type_id);

static int omci_sysfs_parse_profile(const char *buf, size_t count,
				    bool force, u8 *profile)
{
	char input[32];
	char *name;
	unsigned int i;
	int ret;

	if (!count || count >= sizeof(input))
		return -EINVAL;
	memcpy(input, buf, count);
	input[count] = '\0';
	name = strim(input);

	ret = kstrtou8(name, 0, profile);
	if (!ret)
		return force ? (omci_profile_forceable(*profile) ? 0 : -EINVAL) :
			       (omci_profile_valid(*profile) ? 0 : -EINVAL);

	if (force && !strcasecmp(name, "unspecified")) {
		*profile = OMCI_OLT_PROFILE_UNSPEC;
		return 0;
	}

	for (i = OMCI_OLT_PROFILE_GENERIC; i <= OMCI_OLT_PROFILE_ZTE; i++) {
		if (!strcasecmp(name, omci_olt_profile_name(i))) {
			if (force && !omci_profile_forceable(i))
				return -EINVAL;
			*profile = i;
			return 0;
		}
	}
	return -EINVAL;
}

static ssize_t olt_profile_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct omci_device *odev = omci_dev_from_dev(dev);
	u8 profile;
	int ret;

	ret = omci_sysfs_config_get_u8(odev, OMCI_CONFIG_OLT_PROFILE, &profile);
	if (ret)
		return ret;
	return sysfs_emit(buf, "%u %s\n", profile,
			  omci_olt_profile_name(profile));
}

static ssize_t olt_profile_store(struct device *dev,
				 struct device_attribute *attr,
				 const char *buf, size_t count)
{
	struct omci_device *odev = omci_dev_from_dev(dev);
	u8 profile;
	int ret;

	ret = omci_sysfs_parse_profile(buf, count, false, &profile);
	if (ret)
		return ret;
	ret = omci_sysfs_config_set(odev, OMCI_CONFIG_OLT_PROFILE,
				    &profile, sizeof(profile));
	return ret ? ret : count;
}
static DEVICE_ATTR_RW(olt_profile);

static ssize_t olt_profile_force_show(struct device *dev,
				      struct device_attribute *attr, char *buf)
{
	struct omci_device *odev = omci_dev_from_dev(dev);
	u8 profile;
	int ret;

	ret = omci_sysfs_config_get_u8(odev, OMCI_CONFIG_OLT_PROFILE_FORCE,
				       &profile);
	if (ret)
		return ret;
	return sysfs_emit(buf, "%u %s\n", profile,
			  omci_olt_profile_name(profile));
}

static ssize_t olt_profile_force_store(struct device *dev,
				       struct device_attribute *attr,
				       const char *buf, size_t count)
{
	struct omci_device *odev = omci_dev_from_dev(dev);
	u8 profile;
	int ret;

	ret = omci_sysfs_parse_profile(buf, count, true, &profile);
	if (ret)
		return ret;
	ret = omci_sysfs_config_set(odev, OMCI_CONFIG_OLT_PROFILE_FORCE,
				    &profile, sizeof(profile));
	return ret ? ret : count;
}
static DEVICE_ATTR_RW(olt_profile_force);

static ssize_t olt_profile_effective_show(struct device *dev,
					  struct device_attribute *attr,
					  char *buf)
{
	struct omci_device *odev = omci_dev_from_dev(dev);
	u8 profile;

	mutex_lock(&odev->agent.lock);
	profile = odev->agent.profile_effective;
	mutex_unlock(&odev->agent.lock);
	return sysfs_emit(buf, "%u %s\n", profile,
			  omci_olt_profile_name(profile));
}
static DEVICE_ATTR_RO(olt_profile_effective);

static ssize_t olt_profile_quirks_show(struct device *dev,
				       struct device_attribute *attr, char *buf)
{
	struct omci_device *odev = omci_dev_from_dev(dev);
	u32 quirks;

	mutex_lock(&odev->agent.lock);
	quirks = odev->agent.profile_quirks;
	mutex_unlock(&odev->agent.lock);
	return sysfs_emit(buf, "0x%08x\n", quirks);
}
static DEVICE_ATTR_RO(olt_profile_quirks);

static ssize_t olt_vendor_id_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct omci_device *odev = omci_dev_from_dev(dev);
	struct omci_olt_g olt = {};

	if (omci_agent_olt_g_get(odev, &olt) || !olt.vendor_id_valid)
		return sysfs_emit(buf, "unknown\n");
	return sysfs_emit(buf, "%s\n", olt.vendor_id);
}
static DEVICE_ATTR_RO(olt_vendor_id);

static ssize_t olt_equipment_id_show(struct device *dev,
				     struct device_attribute *attr, char *buf)
{
	struct omci_device *odev = omci_dev_from_dev(dev);
	struct omci_olt_g olt = {};

	if (omci_agent_olt_g_get(odev, &olt) || !olt.equipment_id_valid)
		return sysfs_emit(buf, "unknown\n");
	return sysfs_emit(buf, "%s\n", olt.equipment_id);
}
static DEVICE_ATTR_RO(olt_equipment_id);

static ssize_t olt_version_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct omci_device *odev = omci_dev_from_dev(dev);
	struct omci_olt_g olt = {};

	if (omci_agent_olt_g_get(odev, &olt) || !olt.version_valid)
		return sysfs_emit(buf, "unknown\n");
	return sysfs_emit(buf, "%s\n", olt.version);
}
static DEVICE_ATTR_RO(olt_version);

static struct attribute *omci_attrs[] = {
	&dev_attr_dev_id.attr,
	&dev_attr_ifindex.attr,
	&dev_attr_onu_id.attr,
	&dev_attr_gem_port_id.attr,
	&dev_attr_state.attr,
	&dev_attr_channel_up.attr,
	&dev_attr_capabilities.attr,
	&dev_attr_rx_packets.attr,
	&dev_attr_rx_bytes.attr,
	&dev_attr_rx_dropped.attr,
	&dev_attr_tx_packets.attr,
	&dev_attr_tx_bytes.attr,
	&dev_attr_tx_errors.attr,
	&dev_attr_agent_responses.attr,
	&dev_attr_agent_duplicates.attr,
	&dev_attr_agent_unsupported.attr,
	&dev_attr_agent_fake_responses.attr,
	&dev_attr_mib_sync.attr,
	&dev_attr_agent_operational.attr,
	&dev_attr_agent_enabled.attr,
	&dev_attr_agent_permissive.attr,
	&dev_attr_agent_fake_omci.attr,
	&dev_attr_agent_dying_gasp.attr,
	&dev_attr_serial_number.attr,
	&dev_attr_vendor_id.attr,
	&dev_attr_password.attr,
	&dev_attr_equipment_id.attr,
	&dev_attr_hardware_version.attr,
	&dev_attr_software_version_0.attr,
	&dev_attr_software_version_1.attr,
	&dev_attr_traffic_management_option.attr,
	&dev_attr_onu_type.attr,
	&dev_attr_onu_type_id.attr,
	&dev_attr_uni_count.attr,
	&dev_attr_omcc_version.attr,
	&dev_attr_olt_profile.attr,
	&dev_attr_olt_profile_force.attr,
	&dev_attr_olt_profile_effective.attr,
	&dev_attr_olt_profile_quirks.attr,
	&dev_attr_olt_vendor_id.attr,
	&dev_attr_olt_equipment_id.attr,
	&dev_attr_olt_version.attr,
	NULL,
};
static const struct attribute_group omci_group = {
	.name = "omci",
	.attrs = omci_attrs,
};

int omci_sysfs_init(void)
{
	return 0;
}

void omci_sysfs_exit(void)
{
}

int omci_sysfs_register(struct omci_device *odev)
{
	return sysfs_create_group(&odev->xpon->class_dev->kobj, &omci_group);
}

void omci_sysfs_unregister(struct omci_device *odev)
{
	if (odev->xpon && odev->xpon->class_dev)
		sysfs_remove_group(&odev->xpon->class_dev->kobj, &omci_group);
}
