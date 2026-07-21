// SPDX-License-Identifier: GPL-2.0-only
/*
 * In-kernel ONU Management and Control Interface agent
 *
 * The agent implements the baseline OMCI transaction path and owns the
 * operational MIB. Managed entities without a dedicated hardware callback
 * are stored as opaque baseline attribute data so that userspace can inspect,
 * persist and amend them through Generic Netlink.
 */

#include <linux/errno.h>
#include <linux/int_log.h>
#include <linux/kernel.h>
#include <linux/math.h>
#include <linux/math64.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/unaligned.h>
#include <net/netlink.h>

#include "internal.h"

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

#define OMCI_CLASS_ONU_DATA		2
#define OMCI_CLASS_SOFTWARE_IMAGE	7
#define OMCI_CLASS_PPTP_ETHERNET_UNI	11
#define OMCI_CLASS_MAC_BRIDGE_SERVICE_PROFILE 45
#define OMCI_CLASS_MAC_BRIDGE_CONFIG_DATA 46
#define OMCI_CLASS_MAC_BRIDGE_PORT_CONFIG_DATA 47
#define OMCI_CLASS_NETWORK_ADDRESS	137
#define OMCI_CLASS_AUTH_METHOD		148
#define OMCI_CLASS_ONU_G		256
#define OMCI_CLASS_ONU2_G		257
#define OMCI_CLASS_TCONT		262
#define OMCI_CLASS_ANI_G		263
#define OMCI_CLASS_GEM_IWTP		266
#define OMCI_CLASS_GEM_PORT_CTP	268
#define OMCI_CLASS_GAL_ETHERNET_PROFILE 272
#define OMCI_CLASS_PRIORITY_QUEUE	277
#define OMCI_CLASS_TRAFFIC_SCHEDULER	278
#define OMCI_CLASS_VEIP		329

#define OMCI_ONU_G_DYING_GASP_ALARM	7
#define OMCI_ALARM_BITMAP_LEN		28
#define OMCI_ALARM_SEQUENCE_OFFSET	(8 + OMCI_ALARM_BITMAP_LEN)

#define OMCI_ANI_G_ENTITY_ID		0x8001
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
#define OMCI_OLT_G_TIME_OF_DAY_MASK	0x1000
#define OMCI_OLT_G_TIME_OF_DAY_LEN	14

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
	vlan->associated_me = get_unaligned_be16(content + 1);
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
	default:
		return 0;
	}
}

struct omci_class_name {
	u16 class_id;
	const char *name;
};

static const struct omci_class_name omci_classes[] = {
	{ 1, "ONT" },
	{ 2, "ONU data" },
	{ 3, "PON IF line cardholder" },
	{ 4, "PON IF line card" },
	{ 5, "Cardholder" },
	{ 6, "Circuit pack" },
	{ 7, "Software image" },
	{ 8, "UNI" },
	{ 9, "TC adapter" },
	{ 10, "Physical path termination point ATM UNI" },
	{ 11, "Physical path termination point Ethernet UNI" },
	{ 12, "Physical path termination point CES UNI" },
	{ 13, "Logical N x 64 kbit/s sub-port connection termination point (CTP)" },
	{ 14, "Interworking VCC termination point" },
	{ 15, "AAL 1 profile" },
	{ 16, "AAL 5 profile" },
	{ 17, "AAL 1 protocol monitoring history data" },
	{ 18, "AAL 5 performance monitoring history data" },
	{ 19, "AAL 2 profile" },
	{ 20, "Intentionally left blank" },
	{ 21, "CES service profile" },
	{ 22, "Reserved" },
	{ 23, "CES physical interface performance monitoring history data" },
	{ 24, "Ethernet performance monitoring history data" },
	{ 25, "VP network CTP" },
	{ 26, "ATM VP cross-connection" },
	{ 27, "Priority queue" },
	{ 28, "DBR/CBR traffic descriptor" },
	{ 29, "UBR traffic descriptor" },
	{ 30, "SBR1/VBR1 traffic descriptor" },
	{ 31, "SBR2/VBR2 traffic descriptor" },
	{ 32, "SBR3/VBR3 traffic descriptor" },
	{ 33, "ABR traffic descriptor" },
	{ 34, "GFR traffic descriptor" },
	{ 35, "ABT/DT/IT traffic descriptor" },
	{ 36, "UPC disagreement monitoring history data" },
	{ 37, "Intentionally left blank" },
	{ 38, "ANI" },
	{ 39, "PON TC adapter" },
	{ 40, "PON physical path termination point" },
	{ 41, "TC adapter protocol monitoring history data" },
	{ 42, "Threshold data" },
	{ 43, "Operator specific" },
	{ 44, "Vendor specific" },
	{ 45, "MAC bridge service profile" },
	{ 46, "MAC bridge configuration data" },
	{ 47, "MAC bridge port configuration data" },
	{ 48, "MAC bridge port designation data" },
	{ 49, "MAC bridge port filter table data" },
	{ 50, "MAC bridge port bridge table data" },
	{ 51, "MAC bridge performance monitoring history data" },
	{ 52, "MAC bridge port performance monitoring history data" },
	{ 53, "Physical path termination point POTS UNI" },
	{ 54, "Voice CTP" },
	{ 55, "Voice PM history data" },
	{ 56, "AAL2 PVC profile" },
	{ 57, "AAL2 CPS protocol monitoring history data" },
	{ 58, "Voice service profile" },
	{ 59, "LES service profile" },
	{ 60, "AAL2 SSCS parameter profile 1" },
	{ 61, "AAL2 SSCS parameter profile 2" },
	{ 62, "VP performance monitoring history data" },
	{ 63, "Traffic scheduler" },
	{ 64, "T-CONT buffer" },
	{ 65, "UBR+ traffic descriptor" },
	{ 66, "AAL2 SSCS protocol monitoring history data" },
	{ 67, "IP port configuration data" },
	{ 68, "IP router service profile" },
	{ 69, "IP router configuration data" },
	{ 70, "IP router performance monitoring history data 1" },
	{ 71, "IP router performance monitoring history data 2" },
	{ 72, "ICMP performance monitoring history data 1" },
	{ 73, "ICMP performance monitoring history data 2" },
	{ 74, "IP route table" },
	{ 75, "IP static routes" },
	{ 76, "ARP service profile" },
	{ 77, "ARP configuration data" },
	{ 78, "VLAN tagging operation configuration data" },
	{ 79, "MAC bridge port filter pre-assign table" },
	{ 80, "Physical path termination point ISDN UNI" },
	{ 81, "Reserved" },
	{ 82, "Physical path termination point video UNI" },
	{ 83, "Physical path termination point LCT UNI" },
	{ 84, "VLAN tagging filter data" },
	{ 85, "ONU" },
	{ 86, "ATM VC cross-connection" },
	{ 87, "VC network CTP" },
	{ 88, "VC PM history data" },
	{ 89, "Ethernet performance monitoring history data 2" },
	{ 90, "Physical path termination point video ANI" },
	{ 91, "Physical path termination point IEEE 802.11 UNI" },
	{ 92, "IEEE 802.11 station management data 1" },
	{ 93, "IEEE 802.11 station management data 2" },
	{ 94, "IEEE 802.11 general purpose object" },
	{ 95, "IEEE 802.11 MAC and PHY operation and antenna data" },
	{ 96, "IEEE 802.11 performance monitoring history data" },
	{ 97, "IEEE 802.11 PHY FHSS DSSS IR tables" },
	{ 98, "Physical path termination point xDSL UNI part 1" },
	{ 99, "Physical path termination point xDSL UNI part 2" },
	{ 100, "xDSL line inventory and status data part 1" },
	{ 101, "xDSL line inventory and status data part 2" },
	{ 102, "xDSL channel downstream status data" },
	{ 103, "xDSL channel upstream status data" },
	{ 104, "xDSL line configuration profile part 1" },
	{ 105, "xDSL line configuration profile part 2" },
	{ 106, "xDSL line configuration profile part 3" },
	{ 107, "xDSL channel configuration profile" },
	{ 108, "xDSL subcarrier masking downstream profile" },
	{ 109, "xDSL subcarrier masking upstream profile" },
	{ 110, "xDSL PSD mask profile" },
	{ 111, "xDSL downstream radio frequency interference (RFI) bands profile" },
	{ 112, "xDSL xTU-C performance monitoring history data" },
	{ 113, "xDSL xTU-R performance monitoring history data" },
	{ 114, "xDSL xTU-C channel performance monitoring history data" },
	{ 115, "xDSL xTU-R channel performance monitoring history data" },
	{ 116, "TC adaptor performance monitoring history data xDSL" },
	{ 117, "Physical path termination point VDSL UNI (ITU-T G.993.1 VDSL1)" },
	{ 118, "VDSL VTU-O physical data" },
	{ 119, "VDSL VTU-R physical data" },
	{ 120, "VDSL channel data" },
	{ 121, "VDSL line configuration profile" },
	{ 122, "VDSL channel configuration profile" },
	{ 123, "VDSL band plan configuration profile" },
	{ 124, "VDSL VTU-O physical interface monitoring history data" },
	{ 125, "VDSL VTU-R physical interface monitoring history data" },
	{ 126, "VDSL VTU-O channel performance monitoring history data" },
	{ 127, "VDSL VTU-R channel performance monitoring history data" },
	{ 128, "Video return path service profile" },
	{ 129, "Video return path performance monitoring history data" },
	{ 130, "IEEE 802.1p mapper service profile" },
	{ 131, "OLT-G" },
	{ 132, "Multicast interworking VCC termination point" },
	{ 133, "ONU power shedding" },
	{ 134, "IP host config data" },
	{ 135, "IP host performance monitoring history data" },
	{ 136, "TCP/UDP config data" },
	{ 137, "Network address" },
	{ 138, "VoIP config data" },
	{ 139, "VoIP voice CTP" },
	{ 140, "Call control performance monitoring history data" },
	{ 141, "VoIP line status" },
	{ 142, "VoIP media profile" },
	{ 143, "RTP profile data" },
	{ 144, "RTP performance monitoring history data" },
	{ 145, "Network dial plan table" },
	{ 146, "VoIP application service profile" },
	{ 147, "VoIP feature access codes" },
	{ 148, "Authentication security method" },
	{ 149, "SIP config portal" },
	{ 150, "SIP agent config data" },
	{ 151, "SIP agent performance monitoring history data" },
	{ 152, "SIP call initiation performance monitoring history data" },
	{ 153, "SIP user data" },
	{ 154, "MGC config portal" },
	{ 155, "MGC config data" },
	{ 156, "MGC performance monitoring history data" },
	{ 157, "Large string" },
	{ 158, "ONU remote debug" },
	{ 159, "Equipment protection profile" },
	{ 160, "Equipment extension package" },
	{ 161, "Port-mapping package (legacy B-PON)" },
	{ 162, "Physical path termination point MoCA UNI" },
	{ 163, "MoCA Ethernet performance monitoring history data" },
	{ 164, "MoCA interface performance monitoring history data" },
	{ 165, "VDSL2 line configuration extensions" },
	{ 166, "xDSL line inventory and status data part 3" },
	{ 167, "xDSL line inventory and status data part 4" },
	{ 168, "VDSL2 line inventory and status data part 1" },
	{ 169, "VDSL2 line inventory and status data part 2" },
	{ 170, "VDSL2 line inventory and status data part 3" },
	{ 171, "Extended VLAN tagging operation configuration data" },
	{ 256, "ONU-G" },
	{ 257, "ONU2-G" },
	{ 258, "ONU-G (deprecated)" },
	{ 259, "ONU2-G (deprecated)" },
	{ 260, "PON IF line card-G" },
	{ 261, "PON TC adapter-G" },
	{ 262, "T-CONT" },
	{ 263, "ANI-G" },
	{ 264, "UNI-G" },
	{ 265, "ATM interworking VCC termination point" },
	{ 266, "GEM interworking termination point" },
	{ 267, "GEM port performance monitoring history data (obsolete)" },
	{ 268, "GEM port network CTP" },
	{ 269, "VP network CTP" },
	{ 270, "VC network CTP-G" },
	{ 271, "GAL TDM profile (deprecated)" },
	{ 272, "GAL Ethernet profile" },
	{ 273, "Threshold data 1" },
	{ 274, "Threshold data 2" },
	{ 275, "GAL TDM performance monitoring history data (deprecated)" },
	{ 276, "GAL Ethernet performance monitoring history data" },
	{ 277, "Priority queue" },
	{ 278, "Traffic scheduler" },
	{ 279, "Protection data" },
	{ 280, "Traffic descriptor" },
	{ 281, "Multicast GEM interworking termination point" },
	{ 282, "Pseudowire termination point" },
	{ 283, "RTP pseudowire parameters" },
	{ 284, "Pseudowire maintenance profile" },
	{ 285, "Pseudowire performance monitoring history data" },
	{ 286, "Ethernet flow termination point" },
	{ 287, "OMCI" },
	{ 288, "Managed entity" },
	{ 289, "Attribute" },
	{ 290, "Dot1X port extension package" },
	{ 291, "Dot1X configuration profile" },
	{ 292, "Dot1X performance monitoring history data" },
	{ 293, "Radius performance monitoring history data" },
	{ 294, "TU CTP" },
	{ 295, "TU performance monitoring history data" },
	{ 296, "Ethernet performance monitoring history data 3" },
	{ 297, "Port-mapping package" },
	{ 298, "Dot1 rate limiter" },
	{ 299, "Dot1ag maintenance domain" },
	{ 300, "Dot1ag maintenance association" },
	{ 301, "Dot1ag default MD level" },
	{ 302, "Dot1ag MEP" },
	{ 303, "Dot1ag MEP status" },
	{ 304, "Dot1ag MEP CCM database" },
	{ 305, "Dot1ag CFM stack" },
	{ 306, "Dot1ag chassis - management info" },
	{ 307, "Octet string" },
	{ 308, "General purpose buffer" },
	{ 309, "Multicast operations profile" },
	{ 310, "Multicast subscriber config info" },
	{ 311, "Multicast subscriber monitor" },
	{ 312, "FEC performance monitoring history data" },
	{ 313, "RE ANI-G" },
	{ 314, "Physical path termination point RE UNI" },
	{ 315, "RE upstream amplifier" },
	{ 316, "RE downstream amplifier" },
	{ 317, "RE config portal" },
	{ 318, "File transfer controller" },
	{ 319, "CES physical interface performance monitoring history data 2" },
	{ 320, "CES physical interface performance monitoring history data 3" },
	{ 321, "Ethernet frame performance monitoring history data downstream" },
	{ 322, "Ethernet frame performance monitoring history data upstream" },
	{ 323, "VDSL2 line configuration extensions 2" },
	{ 324, "xDSL impulse noise monitor performance monitoring history data" },
	{ 325, "xDSL line inventory and status data part 5" },
	{ 326, "xDSL line inventory and status data part 6" },
	{ 327, "xDSL line inventory and status data part 7" },
	{ 328, "RE common amplifier parameters" },
	{ 329, "Virtual Ethernet interface point" },
	{ 330, "Generic status portal" },
	{ 331, "ONU-E" },
	{ 332, "Enhanced security control" },
	{ 333, "MPLS pseudowire termination point" },
	{ 334, "Ethernet frame extended PM" },
	{ 335, "Simple network management protocol (SNMP) configuration data" },
	{ 336, "ONU dynamic power management control" },
	{ 337, "PW ATM configuration data" },
	{ 338, "PW ATM performance monitoring history data" },
	{ 339, "PW Ethernet configuration data" },
	{ 340, "BBF TR-069 management server" },
	{ 341, "GEM port network CTP performance monitoring history data" },
	{ 342, "TCP/UDP performance monitoring history data" },
	{ 343, "Energy consumption performance monitoring history data" },
	{ 344, "XG-PON TC performance monitoring history data" },
	{ 345, "XG-PON downstream management performance monitoring history data" },
	{ 346, "XG-PON upstream management performance monitoring history data" },
	{ 347, "IPv6 host config data" },
	{ 348, "MAC bridge port ICMPv6 process pre-assign table" },
	{ 349, "Power over Ethernet (PoE) control" },
	{ 400, "Ethernet pseudowire parameters" },
	{ 401, "Physical path termination point RS232/RS485 UNI" },
	{ 402, "RS232/RS485 port operation configuration data" },
	{ 403, "RS232/RS485 performance monitoring history data" },
	{ 404, "L2 multicast GEM interworking termination point" },
	{ 405, "ANI-E" },
	{ 406, "EPON downstream performance monitoring configuration" },
	{ 407, "SIP agent config data 2" },
	{ 408, "xDSL xTU-C performance monitoring history data part 2" },
	{ 409, "PTM performance monitoring history data xDSL" },
	{ 410, "VDSL2 line configuration extensions 3" },
	{ 411, "Vectoring line configuration extensions" },
	{ 412, "xDSL channel configuration profile part 2" },
	{ 413, "xTU data gathering configuration" },
	{ 414, "xDSL line inventory and status data part 8" },
	{ 415, "VDSL2 line inventory and status data part 4" },
	{ 416, "Vectoring line inventory and status data" },
	{ 417, "Data gathering line test, diagnostic and status" },
	{ 419, "EFM bonding group" },
	{ 420, "EFM bonding link" },
	{ 421, "EFM bonding group performance monitoring history data" },
	{ 422, "EFM bonding group performance monitoring history data part 2" },
	{ 423, "EFM bonding link performance monitoring history data" },
	{ 424, "EFM bonding port performance monitoring history data" },
	{ 425, "EFM bonding port performance monitoring history data part 2" },
	{ 426, "Ethernet frame extended PM 64 bit" },
	{ 427, "Physical path termination point xDSL UNI part 3" },
	{ 428, "FAST line configuration profile part 1" },
	{ 429, "FAST line configuration profile part 2" },
	{ 430, "FAST line configuration profile part 3" },
	{ 431, "FAST line configuration profile part 4" },
	{ 432, "FAST channel configuration profile" },
	{ 433, "FAST data path configuration profile" },
	{ 434, "FAST vectoring line configuration extensions" },
	{ 435, "FAST line inventory and status data" },
	{ 436, "FAST line inventory and status data part 2" },
	{ 437, "FAST xTU-C performance monitoring history data" },
	{ 438, "FAST xTU-R performance monitoring history data" },
	{ 439, "OpenFlow config data" },
	{ 440, "Time Status Message" },
	{ 441, "ONU3-G" },
	{ 442, "TWDM System Profile managed entity" },
	{ 443, "TWDM channel managed entity" },
	{ 444, "TWDM channel PHY/LODS performance monitoring history data" },
	{ 445, "TWDM channel XGEM performance monitoring history data" },
	{ 446, "TWDM channel PLOAM performance monitoring history data part 1" },
	{ 447, "TWDM channel PLOAM performance monitoring history data part 2" },
	{ 448, "TWDM channel PLOAM performance monitoring history data part 3" },
	{ 449, "TWDM channel tuning performance monitoring history data part 1" },
	{ 450, "TWDM channel tuning performance monitoring history data part 2" },
	{ 451, "TWDM channel tuning performance monitoring history data part 3" },
	{ 452, "TWDM channel OMCI performance monitoring history data" },
};

static bool omci_class_name_contains(const char *name, const char *needle)
{
	return strnstr(name, needle, strlen(name));
}

static u8 omci_class_category(u16 class_id, const char *name)
{
	if ((class_id >= 240 && class_id <= 255) ||
	    (class_id >= 350 && class_id <= 399) || class_id >= 65280)
		return OMCI_CLASS_CATEGORY_VENDOR;
	if ((class_id >= 172 && class_id <= 239) ||
	    (class_id >= 453 && class_id <= 65279) ||
	    omci_class_name_contains(name, "Reserved") ||
	    omci_class_name_contains(name, "Intentionally left blank"))
		return OMCI_CLASS_CATEGORY_RESERVED;
	if (omci_class_name_contains(name, "performance monitoring") ||
	    omci_class_name_contains(name, "PM history") ||
	    omci_class_name_contains(name, "monitoring history"))
		return OMCI_CLASS_CATEGORY_PERFORMANCE;
	if (omci_class_name_contains(name, "xDSL") ||
	    omci_class_name_contains(name, "VDSL") ||
	    omci_class_name_contains(name, "FAST") ||
	    omci_class_name_contains(name, "EFM bonding"))
		return OMCI_CLASS_CATEGORY_XDSL;
	if (omci_class_name_contains(name, "VoIP") ||
	    omci_class_name_contains(name, "SIP") ||
	    omci_class_name_contains(name, "MGC") ||
	    omci_class_name_contains(name, "RTP") ||
	    omci_class_name_contains(name, "Voice") ||
	    omci_class_name_contains(name, "POTS"))
		return OMCI_CLASS_CATEGORY_VOICE;
	if (omci_class_name_contains(name, "Multicast"))
		return OMCI_CLASS_CATEGORY_MULTICAST;
	if (omci_class_name_contains(name, "security") ||
	    omci_class_name_contains(name, "Authentication") ||
	    omci_class_name_contains(name, "Dot1X") ||
	    omci_class_name_contains(name, "Radius"))
		return OMCI_CLASS_CATEGORY_SECURITY;
	if (omci_class_name_contains(name, "IP ") ||
	    omci_class_name_contains(name, "TCP/UDP") ||
	    omci_class_name_contains(name, "ARP") ||
	    omci_class_name_contains(name, "ICMP") ||
	    omci_class_name_contains(name, "TR-069") ||
	    omci_class_name_contains(name, "SNMP") ||
	    omci_class_name_contains(name, "Network address"))
		return OMCI_CLASS_CATEGORY_LAYER3;
	if (omci_class_name_contains(name, "VLAN") ||
	    omci_class_name_contains(name, "MAC bridge") ||
	    omci_class_name_contains(name, "Ethernet") ||
	    omci_class_name_contains(name, "802.1p") ||
	    omci_class_name_contains(name, "Dot1ag") ||
	    omci_class_name_contains(name, "OpenFlow"))
		return OMCI_CLASS_CATEGORY_LAYER2;
	if (omci_class_name_contains(name, "UNI"))
		return OMCI_CLASS_CATEGORY_UNI;
	if (omci_class_name_contains(name, "ANI") ||
	    omci_class_name_contains(name, "GEM") ||
	    omci_class_name_contains(name, "T-CONT") ||
	    omci_class_name_contains(name, "PON") ||
	    omci_class_name_contains(name, "Traffic scheduler") ||
	    omci_class_name_contains(name, "Priority queue"))
		return OMCI_CLASS_CATEGORY_ANI;
	if (class_id <= 7 || class_id == 133 || class_id == 159 ||
	    class_id == 160 || class_id == 256 || class_id == 257 ||
	    class_id == 297 || class_id == 331 || class_id == 336 ||
	    class_id == 441)
		return OMCI_CLASS_CATEGORY_EQUIPMENT;
	if (class_id == 137 || class_id == 157 || class_id == 158 ||
	    class_id == 287 || class_id == 288 || class_id == 289 ||
	    class_id == 307 || class_id == 308 || class_id == 318 ||
	    class_id == 330 || class_id == 440)
		return OMCI_CLASS_CATEGORY_MANAGEMENT;
	if (class_id < 256)
		return OMCI_CLASS_CATEGORY_LEGACY;

	return OMCI_CLASS_CATEGORY_OTHER;
}

static u8 omci_class_support(u16 class_id)
{
	switch (class_id) {
	case OMCI_CLASS_VLAN_TAGGING_FILTER_DATA:
	case OMCI_CLASS_OLT_G:
	case OMCI_CLASS_EXTENDED_VLAN:
		return OMCI_CLASS_SUPPORT_PARSED;
	case OMCI_CLASS_PPTP_ETHERNET_UNI:
	case OMCI_CLASS_TCONT:
	case OMCI_CLASS_GEM_PORT_CTP:
	case OMCI_CLASS_VEIP:
		return OMCI_CLASS_SUPPORT_PROVISIONED;
	case OMCI_CLASS_ONU_DATA:
	case OMCI_CLASS_SOFTWARE_IMAGE:
	case OMCI_CLASS_ONU_G:
	case OMCI_CLASS_ONU2_G:
	case OMCI_CLASS_ANI_G:
	case OMCI_CLASS_PRIORITY_QUEUE:
	case OMCI_CLASS_TRAFFIC_SCHEDULER:
		return OMCI_CLASS_SUPPORT_NATIVE;
	default:
		return OMCI_CLASS_SUPPORT_SHADOW;
	}
}

static u32 omci_class_flags(u16 class_id, const char *name)
{
	u32 flags = OMCI_CLASS_F_STANDARD;

	if (omci_class_name_contains(name, "deprecated") ||
	    omci_class_name_contains(name, "obsolete"))
		flags |= OMCI_CLASS_F_DEPRECATED;
	if (omci_class_category(class_id, name) == OMCI_CLASS_CATEGORY_RESERVED)
		flags |= OMCI_CLASS_F_RESERVED;
	if (omci_class_category(class_id, name) == OMCI_CLASS_CATEGORY_VENDOR)
		flags |= OMCI_CLASS_F_VENDOR_SPECIFIC;
	if (omci_class_category(class_id, name) == OMCI_CLASS_CATEGORY_PERFORMANCE)
		flags |= OMCI_CLASS_F_PERFORMANCE;
	if (omci_class_name_contains(name, "table") || class_id == 84 ||
	    class_id == 145 || class_id == 171)
		flags |= OMCI_CLASS_F_TABLE;
	switch (class_id) {
	case OMCI_CLASS_PPTP_ETHERNET_UNI:
	case OMCI_CLASS_MAC_BRIDGE_SERVICE_PROFILE:
	case OMCI_CLASS_MAC_BRIDGE_CONFIG_DATA:
	case OMCI_CLASS_MAC_BRIDGE_PORT_CONFIG_DATA:
	case OMCI_CLASS_VLAN_TAGGING_FILTER_DATA:
	case 130:
	case OMCI_CLASS_EXTENDED_VLAN:
	case OMCI_CLASS_TCONT:
	case OMCI_CLASS_GEM_IWTP:
	case OMCI_CLASS_GEM_PORT_CTP:
	case OMCI_CLASS_VEIP:
		flags |= OMCI_CLASS_F_DATAPATH;
		break;
	default:
		break;
	}

	return flags;
}

int omci_agent_class_get(u16 class_id, struct omci_me_class *class)
{
	const char *name;

	if (!class)
		return -EINVAL;
	name = omci_agent_class_name(class_id);
	class->class_id = class_id;
	class->category = omci_class_category(class_id, name);
	class->support = omci_class_support(class_id);
	class->flags = omci_class_flags(class_id, name);
	class->name = name;

	return 0;
}

int omci_agent_class_next(u32 index, struct omci_me_class *class,
			  u32 *next_index)
{
	if (!class || !next_index)
		return -EINVAL;
	if (index >= ARRAY_SIZE(omci_classes))
		return -ENOENT;

	omci_agent_class_get(omci_classes[index].class_id, class);
	*next_index = index + 1;
	return 0;
}

const char *omci_agent_class_name(u16 class_id)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(omci_classes); i++)
		if (omci_classes[i].class_id == class_id)
			return omci_classes[i].name;

	if (class_id >= 172 && class_id <= 239)
		return "Reserved for future managed entities";
	if (class_id >= 240 && class_id <= 255)
		return "Reserved for vendor-specific managed entities";
	if (class_id >= 350 && class_id <= 399)
		return "Vendor-specific managed entity";
	if (class_id >= 453 && class_id <= 65279)
		return "Reserved for future standardization";
	if (class_id >= 65280)
		return "Vendor-specific managed entity";

	return "Unassigned managed entity";
}

static unsigned long omci_mib_key(u16 class_id, u16 entity_id)
{
	return ((unsigned long)class_id << 16) | entity_id;
}

static struct omci_mib_object *
omci_mib_lookup(struct omci_agent *agent, u16 class_id, u16 entity_id)
{
	return xa_load(&agent->mib, omci_mib_key(class_id, entity_id));
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
omci_agent_profile_refresh_locked(struct omci_device *odev,
				  struct omci_olt_g *olt,
				  bool *profile_changed)
{
	struct omci_agent *agent = &odev->agent;
	struct omci_olt_profile_state state;
	bool changed;
	int ret;

	omci_agent_profile_state_locked(agent, olt, &state);
	omci_profile_sanitize_olt_g(olt, state.quirks);
	if (olt)
		state.olt = *olt;

	changed = agent->profile_effective != state.effective ||
		  agent->profile_quirks != state.quirks;

	if (odev->ops->set_olt_profile) {
		ret = odev->ops->set_olt_profile(odev, &state);
		if (ret)
			return ret;
	}

	agent->profile_effective = state.effective;
	agent->profile_quirks = state.quirks;
	if (profile_changed)
		*profile_changed = changed;

	if (changed)
		dev_info(odev->parent,
			 "OMCI OLT profile: configured=%s effective=%s forced=%s quirks=%#x\n",
			 omci_olt_profile_name(state.configured),
			 omci_olt_profile_name(state.effective),
			 omci_olt_profile_name(state.forced), state.quirks);

	return 0;
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
omci_agent_should_fake_result(const struct omci_agent *agent, u8 result)
{
	return omci_agent_fakes_unsupported(agent) &&
	       omci_agent_result_can_be_faked(result);
}

static int omci_mib_add_default(struct omci_agent *agent, u16 class_id,
				u16 entity_id, const void *data, size_t len)
{
	struct omci_mib_object *object;
	int ret;

	object = kzalloc(sizeof(*object), GFP_KERNEL);
	if (!object)
		return -ENOMEM;

	object->class_id = class_id;
	object->entity_id = entity_id;
	object->attr_mask = 0xffff;
	object->origin = OMCI_MIB_ORIGIN_DEFAULT;
	if (data)
		memcpy(object->data, data, min(len, sizeof(object->data)));

	ret = omci_mib_store_locked(agent, object);
	kfree(object);
	return ret;
}

static void omci_agent_refresh_identity_locked(struct omci_agent *agent)
{
	struct omci_mib_object *object;

	object = omci_mib_lookup(agent, OMCI_CLASS_ONU_G, 0);
	if (object) {
		memset(object->data, 0, sizeof(object->data));
		memcpy(object->data, agent->config.vendor_id,
		       sizeof(agent->config.vendor_id));
		memcpy(object->data + 4, agent->config.version,
		       sizeof(agent->config.version));
		memcpy(object->data + 18, agent->config.serial_number,
		       sizeof(agent->config.serial_number));
	}

	object = omci_mib_lookup(agent, OMCI_CLASS_ONU2_G, 0);
	if (object) {
		memset(object->data, 0, sizeof(object->data));
		memcpy(object->data, agent->config.equipment_id,
		       min(sizeof(agent->config.equipment_id),
			   sizeof(object->data)));
		object->data[20] = 0xa0; /* Baseline and extended OMCI capability. */
		object->data[21] = agent->config.traffic_mgmt_option;
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
		agent->config.version_source = identity->version_source;
	}
	if (identity->valid & OMCI_IDENTITY_F_EQUIPMENT_ID) {
		memcpy(agent->config.equipment_id, identity->equipment_id,
		       sizeof(agent->config.equipment_id));
		agent->config.equipment_source = identity->equipment_source;
	}
}

static int omci_agent_populate_defaults(struct omci_agent *agent)
{
	u8 data[OMCI_MAX_ATTR_DATA] = {};
	unsigned int i;
	int ret;

	ret = omci_mib_add_default(agent, OMCI_CLASS_ONU_DATA, 0, data,
				   sizeof(data));
	if (ret)
		return ret;

	for (i = 0; i < 2; i++) {
		memset(data, 0, sizeof(data));
		data[0] = i == 0;
		memcpy(data + 1, agent->config.version,
		       min(sizeof(agent->config.version), sizeof(data) - 1));
		ret = omci_mib_add_default(agent, OMCI_CLASS_SOFTWARE_IMAGE, i,
					   data, sizeof(data));
		if (ret)
			return ret;
	}

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
		memset(data, 0, sizeof(data));
		data[0] = 1; /* Administrative state unlocked. */
		ret = omci_mib_add_default(agent, OMCI_CLASS_PPTP_ETHERNET_UNI,
					   i + 1, data, sizeof(data));
		if (ret)
			return ret;
	}

	memset(data, 0, sizeof(data));
	data[0] = 1;
	ret = omci_mib_add_default(agent, OMCI_CLASS_VEIP, 1, data,
				   sizeof(data));
	if (ret)
		return ret;

	for (i = 0; i < 8; i++) {
		memset(data, 0, sizeof(data));
		put_unaligned_be16(0xffff, data);
		ret = omci_mib_add_default(agent, OMCI_CLASS_TCONT,
					   0x8000 + i, data, sizeof(data));
		if (ret)
			return ret;
		ret = omci_mib_add_default(agent, OMCI_CLASS_PRIORITY_QUEUE,
					   0x8000 + i, data, sizeof(data));
		if (ret)
			return ret;
	}

	ret = omci_mib_add_default(agent, OMCI_CLASS_TRAFFIC_SCHEDULER,
				   0x8000, data, sizeof(data));
	if (ret)
		return ret;

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
	agent->enabled = true;
	agent->permissive = true;
	agent->fake_omci = false;
	agent->dying_gasp = false;
	agent->config.dying_gasp_source = OMCI_CONFIG_SOURCE_DEFAULT;
	agent->config.uni_count = 4;
	agent->config.onu_type = 2;
	agent->config.traffic_mgmt_option = 0;
	agent->config.olt_profile = OMCI_OLT_PROFILE_GENERIC;
	agent->config.olt_profile_force = OMCI_OLT_PROFILE_UNSPEC;
	agent->config.olt_profile_source = OMCI_CONFIG_SOURCE_DEFAULT;
	agent->config.olt_profile_force_source = OMCI_CONFIG_SOURCE_DEFAULT;
	memcpy(agent->config.serial_number, "OPEN0001", 8);
	memcpy(agent->config.vendor_id, "OPEN", 4);
	strscpy(agent->config.version, "OpenWrt", sizeof(agent->config.version));
	strscpy(agent->config.equipment_id, "Airoha EN7523",
		sizeof(agent->config.equipment_id));
	agent->config.serial_source = OMCI_CONFIG_SOURCE_DEFAULT;
	agent->config.vendor_source = OMCI_CONFIG_SOURCE_DEFAULT;
	agent->config.password_source = OMCI_CONFIG_SOURCE_DEFAULT;
	agent->config.version_source = OMCI_CONFIG_SOURCE_DEFAULT;
	agent->config.equipment_source = OMCI_CONFIG_SOURCE_DEFAULT;

	ret = omci_identity_load(odev->parent, &identity);
	if (ret)
		return ret;
	omci_agent_apply_identity(agent, &identity);

	mutex_lock(&agent->lock);
	ret = omci_agent_populate_defaults(agent);
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
	xa_for_each(&agent->mib, index, object) {
		xa_erase(&agent->mib, index);
		kfree(object);
	}
	mutex_unlock(&agent->lock);
	xa_destroy(&agent->mib);
}

static unsigned int omci_mib_count_locked(struct omci_agent *agent)
{
	struct omci_mib_object *object;
	unsigned long index;
	unsigned int count = 0;

	xa_for_each(&agent->mib, index, object)
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
	agent->mib_sync = 0;
	agent->upload_index = 0;
}

static int omci_agent_hw_update(struct omci_device *odev,
				struct omci_mib_object *object,
				u8 action, const u8 *content)
{
	const struct omci_device_ops *ops = odev->ops;
	bool ignore_unsupported_uni;
	u16 value;
	int ret;

	switch (object->class_id) {
	case OMCI_CLASS_TCONT:
		if (!ops->set_tcont)
			return 0;
		if (action == OMCI_MSG_TYPE_DELETE)
			return ops->set_tcont(odev, object->entity_id, 0xffff,
					      false);
		value = action == OMCI_MSG_TYPE_SET ?
			get_unaligned_be16(content + 2) :
			get_unaligned_be16(content);
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
		ret = ops->set_uni(odev, object->entity_id,
				   action != OMCI_MSG_TYPE_DELETE);
		ignore_unsupported_uni = odev->agent.profile_quirks &
					 OMCI_OLT_QUIRK_IGNORE_UNSUPPORTED_UNI;
		if (ret == -EOPNOTSUPP && ignore_unsupported_uni)
			return 0;
		return ret;
	default:
		return 0;
	}
}

static void omci_response_init(u8 *response, const u8 *request, u8 action)
{
	memset(response, 0, OMCI_BASELINE_LEN_NO_MIC);
	memcpy(response, request, 2);
	response[2] = (request[2] & 0x80) | 0x20 | action;
	response[3] = OMCI_BASELINE_DEV_ID;
	memcpy(response + 4, request + 4, 4);
	put_unaligned_be32(40, response + 40);
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
				   const u8 *content)
{
	struct omci_agent *agent = &odev->agent;
	struct omci_mib_object *object;
	u8 result = OMCI_RESULT_SUCCESS;
	int ret;

	if (omci_mib_lookup(agent, class_id, entity_id))
		return OMCI_RESULT_INSTANCE_EXISTS;

	object = kzalloc(sizeof(*object), GFP_KERNEL);
	if (!object)
		return OMCI_RESULT_DEVICE_BUSY;
	object->class_id = class_id;
	object->entity_id = entity_id;
	object->attr_mask = 0xffff;
	object->origin = OMCI_MIB_ORIGIN_OLT;
	memcpy(object->data, content, sizeof(object->data));

	ret = omci_mib_parse_create(object, content);
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
	ret = omci_mib_store_locked(agent, object);
	if (ret) {
		result = OMCI_RESULT_DEVICE_BUSY;
		goto out;
	}
	agent->mib_sync++;
out:
	kfree(object);
	return result;
}

static u8 omci_agent_set_locked(struct omci_device *odev, u16 class_id,
				u16 entity_id, const u8 *content,
				bool *profile_changed)
{
	struct omci_agent *agent = &odev->agent;
	struct omci_mib_object *object;
	u16 mask = get_unaligned_be16(content);
	bool allow_create;
	int ret;

	allow_create = agent->permissive || agent->fake_omci ||
		omci_agent_profile_has_quirk(agent, OMCI_OLT_QUIRK_ALLOW_SET_CREATE);
	object = omci_get_or_create_locked(agent, class_id, entity_id,
					  allow_create);
	if (!object)
		return allow_create ? OMCI_RESULT_DEVICE_BUSY :
				      OMCI_RESULT_UNKNOWN_INSTANCE;
	object->attr_mask |= mask;
	memcpy(object->data, content + 2, sizeof(object->data) - 2);
	ret = omci_mib_parse_set(object, mask, content + 2,
				 sizeof(object->data) - 2);
	if (ret)
		return ret == -ENOSPC ? OMCI_RESULT_DEVICE_BUSY :
					 OMCI_RESULT_PARAMETER_ERROR;
	object->origin = OMCI_MIB_ORIGIN_OLT;
	if (class_id == OMCI_CLASS_OLT_G) {
		ret = omci_agent_profile_refresh_locked(odev, &object->olt_g, profile_changed);
		if (ret)
			return OMCI_RESULT_PROCESSING_ERROR;
	}
	ret = omci_agent_hw_update(odev, object, OMCI_MSG_TYPE_SET, content);
	if (ret)
		return OMCI_RESULT_PROCESSING_ERROR;
	agent->mib_sync++;
	return OMCI_RESULT_SUCCESS;
}

static u8 omci_agent_delete_locked(struct omci_device *odev, u16 class_id,
				   u16 entity_id, bool *profile_changed)
{
	struct omci_agent *agent = &odev->agent;
	struct omci_mib_object *object;
	int ret;

	object = omci_mib_lookup(agent, class_id, entity_id);
	if (!object)
		return OMCI_RESULT_UNKNOWN_INSTANCE;
	if (object->origin == OMCI_MIB_ORIGIN_DEFAULT)
		return OMCI_RESULT_NOT_SUPPORTED;
	ret = omci_agent_hw_update(odev, object, OMCI_MSG_TYPE_DELETE,
				   object->data);
	if (ret)
		return OMCI_RESULT_PROCESSING_ERROR;
	xa_erase(&agent->mib, omci_mib_key(class_id, entity_id));
	kfree(object);
	if (class_id == OMCI_CLASS_OLT_G) {
		ret = omci_agent_profile_refresh_locked(odev, NULL, profile_changed);
		if (ret)
			return OMCI_RESULT_PROCESSING_ERROR;
	}
	agent->mib_sync++;
	return OMCI_RESULT_SUCCESS;
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
					 u8 *response_content)
{
	struct omci_telemetry telemetry = {};
	u8 *data = response_content + 3;
	u16 level;
	int ret;

	memset(response_content, 0, OMCI_BASELINE_LEN_NO_MIC - 8 - 4);
	ret = odev->ops->get_telemetry(odev, &telemetry);
	if (ret)
		return OMCI_RESULT_PROCESSING_ERROR;

	if ((mask & OMCI_ANI_G_RX_LEVEL_MASK) &&
	    !(telemetry.valid & OMCI_TELEMETRY_F_BOSA_RX_POWER)) {
		response_content[0] = OMCI_RESULT_ATTRIBUTE_FAILED;
		put_unaligned_be16(0, response_content + 1);
		put_unaligned_be16(mask, response_content + 3);
		return OMCI_RESULT_ATTRIBUTE_FAILED;
	}
	if ((mask & OMCI_ANI_G_TX_LEVEL_MASK) &&
	    !(telemetry.valid & OMCI_TELEMETRY_F_BOSA_TX_POWER)) {
		response_content[0] = OMCI_RESULT_ATTRIBUTE_FAILED;
		put_unaligned_be16(0, response_content + 1);
		put_unaligned_be16(mask, response_content + 3);
		return OMCI_RESULT_ATTRIBUTE_FAILED;
	}

	response_content[0] = OMCI_RESULT_SUCCESS;
	put_unaligned_be16(mask, response_content + 1);
	if (mask & OMCI_ANI_G_RX_LEVEL_MASK) {
		level = (u16)omci_power_nw_to_ani_g_level(telemetry.bosa_rx_power_nw);
		put_unaligned_be16(level, data);
		data += 2;
	}
	if (mask & OMCI_ANI_G_TX_LEVEL_MASK) {
		level = (u16)omci_power_nw_to_ani_g_level(telemetry.bosa_tx_power_nw);
		put_unaligned_be16(level, data);
	}

	return OMCI_RESULT_SUCCESS;
}

static bool omci_agent_get_uses_ani_g_telemetry(u16 class_id, u16 entity_id,
						u16 mask)
{
	return class_id == OMCI_CLASS_ANI_G &&
	       entity_id == OMCI_ANI_G_ENTITY_ID &&
	       mask &&
	       !(mask & ~OMCI_ANI_G_OPTICAL_LEVEL_MASK);
}

static u8 omci_agent_get_locked(struct omci_device *odev, u16 class_id,
				u16 entity_id, const u8 *content,
				u8 *response_content)
{
	struct omci_agent *agent = &odev->agent;
	struct omci_mib_object *object;
	u16 mask = get_unaligned_be16(content);

	if (omci_agent_get_uses_ani_g_telemetry(class_id, entity_id, mask) &&
	    odev->ops && odev->ops->get_telemetry)
		return omci_agent_get_ani_g_telemetry(odev, mask,
						      response_content);

	object = omci_get_or_create_locked(agent, class_id, entity_id,
					   agent->permissive ||
					   agent->fake_omci);
	if (!object)
		return OMCI_RESULT_UNKNOWN_INSTANCE;
	response_content[0] = OMCI_RESULT_SUCCESS;
	put_unaligned_be16(mask, response_content + 1);
	memcpy(response_content + 3, object->data,
	       OMCI_BASELINE_LEN_NO_MIC - 8 - 3 - 4);
	return OMCI_RESULT_SUCCESS;
}

static void omci_test_result_put(u8 *field, u8 type, u16 value)
{
	field[0] = type;
	put_unaligned_be16(value, field + 1);
}

static void omci_agent_log_baseline(struct omci_device *odev,
				    const char *direction, const u8 *pdu);

static bool omci_agent_is_ani_g_test(const u8 *request)
{
	return (request[2] & 0x1f) == OMCI_MSG_TYPE_TEST &&
	       get_unaligned_be16(request + 4) == OMCI_CLASS_ANI_G &&
	       get_unaligned_be16(request + 6) == OMCI_ANI_G_ENTITY_ID;
}

static int omci_agent_send_ani_g_test_result(struct omci_device *odev,
					     const u8 *request)
{
	struct omci_agent *agent = &odev->agent;
	struct omci_telemetry telemetry = {};
	u8 result[OMCI_BASELINE_LEN_NO_MIC];
	u8 *content = result + 8;
	u16 value;
	int ret;

	if (!odev->ops || !odev->ops->get_telemetry)
		return -EOPNOTSUPP;

	ret = odev->ops->get_telemetry(odev, &telemetry);
	if (ret)
		return ret;

	omci_response_init(result, request, OMCI_MSG_TYPE_TEST_RESULT);

	if (telemetry.valid & OMCI_TELEMETRY_F_BOSA_VOLTAGE) {
		value = omci_voltage_uv_to_test_level(telemetry.bosa_voltage_uv);
		omci_test_result_put(content, OMCI_TEST_TYPE_VOLTAGE,
				     value);
	} else {
		omci_test_result_put(content, OMCI_TEST_TYPE_UNSUPPORTED, 0);
	}

	if (telemetry.valid & OMCI_TELEMETRY_F_BOSA_RX_POWER) {
		value = (u16)omci_power_nw_to_test_level(telemetry.bosa_rx_power_nw);
		omci_test_result_put(content + 3, OMCI_TEST_TYPE_RX_POWER,
				     value);
	} else {
		omci_test_result_put(content + 3, OMCI_TEST_TYPE_UNSUPPORTED,
				     0);
	}

	if (telemetry.valid & OMCI_TELEMETRY_F_BOSA_TX_POWER) {
		value = (u16)omci_power_nw_to_test_level(telemetry.bosa_tx_power_nw);
		omci_test_result_put(content + 6, OMCI_TEST_TYPE_TX_POWER,
				     value);
	} else {
		omci_test_result_put(content + 6, OMCI_TEST_TYPE_UNSUPPORTED,
				     0);
	}

	if (telemetry.valid & OMCI_TELEMETRY_F_BOSA_BIAS) {
		value = omci_bias_ua_to_test_level(telemetry.bosa_bias_ua);
		omci_test_result_put(content + 9, OMCI_TEST_TYPE_BIAS,
				     value);
	} else {
		omci_test_result_put(content + 9, OMCI_TEST_TYPE_UNSUPPORTED,
				     0);
	}

	if (telemetry.valid & OMCI_TELEMETRY_F_BOSA_TEMPERATURE) {
		value = (u16)omci_temperature_mc_to_test_level(telemetry.bosa_temperature_mc);
		omci_test_result_put(content + 12, OMCI_TEST_TYPE_TEMPERATURE,
				     value);
	} else {
		omci_test_result_put(content + 12, OMCI_TEST_TYPE_UNSUPPORTED,
				     0);
	}

	put_unaligned_be16(0, content + 15);

	omci_agent_log_baseline(odev, "TX", result);
	ret = omci_device_xmit(odev, result, sizeof(result));
	if (!ret)
		atomic64_inc(&agent->responses);

	return ret;
}

static int omci_agent_upload_next_locked(struct omci_agent *agent,
					 u16 sequence, u8 *content)
{
	struct omci_mib_object *object;
	unsigned long index;
	u16 upload_pos = 0;

	xa_for_each(&agent->mib, index, object) {
		if (upload_pos++ == sequence)
			goto found;
	}

	return -ENOENT;

found:
	put_unaligned_be16(object->class_id, content);
	put_unaligned_be16(object->entity_id, content + 2);
	put_unaligned_be16(object->attr_mask, content + 4);
	memcpy(content + 6, object->data, 26);
	agent->upload_index = sequence + 1;
	return 0;
}

static void omci_agent_log_baseline(struct omci_device *odev,
				    const char *direction, const u8 *pdu)
{
	u8 action = pdu[2] & 0x1f;
	u16 class_id = get_unaligned_be16(pdu + 4);
	u16 entity_id = get_unaligned_be16(pdu + 6);

	if (action == OMCI_MSG_TYPE_TEST_RESULT)
		dev_info(odev->parent,
			 "OMCI %s: tci=%#06x type=%#04x action=%u class=%u entity=%#06x test_type=%u\n",
			 direction, get_unaligned_be16(pdu), pdu[2], action,
			 class_id, entity_id, pdu[8]);
	else if (action == OMCI_MSG_TYPE_ALARM_NOTIFICATION)
		dev_info(odev->parent,
			 "OMCI %s: tci=%#06x type=%#04x action=%u class=%u entity=%#06x alarm_sequence=%u\n",
			 direction, get_unaligned_be16(pdu), pdu[2], action,
			 class_id, entity_id,
			 pdu[OMCI_ALARM_SEQUENCE_OFFSET]);
	else
		dev_info(odev->parent,
			 "OMCI %s: tci=%#06x type=%#04x action=%u class=%u entity=%#06x result=%u\n",
			 direction, get_unaligned_be16(pdu), pdu[2], action,
			 class_id, entity_id, pdu[8]);
	dev_info(odev->parent, "OMCI %s PDU: %*phN\n",
		 direction, OMCI_BASELINE_LEN_NO_MIC, pdu);
}

static bool omci_agent_build_response_locked(struct omci_device *odev,
					     const u8 *request,
					     u8 *response,
					     bool *unsupported,
					     bool *fake,
					     bool *profile_changed)
{
	struct omci_agent *agent = &odev->agent;
	u8 *content = response + 8;
	u8 action = request[2] & 0x1f;
	u16 class_id = get_unaligned_be16(request + 4);
	u16 entity_id = get_unaligned_be16(request + 6);
	bool shadow_missing;
	u16 sequence;
	u8 result = OMCI_RESULT_SUCCESS;

	omci_response_init(response, request, action);
	shadow_missing = !omci_mib_lookup(agent, class_id, entity_id);
	*unsupported = false;
	*fake = false;
	*profile_changed = false;

	switch (action) {
	case OMCI_MSG_TYPE_CREATE:
		result = omci_agent_create_locked(odev, class_id, entity_id,
						  request + 8);
		if (omci_agent_should_fake_result(agent, result)) {
			result = OMCI_RESULT_SUCCESS;
			*fake = true;
		}
		content[0] = result;
		break;
	case OMCI_MSG_TYPE_DELETE:
		result = omci_agent_delete_locked(odev, class_id, entity_id, profile_changed);
		if (omci_agent_should_fake_result(agent, result)) {
			result = OMCI_RESULT_SUCCESS;
			*fake = true;
		}
		content[0] = result;
		break;
	case OMCI_MSG_TYPE_SET:
	case OMCI_MSG_TYPE_SET_TABLE:
		result = omci_agent_set_locked(odev, class_id, entity_id,
					       request + 8, profile_changed);
		if (omci_agent_should_fake_result(agent, result)) {
			result = OMCI_RESULT_SUCCESS;
			*fake = true;
		} else if (!result && omci_agent_fakes_unsupported(agent) &&
			   !agent->permissive && shadow_missing) {
			*fake = true;
		}
		content[0] = result;
		put_unaligned_be16(0, content + 1);
		put_unaligned_be16(result ? get_unaligned_be16(request + 8) : 0,
				   content + 3);
		break;
	case OMCI_MSG_TYPE_GET:
	case OMCI_MSG_TYPE_GET_NEXT:
	case OMCI_MSG_TYPE_GET_CURRENT_DATA:
		result = omci_agent_get_locked(odev, class_id, entity_id,
					       request + 8, content);
		if (omci_agent_should_fake_result(agent, result)) {
			result = OMCI_RESULT_SUCCESS;
			*fake = true;
		} else if (!result && omci_agent_fakes_unsupported(agent) &&
			   !agent->permissive && shadow_missing) {
			*fake = true;
		}
		if (result)
			content[0] = result;
		break;
	case OMCI_MSG_TYPE_GET_ALL_ALARMS:
		content[0] = OMCI_RESULT_SUCCESS;
		put_unaligned_be16(0, content + 1);
		break;
	case OMCI_MSG_TYPE_GET_ALL_ALARMS_NEXT:
		content[0] = OMCI_RESULT_UNKNOWN_INSTANCE;
		break;
	case OMCI_MSG_TYPE_MIB_UPLOAD:
		agent->upload_index = 0;
		put_unaligned_be16(omci_mib_count_locked(agent), content);
		break;
	case OMCI_MSG_TYPE_MIB_UPLOAD_NEXT:
		sequence = get_unaligned_be16(request + 8);
		omci_agent_upload_next_locked(agent, sequence, content);
		break;
	case OMCI_MSG_TYPE_MIB_RESET:
		omci_agent_reset_olt_objects_locked(agent);
		if (omci_agent_profile_refresh_locked(odev, NULL,
						      profile_changed))
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

	return true;
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
	u8 response[OMCI_BASELINE_LEN_NO_MIC];
	bool unsupported = false;
	bool duplicate = false;
	bool fake = false;
	bool profile_changed = false;
	bool operational_changed = false;
	bool channel_up;
	bool ani_g_test;
	int ret;

	if (skb->len != OMCI_BASELINE_LEN_NO_MIC &&
	    skb->len != OMCI_BASELINE_LEN)
		return;
	if (skb->data[3] != OMCI_BASELINE_DEV_ID)
		return;

	omci_agent_log_baseline(odev, "RX", skb->data);
	ani_g_test = omci_agent_is_ani_g_test(skb->data);

	mutex_lock(&agent->lock);
	if (!agent->enabled) {
		mutex_unlock(&agent->lock);
		return;
	}

	if (agent->last_request_len == OMCI_BASELINE_LEN_NO_MIC &&
	    !memcmp(agent->last_request, skb->data,
		    OMCI_BASELINE_LEN_NO_MIC)) {
		memcpy(response, agent->last_response,
		       agent->last_response_len);
		fake = agent->last_response_fake;
		duplicate = true;
	} else {
		omci_agent_build_response_locked(odev, skb->data, response,
						 &unsupported, &fake,
						 &profile_changed);
		memcpy(agent->last_request, skb->data,
		       OMCI_BASELINE_LEN_NO_MIC);
		memcpy(agent->last_response, response, sizeof(response));
		agent->last_request_len = OMCI_BASELINE_LEN_NO_MIC;
		agent->last_response_len = sizeof(response);
		agent->last_response_fake = fake;
	}
	mutex_unlock(&agent->lock);

	omci_agent_log_baseline(odev, "TX", response);
	ret = omci_device_xmit(odev, response, sizeof(response));
	if (!ret) {
		atomic64_inc(&agent->responses);
		if (!duplicate && !fake && !unsupported) {
			spin_lock_bh(&odev->state_lock);
			channel_up = odev->channel_up;
			spin_unlock_bh(&odev->state_lock);

			mutex_lock(&agent->lock);
			if (channel_up && agent->enabled &&
			    !agent->operational) {
				agent->operational = true;
				operational_changed = true;
			}
			mutex_unlock(&agent->lock);
		}
		if (ani_g_test) {
			ret = omci_agent_send_ani_g_test_result(odev,
								skb->data);
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

	omci_agent_log_baseline(odev, "TX", pdu);
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
	agent->last_request_len = 0;
	agent->last_response_len = 0;
	agent->last_response_fake = false;
	agent->upload_index = 0;
	if (!valid)
		agent->alarm_sequence = 0;
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
	profile_quirks = agent->profile_quirks;
	mutex_unlock(&agent->lock);

	if (nla_put_u8(msg, OMCI_ATTR_AGENT_ENABLED, enabled) ||
	    nla_put_u8(msg, OMCI_ATTR_AGENT_OPERATIONAL, operational) ||
	    nla_put_u8(msg, OMCI_ATTR_AGENT_PERMISSIVE, permissive) ||
	    nla_put_u8(msg, OMCI_ATTR_AGENT_FAKE_OMCI, fake_omci) ||
	    nla_put_u8(msg, OMCI_ATTR_AGENT_DYING_GASP, dying_gasp) ||
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
		source = agent->config.version;
		source_len = strnlen(agent->config.version,
				     sizeof(agent->config.version));
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

int omci_agent_config_set_source(struct omci_device *odev, u16 key,
				 const void *value, size_t len, u8 source)
{
	struct omci_agent *agent = &odev->agent;
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
	int ret = 0;

	if (key >= OMCI_CONFIG_SERIAL_NUMBER &&
	    key <= OMCI_CONFIG_PASSWORD) {
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
		memcpy(agent->config.vendor_id, value, len);
		agent->config.vendor_source = source;
		memcpy(agent->config.serial_number, value,
		       sizeof(agent->config.vendor_id));
		agent->config.serial_source = max(agent->config.serial_source,
						  source);
		break;
	case OMCI_CONFIG_VERSION:
		if (!len || len > sizeof(agent->config.version)) {
			ret = -EINVAL;
			break;
		}
		memset(agent->config.version, 0, sizeof(agent->config.version));
		memcpy(agent->config.version, value, len);
		agent->config.version_source = source;
		break;
	case OMCI_CONFIG_EQUIPMENT_ID:
		if (!len || len > sizeof(agent->config.equipment_id)) {
			ret = -EINVAL;
			break;
		}
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
		agent->dying_gasp = !!scalar;
		agent->config.dying_gasp_source = source;
		agent->last_request_len = 0;
		agent->last_response_len = 0;
		agent->last_response_fake = false;
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
		if (key == OMCI_CONFIG_TRAFFIC_MGMT_OPTION)
			agent->config.traffic_mgmt_option = scalar;
		else if (key == OMCI_CONFIG_ONU_TYPE)
			agent->config.onu_type = scalar;
		else if (key == OMCI_CONFIG_UNI_COUNT)
			agent->config.uni_count = clamp_t(u8, scalar, 1, 16);
		else if (key == OMCI_CONFIG_AGENT_ENABLED) {
			agent->enabled = !!scalar;
			if (!agent->enabled && agent->operational) {
				agent->operational = false;
				operational_changed = true;
			}
		} else if (key == OMCI_CONFIG_AGENT_PERMISSIVE)
			agent->permissive = !!scalar;
		else
			agent->fake_omci = !!scalar;
		agent->last_request_len = 0;
		agent->last_response_len = 0;
		agent->last_response_fake = false;
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
	if (!ret) {
		if (!profile_key)
			omci_agent_refresh_identity_locked(agent);
		agent->last_request_len = 0;
		agent->last_response_len = 0;
		agent->last_response_fake = false;
		agent->mib_sync++;
	}
	mutex_unlock(&agent->lock);
	if (operational_changed)
		omci_agent_report_operational(odev, false);
	return ret;
}

int omci_agent_config_set(struct omci_device *odev, u16 key,
			  const void *value, size_t len)
{
	return omci_agent_config_set_source(odev, key, value, len,
					    OMCI_CONFIG_SOURCE_NETLINK);
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
		*source = agent->config.version_source;
		break;
	case OMCI_CONFIG_EQUIPMENT_ID:
		*source = agent->config.equipment_source;
		break;
	case OMCI_CONFIG_PASSWORD:
		*source = agent->config.password_source;
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
	if (!stored)
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
	struct omci_mib_object *stored;
	struct omci_mib_object *local;
	int ret;

	local = kmemdup(object, sizeof(*local), GFP_KERNEL);
	if (!local)
		return -ENOMEM;

	local->origin = OMCI_MIB_ORIGIN_LOCAL;
	if (local->class_id == OMCI_CLASS_OLT_G) {
		ret = omci_olt_g_parse_set(local, local->attr_mask, local->data,
					   sizeof(local->data));
		if (ret)
			goto out;
	} else if (local->class_id == OMCI_CLASS_VLAN_TAGGING_FILTER_DATA) {
		omci_vlan_filter_parse_create(local, local->data);
	} else if (local->class_id == OMCI_CLASS_EXTENDED_VLAN) {
		local->extended_vlan.max_table_size = OMCI_EXT_VLAN_MAX_RULES;
		local->extended_vlan.valid = true;
		if (local->attr_mask & OMCI_EXT_VLAN_TABLE_MASK)
			omci_ext_vlan_update_rule(&local->extended_vlan,
						  local->data);
	}
	mutex_lock(&agent->lock);
	ret = omci_mib_store_locked(agent, local);
	if (!ret && local->class_id == OMCI_CLASS_OLT_G) {
		stored = omci_mib_lookup(agent, local->class_id, local->entity_id);
		ret = omci_agent_profile_refresh_locked(odev, &stored->olt_g, NULL);
	}
	if (!ret)
		agent->mib_sync++;
	mutex_unlock(&agent->lock);
out:
	kfree(local);
	return ret;
}

int omci_agent_mib_delete(struct omci_device *odev, u16 class_id,
			  u16 entity_id)
{
	struct omci_agent *agent = &odev->agent;
	struct omci_mib_object *object;

	mutex_lock(&agent->lock);
	object = xa_erase(&agent->mib, omci_mib_key(class_id, entity_id));
	if (object && class_id == OMCI_CLASS_OLT_G)
		omci_agent_profile_refresh_locked(odev, NULL, NULL);
	if (object)
		agent->mib_sync++;
	mutex_unlock(&agent->lock);
	kfree(object);
	return object ? 0 : -ENOENT;
}

void omci_agent_mib_reset(struct omci_device *odev, bool all)
{
	struct omci_agent *agent = &odev->agent;
	struct omci_mib_object *object;
	unsigned long index;

	mutex_lock(&agent->lock);
	if (!all) {
		omci_agent_reset_olt_objects_locked(agent);
		omci_agent_profile_refresh_locked(odev, NULL, NULL);
		mutex_unlock(&agent->lock);
		return;
	}
	xa_for_each(&agent->mib, index, object) {
		xa_erase(&agent->mib, index);
		kfree(object);
	}
	agent->mib_sync = 0;
	agent->upload_index = 0;
	omci_agent_populate_defaults(agent);
	omci_agent_profile_refresh_locked(odev, NULL, NULL);
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
	stored = xa_find(&agent->mib, &xa_index, ULONG_MAX, XA_PRESENT);
	if (!stored) {
		ret = -ENOENT;
	} else {
		memcpy(object, stored, sizeof(*object));
		*next_index = xa_index + 1;
		*name = omci_agent_class_name(stored->class_id);
	}
	mutex_unlock(&agent->lock);
	return ret;
}
