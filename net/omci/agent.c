// SPDX-License-Identifier: GPL-2.0-only
/*
 * In-kernel ONU Management and Control Interface agent
 *
 * The agent implements the baseline OMCI transaction path and owns the
 * operational MIB. Managed entities without a dedicated hardware callback
 * are stored as opaque baseline attribute data so that userspace can inspect,
 * persist and amend them through Generic Netlink.
 */

#include <linux/bitmap.h>
#include <linux/errno.h>
#include <linux/int_log.h>
#include <linux/jhash.h>
#include <linux/jiffies.h>
#include <linux/if_vlan.h>
#include <linux/kernel.h>
#include <linux/math.h>
#include <linux/math64.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/unaligned.h>
#include <net/netlink.h>

#include "internal.h"
#include "me.h"
#include "wire.h"

#define OMCI_MSG_TYPE_CREATE		4
#define OMCI_MSG_TYPE_DELETE		6
#define OMCI_MSG_TYPE_SET		8
#define OMCI_MSG_TYPE_GET		9
#define OMCI_MSG_TYPE_GET_ALL_ALARMS	11
#define OMCI_MSG_TYPE_GET_ALL_ALARMS_NEXT 12
#define OMCI_MSG_TYPE_MIB_UPLOAD	13
#define OMCI_MSG_TYPE_MIB_UPLOAD_NEXT	14
#define OMCI_MSG_TYPE_MIB_RESET	15
#define OMCI_MSG_TYPE_ALARM_NOTIFICATION 16
#define OMCI_MSG_TYPE_TEST		18
#define OMCI_MSG_TYPE_START_SW_DOWNLOAD 19
#define OMCI_MSG_TYPE_DOWNLOAD_SECTION	20
#define OMCI_MSG_TYPE_END_SW_DOWNLOAD	21
#define OMCI_MSG_TYPE_ACTIVATE_SW	22
#define OMCI_MSG_TYPE_COMMIT_SW	23
#define OMCI_MSG_TYPE_SYNC_TIME	24
#define OMCI_MSG_TYPE_REBOOT		25
#define OMCI_MSG_TYPE_GET_NEXT		26
#define OMCI_MSG_TYPE_TEST_RESULT	27
#define OMCI_MSG_TYPE_GET_CURRENT_DATA	28
#define OMCI_MSG_TYPE_SET_TABLE	29

#define OMCI_RESULT_SUCCESS		0
#define OMCI_RESULT_PROCESSING_ERROR	1
#define OMCI_RESULT_NOT_SUPPORTED	2
#define OMCI_RESULT_PARAMETER_ERROR	3
#define OMCI_RESULT_UNKNOWN_ME		4
#define OMCI_RESULT_UNKNOWN_INSTANCE	5
#define OMCI_RESULT_DEVICE_BUSY	6
#define OMCI_RESULT_INSTANCE_EXISTS	7
#define OMCI_RESULT_ATTRIBUTE_FAILED	9

#define OMCI_BRIDGE_TP_PPTP_ETH_UNI	1
#define OMCI_BRIDGE_TP_8021P_MAPPER	3
#define OMCI_BRIDGE_TP_GEM_IWTP	5
#define OMCI_BRIDGE_TP_MULTICAST_GEM_IWTP	6
#define OMCI_BRIDGE_TP_VEIP		11

static void omci_agent_reset_table_snapshot_locked(struct omci_agent *agent);

static bool omci_agent_result_can_be_faked(u8 result)
{
	switch (result) {
	case OMCI_RESULT_NOT_SUPPORTED:
	case OMCI_RESULT_UNKNOWN_ME:
	case OMCI_RESULT_UNKNOWN_INSTANCE:
	case OMCI_RESULT_INSTANCE_EXISTS:
		return true;
	default:
		return false;
	}
}

#define OMCI_PRIORITY_QUEUE_ATTR_MASK		0xfff0
#define OMCI_TRAFFIC_SCHEDULER_ATTR_MASK	0xf000

#define OMCI_PRIORITY_QUEUE_QUEUE_CONFIG_MASK	0x8000
#define OMCI_PRIORITY_QUEUE_MAX_SIZE_MASK	0x4000
#define OMCI_PRIORITY_QUEUE_ALLOC_SIZE_MASK	0x2000
#define OMCI_PRIORITY_QUEUE_DISCARD_RESET_MASK	0x1000
#define OMCI_PRIORITY_QUEUE_DISCARD_THRESHOLD_MASK 0x0800
#define OMCI_PRIORITY_QUEUE_RELATED_PORT_MASK	0x0400
#define OMCI_PRIORITY_QUEUE_SCHEDULER_MASK	0x0200
#define OMCI_PRIORITY_QUEUE_WEIGHT_MASK	0x0100
#define OMCI_PRIORITY_QUEUE_BACKPRESSURE_OP_MASK	0x0080
#define OMCI_PRIORITY_QUEUE_BACKPRESSURE_TIME_MASK 0x0040
#define OMCI_PRIORITY_QUEUE_BP_OCCUR_MASK	0x0020
#define OMCI_PRIORITY_QUEUE_BP_CLEAR_MASK	0x0010

#define OMCI_TRAFFIC_SCHEDULER_TCONT_MASK	0x8000
#define OMCI_TRAFFIC_SCHEDULER_PARENT_MASK	0x4000
#define OMCI_TRAFFIC_SCHEDULER_POLICY_MASK	0x2000
#define OMCI_TRAFFIC_SCHEDULER_PRIORITY_MASK	0x1000

#define OMCI_DEFAULT_TCONT_COUNT		8
#define OMCI_DEFAULT_QUEUES_PER_TCONT		1
#define OMCI_MAX_TCONT_RESOURCES		32
#define OMCI_MAX_QUEUES_PER_TCONT		8

#define OMCI_OMCI_ME_TYPE_TABLE_MASK		BIT(15)
#define OMCI_OMCI_MESSAGE_TYPE_TABLE_MASK	BIT(14)
#define OMCI_TABLE_SNAPSHOT_TIMEOUT		(60 * HZ)
#define OMCI_BASELINE_TABLE_FRAGMENT_LEN	29

#define OMCI_MANAGED_ENTITY_NAME_MASK		BIT(15)
#define OMCI_MANAGED_ENTITY_ATTR_TABLE_MASK	BIT(14)
#define OMCI_MANAGED_ENTITY_ACCESS_MASK		BIT(13)
#define OMCI_MANAGED_ENTITY_ALARMS_MASK		BIT(12)
#define OMCI_MANAGED_ENTITY_AVCS_MASK		BIT(11)
#define OMCI_MANAGED_ENTITY_ACTIONS_MASK	BIT(10)
#define OMCI_MANAGED_ENTITY_INSTANCES_MASK	BIT(9)
#define OMCI_MANAGED_ENTITY_SUPPORT_MASK	BIT(8)
#define OMCI_MANAGED_ENTITY_TABLE_MASKS \
	(OMCI_MANAGED_ENTITY_ATTR_TABLE_MASK | \
	 OMCI_MANAGED_ENTITY_ALARMS_MASK | \
	 OMCI_MANAGED_ENTITY_AVCS_MASK | \
	 OMCI_MANAGED_ENTITY_INSTANCES_MASK)

#define OMCI_ATTRIBUTE_NAME_MASK		BIT(15)
#define OMCI_ATTRIBUTE_SIZE_MASK		BIT(14)
#define OMCI_ATTRIBUTE_ACCESS_MASK		BIT(13)
#define OMCI_ATTRIBUTE_FORMAT_MASK		BIT(12)
#define OMCI_ATTRIBUTE_LOWER_LIMIT_MASK	BIT(11)
#define OMCI_ATTRIBUTE_UPPER_LIMIT_MASK	BIT(10)
#define OMCI_ATTRIBUTE_BIT_FIELD_MASK		BIT(9)
#define OMCI_ATTRIBUTE_CODE_POINTS_MASK	BIT(8)
#define OMCI_ATTRIBUTE_SUPPORT_MASK		BIT(7)
#define OMCI_ATTRIBUTE_TABLE_MASKS		OMCI_ATTRIBUTE_CODE_POINTS_MASK

#define OMCI_CAPABILITY_NAME_LEN		25
#define OMCI_ATTRIBUTE_FORMAT_BIT_FIELD	2
#define OMCI_ATTRIBUTE_FORMAT_UNSIGNED		4
#define OMCI_ATTRIBUTE_FORMAT_STRING		5
#define OMCI_ATTRIBUTE_FORMAT_TABLE		7

#define OMCI_ONU_G_DYING_GASP_ALARM	7
#define OMCI_ALARM_BITMAP_LEN		28
#define OMCI_ALARM_SEQUENCE_OFFSET	(8 + OMCI_ALARM_BITMAP_LEN)

#define OMCI_ANI_G_ENTITY_ID		0x8001
#define OMCI_ETHERNET_SLOT		0x01
#define OMCI_VEIP_SLOT			0x0a
#define OMCI_GPON_SLOT			0x80
#define OMCI_ETHERNET_UNIT_TYPE		47
#define OMCI_VEIP_UNIT_TYPE		48
#define OMCI_GPON_UNIT_TYPE		248
#define OMCI_EQUIPMENT_ENTITY_ID(_slot)	(0x0100 | (_slot))
#define OMCI_UNI_ENTITY_ID(_slot, _port)	(((_slot) << 8) | (_port))
#define OMCI_ANI_G_RX_LEVEL_MASK	0x0040
#define OMCI_ANI_G_TX_LEVEL_MASK	0x0004
#define OMCI_ANI_G_OPTICAL_LEVEL_MASK	(OMCI_ANI_G_RX_LEVEL_MASK | \
					 OMCI_ANI_G_TX_LEVEL_MASK)

#define OMCI_TEST_TYPE_UNSUPPORTED	0
#define OMCI_TEST_TYPE_VOLTAGE		1
#define OMCI_TEST_TYPE_RX_POWER		3
#define OMCI_TEST_TYPE_TX_POWER		5
#define OMCI_TEST_TYPE_BIAS		9
#define OMCI_TEST_TYPE_TEMPERATURE	12

#define OMCI_OPTICAL_LEVEL_SCALE	500
#define OMCI_NW_TO_DBM_0P002_OFFSET	30000
#define OMCI_NW_TO_DBUW_0P002_OFFSET	15000

#define OMCI_VLAN_FILTER_LIST_MASK	0x8000
#define OMCI_VLAN_FILTER_FORWARD_MASK	0x4000
#define OMCI_VLAN_FILTER_COUNT_MASK	0x2000

#define OMCI_EXT_VLAN_ASSOC_TYPE_MASK	0x8000
#define OMCI_EXT_VLAN_MAX_SIZE_MASK	0x4000
#define OMCI_EXT_VLAN_INPUT_TPID_MASK	0x2000
#define OMCI_EXT_VLAN_OUTPUT_TPID_MASK	0x1000
#define OMCI_EXT_VLAN_DOWNSTREAM_MASK	0x0800
#define OMCI_EXT_VLAN_TABLE_MASK	0x0400
#define OMCI_EXT_VLAN_ASSOC_PTR_MASK	0x0200
#define OMCI_EXT_VLAN_DSCP_MAP_MASK	0x0100

#define OMCI_OLT_G_VENDOR_ID_MASK	0x8000
#define OMCI_OLT_G_EQUIPMENT_ID_MASK	0x4000
#define OMCI_OLT_G_VERSION_MASK	0x2000
#define OMCI_OMCC_VERSION_G988_2010_BASELINE	0xa0
#define OMCI_OLT_G_TIME_OF_DAY_MASK	0x1000
#define OMCI_OLT_G_TIME_OF_DAY_LEN	14

#define OMCI_ONU_G_ATTR_MASK		GENMASK(15, 3)
#define OMCI_ONU2_G_ATTR_MASK		GENMASK(15, 2)

#define OMCI_ATTR_VALUE_ZERO		U8_MAX

struct omci_get_attr_layout {
	u16 mask;
	u8 offset;
	u8 len;
};

static const struct omci_get_attr_layout omci_onu_g_attr_layout[] = {
	{ BIT(15), 0, 4 },		/* Vendor ID */
	{ BIT(14), 4, 14 },		/* Version */
	{ BIT(13), 18, 8 },		/* Serial number */
	{ BIT(12), 26, 1 },		/* Traffic management option */
	{ BIT(11), 27, 1 },		/* Deprecated */
	{ BIT(10), 28, 1 },		/* Battery backup */
	{ BIT(9), 29, 1 },		/* Administrative state */
	{ BIT(8), 30, 1 },		/* Operational state */
	{ BIT(7), 31, 1 },		/* ONU survival time */
	{ BIT(6), OMCI_ATTR_VALUE_ZERO, 24 },	/* Logical ONU ID */
	{ BIT(5), OMCI_ATTR_VALUE_ZERO, 12 },	/* Logical password */
	{ BIT(4), OMCI_ATTR_VALUE_ZERO, 1 },	/* Credentials status */
	{ BIT(3), OMCI_ATTR_VALUE_ZERO, 1 },	/* Extended TC layer */
};

static const struct omci_get_attr_layout omci_onu2_g_attr_layout[] = {
	{ BIT(15), 0, 20 },		/* Equipment ID */
	{ BIT(14), 20, 1 },		/* OMCC version */
	{ BIT(13), 21, 2 },		/* Vendor product code */
	{ BIT(12), 23, 1 },		/* Security capability */
	{ BIT(11), 24, 1 },		/* Security mode */
	{ BIT(10), 25, 2 },		/* Total priority queues */
	{ BIT(9), 27, 1 },		/* Total traffic schedulers */
	{ BIT(8), 28, 1 },		/* Deprecated */
	{ BIT(7), 29, 2 },		/* Total GEM port IDs */
	{ BIT(6), OMCI_ATTR_VALUE_ZERO, 4 },	/* System uptime */
	{ BIT(5), OMCI_ATTR_VALUE_ZERO, 2 },	/* Connectivity capability */
	{ BIT(4), 31, 1 },		/* Connectivity mode */
	{ BIT(3), OMCI_ATTR_VALUE_ZERO, 2 },	/* QoS configuration */
	{ BIT(2), OMCI_ATTR_VALUE_ZERO, 2 },	/* Priority queue scale */
};

static const u8 *omci_olt_g_consume(const u8 **data, size_t *len,
				    size_t size)
{
	const u8 *value;

	if (*len < size)
		return NULL;
	value = *data;
	*data += size;
	*len -= size;

	return value;
}

static void omci_olt_g_copy_string(char *dest, size_t dest_len,
				   const u8 *source, size_t source_len)
{
	size_t len = min(source_len, dest_len - 1);

	memcpy(dest, source, len);
	dest[len] = '\0';
}

static int omci_olt_g_parse_set(struct omci_mib_object *object, u16 mask,
				const u8 *data, size_t len)
{
	struct omci_olt_g *olt = &object->olt_g;
	const u8 *value;

	if (mask & OMCI_OLT_G_VENDOR_ID_MASK) {
		value = omci_olt_g_consume(&data, &len,
					   OMCI_OLT_VENDOR_ID_LEN);
		if (!value)
			return -EINVAL;
		omci_olt_g_copy_string(olt->vendor_id, sizeof(olt->vendor_id),
				       value, OMCI_OLT_VENDOR_ID_LEN);
		olt->vendor_id_valid = true;
	}
	if (mask & OMCI_OLT_G_EQUIPMENT_ID_MASK) {
		value = omci_olt_g_consume(&data, &len,
					   OMCI_OLT_EQUIPMENT_ID_LEN);
		if (!value)
			return -EINVAL;
		omci_olt_g_copy_string(olt->equipment_id,
				       sizeof(olt->equipment_id), value,
				       OMCI_OLT_EQUIPMENT_ID_LEN);
		olt->equipment_id_valid = true;
	}
	if (mask & OMCI_OLT_G_VERSION_MASK) {
		value = omci_olt_g_consume(&data, &len, OMCI_OLT_VERSION_LEN);
		if (!value)
			return -EINVAL;
		omci_olt_g_copy_string(olt->version, sizeof(olt->version),
				       value, OMCI_OLT_VERSION_LEN);
		olt->version_valid = true;
	}
	if (mask & OMCI_OLT_G_TIME_OF_DAY_MASK) {
		value = omci_olt_g_consume(&data, &len,
					   OMCI_OLT_G_TIME_OF_DAY_LEN);
		if (!value)
			return -EINVAL;
	}
	olt->valid = olt->vendor_id_valid || olt->equipment_id_valid ||
		     olt->version_valid;

	return 0;
}

static void omci_vlan_filter_parse_entries(struct omci_vlan_tagging_filter *filter,
					   const u8 *data)
{
	unsigned int i;

	for (i = 0; i < OMCI_VLAN_FILTER_MAX_ENTRIES; i++) {
		u16 tci = get_unaligned_be16(data + i * sizeof(tci));

		filter->entries[i].tci = tci;
		filter->entries[i].vid = tci & GENMASK(11, 0);
		filter->entries[i].dei = (tci >> 12) & 0x1;
		filter->entries[i].pbit = (tci >> 13) & 0x7;
	}
}

static void omci_vlan_filter_parse_create(struct omci_mib_object *object,
					  const u8 *content)
{
	struct omci_vlan_tagging_filter *filter = &object->vlan_filter;

	omci_vlan_filter_parse_entries(filter, content);
	filter->forward_operation = content[24];
	filter->num_entries = min_t(u8, content[25],
				    OMCI_VLAN_FILTER_MAX_ENTRIES);
	filter->valid = true;
}

static int omci_vlan_filter_parse_set(struct omci_mib_object *object,
				      u16 mask, const u8 *data,
				      size_t len)
{
	struct omci_vlan_tagging_filter *filter = &object->vlan_filter;

	if (mask & OMCI_VLAN_FILTER_LIST_MASK) {
		if (len < 24)
			return -EINVAL;
		omci_vlan_filter_parse_entries(filter, data);
		data += 24;
		len -= 24;
	}
	if (mask & OMCI_VLAN_FILTER_FORWARD_MASK) {
		if (!len)
			return -EINVAL;
		filter->forward_operation = *data++;
		len--;
	}
	if (mask & OMCI_VLAN_FILTER_COUNT_MASK) {
		if (!len)
			return -EINVAL;
		filter->num_entries = min_t(u8, *data,
					    OMCI_VLAN_FILTER_MAX_ENTRIES);
	}
	filter->valid = true;

	return 0;
}

static bool omci_ext_vlan_rule_is_delete(const u8 *raw)
{
	unsigned int i;

	for (i = 8; i < OMCI_EXT_VLAN_RULE_LEN; i++)
		if (raw[i] != 0xff)
			return false;

	return true;
}

static void omci_ext_vlan_parse_rule(struct omci_extended_vlan_rule *rule,
				     const u8 *raw)
{
	u32 word;

	memcpy(rule->raw, raw, sizeof(rule->raw));
	word = get_unaligned_be32(raw);
	rule->filter_outer_pbit = (word >> 28) & 0xf;
	rule->filter_outer_vid = (word >> 15) & 0x1fff;
	rule->filter_outer_tpid_dei = (word >> 12) & 0x7;

	word = get_unaligned_be32(raw + 4);
	rule->filter_inner_pbit = (word >> 28) & 0xf;
	rule->filter_inner_vid = (word >> 15) & 0x1fff;
	rule->filter_inner_tpid_dei = (word >> 12) & 0x7;
	rule->filter_ethertype = word & 0xf;

	word = get_unaligned_be32(raw + 8);
	rule->tags_to_remove = (word >> 30) & 0x3;
	rule->treat_outer_pbit = (word >> 16) & 0xf;
	rule->treat_outer_vid = (word >> 3) & 0x1fff;
	rule->treat_outer_tpid_dei = word & 0x7;

	word = get_unaligned_be32(raw + 12);
	rule->treat_inner_pbit = (word >> 16) & 0xf;
	rule->treat_inner_vid = (word >> 3) & 0x1fff;
	rule->treat_inner_tpid_dei = word & 0x7;
	rule->delete = omci_ext_vlan_rule_is_delete(raw);
}

static int omci_ext_vlan_update_rule(struct omci_extended_vlan *vlan,
				     const u8 *raw)
{
	unsigned int index;
	bool delete;

	delete = omci_ext_vlan_rule_is_delete(raw);
	for (index = 0; index < vlan->rule_count; index++)
		if (!memcmp(vlan->rules[index].raw, raw, 8))
			break;

	if (delete) {
		if (index == vlan->rule_count)
			return 0;
		memmove(&vlan->rules[index], &vlan->rules[index + 1],
			(vlan->rule_count - index - 1) * sizeof(vlan->rules[0]));
		vlan->rule_count--;
		memset(&vlan->rules[vlan->rule_count], 0,
		       sizeof(vlan->rules[0]));
		return 0;
	}

	if (index == vlan->rule_count) {
		if (vlan->rule_count >= OMCI_EXT_VLAN_MAX_RULES)
			return -ENOSPC;
		vlan->rule_count++;
	}
	omci_ext_vlan_parse_rule(&vlan->rules[index], raw);

	return 0;
}

static void omci_ext_vlan_parse_create(struct omci_mib_object *object,
				       const u8 *content)
{
	struct omci_extended_vlan *vlan = &object->extended_vlan;

	vlan->association_type = content[0];
	vlan->associated_me = get_unaligned_be16(content + 24);
	vlan->max_table_size = OMCI_EXT_VLAN_MAX_RULES;
	vlan->valid = true;
}

static const u8 *omci_ext_vlan_consume(const u8 **data, size_t *len,
				       size_t size)
{
	const u8 *value;

	if (*len < size)
		return NULL;
	value = *data;
	*data += size;
	*len -= size;

	return value;
}

static int omci_ext_vlan_parse_set(struct omci_mib_object *object, u16 mask,
				   const u8 *data, size_t len)
{
	struct omci_extended_vlan *vlan = &object->extended_vlan;

	if (mask & OMCI_EXT_VLAN_ASSOC_TYPE_MASK) {
		const u8 *value = omci_ext_vlan_consume(&data, &len, 1);

		if (!value)
			return -EINVAL;
		vlan->association_type = value[0];
	}
	if (mask & OMCI_EXT_VLAN_MAX_SIZE_MASK) {
		const u8 *value = omci_ext_vlan_consume(&data, &len, 2);

		if (!value)
			return -EINVAL;
		vlan->max_table_size = get_unaligned_be16(value);
	}
	if (mask & OMCI_EXT_VLAN_INPUT_TPID_MASK) {
		const u8 *value = omci_ext_vlan_consume(&data, &len, 2);

		if (!value)
			return -EINVAL;
		vlan->input_tpid = get_unaligned_be16(value);
	}
	if (mask & OMCI_EXT_VLAN_OUTPUT_TPID_MASK) {
		const u8 *value = omci_ext_vlan_consume(&data, &len, 2);

		if (!value)
			return -EINVAL;
		vlan->output_tpid = get_unaligned_be16(value);
	}
	if (mask & OMCI_EXT_VLAN_DOWNSTREAM_MASK) {
		const u8 *value = omci_ext_vlan_consume(&data, &len, 1);

		if (!value)
			return -EINVAL;
		vlan->downstream_mode = value[0];
	}
	if (mask & OMCI_EXT_VLAN_TABLE_MASK) {
		const u8 *value = omci_ext_vlan_consume(&data, &len,
						 OMCI_EXT_VLAN_RULE_LEN);
		int ret;

		if (!value)
			return -EINVAL;
		ret = omci_ext_vlan_update_rule(vlan, value);
		if (ret)
			return ret;
	}
	if (mask & OMCI_EXT_VLAN_ASSOC_PTR_MASK) {
		const u8 *value = omci_ext_vlan_consume(&data, &len, 2);

		if (!value)
			return -EINVAL;
		vlan->associated_me = get_unaligned_be16(value);
	}
	if (mask & OMCI_EXT_VLAN_DSCP_MAP_MASK) {
		const u8 *value = omci_ext_vlan_consume(&data, &len,
						 sizeof(vlan->dscp_to_pbit));

		if (!value)
			return -EINVAL;
		memcpy(vlan->dscp_to_pbit, value, sizeof(vlan->dscp_to_pbit));
	}
	vlan->valid = true;

	return 0;
}

struct omci_attr_layout {
	u16 mask;
	u8 offset;
	u8 length;
};

static const struct omci_attr_layout omci_priority_queue_layout[] = {
	{ OMCI_PRIORITY_QUEUE_QUEUE_CONFIG_MASK, 0, 1 },
	{ OMCI_PRIORITY_QUEUE_MAX_SIZE_MASK, 1, 2 },
	{ OMCI_PRIORITY_QUEUE_ALLOC_SIZE_MASK, 3, 2 },
	{ OMCI_PRIORITY_QUEUE_DISCARD_RESET_MASK, 5, 2 },
	{ OMCI_PRIORITY_QUEUE_DISCARD_THRESHOLD_MASK, 7, 2 },
	{ OMCI_PRIORITY_QUEUE_RELATED_PORT_MASK, 9, 4 },
	{ OMCI_PRIORITY_QUEUE_SCHEDULER_MASK, 13, 2 },
	{ OMCI_PRIORITY_QUEUE_WEIGHT_MASK, 15, 1 },
	{ OMCI_PRIORITY_QUEUE_BACKPRESSURE_OP_MASK, 16, 2 },
	{ OMCI_PRIORITY_QUEUE_BACKPRESSURE_TIME_MASK, 18, 4 },
	{ OMCI_PRIORITY_QUEUE_BP_OCCUR_MASK, 22, 2 },
	{ OMCI_PRIORITY_QUEUE_BP_CLEAR_MASK, 24, 2 },
};

static const struct omci_attr_layout omci_traffic_scheduler_layout[] = {
	{ OMCI_TRAFFIC_SCHEDULER_TCONT_MASK, 0, 2 },
	{ OMCI_TRAFFIC_SCHEDULER_PARENT_MASK, 2, 2 },
	{ OMCI_TRAFFIC_SCHEDULER_POLICY_MASK, 4, 1 },
	{ OMCI_TRAFFIC_SCHEDULER_PRIORITY_MASK, 5, 1 },
};

static int omci_attr_parse_set(struct omci_mib_object *object, u16 mask,
			       const u8 *data, size_t len,
			       const struct omci_attr_layout *layout,
			       size_t layout_count, u16 supported_mask)
{
	size_t i;

	if (mask & ~supported_mask)
		return -EINVAL;

	for (i = 0; i < layout_count; i++) {
		if (!(mask & layout[i].mask))
			continue;
		if (len < layout[i].length)
			return -EINVAL;
		memcpy(object->data + layout[i].offset, data,
		       layout[i].length);
		data += layout[i].length;
		len -= layout[i].length;
	}

	return 0;
}

static int omci_attr_serialize(const struct omci_mib_object *object, u16 mask,
			       u8 *data, size_t len, size_t *encoded_len,
			       const struct omci_attr_layout *layout,
			       size_t layout_count, u16 supported_mask)
{
	size_t used = 0;
	size_t i;

	if (mask & ~supported_mask)
		return -EINVAL;

	for (i = 0; i < layout_count; i++) {
		if (!(mask & layout[i].mask))
			continue;
		if (len - used < layout[i].length)
			return -ENOSPC;
		memcpy(data + used, object->data + layout[i].offset,
		       layout[i].length);
		used += layout[i].length;
	}

	*encoded_len = used;
	return 0;
}

static int
omci_priority_queue_serialize(const struct omci_mib_object *object,
			      u16 mask, u8 *data, size_t len,
			      size_t *encoded_len)
{
	return omci_attr_serialize(object, mask, data, len, encoded_len,
				   omci_priority_queue_layout,
				   ARRAY_SIZE(omci_priority_queue_layout),
				   OMCI_PRIORITY_QUEUE_ATTR_MASK);
}

static int
omci_traffic_scheduler_serialize(const struct omci_mib_object *object,
				 u16 mask, u8 *data, size_t len,
				 size_t *encoded_len)
{
	return omci_attr_serialize(object, mask, data, len, encoded_len,
				   omci_traffic_scheduler_layout,
				   ARRAY_SIZE(omci_traffic_scheduler_layout),
				   OMCI_TRAFFIC_SCHEDULER_ATTR_MASK);
}

static int omci_mib_parse_create(struct omci_mib_object *object,
				 const u8 *content)
{
	switch (object->class_id) {
	case OMCI_CLASS_VLAN_TAGGING_FILTER_DATA:
		omci_vlan_filter_parse_create(object, content);
		break;
	case OMCI_CLASS_EXTENDED_VLAN:
		omci_ext_vlan_parse_create(object, content);
		break;
	default:
		break;
	}

	return 0;
}

static int omci_mib_parse_set(struct omci_mib_object *object, u16 mask,
			      const u8 *data, size_t len)
{
	switch (object->class_id) {
	case OMCI_CLASS_OLT_G:
		return omci_olt_g_parse_set(object, mask, data, len);
	case OMCI_CLASS_VLAN_TAGGING_FILTER_DATA:
		return omci_vlan_filter_parse_set(object, mask, data, len);
	case OMCI_CLASS_EXTENDED_VLAN:
		return omci_ext_vlan_parse_set(object, mask, data, len);
	case OMCI_CLASS_PRIORITY_QUEUE:
		return omci_attr_parse_set(object, mask, data, len,
				   omci_priority_queue_layout,
				   ARRAY_SIZE(omci_priority_queue_layout),
				   OMCI_PRIORITY_QUEUE_ATTR_MASK);
	case OMCI_CLASS_TRAFFIC_SCHEDULER:
		return omci_attr_parse_set(object, mask, data, len,
				   omci_traffic_scheduler_layout,
				   ARRAY_SIZE(omci_traffic_scheduler_layout),
				   OMCI_TRAFFIC_SCHEDULER_ATTR_MASK);
	default:
		return 0;
	}
}

static unsigned long omci_mib_key(u16 class_id, u16 entity_id)
{
	return ((unsigned long)class_id << 16) | entity_id;
}

static struct omci_mib_object *
omci_mib_lookup(struct omci_agent *agent, u16 class_id, u16 entity_id)
{
	struct omci_mib_object *object;

	object = xa_load(&agent->mib, omci_mib_key(class_id, entity_id));
	if (object && object->pending_delete)
		return NULL;

	return object;
}

static void omci_agent_set_mib_sync_locked(struct omci_agent *agent, u8 sync)
{
	struct omci_mib_object *object;

	agent->mib_sync = sync;
	object = omci_mib_lookup(agent, OMCI_CLASS_ONU_DATA, 0);
	if (object)
		object->data[0] = sync;
}

static void omci_agent_increment_mib_sync_locked(struct omci_agent *agent)
{
	omci_agent_set_mib_sync_locked(agent, (u8)(agent->mib_sync + 1));
}

static int omci_mib_store_locked(struct omci_agent *agent,
				 const struct omci_mib_object *source)
{
	struct omci_mib_object *object;
	void *old;

	object = kmemdup(source, sizeof(*object), GFP_KERNEL);
	if (!object)
		return -ENOMEM;

	old = xa_store(&agent->mib,
		       omci_mib_key(source->class_id, source->entity_id),
		       object, GFP_KERNEL);
	if (xa_is_err(old)) {
		kfree(object);
		return xa_err(old);
	}
	kfree(old);
	return 0;
}

static struct omci_olt_g *omci_agent_olt_g_locked(struct omci_agent *agent)
{
	struct omci_mib_object *object;

	object = omci_mib_lookup(agent, OMCI_CLASS_OLT_G, 0);
	if (!object || !object->olt_g.valid)
		return NULL;

	return &object->olt_g;
}

int omci_agent_olt_g_get(struct omci_device *odev, struct omci_olt_g *olt)
{
	struct omci_olt_g *stored;
	int ret = 0;

	if (!olt)
		return -EINVAL;

	mutex_lock(&odev->agent.lock);
	stored = omci_agent_olt_g_locked(&odev->agent);
	if (!stored)
		ret = -ENODATA;
	else
		*olt = *stored;
	mutex_unlock(&odev->agent.lock);

	return ret;
}

static void
omci_agent_profile_state_locked(struct omci_agent *agent,
				const struct omci_olt_g *olt,
				struct omci_olt_profile_state *state)
{
	u8 effective;

	memset(state, 0, sizeof(*state));
	state->configured = agent->config.olt_profile;
	state->forced = agent->config.olt_profile_force;

	if (state->forced != OMCI_OLT_PROFILE_UNSPEC)
		effective = state->forced;
	else if (state->configured == OMCI_OLT_PROFILE_AUTO)
		effective = omci_profile_detect(olt);
	else
		effective = state->configured;

	if (effective == OMCI_OLT_PROFILE_UNSPEC ||
	    effective == OMCI_OLT_PROFILE_AUTO)
		effective = OMCI_OLT_PROFILE_GENERIC;

	state->effective = effective;
	state->quirks = omci_profile_quirks(effective);
	if (olt)
		state->olt = *olt;
}

static int
omci_agent_profile_reconcile_locked(struct omci_device *odev,
				    u8 old_profile, u8 new_profile);
static int omci_agent_clear_services_locked(struct omci_device *odev);
static int omci_agent_reconcile_services_locked(struct omci_device *odev);
static void omci_agent_reset_duplicate_locked(struct omci_agent *agent);

static void omci_agent_free_object_array(struct xarray *objects)
{
	struct omci_mib_object *object;
	unsigned long index;

	xa_for_each(objects, index, object) {
		xa_erase(objects, index);
		kfree(object);
	}
}

static int omci_agent_snapshot_vendor_objects_locked(struct omci_agent *agent,
						      struct xarray *snapshot)
{
	struct omci_mib_object *object;
	struct omci_mib_object *copy;
	unsigned long index;
	void *old;

	xa_for_each(&agent->mib, index, object) {
		if (!object->owner_profile)
			continue;
		copy = kmemdup(object, sizeof(*copy), GFP_KERNEL);
		if (!copy)
			return -ENOMEM;
		old = xa_store(snapshot, index, copy, GFP_KERNEL);
		if (xa_is_err(old)) {
			kfree(copy);
			return xa_err(old);
		}
		kfree(old);
	}

	return 0;
}

static int omci_agent_restore_vendor_objects_locked(struct omci_agent *agent,
						     struct xarray *snapshot)
{
	struct omci_mib_object *object;
	unsigned long index;
	void *old;
	int ret = 0;

	xa_for_each(&agent->mib, index, object) {
		if (!object->owner_profile)
			continue;
		xa_erase(&agent->mib, index);
		kfree(object);
	}
	xa_for_each(snapshot, index, object) {
		xa_erase(snapshot, index);
		old = xa_store(&agent->mib, index, object, GFP_KERNEL);
		if (xa_is_err(old)) {
			kfree(object);
			if (!ret)
				ret = xa_err(old);
			continue;
		}
		kfree(old);
	}

	return ret;
}

static int
omci_agent_profile_refresh_locked(struct omci_device *odev,
				  struct omci_olt_g *olt,
				  bool *profile_changed)
{
	struct omci_agent *agent = &odev->agent;
	struct omci_olt_profile_state old_state;
	struct omci_olt_profile_state state;
	struct xarray snapshot;
	u32 old_quirks = agent->profile_quirks;
	u8 old_profile = agent->profile_effective;
	bool changed;
	int ret;

	omci_agent_profile_state_locked(agent, olt, &state);
	omci_profile_sanitize_olt_g(olt, state.quirks);
	if (olt)
		state.olt = *olt;

	changed = old_profile != state.effective || old_quirks != state.quirks;
	if (!changed)
		goto apply_profile;

	xa_init(&snapshot);
	ret = omci_agent_snapshot_vendor_objects_locked(agent, &snapshot);
	if (ret)
		goto destroy_snapshot;
	ret = omci_agent_profile_reconcile_locked(odev, old_profile,
						  state.effective);
	if (ret)
		goto rollback;

apply_profile:
	if (odev->ops->set_olt_profile) {
		ret = odev->ops->set_olt_profile(odev, &state);
		if (ret && changed)
			goto rollback;
		if (ret)
			return ret;
	}

	agent->profile_effective = state.effective;
	agent->profile_quirks = state.quirks;
	if (profile_changed)
		*profile_changed = changed;

	if (changed) {
		omci_agent_reset_table_snapshot_locked(agent);
		agent->upload_index = 0;
		omci_agent_free_object_array(&snapshot);
		xa_destroy(&snapshot);
		dev_info(odev->parent,
			 "OMCI OLT profile: configured=%s effective=%s forced=%s quirks=%#x\n",
			 omci_olt_profile_name(state.configured),
			 omci_olt_profile_name(state.effective),
			 omci_olt_profile_name(state.forced), state.quirks);
	}

	return 0;

rollback:
	omci_agent_restore_vendor_objects_locked(agent, &snapshot);
	agent->profile_effective = old_profile;
	agent->profile_quirks = old_quirks;
	omci_agent_reconcile_services_locked(odev);
	if (odev->ops->set_olt_profile) {
		old_state = state;
		old_state.effective = old_profile;
		old_state.quirks = old_quirks;
		odev->ops->set_olt_profile(odev, &old_state);
	}
	omci_agent_free_object_array(&snapshot);

destroy_snapshot:
	xa_destroy(&snapshot);
	return ret;
}

static bool
omci_agent_profile_has_quirk(const struct omci_agent *agent, u32 quirk)
{
	return agent->profile_quirks & quirk;
}

static bool
omci_agent_fakes_unsupported(const struct omci_agent *agent)
{
	return agent->fake_omci ||
	       omci_agent_profile_has_quirk(agent,
					    OMCI_OLT_QUIRK_FAKE_UNSUPPORTED_SUCCESS);
}

static bool
omci_agent_should_fake_result(const struct omci_agent *agent, u16 class_id,
			      u8 result)
{
	const struct omci_me_desc *desc = omci_me_lookup(agent, class_id);

	if (desc && (desc->flags & OMCI_ME_F_DATAPATH))
		return false;

	return omci_agent_fakes_unsupported(agent) &&
	       omci_agent_result_can_be_faked(result);
}

static int
omci_mib_add_default_profile_mask(struct omci_agent *agent, u8 profile,
				  u16 class_id, u16 entity_id,
				  u16 attr_mask, const void *data,
				  size_t len)
{
	const struct omci_me_desc *desc;
	struct omci_mib_object *object;
	int ret;

	desc = omci_me_lookup_profile(profile, class_id);
	object = kzalloc(sizeof(*object), GFP_KERNEL);
	if (!object)
		return -ENOMEM;

	object->class_id = class_id;
	object->entity_id = entity_id;
	object->attr_mask = attr_mask;
	object->origin = OMCI_MIB_ORIGIN_DEFAULT;
	object->owner_profile = desc && (desc->flags & OMCI_ME_F_VENDOR) ?
		profile : OMCI_OLT_PROFILE_UNSPEC;
	object->data_len = desc ? desc->data_len :
		min_t(size_t, len, sizeof(object->data));
	if (data)
		memcpy(object->data, data, min(len, sizeof(object->data)));

	ret = omci_mib_store_locked(agent, object);
	kfree(object);
	return ret;
}

static int omci_mib_add_default_mask(struct omci_agent *agent, u16 class_id,
				     u16 entity_id, u16 attr_mask,
				     const void *data, size_t len)
{
	u8 profile = agent->profile_effective;

	if (!profile)
		profile = OMCI_OLT_PROFILE_GENERIC;

	return omci_mib_add_default_profile_mask(agent, profile, class_id,
					 entity_id, attr_mask, data, len);
}

static int omci_mib_add_default(struct omci_agent *agent, u16 class_id,
				u16 entity_id, const void *data, size_t len)
{
	return omci_mib_add_default_mask(agent, class_id, entity_id, 0xffff,
					 data, len);
}

static int
omci_agent_seed_huawei_locked(struct omci_device *odev, u8 profile)
{
	struct omci_agent *agent = &odev->agent;
	u8 data[OMCI_MAX_ATTR_DATA] = {};
	int ret;

	data[1] = agent->config.traffic_mgmt_option;
	ret = omci_mib_add_default_profile_mask(agent, profile,
			OMCI_CLASS_HUAWEI_FLOW_MAPPING, 0, GENMASK(15, 7),
			data, sizeof(data));
	if (ret)
		return ret;

	memset(data, 0, sizeof(data));
	memcpy(data, agent->config.version, min_t(size_t, 14,
						 sizeof(agent->config.version)));
	data[14] = 1;
	data[15] = 1;
	data[16] = 1;
	ret = omci_mib_add_default_profile_mask(agent, profile,
			OMCI_CLASS_HUAWEI_SW_IMAGE_EXT, 0, GENMASK(15, 11),
			data, sizeof(data));
	if (ret)
		return ret;

	memset(data, 0, sizeof(data));
	ret = omci_mib_add_default_profile_mask(agent, profile,
			OMCI_CLASS_HUAWEI_MULTICAST_367, 0, GENMASK(15, 11),
			data, sizeof(data));
	if (ret)
		return ret;
	ret = omci_mib_add_default_profile_mask(agent, profile,
			OMCI_CLASS_HUAWEI_MULTICAST_370, 0x0101,
			GENMASK(15, 11), data, sizeof(data));
	if (ret)
		return ret;
	ret = omci_mib_add_default_profile_mask(agent, profile,
			OMCI_CLASS_HUAWEI_MULTICAST_373, 0, GENMASK(15, 11),
			data, sizeof(data));
	if (ret)
		return ret;
	ret = omci_mib_add_default_profile_mask(agent, profile,
			OMCI_CLASS_HUAWEI_MULTICAST_65408, 0, GENMASK(15, 11),
			data, sizeof(data));
	if (ret)
		return ret;
	ret = omci_mib_add_default_profile_mask(agent, profile,
			OMCI_CLASS_HUAWEI_MULTICAST_65414, 0, GENMASK(15, 11),
			data, sizeof(data));
	if (ret)
		return ret;

	return omci_mib_add_default_profile_mask(agent, profile,
			OMCI_CLASS_HUAWEI_MULTICAST_65425, 0, GENMASK(15, 11),
			data, sizeof(data));
}

static int
omci_agent_seed_nokia_locked(struct omci_device *odev, u8 profile)
{
	struct omci_agent *agent = &odev->agent;
	u8 data[OMCI_MAX_ATTR_DATA] = {};
	unsigned int i;
	int ret;

	data[0] = 1;
	data[3] = 1;
	data[6] = 1;
	data[9] = 1;
	data[12] = 1;
	ret = omci_mib_add_default_profile_mask(agent, profile,
			OMCI_CLASS_NOKIA_OPTICAL_SUPERVISION,
			OMCI_ANI_G_ENTITY_ID,
			GENMASK(15, 6), data, sizeof(data));
	if (ret)
		return ret;

	memset(data, 0, sizeof(data));
	memcpy(data + 15, agent->config.vendor_id,
	       sizeof(agent->config.vendor_id));
	memcpy(data + 19, agent->config.serial_number,
	       sizeof(agent->config.serial_number));
	ret = omci_mib_add_default_profile_mask(agent, profile,
			OMCI_CLASS_NOKIA_ONT_GENERIC_V2, 0,
			GENMASK(15, 4), data, sizeof(data));
	if (ret)
		return ret;

	for (i = 0; i < agent->config.uni_count; i++) {
		memset(data, 0, sizeof(data));
		ret = omci_mib_add_default_profile_mask(agent, profile,
				OMCI_CLASS_NOKIA_UNI_SUPPLEMENTAL_V2,
				OMCI_UNI_ENTITY_ID(OMCI_ETHERNET_SLOT,
						   i + 1),
				GENMASK(15, 9), data, sizeof(data));
		if (ret)
			return ret;
	}

	/*
	 * In Nokia GPON HGU / VEIP mode, the OLT assigns Alloc-ID 256 and 257
	 * along with GEM ports 256 and 257 via PLOAM without OMCI Create messages.
	 * Seed the standard VEIP datapath bridge for both GEM 256 and GEM 257.
	 */
	if (agent->config.onu_type == 2) {
		/* T-CONT 0x8000 -> Alloc-ID 256 */
		memset(data, 0, sizeof(data));
		put_unaligned_be16(256, data);
		ret = omci_mib_add_default_profile_mask(agent, profile,
				OMCI_CLASS_TCONT, 0x8000,
				BIT(15), data, sizeof(data));
		if (ret)
			return ret;

		/* GEM Port Network CTP 256 -> port 256, T-CONT 0x8000, bidir(3) */
		memset(data, 0, sizeof(data));
		put_unaligned_be16(256, data);
		put_unaligned_be16(0x8000, data + 2);
		data[4] = 3;
		put_unaligned_be16(0x8000, data + 5);
		ret = omci_mib_add_default_profile_mask(agent, profile,
				OMCI_CLASS_GEM_PORT_CTP, 256,
				GENMASK(15, 6), data, sizeof(data));
		if (ret)
			return ret;

		/*
		 * GEM IWTP 256 -> GEM CTP 256. Interworking option 5 is
		 * "802.1p mapper", so the service profile pointer at offset 3
		 * has to name a real class 130 instance; leaving it zero
		 * advertises mapper interworking with no mapper behind it.
		 */
		memset(data, 0, sizeof(data));
		put_unaligned_be16(256, data);
		data[2] = 5;
		put_unaligned_be16(OMCI_NOKIA_MAPPER_MGMT, data + 3);
		put_unaligned_be16(0x0a01, data + 5);
		ret = omci_mib_add_default_profile_mask(agent, profile,
				OMCI_CLASS_GEM_IWTP, 256,
				GENMASK(15, 8), data, sizeof(data));
		if (ret)
			return ret;

		/* T-CONT 0x8001 -> Alloc-ID 257 */
		memset(data, 0, sizeof(data));
		put_unaligned_be16(257, data);
		ret = omci_mib_add_default_profile_mask(agent, profile,
				OMCI_CLASS_TCONT, 0x8001,
				BIT(15), data, sizeof(data));
		if (ret)
			return ret;

		/* GEM Port Network CTP 257 -> port 257, T-CONT 0x8001, bidir(3) */
		memset(data, 0, sizeof(data));
		put_unaligned_be16(257, data);
		put_unaligned_be16(0x8001, data + 2);
		data[4] = 3;
		put_unaligned_be16(0x8001, data + 5);
		ret = omci_mib_add_default_profile_mask(agent, profile,
				OMCI_CLASS_GEM_PORT_CTP, 257,
				GENMASK(15, 6), data, sizeof(data));
		if (ret)
			return ret;

		/* GEM IWTP 257 -> GEM CTP 257, via 802.1p mapper 2. */
		memset(data, 0, sizeof(data));
		put_unaligned_be16(257, data);
		data[2] = 5;
		put_unaligned_be16(OMCI_NOKIA_MAPPER_HSI, data + 3);
		put_unaligned_be16(0x0a01, data + 5);
		ret = omci_mib_add_default_profile_mask(agent, profile,
				OMCI_CLASS_GEM_IWTP, 257,
				GENMASK(15, 8), data, sizeof(data));
		if (ret)
			return ret;

		/*
		 * 802.1p mappers, one per GEM interworking TP. A reference ONU
		 * carrying service on this OLT points every P-bit at the same
		 * interworking TP, so do the same rather than invent a
		 * priority split the OLT never asked for.
		 *
		 * Layout (G.988 9.3.10): TP pointer at 0, then the eight P-bit
		 * interworking TP pointers, unmarked frame option at 18, DSCP
		 * map at 19, default P-bit marking at 43, TP type at 44.
		 */
		for (i = 0; i < 2; i++) {
			u16 mapper = i ? OMCI_NOKIA_MAPPER_HSI :
					 OMCI_NOKIA_MAPPER_MGMT;
			u16 iwtp = i ? 257 : 256;
			unsigned int pbit;

			memset(data, 0, sizeof(data));
			put_unaligned_be16(0xffff, data);
			for (pbit = 0; pbit < 8; pbit++)
				put_unaligned_be16(iwtp, data + 2 + pbit * 2);
			ret = omci_mib_add_default_profile_mask(agent, profile,
					OMCI_CLASS_8021P_MAPPER, mapper,
					GENMASK(15, 3), data, sizeof(data));
			if (ret)
				return ret;
		}

		/* MAC Bridge Service Profile 1 */
		memset(data, 0, sizeof(data));
		data[0] = 1;
		data[1] = 1;
		data[2] = 1;
		ret = omci_mib_add_default_profile_mask(agent, profile,
				OMCI_CLASS_MAC_BRIDGE_SERVICE_PROFILE, 1,
				GENMASK(15, 6), data, sizeof(data));
		if (ret)
			return ret;

		/* MAC Bridge Port Config 1 -> VEIP (TP type 11, slot 6) */
		memset(data, 0, sizeof(data));
		put_unaligned_be16(1, data);
		data[2] = 1;
		data[3] = 11;
		put_unaligned_be16(OMCI_UNI_ENTITY_ID(6, 1), data + 4);
		ret = omci_mib_add_default_profile_mask(agent, profile,
				OMCI_CLASS_MAC_BRIDGE_PORT_CONFIG_DATA, 1,
				GENMASK(15, 12), data, sizeof(data));
		if (ret)
			return ret;

		/*
		 * MAC Bridge Port 2 -> 802.1p mapper 2 (Internet / HSI).
		 * TP type 3 is "802.1p mapper"; binding the port straight to
		 * the GEM interworking TP with type 5 skips the mapper layer a
		 * working reference ONU on this OLT uses.
		 */
		memset(data, 0, sizeof(data));
		put_unaligned_be16(1, data);
		data[2] = 2;
		data[3] = 3;
		put_unaligned_be16(OMCI_NOKIA_MAPPER_HSI, data + 4);
		ret = omci_mib_add_default_profile_mask(agent, profile,
				OMCI_CLASS_MAC_BRIDGE_PORT_CONFIG_DATA, 2,
				GENMASK(15, 12), data, sizeof(data));
		if (ret)
			return ret;

		/* MAC Bridge Port 3 -> 802.1p mapper 1 (management / VoIP). */
		memset(data, 0, sizeof(data));
		put_unaligned_be16(1, data);
		data[2] = 3;
		data[3] = 3;
		put_unaligned_be16(OMCI_NOKIA_MAPPER_MGMT, data + 4);
		ret = omci_mib_add_default_profile_mask(agent, profile,
				OMCI_CLASS_MAC_BRIDGE_PORT_CONFIG_DATA, 3,
				GENMASK(15, 12), data, sizeof(data));
		if (ret)
			return ret;

		/*
		 * Class 84 (VLAN tagging filter data), one per mapper: VID 100
		 * for high speed internet and VID 660 for management and VoIP.
		 * data[24] is the forward operation, data[25] the number of
		 * valid entries in the filter list.
		 */
		memset(data, 0, sizeof(data));
		put_unaligned_be16(100, data);
		data[24] = 0x10;
		data[25] = 1;
		ret = omci_mib_add_default_profile_mask(agent, profile,
				OMCI_CLASS_VLAN_TAGGING_FILTER_DATA,
				OMCI_NOKIA_MAPPER_HSI,
				GENMASK(15, 13), data, sizeof(data));
		if (ret)
			return ret;

		memset(data, 0, sizeof(data));
		put_unaligned_be16(660, data);
		data[24] = 0x10;
		data[25] = 1;
		ret = omci_mib_add_default_profile_mask(agent, profile,
				OMCI_CLASS_VLAN_TAGGING_FILTER_DATA,
				OMCI_NOKIA_MAPPER_MGMT,
				GENMASK(15, 13), data, sizeof(data));
		if (ret)
			return ret;

		/*
		 * Class 171 (extended VLAN tagging operation configuration
		 * data) on both VEIP instances, association type 10 (VEIP).
		 * G.988 9.3.13 layout: association type at 0, table size at 1,
		 * input and output TPID at 3 and 5, downstream mode at 7 and
		 * the associated ME pointer at 24.
		 */
		memset(data, 0, sizeof(data));
		data[0] = 10;
		put_unaligned_be16(OMCI_EXT_VLAN_MAX_RULES, data + 1);
		put_unaligned_be16(ETH_P_8021Q, data + 3);
		put_unaligned_be16(ETH_P_8021Q, data + 5);
		data[7] = 0;
		put_unaligned_be16(OMCI_UNI_ENTITY_ID(6, 1), data + 24);
		ret = omci_mib_add_default_profile_mask(agent, profile,
				OMCI_CLASS_EXTENDED_VLAN,
				OMCI_UNI_ENTITY_ID(6, 1),
				GENMASK(15, 10), data, sizeof(data));
		if (ret)
			return ret;

		put_unaligned_be16(OMCI_UNI_ENTITY_ID(OMCI_VEIP_SLOT, 1),
				   data + 24);
		ret = omci_mib_add_default_profile_mask(agent, profile,
				OMCI_CLASS_EXTENDED_VLAN,
				OMCI_UNI_ENTITY_ID(OMCI_VEIP_SLOT, 1),
				GENMASK(15, 10), data, sizeof(data));
		if (ret)
			return ret;

		/* Class 131 (OLT-G): pre-seed the Nokia/ALCL OLT identity. */
		memset(data, 0, sizeof(data));
		memcpy(data, "ALCL", 4);
		memcpy(data + 4, "ISAM                ", 20);
		ret = omci_mib_add_default_profile_mask(agent, profile,
				OMCI_CLASS_OLT_G, 0,
				GENMASK(15, 14), data, sizeof(data));
		if (ret)
			return ret;
	}

	return 0;
}

static int
omci_agent_seed_profile_locked(struct omci_device *odev, u8 profile)
{
	switch (profile) {
	case OMCI_OLT_PROFILE_HUAWEI:
		return omci_agent_seed_huawei_locked(odev, profile);
	case OMCI_OLT_PROFILE_NOKIA_ALCL:
		return omci_agent_seed_nokia_locked(odev, profile);
	default:
		return 0;
	}
}

static int
omci_agent_profile_reconcile_locked(struct omci_device *odev,
				    u8 old_profile, u8 new_profile)
{
	struct omci_agent *agent = &odev->agent;
	struct omci_mib_object *object;
	unsigned long index;
	int ret;

	if (old_profile == new_profile)
		return 0;

	xa_for_each(&agent->mib, index, object) {
		if (!object->owner_profile || object->owner_profile == new_profile)
			continue;
		xa_erase(&agent->mib, index);
		kfree(object);
	}

	ret = omci_agent_seed_profile_locked(odev, new_profile);
	if (ret)
		return ret;
	agent->profile_effective = new_profile;
	ret = omci_agent_reconcile_services_locked(odev);
	agent->profile_effective = old_profile;
	if (ret && ret != -EOPNOTSUPP)
		return ret;

	return 0;
}

static int
omci_mib_add_priority_queue_default(struct omci_agent *agent,
				    u16 entity_id, const u8 *data)
{
	return omci_mib_add_default_mask(agent, OMCI_CLASS_PRIORITY_QUEUE,
					 entity_id,
					 OMCI_PRIORITY_QUEUE_ATTR_MASK, data,
					 OMCI_MAX_ATTR_DATA);
}

static int
omci_mib_add_traffic_scheduler_default(struct omci_agent *agent,
				       u16 entity_id, const u8 *data)
{
	return omci_mib_add_default_mask(agent, OMCI_CLASS_TRAFFIC_SCHEDULER,
					 entity_id,
					 OMCI_TRAFFIC_SCHEDULER_ATTR_MASK,
					 data, OMCI_MAX_ATTR_DATA);
}

static u16 omci_mib_count_class(struct omci_agent *agent, u16 class_id)
{
	struct omci_mib_object *object;
	unsigned long index;
	u16 count = 0;

	xa_for_each(&agent->mib, index, object) {
		if (object->class_id == class_id && count < U16_MAX)
			count++;
	}

	return count;
}

static void omci_agent_refresh_identity_locked(struct omci_agent *agent)
{
	struct omci_mib_object *object;
	unsigned long index;
	u16 queues, schedulers;

	xa_for_each(&agent->mib, index, object) {
		if (object->class_id == OMCI_CLASS_SOFTWARE_IMAGE &&
		    object->entity_id < ARRAY_SIZE(agent->config.software_version)) {
			memcpy(object->data,
			       agent->config.software_version[object->entity_id],
			       OMCI_SOFTWARE_VERSION_LEN);
		} else if (object->class_id == OMCI_CLASS_CARDHOLDER) {
			memcpy(object->data + 3, agent->config.equipment_id,
			       sizeof(agent->config.equipment_id));
			memcpy(object->data + 23, agent->config.equipment_id,
			       sizeof(agent->config.equipment_id));
		} else if (object->class_id == OMCI_CLASS_CIRCUIT_PACK) {
			memcpy(object->data + 2, agent->config.serial_number,
			       sizeof(agent->config.serial_number));
			memcpy(object->data + 10, agent->config.version,
			       sizeof(agent->config.version));
			memcpy(object->data + 24, agent->config.vendor_id,
			       sizeof(agent->config.vendor_id));
			memcpy(object->data + 31, agent->config.equipment_id,
			       sizeof(agent->config.equipment_id));
		}
	}

	object = omci_mib_lookup(agent, OMCI_CLASS_ONU_G, 0);
	if (object) {
		memset(object->data, 0, sizeof(object->data));
		memcpy(object->data, agent->config.vendor_id,
		       sizeof(agent->config.vendor_id));
		memcpy(object->data + 4, agent->config.version,
		       sizeof(agent->config.version));
		memcpy(object->data + 18, agent->config.serial_number,
		       sizeof(agent->config.serial_number));
		object->data[26] = agent->config.traffic_mgmt_option;
	}

	object = omci_mib_lookup(agent, OMCI_CLASS_ONU2_G, 0);
	if (object) {
		memset(object->data, 0, sizeof(object->data));
		memcpy(object->data, agent->config.equipment_id,
		       min(sizeof(agent->config.equipment_id),
			   sizeof(object->data)));
		object->data[20] = agent->config.omcc_version;
		queues = omci_mib_count_class(agent, OMCI_CLASS_PRIORITY_QUEUE);
		put_unaligned_be16(queues, object->data + 25);
		schedulers = omci_mib_count_class(agent,
						  OMCI_CLASS_TRAFFIC_SCHEDULER);
		object->data[27] = min_t(u16, schedulers, U8_MAX);
		put_unaligned_be16(32, object->data + 29);
		/*
		 * G.988 9.1.3. The OLT reads these to decide what it may
		 * provision. Left at zero they say the ONU supports no
		 * security and no connectivity model at all, and an OLT that
		 * honours that identifies the ONU and then has nothing it can
		 * legally create.
		 */
		object->data[23] = OMCI_ONU2G_SECURITY_AES128;
		object->data[24] = OMCI_ONU2G_SECURITY_AES128;
		put_unaligned_be16(OMCI_ONU2G_CONNECTIVITY_CAPABILITY,
				   object->data + 35);
		put_unaligned_be16(1, object->data + 40);
	}

	object = omci_mib_lookup(agent, OMCI_CLASS_HUAWEI_SW_IMAGE_EXT, 0);
	if (object)
		memcpy(object->data, agent->config.software_version[0],
		       min_t(size_t, 14, OMCI_SOFTWARE_VERSION_LEN));

	object = omci_mib_lookup(agent, OMCI_CLASS_NOKIA_ONT_GENERIC_V2, 0);
	if (object) {
		memcpy(object->data + 15, agent->config.vendor_id,
		       sizeof(agent->config.vendor_id));
		memcpy(object->data + 19, agent->config.serial_number,
		       sizeof(agent->config.serial_number));
	}
}

static void omci_agent_apply_identity(struct omci_agent *agent,
				      const struct omci_identity *identity)
{
	if (identity->valid & OMCI_IDENTITY_F_SERIAL_NUMBER) {
		memcpy(agent->config.serial_number, identity->serial_number,
		       sizeof(agent->config.serial_number));
		agent->config.serial_source = identity->serial_source;
	}
	if (identity->valid & OMCI_IDENTITY_F_VENDOR_ID) {
		memcpy(agent->config.vendor_id, identity->vendor_id,
		       sizeof(agent->config.vendor_id));
		agent->config.vendor_source = identity->vendor_source;
	}
	if (identity->valid & OMCI_IDENTITY_F_PASSWORD) {
		memcpy(agent->config.password, identity->password,
		       sizeof(agent->config.password));
		agent->config.password_source = identity->password_source;
	}
	if (identity->valid & OMCI_IDENTITY_F_VERSION) {
		memcpy(agent->config.version, identity->version,
		       sizeof(agent->config.version));
		memcpy(agent->config.software_version[0], identity->version,
		       sizeof(agent->config.software_version[0]));
		memcpy(agent->config.software_version[1], identity->version,
		       sizeof(agent->config.software_version[1]));
		agent->config.version_source = identity->version_source;
		agent->config.software_version_source[0] =
			identity->version_source;
		agent->config.software_version_source[1] =
			identity->version_source;
	}
	if (identity->valid & OMCI_IDENTITY_F_EQUIPMENT_ID) {
		memcpy(agent->config.equipment_id, identity->equipment_id,
		       sizeof(agent->config.equipment_id));
		agent->config.equipment_source = identity->equipment_source;
	}
}

static void
omci_agent_identity_state_locked(const struct omci_agent *agent,
				 struct omci_identity *identity)
{
	memset(identity, 0, sizeof(*identity));
	identity->valid = OMCI_IDENTITY_F_SERIAL_NUMBER |
			  OMCI_IDENTITY_F_VENDOR_ID |
			  OMCI_IDENTITY_F_PASSWORD |
			  OMCI_IDENTITY_F_VERSION |
			  OMCI_IDENTITY_F_EQUIPMENT_ID;
	memcpy(identity->serial_number, agent->config.serial_number,
	       sizeof(identity->serial_number));
	memcpy(identity->vendor_id, agent->config.vendor_id,
	       sizeof(identity->vendor_id));
	memcpy(identity->password, agent->config.password,
	       sizeof(identity->password));
	memcpy(identity->version, agent->config.version,
	       sizeof(identity->version));
	memcpy(identity->equipment_id, agent->config.equipment_id,
	       sizeof(identity->equipment_id));
	identity->serial_source = agent->config.serial_source;
	identity->vendor_source = agent->config.vendor_source;
	identity->password_source = agent->config.password_source;
	identity->version_source = agent->config.version_source;
	identity->equipment_source = agent->config.equipment_source;
}

static int omci_agent_get_ani_topology(struct omci_device *odev,
				       struct omci_ani_topology *topology)
{
	u32 last_tcont, last_scheduler, last_queue;
	int ret = 0;

	*topology = (struct omci_ani_topology) {
		.tcont_base = 0x8000,
		.scheduler_base = 0x8000,
		.queue_base = 0x8000,
		.maximum_queue_size = 0xffff,
		.allocated_queue_size = 4,
		.tcont_count = OMCI_DEFAULT_TCONT_COUNT,
		.queues_per_tcont = OMCI_DEFAULT_QUEUES_PER_TCONT,
		.queue_config_option = 1,
		.scheduler_policy = 1,
	};

	if (odev->ops && odev->ops->get_ani_topology) {
		ret = odev->ops->get_ani_topology(odev, topology);
		if (ret)
			return ret;
	}

	if (!topology->tcont_count ||
	    topology->tcont_count > OMCI_MAX_TCONT_RESOURCES ||
	    !topology->queues_per_tcont ||
	    topology->queues_per_tcont > OMCI_MAX_QUEUES_PER_TCONT)
		return -EINVAL;

	last_tcont = topology->tcont_base + topology->tcont_count - 1;
	last_scheduler = topology->scheduler_base + topology->tcont_count - 1;
	last_queue = topology->queue_base +
		     topology->tcont_count * topology->queues_per_tcont - 1;
	if (last_tcont > U16_MAX || last_scheduler > U16_MAX ||
	    last_queue > U16_MAX)
		return -ERANGE;

	return 0;
}

static void
omci_priority_queue_default_data(const struct omci_ani_topology *topology,
				 unsigned int tcont, unsigned int queue,
				 u8 data[OMCI_MAX_ATTR_DATA])
{
	u16 tcont_entity = topology->tcont_base + tcont;
	u16 scheduler_entity = topology->scheduler_base + tcont;
	u16 priority = topology->queues_per_tcont - queue - 1;

	memset(data, 0, OMCI_MAX_ATTR_DATA);
	data[0] = topology->queue_config_option;
	put_unaligned_be16(topology->maximum_queue_size, data + 1);
	put_unaligned_be16(topology->allocated_queue_size, data + 3);
	put_unaligned_be16(tcont_entity, data + 9);
	put_unaligned_be16(priority, data + 11);
	put_unaligned_be16(scheduler_entity, data + 13);
	data[15] = 1;
	put_unaligned_be16(0xffff, data + 22);
}

static void
omci_traffic_scheduler_default_data(const struct omci_ani_topology *topology,
				    unsigned int tcont,
				    u8 data[OMCI_MAX_ATTR_DATA])
{
	memset(data, 0, OMCI_MAX_ATTR_DATA);
	put_unaligned_be16(topology->tcont_base + tcont, data);
	data[4] = topology->scheduler_policy;
}

static void
omci_cardholder_default_data(const struct omci_agent *agent, u8 unit_type,
			     u8 port_count, u8 data[OMCI_MAX_ATTR_DATA])
{
	memset(data, 0, OMCI_MAX_ATTR_DATA);
	data[0] = unit_type;
	data[1] = unit_type;
	data[2] = port_count;
	memcpy(data + 3, agent->config.equipment_id,
	       sizeof(agent->config.equipment_id));
	memcpy(data + 23, agent->config.equipment_id,
	       sizeof(agent->config.equipment_id));
}

static void
omci_circuit_pack_default_data(const struct omci_agent *agent,
			       const struct omci_ani_topology *topology,
			       u8 unit_type, u8 port_count, bool ani,
			       u8 data[OMCI_MAX_ATTR_DATA])
{
	u16 queues;

	memset(data, 0, OMCI_MAX_ATTR_DATA);
	data[0] = unit_type;
	data[1] = port_count;
	memcpy(data + 2, agent->config.serial_number,
	       sizeof(agent->config.serial_number));
	memcpy(data + 10, agent->config.version,
	       sizeof(agent->config.version));
	memcpy(data + 24, agent->config.vendor_id,
	       sizeof(agent->config.vendor_id));
	/* Administrative and operational states are enabled. */
	data[28] = 0;
	data[29] = 0;
	/* The default service model is bridged rather than routed. */
	data[30] = 0;
	memcpy(data + 31, agent->config.equipment_id,
	       sizeof(agent->config.equipment_id));
	if (!ani)
		return;

	queues = topology->tcont_count * topology->queues_per_tcont;
	data[52] = min_t(u16, topology->tcont_count, U8_MAX);
	data[53] = min_t(u16, queues, U8_MAX);
	data[54] = min_t(u16, topology->tcont_count, U8_MAX);
}

static int
omci_agent_add_equipment_default(struct omci_agent *agent,
				 const struct omci_ani_topology *topology,
				 u8 slot, u8 unit_type, u8 port_count,
				 bool ani, u8 data[OMCI_MAX_ATTR_DATA])
{
	u16 entity_id = OMCI_EQUIPMENT_ENTITY_ID(slot);
	int ret;

	omci_cardholder_default_data(agent, unit_type, port_count, data);
	ret = omci_mib_add_default_mask(agent, OMCI_CLASS_CARDHOLDER,
					entity_id, GENMASK(15, 10), data,
					OMCI_MAX_ATTR_DATA);
	if (ret)
		return ret;

	omci_circuit_pack_default_data(agent, topology, unit_type, port_count,
				       ani, data);
	return omci_mib_add_default_mask(agent, OMCI_CLASS_CIRCUIT_PACK,
					 entity_id, GENMASK(15, 2), data,
					 OMCI_MAX_ATTR_DATA);
}

static void omci_uni_g_default_data(u8 data[OMCI_MAX_ATTR_DATA])
{
	memset(data, 0, OMCI_MAX_ATTR_DATA);
	/* Support both OMCI and non-OMCI management. */
	data[3] = 2;
}

static int omci_agent_populate_defaults(struct omci_device *odev)
{
	struct omci_agent *agent = &odev->agent;
	struct omci_ani_topology topology;
	u8 data[OMCI_MAX_ATTR_DATA] = {};
	unsigned int i, queue;
	int ret;

	ret = omci_agent_get_ani_topology(odev, &topology);
	if (ret)
		return ret;

	ret = omci_mib_add_default_mask(agent, OMCI_CLASS_ONU_DATA, 0,
					BIT(15), data, 1);
	if (ret)
		return ret;

	for (i = 0; i < ARRAY_SIZE(agent->config.software_version); i++) {
		memset(data, 0, sizeof(data));
		memcpy(data, agent->config.software_version[i],
		       OMCI_SOFTWARE_VERSION_LEN);
		data[14] = i == 0; /* Committed image. */
		data[15] = i == 0; /* Active image. */
		data[16] = 1;      /* Valid image. */
		ret = omci_mib_add_default_mask(agent, OMCI_CLASS_SOFTWARE_IMAGE,
						i, GENMASK(15, 12), data,
						sizeof(data));
		if (ret)
			return ret;
	}

	ret = omci_agent_add_equipment_default(agent, &topology,
					       OMCI_GPON_SLOT, OMCI_GPON_UNIT_TYPE,
					       1, true, data);
	if (ret)
		return ret;
	ret = omci_agent_add_equipment_default(agent, &topology,
					       OMCI_ETHERNET_SLOT,
					       OMCI_ETHERNET_UNIT_TYPE,
					       agent->config.uni_count, false, data);
	if (ret)
		return ret;
	ret = omci_agent_add_equipment_default(agent, &topology,
					       OMCI_VEIP_SLOT, OMCI_VEIP_UNIT_TYPE,
					       1, false, data);
	if (ret)
		return ret;

	ret = omci_mib_add_default(agent, OMCI_CLASS_ONU_G, 0, data,
				   sizeof(data));
	if (ret)
		return ret;
	ret = omci_mib_add_default(agent, OMCI_CLASS_ONU2_G, 0, data,
				   sizeof(data));
	if (ret)
		return ret;
	memset(data, 0, sizeof(data));
	ret = omci_mib_add_default(agent, OMCI_CLASS_ANI_G,
				   OMCI_ANI_G_ENTITY_ID, data,
				   sizeof(data));
	if (ret)
		return ret;

	for (i = 0; i < agent->config.uni_count; i++) {
		u16 entity_id = OMCI_UNI_ENTITY_ID(OMCI_ETHERNET_SLOT, i + 1);

		memset(data, 0, sizeof(data));
		data[4] = 0; /* Administrative state unlocked. */
		ret = omci_mib_add_default(agent, OMCI_CLASS_PPTP_ETHERNET_UNI,
					   entity_id, data, sizeof(data));
		if (ret)
			return ret;

		omci_uni_g_default_data(data);
		ret = omci_mib_add_default_mask(agent, OMCI_CLASS_UNI_G,
						entity_id, GENMASK(15, 12),
						data, sizeof(data));
		if (ret)
			return ret;
	}

	memset(data, 0, sizeof(data));
	data[0] = 0; /* Administrative state unlocked. */
	ret = omci_mib_add_default(agent, OMCI_CLASS_VEIP,
				   OMCI_UNI_ENTITY_ID(OMCI_VEIP_SLOT, 1), data,
				   sizeof(data));
	if (ret)
		return ret;
	omci_uni_g_default_data(data);
	ret = omci_mib_add_default_mask(agent, OMCI_CLASS_UNI_G,
					OMCI_UNI_ENTITY_ID(OMCI_VEIP_SLOT, 1),
					GENMASK(15, 12), data, sizeof(data));
	if (ret)
		return ret;

	/* Also seed Slot 6 VEIP (0x0601) for compatibility with Alcatel-Lucent / EcoNet OLT profiles */
	memset(data, 0, sizeof(data));
	data[0] = 0;
	ret = omci_mib_add_default(agent, OMCI_CLASS_VEIP,
				   OMCI_UNI_ENTITY_ID(6, 1), data,
				   sizeof(data));
	if (ret)
		return ret;
	omci_uni_g_default_data(data);
	ret = omci_mib_add_default_mask(agent, OMCI_CLASS_UNI_G,
					OMCI_UNI_ENTITY_ID(6, 1),
					GENMASK(15, 12), data, sizeof(data));
	if (ret)
		return ret;

	for (i = 0; i < topology.tcont_count; i++) {
		u16 scheduler_id = topology.scheduler_base + i;

		memset(data, 0, sizeof(data));
		put_unaligned_be16(0xffff, data);
		ret = omci_mib_add_default(agent, OMCI_CLASS_TCONT,
					   topology.tcont_base + i, data,
					   sizeof(data));
		if (ret)
			return ret;

		omci_traffic_scheduler_default_data(&topology, i, data);
		ret = omci_mib_add_traffic_scheduler_default(agent, scheduler_id, data);
		if (ret)
			return ret;

		for (queue = 0; queue < topology.queues_per_tcont; queue++) {
			u16 entity_id = topology.queue_base +
				i * topology.queues_per_tcont + queue;

			omci_priority_queue_default_data(&topology, i, queue, data);
			ret = omci_mib_add_priority_queue_default(agent, entity_id, data);
			if (ret)
				return ret;
		}
	}

	omci_agent_refresh_identity_locked(agent);
	return 0;
}

int omci_agent_init(struct omci_device *odev)
{
	struct omci_agent *agent = &odev->agent;
	struct omci_identity identity;
	int ret;

	mutex_init(&agent->lock);
	xa_init(&agent->mib);
	xa_init(&agent->services);
	agent->enabled = true;
	agent->permissive = false;
	agent->fake_omci = false;
	agent->dying_gasp = false;
	agent->config.dying_gasp_source = OMCI_CONFIG_SOURCE_DEFAULT;
	agent->config.uni_count = 4;
	agent->config.onu_type = OMCI_ONU_TYPE_HGU;
	agent->config.onu_type_source = OMCI_CONFIG_SOURCE_DEFAULT;
	agent->config.traffic_mgmt_option = 0;
	agent->config.olt_profile = OMCI_OLT_PROFILE_AUTO;
	agent->config.olt_profile_force = OMCI_OLT_PROFILE_UNSPEC;
	agent->config.olt_profile_source = OMCI_CONFIG_SOURCE_DEFAULT;
	agent->config.olt_profile_force_source = OMCI_CONFIG_SOURCE_DEFAULT;
	memcpy(agent->config.serial_number, "OPEN0001", 8);
	memcpy(agent->config.vendor_id, "OPEN", 4);
	strscpy(agent->config.version, "OpenWrt", sizeof(agent->config.version));
	strscpy(agent->config.software_version[0], "OpenWrt",
		sizeof(agent->config.software_version[0]));
	strscpy(agent->config.software_version[1], "OpenWrt",
		sizeof(agent->config.software_version[1]));
	agent->config.omcc_version = OMCI_OMCC_VERSION_G988_2010_BASELINE;
	strscpy(agent->config.equipment_id, "Airoha EN7523",
		sizeof(agent->config.equipment_id));
	agent->config.serial_source = OMCI_CONFIG_SOURCE_DEFAULT;
	agent->config.vendor_source = OMCI_CONFIG_SOURCE_DEFAULT;
	agent->config.password_source = OMCI_CONFIG_SOURCE_DEFAULT;
	agent->config.version_source = OMCI_CONFIG_SOURCE_DEFAULT;
	agent->config.software_version_source[0] = OMCI_CONFIG_SOURCE_DEFAULT;
	agent->config.software_version_source[1] = OMCI_CONFIG_SOURCE_DEFAULT;
	agent->config.omcc_version_source = OMCI_CONFIG_SOURCE_DEFAULT;
	agent->config.equipment_source = OMCI_CONFIG_SOURCE_DEFAULT;

	ret = omci_identity_load(odev->parent, &identity);
	if (ret)
		return ret;
	omci_agent_apply_identity(agent, &identity);

	mutex_lock(&agent->lock);
	ret = omci_agent_populate_defaults(odev);
	if (!ret)
		ret = omci_agent_profile_refresh_locked(odev, NULL, NULL);
	mutex_unlock(&agent->lock);
	return ret;
}

void omci_agent_cleanup(struct omci_device *odev)
{
	struct omci_agent *agent = &odev->agent;
	struct omci_mib_object *object;
	unsigned long index;

	mutex_lock(&agent->lock);
	omci_agent_reset_duplicate_locked(agent);
	omci_agent_reset_table_snapshot_locked(agent);
	omci_agent_clear_services_locked(odev);
	xa_for_each(&agent->mib, index, object) {
		xa_erase(&agent->mib, index);
		kfree(object);
	}
	mutex_unlock(&agent->lock);
	xa_destroy(&agent->services);
	xa_destroy(&agent->mib);
}

static bool omci_mib_object_active(const struct omci_agent *agent,
				   const struct omci_mib_object *object)
{
	if (object->pending_delete)
		return false;
	if (object->owner_profile &&
	    object->owner_profile != agent->profile_effective)
		return false;

	return omci_me_active(agent, object->class_id);
}

static void omci_agent_reset_table_snapshot_locked(struct omci_agent *agent)
{
	kfree(agent->table_snapshot);
	agent->table_snapshot = NULL;
	agent->table_snapshot_len = 0;
	agent->table_snapshot_jiffies = 0;
	agent->table_snapshot_class_id = 0;
	agent->table_snapshot_entity_id = 0;
	agent->table_snapshot_mask = 0;
}

static void
omci_set_table_snapshot_locked(struct omci_agent *agent, u16 class_id,
			       u16 entity_id, u16 mask, u8 *table,
			       size_t table_len)
{
	omci_agent_reset_table_snapshot_locked(agent);
	agent->table_snapshot = table;
	agent->table_snapshot_len = table_len;
	agent->table_snapshot_jiffies = jiffies;
	agent->table_snapshot_class_id = class_id;
	agent->table_snapshot_entity_id = entity_id;
	agent->table_snapshot_mask = mask;
}

static int omci_agent_build_me_type_table_locked(struct omci_device *odev,
						 u8 **table,
						 size_t *table_len)
{
	struct omci_agent *agent = &odev->agent;
	struct omci_mib_object *object;
	struct omci_me_class class;
	unsigned long *classes;
	unsigned long class_id;
	unsigned long index;
	u32 next = 0;
	size_t count;
	u8 *data;
	int ret;

	classes = bitmap_zalloc(U16_MAX + 1, GFP_KERNEL);
	if (!classes)
		return -ENOMEM;

	for (;;) {
		ret = omci_me_class_next(odev, next, &class, &next);
		if (ret)
			break;
		if (class.support >= OMCI_CLASS_SUPPORT_PARSED)
			set_bit(class.class_id, classes);
	}
	if (ret != -ENOENT)
		goto out_free_bitmap;

	xa_for_each(&agent->mib, index, object) {
		if (omci_mib_object_active(agent, object))
			set_bit(object->class_id, classes);
	}
	/* Stored bridge/GAL profiles are supported as partial shadow MEs. */
	set_bit(OMCI_CLASS_MAC_BRIDGE_SERVICE_PROFILE, classes);
	set_bit(OMCI_CLASS_MAC_BRIDGE_CONFIG_DATA, classes);
	set_bit(OMCI_CLASS_GAL_ETHERNET_PROFILE, classes);
	set_bit(OMCI_CLASS_OMCI, classes);

	count = bitmap_weight(classes, U16_MAX + 1);
	data = kmalloc_array(count, sizeof(u16), GFP_KERNEL);
	if (!data) {
		ret = -ENOMEM;
		goto out_free_bitmap;
	}

	count = 0;
	for_each_set_bit(class_id, classes, U16_MAX + 1) {
		put_unaligned_be16(class_id, data + count * sizeof(u16));
		count++;
	}
	*table = data;
	*table_len = count * sizeof(u16);
	ret = 0;

out_free_bitmap:
	bitmap_free(classes);
	return ret;
}

static int omci_agent_build_message_type_table(u8 **table, size_t *table_len)
{
	static const u8 message_types[] = {
		OMCI_MSG_TYPE_CREATE,
		OMCI_MSG_TYPE_DELETE,
		OMCI_MSG_TYPE_SET,
		OMCI_MSG_TYPE_GET,
		OMCI_MSG_TYPE_GET_ALL_ALARMS,
		OMCI_MSG_TYPE_GET_ALL_ALARMS_NEXT,
		OMCI_MSG_TYPE_MIB_UPLOAD,
		OMCI_MSG_TYPE_MIB_UPLOAD_NEXT,
		OMCI_MSG_TYPE_MIB_RESET,
		OMCI_MSG_TYPE_ALARM_NOTIFICATION,
		OMCI_MSG_TYPE_TEST,
		OMCI_MSG_TYPE_SYNC_TIME,
		OMCI_MSG_TYPE_REBOOT,
		OMCI_MSG_TYPE_GET_NEXT,
		OMCI_MSG_TYPE_TEST_RESULT,
		OMCI_MSG_TYPE_GET_CURRENT_DATA,
		OMCI_MSG_TYPE_SET_TABLE,
	};

	*table = kmemdup(message_types, sizeof(message_types), GFP_KERNEL);
	if (!*table)
		return -ENOMEM;
	*table_len = sizeof(message_types);
	return 0;
}

static u8 omci_agent_get_omci_table_locked(struct omci_device *odev,
					   u16 entity_id, u16 mask,
					   u8 *response_content,
					   size_t response_capacity,
					   size_t *response_len)
{
	struct omci_agent *agent = &odev->agent;
	u8 *table = NULL;
	size_t table_len = 0;
	int ret;

	if (entity_id || hweight16(mask) != 1 ||
	    !(mask & (OMCI_OMCI_ME_TYPE_TABLE_MASK |
		      OMCI_OMCI_MESSAGE_TYPE_TABLE_MASK)))
		return OMCI_RESULT_PARAMETER_ERROR;
	if (response_capacity < 7)
		return OMCI_RESULT_PARAMETER_ERROR;

	if (mask == OMCI_OMCI_ME_TYPE_TABLE_MASK)
		ret = omci_agent_build_me_type_table_locked(odev, &table,
							    &table_len);
	else
		ret = omci_agent_build_message_type_table(&table, &table_len);
	if (ret)
		return OMCI_RESULT_PROCESSING_ERROR;

	omci_set_table_snapshot_locked(agent, OMCI_CLASS_OMCI, entity_id,
				       mask, table, table_len);

	memset(response_content, 0, response_capacity);
	response_content[0] = OMCI_RESULT_SUCCESS;
	put_unaligned_be16(mask, response_content + 1);
	put_unaligned_be32(table_len, response_content + 3);
	*response_len = 7;
	return OMCI_RESULT_SUCCESS;
}

static bool
omci_agent_me_type_supported_locked(struct omci_device *odev, u16 class_id)
{
	u8 *table;
	size_t table_len;
	size_t offset;
	bool found = false;

	if (omci_agent_build_me_type_table_locked(odev, &table, &table_len))
		return false;
	for (offset = 0; offset + sizeof(u16) <= table_len;
	     offset += sizeof(u16)) {
		if (get_unaligned_be16(table + offset) == class_id) {
			found = true;
			break;
		}
	}
	kfree(table);

	return found;
}

static u8 omci_agent_me_support(const struct omci_me_desc *desc)
{
	if (!desc)
		return 3;

	switch (desc->support) {
	case OMCI_CLASS_SUPPORT_NATIVE:
	case OMCI_CLASS_SUPPORT_PROVISIONED:
		return 1;
	case OMCI_CLASS_SUPPORT_PARSED:
	case OMCI_CLASS_SUPPORT_SHADOW:
		return 3;
	default:
		return 2;
	}
}

static u8 omci_agent_me_access(const struct omci_me_desc *desc)
{
	bool onu_created = desc && (desc->flags & OMCI_ME_F_ONU_CREATED);
	bool olt_created = desc && (desc->actions & OMCI_ME_ACTION_CREATE);

	if (onu_created && olt_created)
		return 3;
	if (olt_created)
		return 2;
	if (onu_created)
		return 1;

	return 3;
}

static u32 omci_agent_me_actions(const struct omci_me_desc *desc,
				 u16 class_id)
{
	if (desc)
		return desc->actions;

	switch (class_id) {
	case OMCI_CLASS_MAC_BRIDGE_SERVICE_PROFILE:
	case OMCI_CLASS_MAC_BRIDGE_CONFIG_DATA:
	case OMCI_CLASS_GAL_ETHERNET_PROFILE:
		return OMCI_ME_ACTION_CREATE | OMCI_ME_ACTION_DELETE |
		       OMCI_ME_ACTION_GET | OMCI_ME_ACTION_SET;
	default:
		return OMCI_ME_ACTION_GET | OMCI_ME_ACTION_SET;
	}
}

static int
omci_build_attr_ids_locked(struct omci_device *odev, u16 class_id,
			   u8 **table, size_t *table_len)
{
	const struct omci_me_desc *desc = omci_me_lookup(&odev->agent, class_id);
	u8 *data;
	unsigned int i;
	int ret;

	if (!desc || !desc->num_attrs) {
		data = kmalloc(1, GFP_KERNEL);
		if (!data)
			return -ENOMEM;
		*table = data;
		*table_len = 0;
		return 0;
	}

	data = kmalloc_array(desc->num_attrs, sizeof(u16), GFP_KERNEL);
	if (!data)
		return -ENOMEM;
	for (i = 0; i < desc->num_attrs; i++) {
		u16 attr_id;

		ret = omci_me_attr_id(&odev->agent, class_id, i, &attr_id);
		if (ret) {
			kfree(data);
			return ret;
		}
		put_unaligned_be16(attr_id, data + i * sizeof(u16));
	}
	*table = data;
	*table_len = desc->num_attrs * sizeof(u16);
	return 0;
}

static int
omci_build_instances_locked(struct omci_device *odev, u16 class_id,
			    u8 **table, size_t *table_len)
{
	struct omci_agent *agent = &odev->agent;
	struct omci_mib_object *object;
	unsigned long index;
	unsigned int count = 0;
	u8 *data;

	if (class_id == OMCI_CLASS_MANAGED_ENTITY) {
		return omci_agent_build_me_type_table_locked(odev, table,
						     table_len);
	}
	if (class_id == OMCI_CLASS_ATTRIBUTE) {
		unsigned int attr_count = omci_me_attr_count(agent);
		unsigned int i;

		data = kmalloc_array(max(attr_count, 1U), sizeof(u16),
				     GFP_KERNEL);
		if (!data)
			return -ENOMEM;
		for (i = 0; i < attr_count; i++)
			put_unaligned_be16(i + 1, data + i * sizeof(u16));
		*table = data;
		*table_len = attr_count * sizeof(u16);
		return 0;
	}
	if (class_id == OMCI_CLASS_OMCI) {
		data = kmalloc(sizeof(u16), GFP_KERNEL);
		if (!data)
			return -ENOMEM;
		put_unaligned_be16(0, data);
		*table = data;
		*table_len = sizeof(u16);
		return 0;
	}

	xa_for_each(&agent->mib, index, object)
		if (object->class_id == class_id &&
		    omci_mib_object_active(agent, object))
			count++;
	data = kmalloc_array(max(count, 1U), sizeof(u16), GFP_KERNEL);
	if (!data)
		return -ENOMEM;
	count = 0;
	xa_for_each(&agent->mib, index, object) {
		if (object->class_id != class_id ||
		    !omci_mib_object_active(agent, object))
			continue;
		put_unaligned_be16(object->entity_id,
				   data + count * sizeof(u16));
		count++;
	}
	*table = data;
	*table_len = count * sizeof(u16);
	return 0;
}

static int omci_agent_empty_table(u8 **table, size_t *table_len)
{
	*table = kmalloc(1, GFP_KERNEL);
	if (!*table)
		return -ENOMEM;
	*table_len = 0;
	return 0;
}

static u8 omci_agent_get_managed_entity_locked(struct omci_device *odev,
					       u16 entity_id, u16 mask,
					       u8 *response_content,
					       size_t response_capacity,
					       size_t *response_len)
{
	struct omci_agent *agent = &odev->agent;
	const struct omci_me_desc *desc;
	size_t used = 3;
	u16 encoded = 0;
	u8 *table = NULL;
	size_t table_len = 0;
	unsigned int bit;
	int ret;

	if (!mask || mask & ~GENMASK(15, 8))
		return OMCI_RESULT_PARAMETER_ERROR;
	if (!omci_agent_me_type_supported_locked(odev, entity_id))
		return OMCI_RESULT_UNKNOWN_INSTANCE;
	desc = omci_me_lookup(agent, entity_id);

	if (mask & OMCI_MANAGED_ENTITY_TABLE_MASKS) {
		if (hweight16(mask) != 1 || response_capacity < 7)
			return OMCI_RESULT_PARAMETER_ERROR;
		switch (mask) {
		case OMCI_MANAGED_ENTITY_ATTR_TABLE_MASK:
			ret = omci_build_attr_ids_locked(odev, entity_id, &table,
							 &table_len);
			break;
		case OMCI_MANAGED_ENTITY_INSTANCES_MASK:
			ret = omci_build_instances_locked(odev, entity_id,
							  &table, &table_len);
			break;
		default:
			ret = omci_agent_empty_table(&table, &table_len);
			break;
		}
		if (ret)
			return OMCI_RESULT_PROCESSING_ERROR;
		omci_set_table_snapshot_locked(agent, OMCI_CLASS_MANAGED_ENTITY,
					       entity_id, mask, table, table_len);
		memset(response_content, 0, response_capacity);
		response_content[0] = OMCI_RESULT_SUCCESS;
		put_unaligned_be16(mask, response_content + 1);
		put_unaligned_be32(table_len, response_content + 3);
		*response_len = 7;
		return OMCI_RESULT_SUCCESS;
	}

	memset(response_content, 0, response_capacity);
	response_content[0] = OMCI_RESULT_SUCCESS;
	for (bit = 0; bit < 16; bit++) {
		u16 attr_mask = BIT(15 - bit);

		if (!(mask & attr_mask))
			continue;
		switch (attr_mask) {
		case OMCI_MANAGED_ENTITY_NAME_MASK: {
			const char *name = omci_me_class_name(entity_id);
			size_t len = strnlen(name, OMCI_CAPABILITY_NAME_LEN);

			if (used + OMCI_CAPABILITY_NAME_LEN > response_capacity)
				return OMCI_RESULT_PARAMETER_ERROR;
			memcpy(response_content + used, name, len);
			used += OMCI_CAPABILITY_NAME_LEN;
			break;
		}
		case OMCI_MANAGED_ENTITY_ACCESS_MASK:
			if (used + 1 > response_capacity)
				return OMCI_RESULT_PARAMETER_ERROR;
			response_content[used++] = omci_agent_me_access(desc);
			break;
		case OMCI_MANAGED_ENTITY_ACTIONS_MASK:
			if (used + 4 > response_capacity)
				return OMCI_RESULT_PARAMETER_ERROR;
			put_unaligned_be32(omci_agent_me_actions(desc, entity_id),
					   response_content + used);
			used += 4;
			break;
		case OMCI_MANAGED_ENTITY_SUPPORT_MASK:
			if (used + 1 > response_capacity)
				return OMCI_RESULT_PARAMETER_ERROR;
			response_content[used++] = omci_agent_me_support(desc);
			break;
		default:
			return OMCI_RESULT_PARAMETER_ERROR;
		}
		encoded |= attr_mask;
	}
	put_unaligned_be16(encoded, response_content + 1);
	*response_len = used;
	return OMCI_RESULT_SUCCESS;
}

static bool omci_agent_attr_is_table(u16 class_id, u16 mask)
{
	if (class_id == OMCI_CLASS_OMCI)
		return mask & (OMCI_OMCI_ME_TYPE_TABLE_MASK |
			       OMCI_OMCI_MESSAGE_TYPE_TABLE_MASK);
	if (class_id == OMCI_CLASS_MANAGED_ENTITY)
		return mask & OMCI_MANAGED_ENTITY_TABLE_MASKS;
	if (class_id == OMCI_CLASS_ATTRIBUTE)
		return mask & OMCI_ATTRIBUTE_CODE_POINTS_MASK;
	if (class_id == OMCI_CLASS_EXTENDED_VLAN)
		return mask == BIT(8);

	return false;
}

static u8 omci_agent_attr_format(u16 class_id,
				 const struct omci_attr_desc *attr)
{
	if (omci_agent_attr_is_table(class_id, attr->mask))
		return OMCI_ATTRIBUTE_FORMAT_TABLE;
	if (attr->len > 4)
		return OMCI_ATTRIBUTE_FORMAT_STRING;
	if (attr->len == 4 && attr->mask == GENMASK(15, 0))
		return OMCI_ATTRIBUTE_FORMAT_BIT_FIELD;

	return OMCI_ATTRIBUTE_FORMAT_UNSIGNED;
}

static u8 omci_agent_get_attribute_locked(struct omci_device *odev,
					  u16 entity_id, u16 mask,
					  u8 *response_content,
					  size_t response_capacity,
					  size_t *response_len)
{
	struct omci_agent *agent = &odev->agent;
	const struct omci_me_desc *desc;
	const struct omci_attr_desc *attr;
	unsigned int attr_index;
	size_t used = 3;
	u16 class_id;
	u16 encoded = 0;
	unsigned int bit;
	u32 upper;
	int ret;

	if (!mask || mask & ~GENMASK(15, 7))
		return OMCI_RESULT_PARAMETER_ERROR;
	ret = omci_me_attr_get(agent, entity_id, &class_id, &attr_index,
			       &desc, &attr);
	if (ret)
		return OMCI_RESULT_UNKNOWN_INSTANCE;

	if (mask & OMCI_ATTRIBUTE_TABLE_MASKS) {
		u8 *table;
		size_t table_len;

		if (hweight16(mask) != 1 || response_capacity < 7)
			return OMCI_RESULT_PARAMETER_ERROR;
		ret = omci_agent_empty_table(&table, &table_len);
		if (ret)
			return OMCI_RESULT_PROCESSING_ERROR;
		omci_set_table_snapshot_locked(agent, OMCI_CLASS_ATTRIBUTE,
					       entity_id, mask, table, table_len);
		memset(response_content, 0, response_capacity);
		response_content[0] = OMCI_RESULT_SUCCESS;
		put_unaligned_be16(mask, response_content + 1);
		put_unaligned_be32(table_len, response_content + 3);
		*response_len = 7;
		return OMCI_RESULT_SUCCESS;
	}

	memset(response_content, 0, response_capacity);
	response_content[0] = OMCI_RESULT_SUCCESS;
	for (bit = 0; bit < 16; bit++) {
		u16 attr_mask = BIT(15 - bit);

		if (!(mask & attr_mask))
			continue;
		switch (attr_mask) {
		case OMCI_ATTRIBUTE_NAME_MASK: {
			char name[OMCI_CAPABILITY_NAME_LEN + 1];

			if (used + OMCI_CAPABILITY_NAME_LEN > response_capacity)
				return OMCI_RESULT_PARAMETER_ERROR;
			snprintf(name, sizeof(name), "%u attribute %u",
				 class_id, attr_index + 1);
			memcpy(response_content + used, name,
			       strnlen(name, OMCI_CAPABILITY_NAME_LEN));
			used += OMCI_CAPABILITY_NAME_LEN;
			break;
		}
		case OMCI_ATTRIBUTE_SIZE_MASK:
			if (used + 2 > response_capacity)
				return OMCI_RESULT_PARAMETER_ERROR;
			put_unaligned_be16(attr->len, response_content + used);
			used += 2;
			break;
		case OMCI_ATTRIBUTE_ACCESS_MASK:
			if (used + 1 > response_capacity)
				return OMCI_RESULT_PARAMETER_ERROR;
			response_content[used++] = attr->access & 0x7;
			break;
		case OMCI_ATTRIBUTE_FORMAT_MASK:
			if (used + 1 > response_capacity)
				return OMCI_RESULT_PARAMETER_ERROR;
			response_content[used++] =
				omci_agent_attr_format(class_id, attr);
			break;
		case OMCI_ATTRIBUTE_LOWER_LIMIT_MASK:
		case OMCI_ATTRIBUTE_BIT_FIELD_MASK:
			if (used + 4 > response_capacity)
				return OMCI_RESULT_PARAMETER_ERROR;
			put_unaligned_be32(0, response_content + used);
			used += 4;
			break;
		case OMCI_ATTRIBUTE_UPPER_LIMIT_MASK:
			if (used + 4 > response_capacity)
				return OMCI_RESULT_PARAMETER_ERROR;
			upper = attr->len >= sizeof(u32) ? U32_MAX :
				GENMASK(attr->len * 8 - 1, 0);
			put_unaligned_be32(upper, response_content + used);
			used += 4;
			break;
		case OMCI_ATTRIBUTE_SUPPORT_MASK:
			if (used + 1 > response_capacity)
				return OMCI_RESULT_PARAMETER_ERROR;
			response_content[used++] = 3;
			break;
		default:
			return OMCI_RESULT_PARAMETER_ERROR;
		}
		encoded |= attr_mask;
	}
	put_unaligned_be16(encoded, response_content + 1);
	*response_len = used;
	return OMCI_RESULT_SUCCESS;
}

static u8 omci_agent_get_next_locked(struct omci_device *odev,
				     u16 class_id, u16 entity_id,
				     u8 device_id, const u8 *content,
				     size_t content_len, u8 *response_content,
				     size_t response_capacity,
				     size_t *response_len)
{
	struct omci_agent *agent = &odev->agent;
	size_t fragment_len;
	size_t copy_len;
	size_t offset;
	u16 sequence;
	u16 mask;

	if (content_len < 4 || response_capacity < 3)
		return OMCI_RESULT_PARAMETER_ERROR;
	if (!omci_me_active(agent, class_id))
		return OMCI_RESULT_UNKNOWN_ME;
	if (!omci_me_action_allowed(agent, class_id, OMCI_MSG_TYPE_GET_NEXT))
		return OMCI_RESULT_NOT_SUPPORTED;
	mask = get_unaligned_be16(content);
	sequence = get_unaligned_be16(content + 2);
	if (hweight16(mask) != 1)
		return OMCI_RESULT_PARAMETER_ERROR;
	if (!agent->table_snapshot ||
	    time_after(jiffies, agent->table_snapshot_jiffies +
		       OMCI_TABLE_SNAPSHOT_TIMEOUT) ||
	    agent->table_snapshot_class_id != class_id ||
	    agent->table_snapshot_entity_id != entity_id ||
	    agent->table_snapshot_mask != mask)
		return OMCI_RESULT_PARAMETER_ERROR;

	fragment_len = device_id == OMCI_BASELINE_DEV_ID ?
		OMCI_BASELINE_TABLE_FRAGMENT_LEN : response_capacity - 3;
	if (sequence > SIZE_MAX / fragment_len)
		return OMCI_RESULT_PARAMETER_ERROR;
	offset = (size_t)sequence * fragment_len;
	if (offset >= agent->table_snapshot_len)
		return OMCI_RESULT_PARAMETER_ERROR;

	copy_len = min(fragment_len, agent->table_snapshot_len - offset);
	memset(response_content, 0, response_capacity);
	response_content[0] = OMCI_RESULT_SUCCESS;
	put_unaligned_be16(mask, response_content + 1);
	memcpy(response_content + 3, agent->table_snapshot + offset, copy_len);
	*response_len = 3 + copy_len;
	agent->table_snapshot_jiffies = jiffies;
	return OMCI_RESULT_SUCCESS;
}

static bool omci_mib_object_uploadable(const struct omci_agent *agent,
				       const struct omci_mib_object *object)
{
	const struct omci_me_desc *desc;

	if (!omci_mib_object_active(agent, object))
		return false;
	desc = omci_me_lookup(agent, object->class_id);

	return !desc || !(desc->flags & OMCI_ME_F_NO_MIB_UPLOAD);
}

static unsigned int omci_mib_count_locked(struct omci_agent *agent)
{
	struct omci_mib_object *object;
	unsigned long index;
	unsigned int count = 0;

	xa_for_each(&agent->mib, index, object)
		if (omci_mib_object_uploadable(agent, object))
			count++;
	return count;
}

static void omci_agent_reset_olt_objects_locked(struct omci_agent *agent)
{
	struct omci_mib_object *object;
	unsigned long index;

	xa_for_each(&agent->mib, index, object) {
		if (object->origin != OMCI_MIB_ORIGIN_OLT)
			continue;
		xa_erase(&agent->mib, index);
		kfree(object);
	}
}

static int omci_agent_mib_reset_locked(struct omci_device *odev, bool all,
				       bool *profile_changed)
{
	struct omci_agent *agent = &odev->agent;
	struct omci_mib_object *object;
	unsigned long index;
	int reset_ret;
	int ret;

	omci_agent_reset_table_snapshot_locked(agent);
	reset_ret = omci_agent_clear_services_locked(odev);

	if (all) {
		xa_for_each(&agent->mib, index, object) {
			xa_erase(&agent->mib, index);
			kfree(object);
		}
	} else {
		omci_agent_reset_olt_objects_locked(agent);
	}

	/*
	 * A MIB reset restores ONU-created MEs to their initial values. Rebuild
	 * them as well as the selected profile's vendor MEs so repeated resets
	 * expose the same MIB and capability table as a fresh agent.
	 */
	ret = omci_agent_populate_defaults(odev);
	if (!ret)
		ret = omci_agent_seed_profile_locked(odev,
						     agent->profile_effective);
	if (!ret)
		ret = omci_agent_profile_refresh_locked(odev, NULL, profile_changed);

	omci_agent_set_mib_sync_locked(agent, 0);
	agent->upload_index = 0;
	omci_agent_reset_duplicate_locked(agent);

	return ret ?: reset_ret;
}

static int omci_agent_hw_update(struct omci_device *odev,
				struct omci_mib_object *object,
				u8 action, const u8 *content)
{
	const struct omci_device_ops *ops = odev->ops;
	const struct omci_me_desc *desc;
	bool ignore_unsupported_uni;
	bool enable;
	u16 entity_id;
	u16 value;
	int ret;

	desc = omci_me_lookup(&odev->agent, object->class_id);
	if (action != OMCI_MSG_TYPE_DELETE && desc &&
	    (desc->flags & OMCI_ME_F_DATAPATH) &&
	    desc->support == OMCI_CLASS_SUPPORT_SHADOW)
		return -EOPNOTSUPP;

	switch (object->class_id) {
	case OMCI_CLASS_TCONT:
		if (!ops->set_tcont)
			return 0;
		if (action == OMCI_MSG_TYPE_DELETE)
			return ops->set_tcont(odev, object->entity_id, 0xffff,
					      false);
		value = get_unaligned_be16(object->data);
		return ops->set_tcont(odev, object->entity_id, value, true);
	case OMCI_CLASS_GEM_PORT_CTP:
		if (!ops->set_gem_port)
			return 0;
		return ops->set_gem_port(odev, object->entity_id,
					 get_unaligned_be16(object->data),
					 get_unaligned_be16(object->data + 2),
					 object->data[4],
					 action != OMCI_MSG_TYPE_DELETE, false);
	case OMCI_CLASS_PPTP_ETHERNET_UNI:
	case OMCI_CLASS_VEIP:
		if (!ops->set_uni)
			return 0;
		if (action == OMCI_MSG_TYPE_DELETE)
			enable = false;
		else if (object->class_id == OMCI_CLASS_PPTP_ETHERNET_UNI)
			enable = object->data[4] == 0;
		else
			enable = object->data[0] == 0;
		entity_id = omci_profile_normalize_uni_entity(
			odev->agent.profile_effective, object->entity_id);
		ret = ops->set_uni(odev, entity_id, enable);
		ignore_unsupported_uni = odev->agent.profile_quirks &
					 OMCI_OLT_QUIRK_IGNORE_UNSUPPORTED_UNI;
		if (ret == -EOPNOTSUPP && ignore_unsupported_uni)
			return 0;
		return ret;
	default:
		return 0;
	}
}

static bool
omci_agent_datapath_class(const struct omci_agent *agent, u16 class_id)
{
	const struct omci_me_desc *desc = omci_me_lookup(agent, class_id);

	return desc && (desc->flags & OMCI_ME_F_DATAPATH);
}

static void omci_agent_free_service_array(struct xarray *services)
{
	struct omci_service_state *state;
	unsigned long index;

	xa_for_each(services, index, state) {
		xa_erase(services, index);
		kfree(state);
	}
}

static int omci_agent_clear_services_locked(struct omci_device *odev)
{
	struct omci_agent *agent = &odev->agent;
	struct omci_service_state *state;
	unsigned long index;
	int first_error = 0;
	int ret;

	xa_for_each(&agent->services, index, state) {
		if (odev->ops->delete_service) {
			ret = odev->ops->delete_service(odev, state->config.cookie);
			if (ret && ret != -ENOENT && !first_error)
				first_error = ret;
		}
		xa_erase(&agent->services, index);
		kfree(state);
	}

	return first_error;
}

static int omci_agent_stage_service(struct xarray *services,
				    const struct omci_service_config *service)
{
	struct omci_service_state *state;
	void *old;

	state = kmalloc(sizeof(*state), GFP_KERNEL);
	if (!state)
		return -ENOMEM;
	state->config = *service;

	old = xa_store(services, service->cookie, state, GFP_KERNEL);
	if (xa_is_err(old)) {
		kfree(state);
		return xa_err(old);
	}
	kfree(old);
	return 0;
}

static int omci_agent_apply_services_locked(struct omci_device *odev,
					    struct xarray *desired)
{
	struct omci_agent *agent = &odev->agent;
	struct omci_service_state *state;
	struct omci_service_state *old_state;
	unsigned long index;
	int ret = 0;

	if (!xa_empty(desired) && !odev->ops->replace_service)
		return -EOPNOTSUPP;

	/* Reserve all new cookies before changing hardware state. */
	xa_for_each(desired, index, state) {
		if (xa_load(&agent->services, index))
			continue;
		ret = xa_reserve(&agent->services, index, GFP_KERNEL);
		if (ret)
			goto release_reservations;
	}

	/* Add or replace all desired rules while the previous set remains live. */
	xa_for_each(desired, index, state) {
		ret = odev->ops->replace_service(odev, &state->config);
		if (ret) {
			dev_warn(odev->parent,
				 "OMCI install failed for service %#x (UNI %#x GEM %u T-CONT %#x queue %u): %d; rolling back\n",
				 state->config.cookie,
				 state->config.uni_entity_id,
				 state->config.gem_port_id,
				 state->config.tcont_entity_id,
				 state->config.queue, ret);
			goto rollback_hardware;
		}
	}

	/* Delete rules that are no longer part of the resolved service graph. */
	xa_for_each(&agent->services, index, old_state) {
		if (xa_load(desired, index))
			continue;
		if (!odev->ops->delete_service)
			continue;
		ret = odev->ops->delete_service(odev, old_state->config.cookie);
		if (ret && ret != -ENOENT)
			goto rollback_hardware;
	}

	/* Hardware is consistent; replace the software snapshot. */
	omci_agent_free_service_array(&agent->services);
	xa_for_each(desired, index, state) {
		void *entry;

		xa_erase(desired, index);
		entry = xa_store(&agent->services, index, state, GFP_KERNEL);
		if (WARN_ON(xa_is_err(entry))) {
			kfree(state);
			continue;
		}
		kfree(entry);
	}
	return 0;

rollback_hardware:
	/* Restore every old rule, including rules deleted before a failure. */
	if (odev->ops->replace_service)
		xa_for_each(&agent->services, index, old_state)
			odev->ops->replace_service(odev, &old_state->config);

	/* Remove newly introduced cookies that were not in the old snapshot. */
	if (odev->ops->delete_service)
		xa_for_each(desired, index, state)
			if (!xa_load(&agent->services, index))
				odev->ops->delete_service(odev,
						  state->config.cookie);

release_reservations:
	xa_for_each(desired, index, state)
		if (!xa_load(&agent->services, index))
			xa_release(&agent->services, index);
	return ret;
}

static bool omci_agent_uni_enabled(struct omci_agent *agent, u8 tp_type,
				   u16 entity_id)
{
	struct omci_mib_object *object;

	if (tp_type == OMCI_BRIDGE_TP_PPTP_ETH_UNI) {
		object = omci_mib_lookup(agent, OMCI_CLASS_PPTP_ETHERNET_UNI,
					 entity_id);
		return object && object->data[4] == 0;
	}
	if (tp_type == OMCI_BRIDGE_TP_VEIP) {
		object = omci_mib_lookup(agent, OMCI_CLASS_VEIP, entity_id);
		return object && object->data[0] == 0;
	}

	return false;
}

static u32 omci_agent_service_cookie(u16 lan_port, u16 gem_ctp,
				     u16 gem_port, u16 selector)
{
	u32 cookie;

	cookie = jhash_3words(((u32)lan_port << 16) | gem_ctp,
			      ((u32)gem_port << 16) | selector,
			      0x4f4d4349, 0);

	return cookie ?: 1;
}

static bool omci_agent_service_vlan_valid(u16 vid)
{
	return vid > 0 && vid < VLAN_VID_MASK;
}

static int
omci_agent_stage_service_rule_locked(struct omci_device *odev,
				     struct xarray *services,
				     u16 lan_port_entity,
				     u16 uni_entity, u16 gem_iwtp_entity,
				     u8 mapper_pcp, bool mapper_pcp_valid,
				     const struct omci_vlan_filter_entry *filter,
				     const struct omci_extended_vlan_rule *ext_rule,
				     u16 selector, bool *default_installed,
				     bool multicast, u16 ani_entity_id,
				     bool ani_valid)
{
	struct omci_agent *agent = &odev->agent;
	struct omci_mib_object *iwtp;
	struct omci_mib_object *gem;
	struct omci_mib_object *tcont;
	struct omci_service_config service = {};
	u16 gem_ctp_entity;

	iwtp = omci_mib_lookup(agent, OMCI_CLASS_GEM_IWTP, gem_iwtp_entity);
	if (!iwtp)
		return -ENOENT;
	gem_ctp_entity = get_unaligned_be16(iwtp->data);
	gem = omci_mib_lookup(agent, OMCI_CLASS_GEM_PORT_CTP, gem_ctp_entity);
	if (!gem)
		return -ENOENT;
	if (gem->data[4] == OMCI_GEM_PORT_DIRECTION_ANI_TO_UNI)
		return 0;

	service.uni_entity_id = omci_profile_normalize_uni_entity(
		agent->profile_effective, uni_entity);
	service.gem_ctp_entity_id = gem_ctp_entity;
	service.gem_port_id = get_unaligned_be16(gem->data);
	service.tcont_entity_id = get_unaligned_be16(gem->data + 2);
	tcont = omci_mib_lookup(agent, OMCI_CLASS_TCONT,
				 service.tcont_entity_id);
	if (!tcont || get_unaligned_be16(tcont->data) == 0xffff)
		return -ENOENT;
	/* Carry the Alloc-ID: the backend may need it to rebuild the channel. */
	service.alloc_id = get_unaligned_be16(tcont->data);
	service.direction = gem->data[4];
	service.multicast = multicast;
	service.multicast_ani_entity_id = ani_entity_id;
	service.multicast_ani_valid = ani_valid;
	if (mapper_pcp_valid) {
		service.pcp = mapper_pcp;
		service.pcp_valid = true;
		service.queue = mapper_pcp;
	}
	if (filter) {
		service.vlan_id = filter->vid;
		service.vlan_valid =
			omci_agent_service_vlan_valid(filter->vid);
		service.pcp = filter->pbit;
		service.pcp_valid = true;
		service.queue = filter->pbit;
	}
	if (ext_rule) {
		struct omci_extended_vlan_rule rule = *ext_rule;
		bool network_vlan = false;

		omci_profile_normalize_vlan_rule(agent->profile_effective, &rule);
		memcpy(service.vlan_treatment, rule.raw + 8,
		       sizeof(service.vlan_treatment));

		/*
		 * pon0 exposes the ANI/network side of the service.  Class 171
		 * therefore selects the VLAN after the OMCI treatment, not the
		 * customer-side filter VLAN.  Userspace creates that network VLAN
		 * directly (for example pon0.1894), so the backend only needs the
		 * resulting classifier and does not need to rewrite the skb.
		 */
		if (omci_agent_service_vlan_valid(rule.treat_inner_vid)) {
			service.vlan_id = rule.treat_inner_vid;
			network_vlan = true;
		} else if (omci_agent_service_vlan_valid(rule.treat_outer_vid)) {
			service.vlan_id = rule.treat_outer_vid;
			network_vlan = true;
		} else if (omci_agent_service_vlan_valid(rule.filter_inner_vid)) {
			service.vlan_id = rule.filter_inner_vid;
			network_vlan = true;
		} else if (omci_agent_service_vlan_valid(rule.filter_outer_vid)) {
			service.vlan_id = rule.filter_outer_vid;
			network_vlan = true;
		}
		service.vlan_valid = network_vlan;

		if (rule.filter_inner_pbit <= 7) {
			service.pcp = rule.filter_inner_pbit;
			service.pcp_valid = true;
			service.queue = rule.filter_inner_pbit;
		} else if (rule.filter_outer_pbit <= 7) {
			service.pcp = rule.filter_outer_pbit;
			service.pcp_valid = true;
			service.queue = rule.filter_outer_pbit;
		}

		/*
		 * Preparatory Class 171 rules can describe only a customer-side
		 * rewrite and still carry no host-facing VLAN.  They are valid OMCI
		 * state, but they must not make the whole mapper Set fail.
		 */
		if (!network_vlan)
			return -EOPNOTSUPP;
	}
	if (!service.vlan_valid && !service.pcp_valid && !*default_installed) {
		service.default_service = true;
		*default_installed = true;
	}
	service.cookie = omci_agent_service_cookie(lan_port_entity,
			gem_ctp_entity, service.gem_port_id, selector);

	/*
	 * Trace every rule the resolver wants, not only the ones the backend
	 * accepts, so an install failure can be attributed to a specific rule.
	 */
	dev_dbg(odev->parent,
		"OMCI stage service %#x: UNI %#x (raw %#x) LAN port %#x GEM %u T-CONT %#x queue %u PCP %s%u VLAN %s%u selector %u%s%s\n",
		service.cookie, service.uni_entity_id, uni_entity,
		lan_port_entity, service.gem_port_id, service.tcont_entity_id,
		service.queue, service.pcp_valid ? "" : "any/", service.pcp,
		service.vlan_valid ? "" : "any/", service.vlan_id, selector,
		service.multicast ? " multicast" : "",
		service.default_service ? " default" : "");

	return omci_agent_stage_service(services, &service);
}

static bool
omci_agent_vlan_associated(const struct omci_mib_object *object,
			   u16 lan_port_entity, u16 uni_entity,
			   u16 bridge_port_entity, u16 service_entity)
{
	if (object->class_id == OMCI_CLASS_VLAN_TAGGING_FILTER_DATA)
		return object->entity_id == lan_port_entity ||
		       object->entity_id == uni_entity ||
		       object->entity_id == bridge_port_entity ||
		       object->entity_id == service_entity;
	if (object->class_id == OMCI_CLASS_EXTENDED_VLAN)
		return object->extended_vlan.valid &&
		       (object->extended_vlan.associated_me == lan_port_entity ||
			object->extended_vlan.associated_me == uni_entity ||
			object->extended_vlan.associated_me == bridge_port_entity ||
			object->extended_vlan.associated_me == service_entity ||
			object->entity_id == lan_port_entity ||
			object->entity_id == bridge_port_entity ||
			object->entity_id == service_entity);

	return false;
}

static int
omci_agent_stage_path_locked(struct omci_device *odev,
			     struct xarray *services,
			     const struct omci_mib_object *lan_port,
			     u16 uni_entity, u16 bridge_port_entity,
			     u16 service_entity, u16 gem_iwtp_entity,
			     u8 pcp, bool pcp_valid, bool multicast,
			     u16 ani_entity_id, bool ani_valid,
			     bool *default_installed)
{
	struct omci_agent *agent = &odev->agent;
	struct omci_mib_object *object;
	unsigned long index;
	u16 selector = pcp_valid ? 0x100 | pcp : 0;
	bool installed = false;
	int ret;

	xa_for_each(&agent->mib, index, object) {
		unsigned int i;

		if (!omci_mib_object_active(agent, object) ||
		    !omci_agent_vlan_associated(object, lan_port->entity_id,
						 uni_entity, bridge_port_entity,
						 service_entity))
			continue;
		if (object->class_id == OMCI_CLASS_VLAN_TAGGING_FILTER_DATA &&
		    object->vlan_filter.valid) {
			for (i = 0; i < object->vlan_filter.num_entries; i++) {
				ret = omci_agent_stage_service_rule_locked(
					odev, services, lan_port->entity_id,
					uni_entity, gem_iwtp_entity, pcp, pcp_valid,
					&object->vlan_filter.entries[i], NULL,
					selector + i + 1, default_installed,
					multicast, ani_entity_id, ani_valid);
				if (ret == -EOPNOTSUPP)
					continue;
				if (ret)
					return ret;
				installed = true;
			}
		} else if (object->class_id == OMCI_CLASS_EXTENDED_VLAN &&
			   object->extended_vlan.valid) {
			for (i = 0; i < object->extended_vlan.rule_count; i++) {
				ret = omci_agent_stage_service_rule_locked(
					odev, services, lan_port->entity_id,
					uni_entity, gem_iwtp_entity, pcp, pcp_valid,
					NULL, &object->extended_vlan.rules[i],
					selector + i + 0x20, default_installed,
					multicast, ani_entity_id, ani_valid);
				if (ret == -EOPNOTSUPP)
					continue;
				if (ret)
					return ret;
				installed = true;
			}
		}
	}

	if (installed)
		return 0;

	return omci_agent_stage_service_rule_locked(
		odev, services, lan_port->entity_id, uni_entity, gem_iwtp_entity,
		pcp, pcp_valid, NULL, NULL, selector, default_installed,
		multicast, ani_entity_id, ani_valid);
}

static int
omci_agent_resolve_bridge_locked(struct omci_device *odev,
				 struct xarray *services,
				 const struct omci_mib_object *lan_port,
				 bool *default_installed)
{
	struct omci_agent *agent = &odev->agent;
	struct omci_mib_object *wan_port;
	struct omci_mib_object *mapper;
	unsigned long index;
	u16 bridge_id = get_unaligned_be16(lan_port->data);
	u16 uni_entity = get_unaligned_be16(lan_port->data + 4);
	u8 lan_type = lan_port->data[3];
	int ret;

	if (!omci_agent_uni_enabled(agent, lan_type, uni_entity))
		return 0;

	xa_for_each(&agent->mib, index, wan_port) {
		u16 mapper_entity;
		u16 gem_iwtp_entity;
		u8 wan_type;
		unsigned int pcp;

		if (wan_port->class_id !=
		    OMCI_CLASS_MAC_BRIDGE_PORT_CONFIG_DATA ||
		    wan_port == lan_port ||
		    get_unaligned_be16(wan_port->data) != bridge_id)
			continue;
		wan_type = wan_port->data[3];
		if (wan_type == OMCI_BRIDGE_TP_GEM_IWTP ||
		    wan_type == OMCI_BRIDGE_TP_MULTICAST_GEM_IWTP) {
			bool multicast =
				wan_type == OMCI_BRIDGE_TP_MULTICAST_GEM_IWTP;
			bool ani_valid = false;
			u16 ani_entity_id = 0;

			gem_iwtp_entity = get_unaligned_be16(wan_port->data + 4);
			if (multicast &&
			    !omci_profile_resolve_multicast_ani(
				agent->profile_effective, wan_port->entity_id,
				&ani_entity_id))
				ani_valid = true;
			ret = omci_agent_stage_path_locked(
				odev, services, lan_port, uni_entity,
				wan_port->entity_id, gem_iwtp_entity,
				gem_iwtp_entity, 0, false, multicast,
				ani_entity_id, ani_valid, default_installed);
			if (ret && ret != -ENOENT)
				return ret;
			continue;
		}
		if (wan_type != OMCI_BRIDGE_TP_8021P_MAPPER)
			continue;
		mapper_entity = get_unaligned_be16(wan_port->data + 4);
		mapper = omci_mib_lookup(agent, OMCI_CLASS_8021P_MAPPER,
					 mapper_entity);
		if (!mapper)
			continue;
		for (pcp = 0; pcp < 8; pcp++) {
			gem_iwtp_entity = get_unaligned_be16(mapper->data + 2 +
							 pcp * 2);
			if (!gem_iwtp_entity || gem_iwtp_entity == 0xffff)
				continue;
			ret = omci_agent_stage_path_locked(
				odev, services, lan_port, uni_entity,
				wan_port->entity_id, mapper_entity,
				gem_iwtp_entity, pcp, true, false, 0, false,
				default_installed);
			if (ret && ret != -ENOENT)
				return ret;
		}
	}

	return 0;
}

static int omci_agent_reconcile_services_locked(struct omci_device *odev)
{
	struct omci_agent *agent = &odev->agent;
	struct omci_mib_object *object;
	struct xarray desired;
	unsigned long index;
	bool default_installed = false;
	int ret;

	xa_init(&desired);
	xa_for_each(&agent->mib, index, object) {
		u8 tp_type;

		if (!omci_mib_object_active(agent, object) ||
		    object->class_id != OMCI_CLASS_MAC_BRIDGE_PORT_CONFIG_DATA)
			continue;
		tp_type = object->data[3];
		if (tp_type != OMCI_BRIDGE_TP_PPTP_ETH_UNI &&
		    tp_type != OMCI_BRIDGE_TP_VEIP)
			continue;
		ret = omci_agent_resolve_bridge_locked(
			odev, &desired, object, &default_installed);
		if (ret)
			goto out;
	}

	ret = omci_agent_apply_services_locked(odev, &desired);

out:
	omci_agent_free_service_array(&desired);
	xa_destroy(&desired);
	return ret;
}

static struct omci_mib_object *
omci_get_or_create_locked(struct omci_agent *agent, u16 class_id,
			  u16 entity_id, bool create)
{
	struct omci_mib_object *object;
	struct omci_mib_object *stored;
	int ret;

	stored = omci_mib_lookup(agent, class_id, entity_id);
	if (stored || !create)
		return stored;

	object = kzalloc(sizeof(*object), GFP_KERNEL);
	if (!object)
		return NULL;
	object->class_id = class_id;
	object->entity_id = entity_id;
	object->origin = OMCI_MIB_ORIGIN_OLT;

	ret = omci_mib_store_locked(agent, object);
	kfree(object);
	if (ret)
		return NULL;

	return omci_mib_lookup(agent, class_id, entity_id);
}

static u8 omci_agent_create_locked(struct omci_device *odev,
				   u16 class_id, u16 entity_id,
				   const u8 *content, size_t content_len)
{
	struct omci_agent *agent = &odev->agent;
	const struct omci_me_desc *desc;
	struct omci_mib_object *stored = NULL;
	struct omci_mib_object *object;
	u8 result = OMCI_RESULT_SUCCESS;
	bool hardware_applied = false;
	int ret;

	if (!omci_me_active(agent, class_id))
		return OMCI_RESULT_UNKNOWN_ME;
	if (!omci_me_action_allowed(agent, class_id, OMCI_MSG_TYPE_CREATE))
		return OMCI_RESULT_NOT_SUPPORTED;
	if (omci_mib_lookup(agent, class_id, entity_id))
		return OMCI_RESULT_INSTANCE_EXISTS;

	desc = omci_me_lookup(agent, class_id);
	object = kzalloc(sizeof(*object), GFP_KERNEL);
	if (!object)
		return OMCI_RESULT_DEVICE_BUSY;
	object->class_id = class_id;
	object->entity_id = entity_id;
	object->origin = OMCI_MIB_ORIGIN_OLT;
	object->owner_profile = desc && (desc->flags & OMCI_ME_F_VENDOR) ?
		agent->profile_effective : OMCI_OLT_PROFILE_UNSPEC;
	object->data_len = desc ? desc->data_len :
		min_t(size_t, content_len, sizeof(object->data));

	if (desc) {
		ret = omci_me_decode_create(desc, object, content, content_len);
	} else {
		memcpy(object->data, content,
		       min_t(size_t, content_len, sizeof(object->data)));
		object->attr_mask = 0xffff;
		ret = 0;
	}
	if (ret) {
		result = OMCI_RESULT_PARAMETER_ERROR;
		goto out;
	}
	ret = omci_mib_parse_create(object, object->data);
	if (ret) {
		result = OMCI_RESULT_PARAMETER_ERROR;
		goto out;
	}
	ret = omci_agent_hw_update(odev, object, OMCI_MSG_TYPE_CREATE,
				   content);
	if (ret) {
		result = OMCI_RESULT_PROCESSING_ERROR;
		goto out;
	}
	hardware_applied = true;
	ret = omci_mib_store_locked(agent, object);
	if (ret) {
		result = OMCI_RESULT_DEVICE_BUSY;
		goto rollback;
	}
	stored = omci_mib_lookup(agent, class_id, entity_id);
	if (omci_agent_datapath_class(agent, class_id)) {
		ret = omci_agent_reconcile_services_locked(odev);
		if (ret) {
			result = OMCI_RESULT_PROCESSING_ERROR;
			goto rollback;
		}
	}
	omci_agent_increment_mib_sync_locked(agent);
	goto out;

rollback:
	if (stored) {
		xa_erase(&agent->mib, omci_mib_key(class_id, entity_id));
		kfree(stored);
	}
	if (hardware_applied)
		omci_agent_hw_update(odev, object, OMCI_MSG_TYPE_DELETE,
				     object->data);
out:
	kfree(object);
	return result;
}

static u8 omci_agent_set_locked(struct omci_device *odev, u16 class_id,
				u16 entity_id, u8 action, const u8 *content,
				size_t content_len, bool *profile_changed)
{
	struct omci_agent *agent = &odev->agent;
	const struct omci_me_desc *desc;
	struct omci_mib_object *previous = NULL;
	struct omci_mib_object *object;
	struct omci_mib_object *existing;
	bool hardware_applied = false;
	bool allow_create;
	bool existed;
	u16 mask;
	int ret;

	if (content_len < 2)
		return OMCI_RESULT_PARAMETER_ERROR;
	if (!omci_me_active(agent, class_id))
		return OMCI_RESULT_UNKNOWN_ME;
	if (!omci_me_action_allowed(agent, class_id, action))
		return OMCI_RESULT_NOT_SUPPORTED;

	mask = get_unaligned_be16(content);
	desc = omci_me_lookup(agent, class_id);
	allow_create = agent->permissive || agent->fake_omci ||
		omci_agent_profile_has_quirk(agent,
					     OMCI_OLT_QUIRK_ALLOW_SET_CREATE);
	existing = omci_mib_lookup(agent, class_id, entity_id);
	existed = !!existing;
	if (existing) {
		previous = kmemdup(existing, sizeof(*previous), GFP_KERNEL);
		if (!previous)
			return OMCI_RESULT_DEVICE_BUSY;
	}
	object = omci_get_or_create_locked(agent, class_id, entity_id,
					  allow_create);
	if (!object) {
		ret = allow_create ? OMCI_RESULT_DEVICE_BUSY :
				     OMCI_RESULT_UNKNOWN_INSTANCE;
		goto out_free_previous;
	}
	if (!object->data_len)
		object->data_len = desc ? desc->data_len : sizeof(object->data);
	if (!object->owner_profile && desc && (desc->flags & OMCI_ME_F_VENDOR))
		object->owner_profile = agent->profile_effective;

	if (desc) {
		ret = omci_me_decode_set(desc, object, mask, content + 2,
					 content_len - 2);
	} else if (class_id == OMCI_CLASS_PRIORITY_QUEUE ||
		   class_id == OMCI_CLASS_TRAFFIC_SCHEDULER) {
		ret = omci_mib_parse_set(object, mask, content + 2,
					 content_len - 2);
	} else {
		memcpy(object->data, content + 2,
		       min_t(size_t, content_len - 2, sizeof(object->data)));
		object->attr_mask |= mask;
		ret = 0;
	}
	if (!ret && desc && (class_id == OMCI_CLASS_OLT_G ||
				     class_id == OMCI_CLASS_VLAN_TAGGING_FILTER_DATA ||
				     class_id == OMCI_CLASS_EXTENDED_VLAN))
		ret = omci_mib_parse_set(object, mask, content + 2,
					 content_len - 2);
	if (!ret && !desc && class_id == OMCI_CLASS_OLT_G)
		ret = omci_mib_parse_set(object, mask, content + 2,
					 content_len - 2);
	if (ret)
		goto rollback_parameter;

	/* Keep ONU-created instances across a subsequent MIB reset. */
	if (!existed)
		object->origin = OMCI_MIB_ORIGIN_OLT;
	if (class_id == OMCI_CLASS_OLT_G) {
		ret = omci_agent_profile_refresh_locked(odev, &object->olt_g,
							profile_changed);
		if (ret)
			goto rollback_processing;
	}
	ret = omci_agent_hw_update(odev, object, action, content);
	if (ret)
		goto rollback_processing;
	hardware_applied = true;
	if (omci_agent_datapath_class(agent, class_id)) {
		ret = omci_agent_reconcile_services_locked(odev);
		if (ret)
			goto rollback_processing;
	}
	omci_agent_increment_mib_sync_locked(agent);
	ret = OMCI_RESULT_SUCCESS;
	goto out_free_previous;

rollback_parameter:
	if (existed) {
		*object = *previous;
	} else {
		xa_erase(&agent->mib, omci_mib_key(class_id, entity_id));
		kfree(object);
	}
	ret = ret == -ENOSPC ? OMCI_RESULT_DEVICE_BUSY :
				     OMCI_RESULT_PARAMETER_ERROR;
	goto out_free_previous;

rollback_processing:
	if (existed) {
		*object = *previous;
		if (hardware_applied)
			omci_agent_hw_update(odev, object, OMCI_MSG_TYPE_SET,
					     object->data);
	} else {
		xa_erase(&agent->mib, omci_mib_key(class_id, entity_id));
		if (hardware_applied)
			omci_agent_hw_update(odev, object, OMCI_MSG_TYPE_DELETE,
					     object->data);
		kfree(object);
	}
	ret = OMCI_RESULT_PROCESSING_ERROR;

out_free_previous:
	kfree(previous);
	return ret;
}

static u8 omci_agent_delete_locked(struct omci_device *odev, u16 class_id,
				   u16 entity_id, bool *profile_changed)
{
	struct omci_agent *agent = &odev->agent;
	const struct omci_me_desc *desc;
	struct omci_mib_object *object;
	u8 restore_action;
	int ret;

	if (!omci_me_active(agent, class_id))
		return OMCI_RESULT_UNKNOWN_ME;
	if (!omci_me_action_allowed(agent, class_id, OMCI_MSG_TYPE_DELETE))
		return OMCI_RESULT_NOT_SUPPORTED;
	object = omci_mib_lookup(agent, class_id, entity_id);
	if (!object)
		return OMCI_RESULT_UNKNOWN_INSTANCE;
	if (object->origin == OMCI_MIB_ORIGIN_DEFAULT)
		return OMCI_RESULT_NOT_SUPPORTED;

	desc = omci_me_lookup(agent, class_id);
	restore_action = desc && (desc->flags & OMCI_ME_F_ONU_CREATED) ?
		OMCI_MSG_TYPE_SET : OMCI_MSG_TYPE_CREATE;
	object->pending_delete = true;

	if (class_id == OMCI_CLASS_OLT_G) {
		ret = omci_agent_profile_refresh_locked(odev, NULL,
						profile_changed);
		if (ret)
			goto rollback_mib;
	}
	if (omci_agent_datapath_class(agent, class_id)) {
		ret = omci_agent_reconcile_services_locked(odev);
		if (ret)
			goto rollback_profile;
	}
	ret = omci_agent_hw_update(odev, object, OMCI_MSG_TYPE_DELETE,
				   object->data);
	if (ret)
		goto rollback_services;

	xa_erase(&agent->mib, omci_mib_key(class_id, entity_id));
	kfree(object);
	omci_agent_increment_mib_sync_locked(agent);
	return OMCI_RESULT_SUCCESS;

rollback_services:
	object->pending_delete = false;
	omci_agent_hw_update(odev, object, restore_action, object->data);
	omci_agent_reconcile_services_locked(odev);
	return OMCI_RESULT_PROCESSING_ERROR;
rollback_profile:
	if (class_id == OMCI_CLASS_OLT_G)
		omci_agent_profile_refresh_locked(odev, &object->olt_g, NULL);
rollback_mib:
	object->pending_delete = false;
	return OMCI_RESULT_PROCESSING_ERROR;
}

static s16 omci_power_nw_to_0p002_db(u32 power_nw, s32 offset)
{
	s64 value;

	if (!power_nw)
		return -32768;

	value = DIV_ROUND_CLOSEST_ULL((u64)intlog10(power_nw) *
				      OMCI_OPTICAL_LEVEL_SCALE * 10,
				      1U << 24);
	value -= offset;

	return clamp_t(s64, value, -32768, 32767);
}

static s16 omci_power_nw_to_ani_g_level(u32 power_nw)
{
	return omci_power_nw_to_0p002_db(power_nw,
					 OMCI_NW_TO_DBM_0P002_OFFSET);
}

static s16 omci_power_nw_to_test_level(u32 power_nw)
{
	return omci_power_nw_to_0p002_db(power_nw,
					 OMCI_NW_TO_DBUW_0P002_OFFSET);
}

static s16 omci_temperature_mc_to_test_level(s32 temperature_mc)
{
	s64 value;

	value = DIV_S64_ROUND_CLOSEST((s64)temperature_mc * 256, 1000);

	return clamp_t(s64, value, -32768, 32767);
}

static u16 omci_voltage_uv_to_test_level(u32 voltage_uv)
{
	return min_t(u32, DIV_ROUND_CLOSEST(voltage_uv, 20000), 65535);
}

static u16 omci_bias_ua_to_test_level(u32 bias_ua)
{
	return min_t(u32, DIV_ROUND_CLOSEST(bias_ua, 2), 65535);
}

static u8 omci_agent_get_ani_g_telemetry(struct omci_device *odev, u16 mask,
					 u8 *response_content,
					 size_t response_capacity,
					 size_t *response_len)
{
	struct omci_telemetry telemetry = {};
	u8 *data = response_content + 3;
	size_t used = 3;
	u16 level;
	int ret;

	if (response_capacity < 3)
		return OMCI_RESULT_PARAMETER_ERROR;
	memset(response_content, 0, response_capacity);
	ret = odev->ops->get_telemetry(odev, &telemetry);
	if (ret)
		return OMCI_RESULT_PROCESSING_ERROR;

	if ((mask & OMCI_ANI_G_RX_LEVEL_MASK) &&
	    !(telemetry.valid & OMCI_TELEMETRY_F_BOSA_RX_POWER))
		goto attribute_failed;
	if ((mask & OMCI_ANI_G_TX_LEVEL_MASK) &&
	    !(telemetry.valid & OMCI_TELEMETRY_F_BOSA_TX_POWER))
		goto attribute_failed;
	if (hweight16(mask & OMCI_ANI_G_OPTICAL_LEVEL_MASK) * sizeof(u16) >
	    response_capacity - used)
		goto attribute_failed;

	response_content[0] = OMCI_RESULT_SUCCESS;
	put_unaligned_be16(mask, response_content + 1);
	if (mask & OMCI_ANI_G_RX_LEVEL_MASK) {
		level = (u16)omci_power_nw_to_ani_g_level(
			telemetry.bosa_rx_power_nw);
		put_unaligned_be16(level, data);
		data += 2;
		used += 2;
	}
	if (mask & OMCI_ANI_G_TX_LEVEL_MASK) {
		level = (u16)omci_power_nw_to_ani_g_level(
			telemetry.bosa_tx_power_nw);
		put_unaligned_be16(level, data);
		used += 2;
	}
	*response_len = used;
	return OMCI_RESULT_SUCCESS;

attribute_failed:
	if (response_capacity < 5)
		return OMCI_RESULT_PARAMETER_ERROR;
	response_content[0] = OMCI_RESULT_ATTRIBUTE_FAILED;
	put_unaligned_be16(0, response_content + 1);
	put_unaligned_be16(mask, response_content + 3);
	*response_len = 5;
	return OMCI_RESULT_ATTRIBUTE_FAILED;
}

static bool omci_agent_get_uses_ani_g_telemetry(u16 class_id, u16 entity_id,
						u16 mask)
{
	return class_id == OMCI_CLASS_ANI_G &&
	       entity_id == OMCI_ANI_G_ENTITY_ID &&
	       mask &&
	       !(mask & ~OMCI_ANI_G_OPTICAL_LEVEL_MASK);
}

static u8 omci_agent_get_masked_object(const struct omci_mib_object *object,
				       const struct omci_get_attr_layout *layout,
				       size_t layout_len, u16 supported_mask,
				       u16 mask, u8 *response_content,
				       size_t response_capacity,
				       size_t *response_len)
{
	u8 *data = response_content + 3;
	size_t available;
	size_t used = 3;
	unsigned int i;

	if (response_capacity < 3)
		return OMCI_RESULT_PARAMETER_ERROR;
	memset(response_content, 0, response_capacity);
	available = response_capacity - used;
	if (!mask || mask & ~supported_mask)
		goto attribute_failed;

	for (i = 0; i < layout_len; i++) {
		const struct omci_get_attr_layout *attr = &layout[i];

		if (!(mask & attr->mask))
			continue;
		if (attr->len > available)
			goto attribute_failed;

		if (attr->offset == OMCI_ATTR_VALUE_ZERO)
			memset(data, 0, attr->len);
		else
			memcpy(data, object->data + attr->offset, attr->len);
		data += attr->len;
		used += attr->len;
		available -= attr->len;
	}

	response_content[0] = OMCI_RESULT_SUCCESS;
	put_unaligned_be16(mask, response_content + 1);
	*response_len = used;
	return OMCI_RESULT_SUCCESS;

attribute_failed:
	if (response_capacity < 5)
		return OMCI_RESULT_PARAMETER_ERROR;
	response_content[0] = OMCI_RESULT_ATTRIBUTE_FAILED;
	put_unaligned_be16(0, response_content + 1);
	put_unaligned_be16(mask, response_content + 3);
	*response_len = 5;
	return OMCI_RESULT_ATTRIBUTE_FAILED;
}

static int
omci_agent_refresh_nokia_optical(struct omci_device *odev,
				 struct omci_mib_object *object)
{
	struct omci_telemetry telemetry = {};
	u16 value;
	int ret;

	if (!odev->ops->get_telemetry)
		return -EOPNOTSUPP;
	ret = odev->ops->get_telemetry(odev, &telemetry);
	if (ret)
		return ret;

	object->data[0] = !!(telemetry.valid & OMCI_TELEMETRY_F_BOSA_VOLTAGE);
	if (object->data[0]) {
		value = min_t(u32, DIV_ROUND_CLOSEST(telemetry.bosa_voltage_uv,
							  10000), U16_MAX);
		put_unaligned_be16(value, object->data + 1);
	}
	object->data[3] = !!(telemetry.valid & OMCI_TELEMETRY_F_BOSA_RX_POWER);
	if (object->data[3]) {
		value = min_t(u32, DIV_ROUND_CLOSEST(telemetry.bosa_rx_power_nw,
							  10000), U16_MAX);
		put_unaligned_be16(value, object->data + 4);
	}
	object->data[6] = !!(telemetry.valid & OMCI_TELEMETRY_F_BOSA_TX_POWER);
	if (object->data[6]) {
		value = min_t(u32, DIV_ROUND_CLOSEST(telemetry.bosa_tx_power_nw,
							  10000), U16_MAX);
		put_unaligned_be16(value, object->data + 7);
	}
	object->data[9] = !!(telemetry.valid & OMCI_TELEMETRY_F_BOSA_BIAS);
	if (object->data[9]) {
		value = min_t(u64, DIV_ROUND_CLOSEST_ULL(
					(u64)telemetry.bosa_bias_ua * 2, 1000000),
			      U16_MAX);
		put_unaligned_be16(value, object->data + 10);
	}
	object->data[12] = !!(telemetry.valid &
				      OMCI_TELEMETRY_F_BOSA_TEMPERATURE);
	if (object->data[12]) {
		value = clamp_t(s32, DIV_S64_ROUND_CLOSEST(
					telemetry.bosa_temperature_mc, 1000),
				0, U16_MAX);
		put_unaligned_be16(value, object->data + 13);
	}

	return 0;
}

static u8 omci_agent_get_locked(struct omci_device *odev, u16 class_id,
				u16 entity_id, const u8 *content,
				size_t content_len, u8 *response_content,
				size_t response_capacity,
				size_t *response_len)
{
	struct omci_agent *agent = &odev->agent;
	const struct omci_me_desc *desc;
	struct omci_mib_object *object;
	size_t encoded_len = 0;
	u16 encoded_mask;
	u16 mask;
	int ret;

	if (content_len < 2 || response_capacity < 1)
		return OMCI_RESULT_PARAMETER_ERROR;
	mask = get_unaligned_be16(content);
	if (!omci_me_active(agent, class_id))
		return OMCI_RESULT_UNKNOWN_ME;
	if (!omci_me_action_allowed(agent, class_id, OMCI_MSG_TYPE_GET))
		return OMCI_RESULT_NOT_SUPPORTED;
	if (class_id == OMCI_CLASS_OMCI)
		return omci_agent_get_omci_table_locked(odev, entity_id, mask,
							response_content,
							response_capacity,
							response_len);
	if (class_id == OMCI_CLASS_MANAGED_ENTITY)
		return omci_agent_get_managed_entity_locked(odev, entity_id,
			mask, response_content, response_capacity, response_len);
	if (class_id == OMCI_CLASS_ATTRIBUTE)
		return omci_agent_get_attribute_locked(odev, entity_id, mask,
			response_content, response_capacity, response_len);

	if (omci_agent_get_uses_ani_g_telemetry(class_id, entity_id, mask) &&
	    odev->ops && odev->ops->get_telemetry)
		return omci_agent_get_ani_g_telemetry(odev, mask,
						      response_content,
						      response_capacity,
						      response_len);

	object = omci_get_or_create_locked(agent, class_id, entity_id,
					   agent->permissive ||
					   agent->fake_omci);
	if (!object)
		return OMCI_RESULT_UNKNOWN_INSTANCE;
	if (class_id == OMCI_CLASS_NOKIA_OPTICAL_SUPERVISION &&
	    omci_agent_refresh_nokia_optical(odev, object))
		return OMCI_RESULT_PROCESSING_ERROR;

	switch (class_id) {
	case OMCI_CLASS_ONU_G:
		return omci_agent_get_masked_object(
			object, omci_onu_g_attr_layout,
			ARRAY_SIZE(omci_onu_g_attr_layout), OMCI_ONU_G_ATTR_MASK,
			mask, response_content, response_capacity, response_len);
	case OMCI_CLASS_ONU2_G:
		return omci_agent_get_masked_object(
			object, omci_onu2_g_attr_layout,
			ARRAY_SIZE(omci_onu2_g_attr_layout), OMCI_ONU2_G_ATTR_MASK,
			mask, response_content, response_capacity, response_len);
	default:
		break;
	}

	if (response_capacity < 3)
		return OMCI_RESULT_PARAMETER_ERROR;
	memset(response_content, 0, response_capacity);
	response_content[0] = OMCI_RESULT_SUCCESS;
	if (class_id == OMCI_CLASS_PRIORITY_QUEUE) {
		put_unaligned_be16(mask, response_content + 1);
		ret = omci_priority_queue_serialize(object, mask,
					    response_content + 3,
					    response_capacity - 3,
					    &encoded_len);
		if (ret)
			goto attribute_failed;
	} else if (class_id == OMCI_CLASS_TRAFFIC_SCHEDULER) {
		put_unaligned_be16(mask, response_content + 1);
		ret = omci_traffic_scheduler_serialize(object, mask,
					       response_content + 3,
					       response_capacity - 3,
					       &encoded_len);
		if (ret)
			goto attribute_failed;
	} else {
		desc = omci_me_lookup(agent, class_id);
		if (!desc) {
			encoded_len = min_t(size_t, object->data_len,
					    response_capacity - 3);
			put_unaligned_be16(mask, response_content + 1);
			memcpy(response_content + 3, object->data, encoded_len);
			*response_len = 3 + encoded_len;
			return OMCI_RESULT_SUCCESS;
		}
		ret = omci_me_encode_attributes(desc, object, mask,
						response_content + 3,
						response_capacity - 3,
						&encoded_mask, &encoded_len);
		if (ret || encoded_mask != mask)
			goto attribute_failed;
		put_unaligned_be16(encoded_mask, response_content + 1);
	}
	*response_len = 3 + encoded_len;
	return OMCI_RESULT_SUCCESS;

attribute_failed:
	if (response_capacity < 5)
		return OMCI_RESULT_PARAMETER_ERROR;
	response_content[0] = OMCI_RESULT_ATTRIBUTE_FAILED;
	put_unaligned_be16(0, response_content + 1);
	put_unaligned_be16(mask, response_content + 3);
	*response_len = 5;
	return OMCI_RESULT_ATTRIBUTE_FAILED;
}

static void omci_test_result_put(u8 *field, u8 type, u16 value)
{
	field[0] = type;
	put_unaligned_be16(value, field + 1);
}

static bool omci_agent_is_ani_g_test(const struct omci_wire_request *request)
{
	return (request->message_type & 0x1f) == OMCI_MSG_TYPE_TEST &&
	       request->class_id == OMCI_CLASS_ANI_G &&
	       request->entity_id == OMCI_ANI_G_ENTITY_ID;
}

static void omci_agent_log_wire(struct omci_device *odev,
				const char *direction, const void *data,
				size_t len)
{
	struct omci_wire_request request;
	bool response;
	u8 action;

	if (omci_wire_decode(data, len, &request)) {
		dev_warn(odev->parent, "OMCI %s invalid PDU: %*phN\n",
			 direction, (int)len, data);
		return;
	}

	action = request.message_type & 0x1f;
	response = request.message_type & 0x20;
	if (action == OMCI_MSG_TYPE_MIB_UPLOAD && response &&
	    request.payload_len >= 2) {
		dev_dbg(odev->parent,
			"OMCI %s: tci=%#06x type=%#04x action=%u device=%#04x class=%u entity=%#06x payload=%u command-count=%u\n",
			direction, request.transaction_id, request.message_type,
			action, request.device_id, request.class_id,
			request.entity_id, request.payload_len,
			get_unaligned_be16(request.payload));
	} else if (action == OMCI_MSG_TYPE_MIB_UPLOAD_NEXT &&
		   request.payload_len >= 2) {
		dev_dbg(odev->parent,
			"OMCI %s: tci=%#06x type=%#04x action=%u device=%#04x class=%u entity=%#06x payload=%u %s=%u\n",
			direction, request.transaction_id, request.message_type,
			action, request.device_id, request.class_id,
			request.entity_id, request.payload_len,
			response ? "returned-class" : "sequence",
			get_unaligned_be16(request.payload));
	} else if (action == OMCI_MSG_TYPE_GET_NEXT &&
		   request.payload_len >= (response ? 3 : 4)) {
		u16 mask = response ? get_unaligned_be16(request.payload + 1) :
				      get_unaligned_be16(request.payload);
		u16 sequence = response ? 0 :
				  get_unaligned_be16(request.payload + 2);

		dev_dbg(odev->parent,
			"OMCI %s: tci=%#06x type=%#04x action=%u device=%#04x class=%u entity=%#06x payload=%u result=%u mask=%#06x sequence=%u\n",
			direction, request.transaction_id, request.message_type,
			action, request.device_id, request.class_id,
			request.entity_id, request.payload_len,
			response ? request.payload[0] : 0, mask, sequence);
	} else {
		u8 result = request.payload_len ? request.payload[0] : 0;

		dev_dbg(odev->parent,
			"OMCI %s: tci=%#06x type=%#04x action=%u device=%#04x class=%u entity=%#06x payload=%u result=%u\n",
			direction, request.transaction_id, request.message_type,
			action, request.device_id, request.class_id,
			request.entity_id, request.payload_len, result);
	}
	dev_dbg(odev->parent, "OMCI %s PDU: %*phN\n",
		direction, (int)len, data);
}

static int
omci_agent_send_ani_g_test_result(struct omci_device *odev,
				  const struct omci_wire_request *request)
{
	struct omci_agent *agent = &odev->agent;
	struct omci_telemetry telemetry = {};
	u8 pdu[OMCI_BASELINE_LEN_NO_MIC];
	u8 content[17] = {};
	size_t pdu_len;
	u16 value;
	int ret;

	if (!odev->ops || !odev->ops->get_telemetry)
		return -EOPNOTSUPP;

	ret = odev->ops->get_telemetry(odev, &telemetry);
	if (ret)
		return ret;

	if (telemetry.valid & OMCI_TELEMETRY_F_BOSA_VOLTAGE) {
		value = omci_voltage_uv_to_test_level(telemetry.bosa_voltage_uv);
		omci_test_result_put(content, OMCI_TEST_TYPE_VOLTAGE, value);
	} else {
		omci_test_result_put(content, OMCI_TEST_TYPE_UNSUPPORTED, 0);
	}

	if (telemetry.valid & OMCI_TELEMETRY_F_BOSA_RX_POWER) {
		value = (u16)omci_power_nw_to_test_level(
			telemetry.bosa_rx_power_nw);
		omci_test_result_put(content + 3, OMCI_TEST_TYPE_RX_POWER,
				     value);
	} else {
		omci_test_result_put(content + 3, OMCI_TEST_TYPE_UNSUPPORTED, 0);
	}

	if (telemetry.valid & OMCI_TELEMETRY_F_BOSA_TX_POWER) {
		value = (u16)omci_power_nw_to_test_level(
			telemetry.bosa_tx_power_nw);
		omci_test_result_put(content + 6, OMCI_TEST_TYPE_TX_POWER,
				     value);
	} else {
		omci_test_result_put(content + 6, OMCI_TEST_TYPE_UNSUPPORTED, 0);
	}

	if (telemetry.valid & OMCI_TELEMETRY_F_BOSA_BIAS) {
		value = omci_bias_ua_to_test_level(telemetry.bosa_bias_ua);
		omci_test_result_put(content + 9, OMCI_TEST_TYPE_BIAS, value);
	} else {
		omci_test_result_put(content + 9, OMCI_TEST_TYPE_UNSUPPORTED, 0);
	}

	if (telemetry.valid & OMCI_TELEMETRY_F_BOSA_TEMPERATURE) {
		value = (u16)omci_temperature_mc_to_test_level(
			telemetry.bosa_temperature_mc);
		omci_test_result_put(content + 12, OMCI_TEST_TYPE_TEMPERATURE,
				     value);
	} else {
		omci_test_result_put(content + 12, OMCI_TEST_TYPE_UNSUPPORTED, 0);
	}
	put_unaligned_be16(0, content + 15);

	ret = omci_wire_encode_response(request, OMCI_MSG_TYPE_TEST_RESULT,
					content, sizeof(content), pdu,
					sizeof(pdu), &pdu_len);
	if (ret)
		return ret;

	omci_agent_log_wire(odev, "TX", pdu, pdu_len);
	ret = omci_device_xmit(odev, pdu, pdu_len);
	if (!ret)
		atomic64_inc(&agent->responses);

	return ret;
}

static int omci_agent_upload_next_locked(struct omci_agent *agent,
					 u16 sequence, u8 *content)
{
	const struct omci_me_desc *desc;
	struct omci_mib_object *object;
	unsigned long index;
	size_t encoded_len = 0;
	u16 encoded_mask = 0;
	u16 upload_pos = 0;
	int ret;

	xa_for_each(&agent->mib, index, object) {
		if (!omci_mib_object_uploadable(agent, object))
			continue;
		if (upload_pos++ == sequence)
			goto found;
	}

	return -ENOENT;

found:
	put_unaligned_be16(object->class_id, content);
	put_unaligned_be16(object->entity_id, content + 2);
	desc = omci_me_lookup(agent, object->class_id);
	if (desc) {
		u16 mask = object->attr_mask & desc->mib_upload_mask;

		ret = omci_me_encode_attributes(desc, object, mask,
						content + 6, 26,
						&encoded_mask, &encoded_len);
		if (ret && mask)
			return ret;
	} else {
		encoded_mask = object->attr_mask;
		encoded_len = min_t(size_t, object->data_len ?: 26, 26);
		memcpy(content + 6, object->data, encoded_len);
	}
	put_unaligned_be16(encoded_mask, content + 4);
	if (encoded_len < 26)
		memset(content + 6 + encoded_len, 0, 26 - encoded_len);
	agent->upload_index = sequence + 1;
	return 0;
}

static int
omci_agent_build_response_locked(struct omci_device *odev,
				 const struct omci_wire_request *request,
				 u8 *content, size_t capacity,
				 size_t *content_len, bool *unsupported,
				 bool *fake, bool *profile_changed)
{
	struct omci_agent *agent = &odev->agent;
	u8 action = request->message_type & 0x1f;
	u16 class_id = request->class_id;
	u16 entity_id = request->entity_id;
	bool shadow_missing;
	u16 sequence;
	u16 mask = 0;
	u8 result = OMCI_RESULT_SUCCESS;
	int ret;

	if (!capacity)
		return -EMSGSIZE;
	memset(content, 0, capacity);
	shadow_missing = !omci_mib_lookup(agent, class_id, entity_id);
	*unsupported = false;
	*fake = false;
	*profile_changed = false;
	*content_len = 1;

	switch (action) {
	case OMCI_MSG_TYPE_CREATE:
		result = omci_agent_create_locked(odev, class_id, entity_id,
						 request->payload,
						 request->payload_len);
		if (omci_agent_should_fake_result(agent, class_id, result)) {
			result = OMCI_RESULT_SUCCESS;
			*fake = true;
		}
		content[0] = result;
		break;
	case OMCI_MSG_TYPE_DELETE:
		result = omci_agent_delete_locked(odev, class_id, entity_id,
						 profile_changed);
		if (omci_agent_should_fake_result(agent, class_id, result)) {
			result = OMCI_RESULT_SUCCESS;
			*fake = true;
		}
		content[0] = result;
		break;
	case OMCI_MSG_TYPE_SET:
	case OMCI_MSG_TYPE_SET_TABLE:
		result = omci_agent_set_locked(odev, class_id, entity_id, action,
					      request->payload,
					      request->payload_len,
					      profile_changed);
		if (omci_agent_should_fake_result(agent, class_id, result)) {
			result = OMCI_RESULT_SUCCESS;
			*fake = true;
		} else if (!result && omci_agent_fakes_unsupported(agent) &&
			   !agent->permissive && shadow_missing) {
			*fake = true;
		}
		if (request->payload_len >= 2)
			mask = get_unaligned_be16(request->payload);
		if (capacity < 5)
			return -EMSGSIZE;
		content[0] = result;
		put_unaligned_be16(0, content + 1);
		put_unaligned_be16(result ? mask : 0, content + 3);
		*content_len = 5;
		break;
	case OMCI_MSG_TYPE_GET:
	case OMCI_MSG_TYPE_GET_CURRENT_DATA:
		result = omci_agent_get_locked(odev, class_id, entity_id,
					      request->payload,
					      request->payload_len, content,
					      capacity, content_len);
		if (omci_agent_should_fake_result(agent, class_id, result)) {
			content[0] = OMCI_RESULT_SUCCESS;
			*content_len = 1;
			*fake = true;
		} else if (!result && omci_agent_fakes_unsupported(agent) &&
			   !agent->permissive && shadow_missing) {
			*fake = true;
		}
		if (result && !*fake) {
			content[0] = result;
			*content_len = 1;
		}
		break;
	case OMCI_MSG_TYPE_GET_NEXT:
		result = omci_agent_get_next_locked(odev, class_id, entity_id,
						    request->device_id,
						    request->payload,
						    request->payload_len,
						    content, capacity,
						    content_len);
		if (result) {
			content[0] = result;
			*content_len = 1;
		}
		break;
	case OMCI_MSG_TYPE_GET_ALL_ALARMS:
		if (capacity < 3)
			return -EMSGSIZE;
		content[0] = OMCI_RESULT_SUCCESS;
		put_unaligned_be16(0, content + 1);
		*content_len = 3;
		break;
	case OMCI_MSG_TYPE_GET_ALL_ALARMS_NEXT:
		content[0] = OMCI_RESULT_UNKNOWN_INSTANCE;
		break;
	case OMCI_MSG_TYPE_MIB_UPLOAD:
		if (capacity < 2)
			return -EMSGSIZE;
		agent->upload_index = 0;
		put_unaligned_be16(omci_mib_count_locked(agent), content);
		*content_len = 2;
		break;
	case OMCI_MSG_TYPE_MIB_UPLOAD_NEXT:
		if (request->payload_len < 2 || capacity < 32)
			return -EMSGSIZE;
		sequence = get_unaligned_be16(request->payload);
		ret = omci_agent_upload_next_locked(agent, sequence, content);
		if (ret)
			memset(content, 0, 32);
		*content_len = 32;
		break;
	case OMCI_MSG_TYPE_MIB_RESET:
		if (omci_agent_mib_reset_locked(odev, false, profile_changed))
			content[0] = OMCI_RESULT_PROCESSING_ERROR;
		else
			content[0] = OMCI_RESULT_SUCCESS;
		break;
	case OMCI_MSG_TYPE_SYNC_TIME:
	case OMCI_MSG_TYPE_REBOOT:
	case OMCI_MSG_TYPE_TEST:
		content[0] = OMCI_RESULT_SUCCESS;
		break;
	case OMCI_MSG_TYPE_START_SW_DOWNLOAD:
	case OMCI_MSG_TYPE_DOWNLOAD_SECTION:
	case OMCI_MSG_TYPE_END_SW_DOWNLOAD:
	case OMCI_MSG_TYPE_ACTIVATE_SW:
	case OMCI_MSG_TYPE_COMMIT_SW:
	default:
		if (omci_agent_fakes_unsupported(agent)) {
			content[0] = OMCI_RESULT_SUCCESS;
			*fake = true;
		} else {
			content[0] = OMCI_RESULT_NOT_SUPPORTED;
			*unsupported = true;
		}
		break;
	}

	return 0;
}

static void omci_agent_reset_duplicate_locked(struct omci_agent *agent)
{
	dev_kfree_skb(agent->last_request);
	dev_kfree_skb(agent->last_response);
	agent->last_request = NULL;
	agent->last_response = NULL;
	agent->last_request_hash = 0;
	agent->last_response_fake = false;
}

static int
omci_agent_cache_exchange_locked(struct omci_agent *agent,
				 const struct sk_buff *request,
				 const void *response, size_t response_len,
				 bool fake)
{
	struct sk_buff *cached_request;
	struct sk_buff *cached_response;

	cached_request = alloc_skb(request->len, GFP_KERNEL);
	if (!cached_request)
		return -ENOMEM;
	skb_put_data(cached_request, request->data, request->len);
	cached_response = alloc_skb(response_len, GFP_KERNEL);
	if (!cached_response) {
		kfree_skb(cached_request);
		return -ENOMEM;
	}
	skb_put_data(cached_response, response, response_len);

	omci_agent_reset_duplicate_locked(agent);
	agent->last_request = cached_request;
	agent->last_response = cached_response;
	agent->last_request_hash = jhash(request->data, request->len, 0);
	agent->last_response_fake = fake;
	return 0;
}

static void
omci_agent_report_operational(struct omci_device *odev, bool operational)
{
	if (odev->ops->set_operational)
		odev->ops->set_operational(odev, operational);
	omci_device_notify(odev, OMCI_EVENT_OPERATIONAL_CHANGE);
}

void omci_agent_receive(struct omci_device *odev, const struct sk_buff *skb)
{
	struct omci_agent *agent = &odev->agent;
	struct omci_wire_request request;
	u8 *response = NULL;
	u8 *content = NULL;
	size_t response_len = 0;
	size_t content_len = 0;
	size_t content_capacity;
	u32 request_hash;
	bool unsupported = false;
	bool duplicate = false;
	bool fake = false;
	bool profile_changed = false;
	bool operational_changed = false;
	bool channel_up;
	bool ani_g_test;
	int ret;

	ret = omci_wire_decode(skb->data, skb->len, &request);
	if (ret)
		return;
	omci_agent_log_wire(odev, "RX", skb->data, skb->len);
	ani_g_test = omci_agent_is_ani_g_test(&request);
	request_hash = jhash(skb->data, skb->len, 0);

	response = kmalloc(OMCI_MAX_PDU_LEN, GFP_KERNEL);
	content_capacity = request.device_id == OMCI_BASELINE_DEV_ID ? 32 :
		OMCI_MAX_PDU_LEN - OMCI_EXTENDED_HEADER_LEN;
	content = kzalloc(content_capacity, GFP_KERNEL);
	if (!response || !content)
		goto out;

	mutex_lock(&agent->lock);
	if (!agent->enabled) {
		mutex_unlock(&agent->lock);
		goto out;
	}

	if (agent->last_request && agent->last_response &&
	    agent->last_request_hash == request_hash &&
	    agent->last_request->len == skb->len &&
	    !memcmp(agent->last_request->data, skb->data, skb->len)) {
		response_len = agent->last_response->len;
		memcpy(response, agent->last_response->data, response_len);
		fake = agent->last_response_fake;
		duplicate = true;
	} else {
		ret = omci_agent_build_response_locked(
			odev, &request, content, content_capacity, &content_len,
			&unsupported, &fake, &profile_changed);
		if (!ret)
			ret = omci_wire_encode_response(
				&request, request.message_type & 0x1f,
				content, content_len, response, OMCI_MAX_PDU_LEN,
				&response_len);
		if (ret) {
			mutex_unlock(&agent->lock);
			goto out;
		}
		omci_agent_cache_exchange_locked(agent, skb, response,
						 response_len, fake);
	}
	mutex_unlock(&agent->lock);

	omci_agent_log_wire(odev, "TX", response, response_len);
	ret = omci_device_xmit(odev, response, response_len);
	if (!ret) {
		atomic64_inc(&agent->responses);
		if (!duplicate && !fake && !unsupported) {
			spin_lock_bh(&odev->state_lock);
			channel_up = odev->channel_up;
			spin_unlock_bh(&odev->state_lock);

			mutex_lock(&agent->lock);
			if (channel_up && agent->enabled && !agent->operational) {
				agent->operational = true;
				operational_changed = true;
			}
			mutex_unlock(&agent->lock);
		}
		if (ani_g_test) {
			ret = omci_agent_send_ani_g_test_result(odev, &request);
			if (ret)
				dev_dbg(odev->parent,
					"OMCI ANI-G test result unavailable: %d\n",
					ret);
		}
	}
	if (duplicate)
		atomic64_inc(&agent->duplicates);
	if (unsupported) {
		atomic64_inc(&agent->unsupported);
		omci_device_notify(odev, OMCI_EVENT_UNSUPPORTED);
	}
	if (fake)
		atomic64_inc(&agent->fake_responses);
	if (profile_changed)
		omci_device_notify(odev, OMCI_EVENT_PROFILE_CHANGE);
	if (operational_changed)
		omci_agent_report_operational(odev, true);

out:
	kfree(content);
	kfree(response);
}

int omci_agent_send_dying_gasp(struct omci_device *odev)
{
	struct omci_agent *agent = &odev->agent;
	u8 pdu[OMCI_BASELINE_LEN_NO_MIC] = {};
	u8 sequence;
	bool enabled;
	int ret;

	mutex_lock(&agent->lock);
	enabled = agent->enabled && agent->dying_gasp;
	if (enabled) {
		sequence = ++agent->alarm_sequence;
		if (!sequence)
			sequence = ++agent->alarm_sequence;
	}
	mutex_unlock(&agent->lock);
	if (!enabled)
		return -EOPNOTSUPP;

	pdu[2] = OMCI_MSG_TYPE_ALARM_NOTIFICATION;
	pdu[3] = OMCI_BASELINE_DEV_ID;
	put_unaligned_be16(OMCI_CLASS_ONU_G, pdu + 4);
	put_unaligned_be16(0, pdu + 6);
	pdu[8 + OMCI_ONU_G_DYING_GASP_ALARM / 8] |=
		BIT(7 - (OMCI_ONU_G_DYING_GASP_ALARM % 8));
	pdu[OMCI_ALARM_SEQUENCE_OFFSET] = sequence;
	put_unaligned_be32(40, pdu + 40);

	ret = omci_device_xmit(odev, pdu, sizeof(pdu));
	if (ret)
		return ret;

	omci_agent_log_wire(odev, "TX", pdu, sizeof(pdu));
	omci_device_notify(odev, OMCI_EVENT_DYING_GASP);
	return 0;
}

void omci_agent_channel_changed(struct omci_device *odev, bool valid)
{
	struct omci_agent *agent = &odev->agent;
	bool operational_changed;

	mutex_lock(&agent->lock);
	operational_changed = agent->operational;
	agent->operational = false;
	omci_agent_reset_duplicate_locked(agent);
	omci_agent_reset_table_snapshot_locked(agent);
	agent->upload_index = 0;
	if (!valid) {
		agent->alarm_sequence = 0;
		omci_agent_clear_services_locked(odev);
	} else {
		omci_agent_reconcile_services_locked(odev);
	}
	mutex_unlock(&agent->lock);

	if (operational_changed)
		omci_agent_report_operational(odev, false);
}

int omci_agent_put_status(struct sk_buff *msg, struct omci_device *odev)
{
	struct omci_agent *agent = &odev->agent;
	u32 count;
	u32 profile_quirks;
	u16 sync;
	u8 profile_configured;
	u8 profile_effective;
	u8 profile_forced;
	u8 onu_type;
	bool enabled;
	bool permissive;
	bool fake_omci;
	bool dying_gasp;
	bool operational;

	mutex_lock(&agent->lock);
	count = omci_mib_count_locked(agent);
	sync = agent->mib_sync;
	enabled = agent->enabled;
	permissive = agent->permissive;
	fake_omci = agent->fake_omci;
	dying_gasp = agent->dying_gasp;
	operational = agent->operational;
	profile_configured = agent->config.olt_profile;
	profile_effective = agent->profile_effective;
	profile_forced = agent->config.olt_profile_force;
	onu_type = agent->config.onu_type;
	profile_quirks = agent->profile_quirks;
	mutex_unlock(&agent->lock);

	if (nla_put_u8(msg, OMCI_ATTR_AGENT_ENABLED, enabled) ||
	    nla_put_u8(msg, OMCI_ATTR_AGENT_OPERATIONAL, operational) ||
	    nla_put_u8(msg, OMCI_ATTR_AGENT_PERMISSIVE, permissive) ||
	    nla_put_u8(msg, OMCI_ATTR_AGENT_FAKE_OMCI, fake_omci) ||
	    nla_put_u8(msg, OMCI_ATTR_AGENT_DYING_GASP, dying_gasp) ||
	    nla_put_u8(msg, OMCI_ATTR_ONU_TYPE, onu_type) ||
	    nla_put_u8(msg, OMCI_ATTR_OLT_PROFILE_CONFIGURED,
		       profile_configured) ||
	    nla_put_u8(msg, OMCI_ATTR_OLT_PROFILE_EFFECTIVE,
		       profile_effective) ||
	    nla_put_u8(msg, OMCI_ATTR_OLT_PROFILE_FORCED, profile_forced) ||
	    nla_put_u32(msg, OMCI_ATTR_OLT_PROFILE_QUIRKS, profile_quirks) ||
	    nla_put_u16(msg, OMCI_ATTR_MIB_SYNC, sync) ||
	    nla_put_u32(msg, OMCI_ATTR_MIB_OBJECTS, count) ||
	    nla_put_u64_64bit(msg, OMCI_ATTR_AGENT_RESPONSES,
			      atomic64_read(&agent->responses), OMCI_ATTR_PAD) ||
	    nla_put_u64_64bit(msg, OMCI_ATTR_AGENT_DUPLICATES,
			      atomic64_read(&agent->duplicates), OMCI_ATTR_PAD) ||
	    nla_put_u64_64bit(msg, OMCI_ATTR_AGENT_UNSUPPORTED,
			      atomic64_read(&agent->unsupported), OMCI_ATTR_PAD) ||
	    nla_put_u64_64bit(msg, OMCI_ATTR_AGENT_FAKE_RESPONSES,
			      atomic64_read(&agent->fake_responses), OMCI_ATTR_PAD))
		return -EMSGSIZE;
	return 0;
}

int omci_agent_config_get(struct omci_device *odev, u16 key,
			  void *value, size_t *len)
{
	struct omci_agent *agent = &odev->agent;
	const void *source = NULL;
	size_t source_len = 0;
	u8 scalar;

	mutex_lock(&agent->lock);
	switch (key) {
	case OMCI_CONFIG_SERIAL_NUMBER:
		source = agent->config.serial_number;
		source_len = sizeof(agent->config.serial_number);
		break;
	case OMCI_CONFIG_VENDOR_ID:
		source = agent->config.vendor_id;
		source_len = sizeof(agent->config.vendor_id);
		break;
	case OMCI_CONFIG_VERSION:
	case OMCI_CONFIG_HARDWARE_VERSION:
		source = agent->config.version;
		source_len = strnlen(agent->config.version,
				     sizeof(agent->config.version));
		break;
	case OMCI_CONFIG_SOFTWARE_VERSION_0:
		source = agent->config.software_version[0];
		source_len = strnlen(agent->config.software_version[0],
				     sizeof(agent->config.software_version[0]));
		break;
	case OMCI_CONFIG_SOFTWARE_VERSION_1:
		source = agent->config.software_version[1];
		source_len = strnlen(agent->config.software_version[1],
				     sizeof(agent->config.software_version[1]));
		break;
	case OMCI_CONFIG_EQUIPMENT_ID:
		source = agent->config.equipment_id;
		source_len = strnlen(agent->config.equipment_id,
				     sizeof(agent->config.equipment_id));
		break;
	case OMCI_CONFIG_PASSWORD:
		source = agent->config.password;
		source_len = sizeof(agent->config.password);
		break;
	case OMCI_CONFIG_TRAFFIC_MGMT_OPTION:
		scalar = agent->config.traffic_mgmt_option;
		source = &scalar;
		source_len = sizeof(scalar);
		break;
	case OMCI_CONFIG_ONU_TYPE:
		scalar = agent->config.onu_type;
		source = &scalar;
		source_len = sizeof(scalar);
		break;
	case OMCI_CONFIG_UNI_COUNT:
		scalar = agent->config.uni_count;
		source = &scalar;
		source_len = sizeof(scalar);
		break;
	case OMCI_CONFIG_AGENT_ENABLED:
		scalar = agent->enabled;
		source = &scalar;
		source_len = sizeof(scalar);
		break;
	case OMCI_CONFIG_AGENT_PERMISSIVE:
		scalar = agent->permissive;
		source = &scalar;
		source_len = sizeof(scalar);
		break;
	case OMCI_CONFIG_AGENT_FAKE_OMCI:
		scalar = agent->fake_omci;
		source = &scalar;
		source_len = sizeof(scalar);
		break;
	case OMCI_CONFIG_AGENT_DYING_GASP:
		scalar = agent->dying_gasp;
		source = &scalar;
		source_len = sizeof(scalar);
		break;
	case OMCI_CONFIG_OMCC_VERSION:
		scalar = agent->config.omcc_version;
		source = &scalar;
		source_len = sizeof(scalar);
		break;
	case OMCI_CONFIG_OLT_PROFILE:
		scalar = agent->config.olt_profile;
		source = &scalar;
		source_len = sizeof(scalar);
		break;
	case OMCI_CONFIG_OLT_PROFILE_FORCE:
		scalar = agent->config.olt_profile_force;
		source = &scalar;
		source_len = sizeof(scalar);
		break;
	default:
		mutex_unlock(&agent->lock);
		return -EINVAL;
	}

	if (*len < source_len) {
		*len = source_len;
		mutex_unlock(&agent->lock);
		return -ENOSPC;
	}
	memcpy(value, source, source_len);
	*len = source_len;
	mutex_unlock(&agent->lock);
	return 0;
}

static int
__omci_agent_config_set_source(struct omci_device *odev, u16 key,
			       const void *value, size_t len, u8 source,
			       bool notify)
{
	struct omci_agent *agent = &odev->agent;
	struct omci_identity identity;
	struct omci_olt_g *olt;
	u8 normalized[OMCI_MAX_CONFIG_VALUE];
	size_t normalized_len = sizeof(normalized);
	u8 old_profile_source;
	u8 old_force_source;
	u8 old_profile;
	u8 old_force;
	u8 scalar;
	bool profile_key = false;
	bool operational_changed = false;
	bool changed = false;
	int ret = 0;

	if ((key >= OMCI_CONFIG_SERIAL_NUMBER &&
	     key <= OMCI_CONFIG_PASSWORD) ||
	    key == OMCI_CONFIG_HARDWARE_VERSION ||
	    key == OMCI_CONFIG_SOFTWARE_VERSION_0 ||
	    key == OMCI_CONFIG_SOFTWARE_VERSION_1) {
		ret = omci_identity_normalize_config(key, value, len,
						     normalized, &normalized_len);
		if (ret)
			return ret;
		value = normalized;
		len = normalized_len;
	}

	mutex_lock(&agent->lock);
	old_profile = agent->config.olt_profile;
	old_force = agent->config.olt_profile_force;
	old_profile_source = agent->config.olt_profile_source;
	old_force_source = agent->config.olt_profile_force_source;
	switch (key) {
	case OMCI_CONFIG_SERIAL_NUMBER:
		if (len != sizeof(agent->config.serial_number)) {
			ret = -EINVAL;
			break;
		}
		changed = memcmp(agent->config.serial_number, value, len);
		memcpy(agent->config.serial_number, value, len);
		agent->config.serial_source = source;
		memcpy(agent->config.vendor_id, value,
		       sizeof(agent->config.vendor_id));
		agent->config.vendor_source = source;
		break;
	case OMCI_CONFIG_VENDOR_ID:
		if (len != sizeof(agent->config.vendor_id)) {
			ret = -EINVAL;
			break;
		}
		changed = memcmp(agent->config.vendor_id, value, len);
		memcpy(agent->config.vendor_id, value, len);
		agent->config.vendor_source = source;
		memcpy(agent->config.serial_number, value,
		       sizeof(agent->config.vendor_id));
		agent->config.serial_source = max(agent->config.serial_source,
						  source);
		break;
	case OMCI_CONFIG_VERSION:
		if (len != sizeof(agent->config.version)) {
			ret = -EINVAL;
			break;
		}
		changed = memcmp(agent->config.version, value, len) ||
			  memcmp(agent->config.software_version[0], value, len) ||
			  memcmp(agent->config.software_version[1], value, len);
		memcpy(agent->config.version, value, len);
		memcpy(agent->config.software_version[0], value, len);
		memcpy(agent->config.software_version[1], value, len);
		agent->config.version_source = source;
		agent->config.software_version_source[0] = source;
		agent->config.software_version_source[1] = source;
		break;
	case OMCI_CONFIG_HARDWARE_VERSION:
		if (len != sizeof(agent->config.version)) {
			ret = -EINVAL;
			break;
		}
		changed = memcmp(agent->config.version, value, len);
		memcpy(agent->config.version, value, len);
		agent->config.version_source = source;
		break;
	case OMCI_CONFIG_SOFTWARE_VERSION_0:
	case OMCI_CONFIG_SOFTWARE_VERSION_1: {
		unsigned int image = key == OMCI_CONFIG_SOFTWARE_VERSION_0 ? 0 : 1;

		if (len != sizeof(agent->config.software_version[image])) {
			ret = -EINVAL;
			break;
		}
		changed = memcmp(agent->config.software_version[image], value, len);
		memcpy(agent->config.software_version[image], value, len);
		agent->config.software_version_source[image] = source;
		break;
	}
	case OMCI_CONFIG_EQUIPMENT_ID:
		if (!len || len > sizeof(agent->config.equipment_id)) {
			ret = -EINVAL;
			break;
		}
		changed = memcmp(agent->config.equipment_id, value, len) ||
			  memchr_inv(agent->config.equipment_id + len, 0,
				     sizeof(agent->config.equipment_id) - len);
		memset(agent->config.equipment_id, 0,
		       sizeof(agent->config.equipment_id));
		memcpy(agent->config.equipment_id, value, len);
		agent->config.equipment_source = source;
		break;
	case OMCI_CONFIG_PASSWORD:
		if (len != sizeof(agent->config.password)) {
			ret = -EINVAL;
			break;
		}
		changed = memcmp(agent->config.password, value, len);
		memcpy(agent->config.password, value, len);
		agent->config.password_source = source;
		break;
	case OMCI_CONFIG_OLT_PROFILE:
		if (len != sizeof(scalar)) {
			ret = -EINVAL;
			break;
		}
		scalar = *(const u8 *)value;
		if (!omci_profile_valid(scalar)) {
			ret = -EINVAL;
			break;
		}
		changed = agent->config.olt_profile != scalar;
		agent->config.olt_profile = scalar;
		agent->config.olt_profile_source = source;
		profile_key = true;
		break;
	case OMCI_CONFIG_OLT_PROFILE_FORCE:
		if (len != sizeof(scalar)) {
			ret = -EINVAL;
			break;
		}
		scalar = *(const u8 *)value;
		if (!omci_profile_forceable(scalar)) {
			ret = -EINVAL;
			break;
		}
		changed = agent->config.olt_profile_force != scalar;
		agent->config.olt_profile_force = scalar;
		agent->config.olt_profile_force_source = source;
		profile_key = true;
		break;
	case OMCI_CONFIG_AGENT_DYING_GASP:
		if (len != sizeof(scalar)) {
			ret = -EINVAL;
			break;
		}
		scalar = *(const u8 *)value;
		changed = agent->dying_gasp != !!scalar;
		agent->dying_gasp = !!scalar;
		agent->config.dying_gasp_source = source;
		omci_agent_reset_duplicate_locked(agent);
		break;
	case OMCI_CONFIG_OMCC_VERSION:
		if (len != sizeof(scalar)) {
			ret = -EINVAL;
			break;
		}
		scalar = *(const u8 *)value;
		if (!scalar) {
			ret = -EINVAL;
			break;
		}
		changed = agent->config.omcc_version != scalar;
		agent->config.omcc_version = scalar;
		agent->config.omcc_version_source = source;
		break;
	case OMCI_CONFIG_TRAFFIC_MGMT_OPTION:
	case OMCI_CONFIG_ONU_TYPE:
	case OMCI_CONFIG_UNI_COUNT:
	case OMCI_CONFIG_AGENT_ENABLED:
	case OMCI_CONFIG_AGENT_PERMISSIVE:
	case OMCI_CONFIG_AGENT_FAKE_OMCI:
		if (len != sizeof(scalar)) {
			ret = -EINVAL;
			break;
		}
		scalar = *(const u8 *)value;
		if (key == OMCI_CONFIG_TRAFFIC_MGMT_OPTION) {
			changed = agent->config.traffic_mgmt_option != scalar;
			agent->config.traffic_mgmt_option = scalar;
		} else if (key == OMCI_CONFIG_ONU_TYPE) {
			if (scalar > OMCI_ONU_TYPE_CBU) {
				ret = -EINVAL;
				break;
			}
			changed = agent->config.onu_type != scalar;
			agent->config.onu_type = scalar;
			agent->config.onu_type_source = source;
		} else if (key == OMCI_CONFIG_UNI_COUNT) {
			scalar = clamp_t(u8, scalar, 1, 16);
			changed = agent->config.uni_count != scalar;
			agent->config.uni_count = scalar;
		} else if (key == OMCI_CONFIG_AGENT_ENABLED) {
			changed = agent->enabled != !!scalar;
			agent->enabled = !!scalar;
			if (!agent->enabled && agent->operational) {
				agent->operational = false;
				operational_changed = true;
			}
		} else if (key == OMCI_CONFIG_AGENT_PERMISSIVE) {
			changed = agent->permissive != !!scalar;
			agent->permissive = !!scalar;
		} else {
			changed = agent->fake_omci != !!scalar;
			agent->fake_omci = !!scalar;
		}
		omci_agent_reset_duplicate_locked(agent);
		break;
	default:
		ret = -EINVAL;
		break;
	}
	if (!ret && profile_key) {
		olt = omci_agent_olt_g_locked(agent);
		ret = omci_agent_profile_refresh_locked(odev, olt, NULL);
		if (ret) {
			agent->config.olt_profile = old_profile;
			agent->config.olt_profile_force = old_force;
			agent->config.olt_profile_source = old_profile_source;
			agent->config.olt_profile_force_source = old_force_source;
		}
	}
	if (!ret && changed && !profile_key) {
		omci_agent_refresh_identity_locked(agent);
		omci_agent_reset_duplicate_locked(agent);
		omci_agent_reset_table_snapshot_locked(agent);
		agent->upload_index = 0;
	}
	if (!ret && notify && changed)
		omci_agent_identity_state_locked(agent, &identity);
	mutex_unlock(&agent->lock);
	if (operational_changed)
		omci_agent_report_operational(odev, false);
	if (!ret && notify && changed && odev->ops->config_changed)
		odev->ops->config_changed(odev, key, &identity);
	return ret;
}

int omci_agent_config_set_source(struct omci_device *odev, u16 key,
				 const void *value, size_t len, u8 source)
{
	return __omci_agent_config_set_source(odev, key, value, len, source,
					      false);
}

int omci_agent_config_set(struct omci_device *odev, u16 key,
			  const void *value, size_t len)
{
	return omci_agent_config_set_userspace(odev, key, value, len,
					       OMCI_CONFIG_SOURCE_NETLINK);
}

int omci_agent_config_set_userspace(struct omci_device *odev, u16 key,
				    const void *value, size_t len, u8 source)
{
	if (source != OMCI_CONFIG_SOURCE_NETLINK &&
	    source != OMCI_CONFIG_SOURCE_SYSFS)
		return -EINVAL;

	return __omci_agent_config_set_source(odev, key, value, len, source,
					      true);
}

int omci_agent_config_source_get(struct omci_device *odev, u16 key, u8 *source)
{
	struct omci_agent *agent = &odev->agent;
	int ret = 0;

	if (!source)
		return -EINVAL;
	mutex_lock(&agent->lock);
	switch (key) {
	case OMCI_CONFIG_SERIAL_NUMBER:
		*source = agent->config.serial_source;
		break;
	case OMCI_CONFIG_VENDOR_ID:
		*source = agent->config.vendor_source;
		break;
	case OMCI_CONFIG_VERSION:
	case OMCI_CONFIG_HARDWARE_VERSION:
		*source = agent->config.version_source;
		break;
	case OMCI_CONFIG_SOFTWARE_VERSION_0:
		*source = agent->config.software_version_source[0];
		break;
	case OMCI_CONFIG_SOFTWARE_VERSION_1:
		*source = agent->config.software_version_source[1];
		break;
	case OMCI_CONFIG_EQUIPMENT_ID:
		*source = agent->config.equipment_source;
		break;
	case OMCI_CONFIG_PASSWORD:
		*source = agent->config.password_source;
		break;
	case OMCI_CONFIG_ONU_TYPE:
		*source = agent->config.onu_type_source;
		break;
	case OMCI_CONFIG_OLT_PROFILE:
		*source = agent->config.olt_profile_source;
		break;
	case OMCI_CONFIG_OLT_PROFILE_FORCE:
		*source = agent->config.olt_profile_force_source;
		break;
	case OMCI_CONFIG_AGENT_DYING_GASP:
		*source = agent->config.dying_gasp_source;
		break;
	case OMCI_CONFIG_OMCC_VERSION:
		*source = agent->config.omcc_version_source;
		break;
	default:
		*source = OMCI_CONFIG_SOURCE_UNSPEC;
		ret = -ENOENT;
		break;
	}
	mutex_unlock(&agent->lock);
	return ret;
}

int omci_agent_mib_get(struct omci_device *odev, u16 class_id, u16 entity_id,
		       struct omci_mib_object *object)
{
	struct omci_agent *agent = &odev->agent;
	struct omci_mib_object *stored;
	int ret = 0;

	mutex_lock(&agent->lock);
	stored = omci_mib_lookup(agent, class_id, entity_id);
	if (!stored || !omci_mib_object_active(agent, stored))
		ret = -ENOENT;
	else
		memcpy(object, stored, sizeof(*object));
	mutex_unlock(&agent->lock);
	return ret;
}

int omci_agent_mib_set(struct omci_device *odev,
		       const struct omci_mib_object *object)
{
	struct omci_agent *agent = &odev->agent;
	const struct omci_me_desc *desc;
	struct omci_mib_object *stored;
	struct omci_mib_object *local;
	bool datapath;
	int ret;

	local = kmemdup(object, sizeof(*local), GFP_KERNEL);
	if (!local)
		return -ENOMEM;

	mutex_lock(&agent->lock);
	desc = omci_me_lookup(agent, local->class_id);
	if (!omci_me_active(agent, local->class_id)) {
		ret = -ENOENT;
		goto unlock;
	}
	local->origin = OMCI_MIB_ORIGIN_LOCAL;
	local->owner_profile = desc && (desc->flags & OMCI_ME_F_VENDOR) ?
		agent->profile_effective : OMCI_OLT_PROFILE_UNSPEC;
	if (!local->data_len)
		local->data_len = desc ? desc->data_len : sizeof(local->data);
	if (local->data_len > sizeof(local->data)) {
		ret = -E2BIG;
		goto unlock;
	}
	if (local->class_id == OMCI_CLASS_OLT_G) {
		ret = omci_olt_g_parse_set(local, local->attr_mask, local->data,
					   local->data_len);
		if (ret)
			goto unlock;
	} else if (local->class_id == OMCI_CLASS_VLAN_TAGGING_FILTER_DATA) {
		omci_vlan_filter_parse_create(local, local->data);
	} else if (local->class_id == OMCI_CLASS_EXTENDED_VLAN) {
		omci_ext_vlan_parse_create(local, local->data);
		if (local->attr_mask & OMCI_EXT_VLAN_TABLE_MASK)
			omci_ext_vlan_update_rule(&local->extended_vlan,
						  local->data);
	}

	ret = omci_mib_store_locked(agent, local);
	if (ret)
		goto unlock;
	stored = omci_mib_lookup(agent, local->class_id, local->entity_id);
	if (local->class_id == OMCI_CLASS_OLT_G)
		ret = omci_agent_profile_refresh_locked(odev, &stored->olt_g,
							NULL);
	datapath = omci_agent_datapath_class(agent, local->class_id);
	if (!ret && datapath)
		ret = omci_agent_reconcile_services_locked(odev);
	if (!ret)
		omci_agent_reset_duplicate_locked(agent);

unlock:
	mutex_unlock(&agent->lock);
	kfree(local);
	return ret;
}

int omci_agent_mib_delete(struct omci_device *odev, u16 class_id,
			  u16 entity_id)
{
	struct omci_agent *agent = &odev->agent;
	struct omci_mib_object *object;
	bool datapath;
	int ret = 0;

	mutex_lock(&agent->lock);
	object = xa_erase(&agent->mib, omci_mib_key(class_id, entity_id));
	if (!object) {
		ret = -ENOENT;
		goto unlock;
	}
	if (class_id == OMCI_CLASS_OLT_G)
		ret = omci_agent_profile_refresh_locked(odev, NULL, NULL);
	datapath = omci_agent_datapath_class(agent, class_id);
	if (!ret && datapath)
		ret = omci_agent_reconcile_services_locked(odev);
	if (!ret)
		omci_agent_reset_duplicate_locked(agent);

unlock:
	mutex_unlock(&agent->lock);
	kfree(object);
	return ret;
}

void omci_agent_mib_reset(struct omci_device *odev, bool all)
{
	struct omci_agent *agent = &odev->agent;

	mutex_lock(&agent->lock);
	omci_agent_mib_reset_locked(odev, all, NULL);
	mutex_unlock(&agent->lock);
}

int omci_agent_mib_next(struct omci_device *odev, u32 index,
			struct omci_mib_object *object, u32 *next_index,
			const char **name)
{
	struct omci_agent *agent = &odev->agent;
	struct omci_mib_object *stored;
	unsigned long xa_index = index;
	int ret = 0;

	mutex_lock(&agent->lock);
	for (;;) {
		stored = xa_find(&agent->mib, &xa_index, ULONG_MAX, XA_PRESENT);
		if (!stored) {
			ret = -ENOENT;
			break;
		}
		if (omci_mib_object_active(agent, stored))
			break;
		xa_index++;
	}
	if (!ret) {
		memcpy(object, stored, sizeof(*object));
		*next_index = xa_index + 1;
		*name = omci_me_class_name(stored->class_id);
	}
	mutex_unlock(&agent->lock);
	return ret;
}

int omci_device_reconcile_services(struct omci_device *odev)
{
	struct omci_agent *agent;
	int ret;

	if (!odev)
		return -EINVAL;

	agent = &odev->agent;
	mutex_lock(&agent->lock);
	ret = omci_agent_reconcile_services_locked(odev);
	mutex_unlock(&agent->lock);

	return ret;
}
EXPORT_SYMBOL_GPL(omci_device_reconcile_services);
