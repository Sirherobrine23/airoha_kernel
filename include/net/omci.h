/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _NET_OMCI_H
#define _NET_OMCI_H

#include <linux/bits.h>
#include <linux/types.h>
#include <uapi/linux/omci.h>

struct device;
struct omci_device;
struct sk_buff;

#define OMCI_IDENTITY_F_SERIAL_NUMBER	BIT(0)
#define OMCI_IDENTITY_F_VENDOR_ID	BIT(1)
#define OMCI_IDENTITY_F_PASSWORD	BIT(2)
#define OMCI_IDENTITY_F_VERSION	BIT(3)
#define OMCI_IDENTITY_F_EQUIPMENT_ID	BIT(4)

/**
 * struct omci_identity - normalized ONU identity and registration data
 * @valid: OMCI_IDENTITY_F_* bitmap
 * @serial_number: canonical eight-byte GPON serial number
 * @vendor_id: canonical four-byte vendor identifier
 * @password: canonical ten-byte GPON registration password
 * @version: ONU-G version field
 * @equipment_id: ONU2-G equipment identifier
 * @serial_source: enum omci_config_source for @serial_number
 * @vendor_source: enum omci_config_source for @vendor_id
 * @password_source: enum omci_config_source for @password
 * @version_source: enum omci_config_source for @version
 * @equipment_source: enum omci_config_source for @equipment_id
 */
struct omci_identity {
	u32 valid;
	u8 serial_number[8];
	u8 vendor_id[OMCI_OLT_VENDOR_ID_LEN];
	u8 password[10];
	u8 version[OMCI_OLT_VERSION_LEN];
	u8 equipment_id[OMCI_OLT_EQUIPMENT_ID_LEN];
	u8 serial_source;
	u8 vendor_source;
	u8 password_source;
	u8 version_source;
	u8 equipment_source;
};

/**
 * struct omci_me_class - managed entity class descriptor
 * @class_id: ITU managed entity class identifier
 * @category: enum omci_class_category
 * @support: enum omci_class_support
 * @flags: OMCI_CLASS_F_* bitmap
 * @name: stable human-readable managed entity name
 */
struct omci_me_class {
	u16 class_id;
	u8 category;
	u8 support;
	u32 flags;
	const char *name;
};

enum omci_gem_port_direction {
	OMCI_GEM_PORT_DIRECTION_UNI_TO_ANI = 1,
	OMCI_GEM_PORT_DIRECTION_ANI_TO_UNI = 2,
	OMCI_GEM_PORT_DIRECTION_BIDIRECTIONAL = 3,
};

/**
 * struct omci_olt_g - parsed OLT-G identification attributes
 * @vendor_id: four-character OLT vendor identifier
 * @equipment_id: OLT equipment or model identifier
 * @version: OLT software or hardware version identifier
 * @vendor_id_valid: @vendor_id has been received from the OLT
 * @equipment_id_valid: @equipment_id has been received from the OLT
 * @version_valid: @version has been received from the OLT
 * @valid: at least one identification attribute is available
 */
struct omci_olt_g {
	char vendor_id[OMCI_OLT_VENDOR_ID_LEN + 1];
	char equipment_id[OMCI_OLT_EQUIPMENT_ID_LEN + 1];
	char version[OMCI_OLT_VERSION_LEN + 1];
	bool vendor_id_valid;
	bool equipment_id_valid;
	bool version_valid;
	bool valid;
};

/**
 * struct omci_olt_profile_state - resolved OLT interoperability policy
 * @configured: user-selected profile, including auto
 * @effective: active concrete profile
 * @forced: forced concrete profile, or OMCI_OLT_PROFILE_UNSPEC
 * @quirks: OMCI_OLT_QUIRK_* bitmap for @effective
 * @olt: latest parsed OLT-G identity
 */
struct omci_olt_profile_state {
	u8 configured;
	u8 effective;
	u8 forced;
	u32 quirks;
	struct omci_olt_g olt;
};

/**
 * struct omci_vlan_filter_entry - parsed VLAN tagging filter entry
 * @tci: original VLAN tag control information value
 * @vid: VLAN identifier
 * @pbit: priority code point
 * @dei: drop eligible indicator
 */
struct omci_vlan_filter_entry {
	u16 tci;
	u16 vid;
	u8 pbit;
	u8 dei;
};

/**
 * struct omci_vlan_tagging_filter - parsed class 84 attributes
 * @entries: valid VLAN filter entries
 * @forward_operation: forwarding operation from the managed entity
 * @num_entries: number of valid entries in @entries
 * @valid: parsed data is available
 */
struct omci_vlan_tagging_filter {
	struct omci_vlan_filter_entry entries[OMCI_VLAN_FILTER_MAX_ENTRIES];
	u8 forward_operation;
	u8 num_entries;
	bool valid;
};

/**
 * struct omci_extended_vlan_rule - parsed class 171 table entry
 * @raw: original 16-byte table entry
 * @filter_outer_vid: outer-tag VLAN identifier match
 * @filter_inner_vid: inner-tag VLAN identifier match
 * @treat_outer_vid: outer-tag VLAN identifier treatment
 * @treat_inner_vid: inner-tag VLAN identifier treatment
 * @filter_outer_pbit: outer-tag priority match
 * @filter_outer_tpid_dei: outer-tag TPID/DEI match mode
 * @filter_inner_pbit: inner-tag priority match
 * @filter_inner_tpid_dei: inner-tag TPID/DEI match mode
 * @filter_ethertype: EtherType match mode
 * @tags_to_remove: number of tags removed, or discard mode
 * @treat_outer_pbit: outer-tag priority treatment
 * @treat_outer_tpid_dei: outer-tag TPID/DEI treatment mode
 * @treat_inner_pbit: inner-tag priority treatment
 * @treat_inner_tpid_dei: inner-tag TPID/DEI treatment mode
 * @delete: entry encodes a table deletion
 */
struct omci_extended_vlan_rule {
	u8 raw[OMCI_EXT_VLAN_RULE_LEN];
	u16 filter_outer_vid;
	u16 filter_inner_vid;
	u16 treat_outer_vid;
	u16 treat_inner_vid;
	u8 filter_outer_pbit;
	u8 filter_outer_tpid_dei;
	u8 filter_inner_pbit;
	u8 filter_inner_tpid_dei;
	u8 filter_ethertype;
	u8 tags_to_remove;
	u8 treat_outer_pbit;
	u8 treat_outer_tpid_dei;
	u8 treat_inner_pbit;
	u8 treat_inner_tpid_dei;
	bool delete;
};

/**
 * struct omci_extended_vlan - parsed class 171 attributes and table
 * @rules: parsed VLAN operation table
 * @dscp_to_pbit: DSCP-to-P-bit mapping attribute
 * @input_tpid: input TPID
 * @output_tpid: output TPID
 * @associated_me: associated managed entity pointer
 * @max_table_size: advertised maximum number of table entries
 * @association_type: associated managed entity type
 * @downstream_mode: downstream VLAN processing mode
 * @rule_count: number of valid rules in @rules
 * @valid: parsed data is available
 */
struct omci_extended_vlan {
	struct omci_extended_vlan_rule rules[OMCI_EXT_VLAN_MAX_RULES];
	u8 dscp_to_pbit[24];
	u16 input_tpid;
	u16 output_tpid;
	u16 associated_me;
	u16 max_table_size;
	u8 association_type;
	u8 downstream_mode;
	u8 rule_count;
	bool valid;
};

/**
 * struct omci_telemetry - current PON and optical telemetry
 * @valid: OMCI_TELEMETRY_F_* bitmap
 * @bosa_temperature_mc: BOSA temperature in milli-degrees Celsius
 * @bosa_voltage_uv: optical frontend supply voltage in microvolts
 * @bosa_bias_ua: laser bias current in microamps
 * @bosa_tx_power_nw: transmitted optical power in nanowatts
 * @bosa_rx_power_nw: received optical power in nanowatts
 * @bosa_alarms: hardware-specific optical alarm bitmap
 * @downstream_fec: enum omci_fec_status
 * @upstream_fec: enum omci_fec_status
 */
struct omci_telemetry {
	u32 valid;
	s32 bosa_temperature_mc;
	u32 bosa_voltage_uv;
	u32 bosa_bias_ua;
	u32 bosa_tx_power_nw;
	u32 bosa_rx_power_nw;
	u32 bosa_alarms;
	u8 downstream_fec;
	u8 upstream_fec;
};

/**
 * struct omci_ani_topology - pre-existing upstream traffic resources
 * @tcont_base: first T-CONT managed entity ID
 * @scheduler_base: first Traffic Scheduler managed entity ID
 * @queue_base: first Priority Queue managed entity ID
 * @maximum_queue_size: maximum queue size reported in 2048-byte blocks
 * @allocated_queue_size: initial allocated queue size in 2048-byte blocks
 * @tcont_count: number of upstream T-CONT resources
 * @queues_per_tcont: number of upstream priority queues per T-CONT
 * @queue_config_option: Priority Queue configuration option
 * @scheduler_policy: initial Traffic Scheduler policy
 *
 * The OMCI agent uses this description to seed complete Priority Queue and
 * Traffic Scheduler managed entities before the first MIB upload. Entity IDs
 * are allocated contiguously. Queue entity IDs are grouped by T-CONT.
 */
/**
 * struct omci_service_config - normalized upstream OMCI service
 * @cookie: stable identifier used to replace or delete one service rule
 * @uni_entity_id: normalized PPTP Ethernet UNI or VEIP entity
 * @gem_ctp_entity_id: GEM port network CTP managed entity
 * @gem_port_id: GEM port identifier programmed in the GPON MAC
 * @tcont_entity_id: T-CONT managed entity associated with the GEM port
 * @vlan_id: VLAN identifier used for upstream classification
 * @pcp: IEEE 802.1p priority used for classification and queue selection
 * @queue: hardware upstream queue
 * @direction: enum omci_gem_port_direction
 * @vlan_treatment: raw class 171 treatment words for the backend
 * @multicast_ani_entity_id: Dasan WAN bridge port used as multicast ANI
 * @vlan_valid: @vlan_id participates in classification
 * @pcp_valid: @pcp participates in classification
 * @vlan_treatment_valid: @vlan_treatment requires backend processing
 * @multicast_ani_valid: @multicast_ani_entity_id was profile-resolved
 * @multicast: service represents a multicast GEM path
 * @default_service: fallback service when no VLAN/PCP rule matches
 */
struct omci_service_config {
	u32 cookie;
	u16 uni_entity_id;
	u16 gem_ctp_entity_id;
	u16 gem_port_id;
	u16 tcont_entity_id;
	u16 vlan_id;
	u8 pcp;
	u8 queue;
	u8 direction;
	u8 vlan_treatment[8];
	u16 multicast_ani_entity_id;
	bool vlan_valid;
	bool pcp_valid;
	bool vlan_treatment_valid;
	bool multicast_ani_valid;
	bool multicast;
	bool default_service;
};

struct omci_ani_topology {
	u16 tcont_base;
	u16 scheduler_base;
	u16 queue_base;
	u16 maximum_queue_size;
	u16 allocated_queue_size;
	u8 tcont_count;
	u8 queues_per_tcont;
	u8 queue_config_option;
	u8 scheduler_policy;
};

/**
 * struct omci_device_ops - hardware transport and provisioning operations
 * @start: acquire and start the OMCI transport independently of netdev state
 * @stop: stop and release the OMCI transport
 * @xmit: transmit an OMCI PDU; consumes @skb only on success
 * @get_ani_topology: describe pre-existing ANI scheduling resources
 * @set_tcont: configure a T-CONT mapping
 * @set_gem_port: configure a GEM port
 * @set_uni: enable or disable a UNI
 * @replace_service: atomically add or replace a normalized upstream service
 * @delete_service: remove a normalized upstream service by cookie
 * @get_telemetry: refresh PON FEC and optical telemetry
 * @set_olt_profile: apply a resolved OLT interoperability profile
 * @set_operational: report whether the in-kernel OMCI agent is operational
 * @config_changed: report a normalized runtime OMCI configuration change
 */
struct omci_device_ops {
	int (*start)(struct omci_device *odev);
	void (*stop)(struct omci_device *odev);
	int (*xmit)(struct omci_device *odev, struct sk_buff *skb,
		    u16 gem_port_id);
	int (*get_ani_topology)(struct omci_device *odev,
				struct omci_ani_topology *topology);
	int (*set_tcont)(struct omci_device *odev, u16 entity_id,
			 u16 alloc_id, bool valid);
	int (*set_gem_port)(struct omci_device *odev, u16 entity_id,
			    u16 gem_port_id, u16 tcont_entity_id,
			    u8 direction, bool valid, bool encrypted);
	int (*set_uni)(struct omci_device *odev, u16 entity_id, bool enable);
	int (*replace_service)(struct omci_device *odev,
			       const struct omci_service_config *service);
	int (*delete_service)(struct omci_device *odev, u32 cookie);
	int (*get_telemetry)(struct omci_device *odev,
			     struct omci_telemetry *telemetry);
	int (*set_olt_profile)(struct omci_device *odev,
			       const struct omci_olt_profile_state *state);
	void (*set_operational)(struct omci_device *odev, bool operational);
	void (*config_changed)(struct omci_device *odev, u16 key,
			       const struct omci_identity *identity);
};

struct omci_device *
omci_device_register(struct device *parent, u32 ifindex, u32 capabilities,
		     const struct omci_device_ops *ops, void *priv);
void omci_device_unregister(struct omci_device *odev);
int omci_device_start(struct omci_device *odev);
void omci_device_stop(struct omci_device *odev);

void *omci_device_priv(const struct omci_device *odev);
u32 omci_device_id(const struct omci_device *odev);
const char *omci_olt_profile_name(u8 profile);

int omci_identity_load(struct device *dev, struct omci_identity *identity);
void gpon_random_serial_number(const u8 vendor[4], struct omci_identity *identity);
void omci_device_set_identity_info(struct omci_device *odev,
				   const struct omci_identity *identity);
void omci_device_set_identity(struct omci_device *odev,
			      const u8 serial_number[8],
			      const u8 password[10]);
void omci_device_set_onu_id(struct omci_device *odev, u16 onu_id);
void omci_device_set_channel(struct omci_device *odev, u16 gem_port_id,
			     bool valid);
void omci_device_set_state(struct omci_device *odev, u8 state);
void omci_device_reset_session(struct omci_device *odev);
int omci_device_set_dying_gasp_enabled(struct omci_device *odev,
				       bool enabled, u8 source);
int omci_device_send_dying_gasp(struct omci_device *odev);
void omci_device_receive(struct omci_device *odev, struct sk_buff *skb,
			 u16 gem_port_id, u32 flags);

#endif /* _NET_OMCI_H */
