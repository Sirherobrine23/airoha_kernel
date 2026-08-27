/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _NET_OMCI_ME_H
#define _NET_OMCI_ME_H

#include <linux/bits.h>
#include <linux/types.h>

struct omci_agent;
struct omci_device;
struct omci_me_class;
struct omci_mib_object;

#define OMCI_CLASS_ONU_DATA		2
#define OMCI_CLASS_CARDHOLDER		5
#define OMCI_CLASS_CIRCUIT_PACK		6
#define OMCI_CLASS_SOFTWARE_IMAGE	7
#define OMCI_CLASS_PPTP_ETHERNET_UNI	11
#define OMCI_CLASS_MAC_BRIDGE_SERVICE_PROFILE	45
#define OMCI_CLASS_MAC_BRIDGE_CONFIG_DATA	46
#define OMCI_CLASS_MAC_BRIDGE_PORT_CONFIG_DATA	47
#define OMCI_CLASS_VLAN_TAGGING_FILTER_DATA	84
#define OMCI_CLASS_8021P_MAPPER		130

/*
 * 802.1p mapper instances seeded for the Nokia/ALCL profile. One per GEM
 * interworking termination point: mapper 1 fronts GEM 256 (management and
 * VoIP), mapper 2 fronts GEM 257 (high speed internet).
 */
#define OMCI_NOKIA_MAPPER_MGMT		1
#define OMCI_NOKIA_MAPPER_HSI		2
#define OMCI_CLASS_OLT_G		131
#define OMCI_CLASS_NETWORK_ADDRESS	137
#define OMCI_CLASS_AUTH_METHOD		148
#define OMCI_CLASS_EXTENDED_VLAN	171
#define OMCI_CLASS_ONU_G		256
#define OMCI_CLASS_ONU2_G		257

/*
 * ONU2-G security capability and security mode, G.988 9.1.3: 1 selects
 * AES-128, the only cipher GPON defines.
 */
#define OMCI_ONU2G_SECURITY_AES128	1

/*
 * ONU2-G connectivity capability, G.988 Table 9.1.3-1. One bit per forwarding
 * model: N:1 bridging, 1:M mapping, 1:P filtering, N:M bridge-mapping,
 * 1:MP map-filtering, N:P bridge-filtering and N:MP bridge-map-filtering.
 * The agent builds its datapath out of MAC bridges, GEM interworking TPs and
 * a VEIP, so advertise the full set the way a stock ONU does and let the OLT
 * pick.
 */
#define OMCI_ONU2G_CONNECTIVITY_CAPABILITY	0x007f
#define OMCI_CLASS_TCONT		262
#define OMCI_CLASS_ANI_G		263
#define OMCI_CLASS_UNI_G		264
#define OMCI_CLASS_GEM_IWTP		266
#define OMCI_CLASS_GEM_PORT_CTP	268
#define OMCI_CLASS_GAL_ETHERNET_PROFILE	272
#define OMCI_CLASS_PRIORITY_QUEUE	277
#define OMCI_CLASS_TRAFFIC_SCHEDULER	278
#define OMCI_CLASS_OMCI		287
#define OMCI_CLASS_MANAGED_ENTITY	288
#define OMCI_CLASS_ATTRIBUTE		289
#define OMCI_CLASS_VEIP		329

#define OMCI_CLASS_HUAWEI_FLOW_MAPPING	350
#define OMCI_CLASS_HUAWEI_VLAN_MAPPING	352
#define OMCI_CLASS_HUAWEI_SW_IMAGE_EXT	353
#define OMCI_CLASS_HUAWEI_MULTICAST_367	367
#define OMCI_CLASS_HUAWEI_MULTICAST_370	370
#define OMCI_CLASS_HUAWEI_MULTICAST_373	373
#define OMCI_CLASS_HUAWEI_MULTICAST_65408	65408
#define OMCI_CLASS_HUAWEI_MULTICAST_65414	65414
#define OMCI_CLASS_HUAWEI_MULTICAST_65425	65425

#define OMCI_CLASS_NOKIA_OPTICAL_SUPERVISION	65295
#define OMCI_CLASS_NOKIA_ONT_GENERIC_V2	65296
#define OMCI_CLASS_NOKIA_UNI_SUPPLEMENTAL_V2	65297
#define OMCI_CLASS_NOKIA_GEM_PM_PART2	65300
#define OMCI_CLASS_NOKIA_VLAN_DS_SUPPLEMENTAL	65305
#define OMCI_CLASS_NOKIA_VLAN_US_POLICER	65306

#define OMCI_ME_ACTION_CREATE		BIT(4)
#define OMCI_ME_ACTION_DELETE		BIT(6)
#define OMCI_ME_ACTION_SET		BIT(8)
#define OMCI_ME_ACTION_GET		BIT(9)
#define OMCI_ME_ACTION_GET_NEXT		BIT(26)
#define OMCI_ME_ACTION_MIB_UPLOAD	BIT(13)
#define OMCI_ME_ACTION_MIB_UPLOAD_NEXT	BIT(14)
#define OMCI_ME_ACTION_MIB_RESET	BIT(15)
#define OMCI_ME_ACTION_SET_TABLE	BIT(29)

#define OMCI_ME_F_NO_MIB_UPLOAD	BIT(0)
#define OMCI_ME_F_ONU_CREATED		BIT(1)
#define OMCI_ME_F_DATAPATH		BIT(2)
#define OMCI_ME_F_SAFE_FAKE		BIT(3)
#define OMCI_ME_F_VENDOR		BIT(4)

#define OMCI_PROFILE_BIT(_profile)	BIT(_profile)
#define OMCI_PROFILE_STANDARD_MASK	GENMASK(7, 1)

enum omci_attr_access {
	OMCI_ATTR_ACCESS_READ		= BIT(0),
	OMCI_ATTR_ACCESS_WRITE		= BIT(1),
	OMCI_ATTR_ACCESS_SET_CREATE	= BIT(2),
};

struct omci_attr_desc {
	u16 mask;
	u8 offset;
	u8 len;
	u8 access;
};

struct omci_me_desc {
	u16 class_id;
	const char *name;
	u32 profile_mask;
	u32 actions;
	u32 flags;
	u16 valid_attr_mask;
	u16 mib_upload_mask;
	u8 data_len;
	u8 category;
	u8 support;
	const struct omci_attr_desc *attrs;
	unsigned int num_attrs;
};

const struct omci_me_desc *omci_me_lookup(const struct omci_agent *agent,
					  u16 class_id);
const struct omci_me_desc *omci_me_lookup_profile(u8 profile, u16 class_id);
bool omci_me_active(const struct omci_agent *agent, u16 class_id);
bool omci_me_action_allowed(const struct omci_agent *agent, u16 class_id,
			    u8 message_type);

int omci_me_decode_create(const struct omci_me_desc *desc,
			  struct omci_mib_object *object,
			  const u8 *data, size_t len);
int omci_me_decode_set(const struct omci_me_desc *desc,
			       struct omci_mib_object *object, u16 mask,
			       const u8 *data, size_t len);
int omci_me_encode_attributes(const struct omci_me_desc *desc,
			      const struct omci_mib_object *object,
			      u16 mask, u8 *data, size_t len,
			      u16 *encoded_mask, size_t *encoded_len);

const char *omci_me_class_name(u16 class_id);
int omci_me_class_get(struct omci_device *odev, u16 class_id,
		      struct omci_me_class *class);
int omci_me_class_next(struct omci_device *odev, u32 index,
		       struct omci_me_class *class, u32 *next_index);
int omci_me_attr_id(const struct omci_agent *agent, u16 class_id,
		    unsigned int attr_index, u16 *attr_id);
int omci_me_attr_get(const struct omci_agent *agent, u16 attr_id,
		     u16 *class_id, unsigned int *attr_index,
		     const struct omci_me_desc **desc,
		     const struct omci_attr_desc **attr);
unsigned int omci_me_attr_count(const struct omci_agent *agent);

#endif /* _NET_OMCI_ME_H */
