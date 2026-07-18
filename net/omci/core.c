// SPDX-License-Identifier: GPL-2.0-only
/*
 * Generic ONU Management and Control Interface core
 *
 * The core owns the in-kernel baseline OMCI agent, the operational MIB and
 * an optional Generic Netlink observer and administration endpoint.
 */

#include <linux/atomic.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/netlink.h>
#include <linux/notifier.h>
#include <linux/slab.h>
#include <linux/unaligned.h>
#include <net/genetlink.h>
#include <net/netlink.h>

#include "internal.h"

static LIST_HEAD(omci_devices);
static DEFINE_MUTEX(omci_devices_lock);
static atomic_t omci_next_id = ATOMIC_INIT(0);
static struct genl_family omci_genl_family;

static const struct nla_policy omci_policy[OMCI_ATTR_MAX + 1] = {
	[OMCI_ATTR_DEV_ID] = { .type = NLA_U32 },
	[OMCI_ATTR_PDU] = {
		.type = NLA_BINARY,
		.len = OMCI_MAX_PDU_LEN,
	},
	[OMCI_ATTR_AGENT_ENABLED] = { .type = NLA_U8 },
	[OMCI_ATTR_AGENT_PERMISSIVE] = { .type = NLA_U8 },
	[OMCI_ATTR_CLASS_ID] = { .type = NLA_U16 },
	[OMCI_ATTR_ENTITY_ID] = { .type = NLA_U16 },
	[OMCI_ATTR_ATTR_MASK] = { .type = NLA_U16 },
	[OMCI_ATTR_ATTR_DATA] = {
		.type = NLA_BINARY,
		.len = OMCI_MAX_ATTR_DATA,
	},
	[OMCI_ATTR_ORIGIN] = { .type = NLA_U8 },
	[OMCI_ATTR_INDEX] = { .type = NLA_U32 },
	[OMCI_ATTR_CONFIG_KEY] = { .type = NLA_U16 },
	[OMCI_ATTR_CONFIG_VALUE] = {
		.type = NLA_BINARY,
		.len = OMCI_MAX_CONFIG_VALUE,
	},
	[OMCI_ATTR_VLAN_TAGGING_FILTER] = { .type = NLA_NESTED },
	[OMCI_ATTR_EXTENDED_VLAN] = { .type = NLA_NESTED },
};

static struct omci_device *omci_find_locked(u32 id)
{
	struct omci_device *odev;

	list_for_each_entry(odev, &omci_devices, list)
		if (odev->id == id)
			return odev;

	return NULL;
}

static struct omci_device *omci_get_from_info(struct genl_info *info)
{
	u32 id = 0;

	if (info->attrs[OMCI_ATTR_DEV_ID])
		id = nla_get_u32(info->attrs[OMCI_ATTR_DEV_ID]);

	return omci_find_locked(id);
}

static int omci_put_telemetry(struct sk_buff *msg, struct omci_device *odev)
{
	struct omci_telemetry telemetry = {};

	if (odev->ops->get_telemetry)
		odev->ops->get_telemetry(odev, &telemetry);

	if (nla_put_u32(msg, OMCI_ATTR_TELEMETRY_VALID, telemetry.valid))
		return -EMSGSIZE;

	if ((telemetry.valid & OMCI_TELEMETRY_F_FEC_DOWNSTREAM) &&
	    nla_put_u8(msg, OMCI_ATTR_FEC_DOWNSTREAM,
		       telemetry.downstream_fec))
		return -EMSGSIZE;
	if ((telemetry.valid & OMCI_TELEMETRY_F_FEC_UPSTREAM) &&
	    nla_put_u8(msg, OMCI_ATTR_FEC_UPSTREAM, telemetry.upstream_fec))
		return -EMSGSIZE;
	if ((telemetry.valid & OMCI_TELEMETRY_F_BOSA_TEMPERATURE) &&
	    nla_put_s32(msg, OMCI_ATTR_BOSA_TEMPERATURE_MC,
			telemetry.bosa_temperature_mc))
		return -EMSGSIZE;
	if ((telemetry.valid & OMCI_TELEMETRY_F_BOSA_VOLTAGE) &&
	    nla_put_u32(msg, OMCI_ATTR_BOSA_VOLTAGE_UV,
			telemetry.bosa_voltage_uv))
		return -EMSGSIZE;
	if ((telemetry.valid & OMCI_TELEMETRY_F_BOSA_BIAS) &&
	    nla_put_u32(msg, OMCI_ATTR_BOSA_BIAS_UA, telemetry.bosa_bias_ua))
		return -EMSGSIZE;
	if ((telemetry.valid & OMCI_TELEMETRY_F_BOSA_TX_POWER) &&
	    nla_put_u32(msg, OMCI_ATTR_BOSA_TX_POWER_NW,
			telemetry.bosa_tx_power_nw))
		return -EMSGSIZE;
	if ((telemetry.valid & OMCI_TELEMETRY_F_BOSA_RX_POWER) &&
	    nla_put_u32(msg, OMCI_ATTR_BOSA_RX_POWER_NW,
			telemetry.bosa_rx_power_nw))
		return -EMSGSIZE;
	if ((telemetry.valid & OMCI_TELEMETRY_F_BOSA_ALARMS) &&
	    nla_put_u32(msg, OMCI_ATTR_BOSA_ALARMS, telemetry.bosa_alarms))
		return -EMSGSIZE;

	return 0;
}

static int omci_put_status(struct sk_buff *msg, struct omci_device *odev)
{
	u32 owner_portid;
	u32 flags = 0;
	u16 onu_id;
	u16 gem_port_id;
	u8 state;

	mutex_lock(&odev->owner_lock);
	owner_portid = odev->owner_portid;
	mutex_unlock(&odev->owner_lock);

	spin_lock_bh(&odev->state_lock);
	onu_id = odev->onu_id;
	gem_port_id = odev->gem_port_id;
	state = odev->state;
	if (odev->channel_up)
		flags |= OMCI_F_CHANNEL_UP;
	spin_unlock_bh(&odev->state_lock);

	if (nla_put_u32(msg, OMCI_ATTR_DEV_ID, odev->id) ||
	    nla_put_u32(msg, OMCI_ATTR_IFINDEX, odev->ifindex) ||
	    nla_put_u16(msg, OMCI_ATTR_ONU_ID, onu_id) ||
	    nla_put_u16(msg, OMCI_ATTR_GEM_PORT_ID, gem_port_id) ||
	    nla_put_u8(msg, OMCI_ATTR_STATE, state) ||
	    nla_put_u32(msg, OMCI_ATTR_FLAGS, flags) ||
	    nla_put_u32(msg, OMCI_ATTR_CAPABILITIES, odev->capabilities) ||
	    nla_put_u32(msg, OMCI_ATTR_OWNER_PORTID, owner_portid) ||
	    nla_put_u64_64bit(msg, OMCI_ATTR_RX_PACKETS,
			      atomic64_read(&odev->rx_packets), OMCI_ATTR_PAD) ||
	    nla_put_u64_64bit(msg, OMCI_ATTR_RX_BYTES,
			      atomic64_read(&odev->rx_bytes), OMCI_ATTR_PAD) ||
	    nla_put_u64_64bit(msg, OMCI_ATTR_RX_DROPPED,
			      atomic64_read(&odev->rx_dropped), OMCI_ATTR_PAD) ||
	    nla_put_u64_64bit(msg, OMCI_ATTR_TX_PACKETS,
			      atomic64_read(&odev->tx_packets), OMCI_ATTR_PAD) ||
	    nla_put_u64_64bit(msg, OMCI_ATTR_TX_BYTES,
			      atomic64_read(&odev->tx_bytes), OMCI_ATTR_PAD) ||
	    nla_put_u64_64bit(msg, OMCI_ATTR_TX_ERRORS,
			      atomic64_read(&odev->tx_errors), OMCI_ATTR_PAD))
		return -EMSGSIZE;

	if (omci_put_telemetry(msg, odev))
		return -EMSGSIZE;

	return omci_agent_put_status(msg, odev);
}

static int omci_cmd_get(struct sk_buff *skb, struct genl_info *info)
{
	struct omci_device *odev;
	struct sk_buff *msg;
	void *hdr;
	int ret;

	mutex_lock(&omci_devices_lock);
	odev = omci_get_from_info(info);
	if (!odev) {
		ret = -ENODEV;
		goto out_unlock;
	}

	msg = genlmsg_new(NLMSG_DEFAULT_SIZE, GFP_KERNEL);
	if (!msg) {
		ret = -ENOMEM;
		goto out_unlock;
	}

	hdr = genlmsg_put_reply(msg, info, &omci_genl_family, 0,
				OMCI_CMD_GET);
	if (!hdr) {
		ret = -EMSGSIZE;
		goto free_msg;
	}

	ret = omci_put_status(msg, odev);
	if (ret)
		goto cancel_msg;

	genlmsg_end(msg, hdr);
	mutex_unlock(&omci_devices_lock);
	return genlmsg_reply(msg, info);

cancel_msg:
	genlmsg_cancel(msg, hdr);
free_msg:
	nlmsg_free(msg);
out_unlock:
	mutex_unlock(&omci_devices_lock);
	return ret;
}

static int omci_cmd_bind(struct sk_buff *skb, struct genl_info *info)
{
	struct net *net = genl_info_net(info);
	struct omci_device *odev;
	int ret = 0;

	mutex_lock(&omci_devices_lock);
	odev = omci_get_from_info(info);
	if (!odev) {
		ret = -ENODEV;
		goto out_unlock_devices;
	}

	mutex_lock(&odev->owner_lock);
	if (odev->owner_portid &&
	    (odev->owner_portid != info->snd_portid ||
	     odev->owner_net != net)) {
		ret = -EBUSY;
		goto out_unlock_owner;
	}

	if (!odev->owner_portid) {
		odev->owner_net = get_net(net);
		odev->owner_portid = info->snd_portid;
		schedule_work(&odev->rx_work);
	}

out_unlock_owner:
	mutex_unlock(&odev->owner_lock);
out_unlock_devices:
	mutex_unlock(&omci_devices_lock);
	return ret;
}

static int omci_cmd_unbind(struct sk_buff *skb, struct genl_info *info)
{
	struct omci_device *odev;
	struct net *owner_net = NULL;
	int ret = 0;

	mutex_lock(&omci_devices_lock);
	odev = omci_get_from_info(info);
	if (!odev) {
		ret = -ENODEV;
		goto out_unlock_devices;
	}

	mutex_lock(&odev->owner_lock);
	if (odev->owner_portid != info->snd_portid ||
	    odev->owner_net != genl_info_net(info)) {
		ret = -EPERM;
		goto out_unlock_owner;
	}

	owner_net = odev->owner_net;
	odev->owner_net = NULL;
	odev->owner_portid = 0;

out_unlock_owner:
	mutex_unlock(&odev->owner_lock);
out_unlock_devices:
	mutex_unlock(&omci_devices_lock);
	if (owner_net)
		put_net(owner_net);
	return ret;
}

int omci_validate_tx(struct omci_device *odev, struct sk_buff *skb)
{
	const u8 *data = skb->data;
	u16 content_len;
	u16 pdu_len;

	if (skb->len < 4)
		return -EMSGSIZE;

	switch (data[3]) {
	case OMCI_BASELINE_DEV_ID:
		if (skb->len != OMCI_BASELINE_LEN_NO_MIC &&
		    skb->len != OMCI_BASELINE_LEN)
			return -EMSGSIZE;
		if ((odev->capabilities & OMCI_CAP_HW_MIC) &&
		    skb->len == OMCI_BASELINE_LEN)
			skb_trim(skb, OMCI_BASELINE_LEN_NO_MIC);
		break;
	case OMCI_EXTENDED_DEV_ID:
		if (skb->len < OMCI_EXTENDED_HEADER_LEN)
			return -EMSGSIZE;
		content_len = get_unaligned_be16(data + 8);
		pdu_len = OMCI_EXTENDED_HEADER_LEN + content_len;
		if (pdu_len > OMCI_MAX_PDU_LEN || skb->len < pdu_len)
			return -EMSGSIZE;
		if (skb->len > pdu_len) {
			if (skb->len - pdu_len != OMCI_MIC_LEN)
				return -EMSGSIZE;
			if (odev->capabilities & OMCI_CAP_HW_MIC)
				skb_trim(skb, pdu_len);
		}
		break;
	default:
		return -EPROTO;
	}

	return 0;
}

int omci_device_xmit(struct omci_device *odev, const void *data, size_t len)
{
	struct sk_buff *tx_skb;
	u16 gem_port_id;
	bool channel_up;
	u32 tx_len;
	int ret;

	spin_lock_bh(&odev->state_lock);
	gem_port_id = odev->gem_port_id;
	channel_up = odev->channel_up;
	spin_unlock_bh(&odev->state_lock);
	if (!channel_up)
		return -ENOLINK;

	tx_skb = alloc_skb(len, GFP_KERNEL);
	if (!tx_skb)
		return -ENOMEM;
	skb_put_data(tx_skb, data, len);
	ret = omci_validate_tx(odev, tx_skb);
	if (ret)
		goto free_skb;

	tx_len = tx_skb->len;
	ret = odev->ops->xmit(odev, tx_skb, gem_port_id);
	if (ret)
		goto free_skb;

	atomic64_inc(&odev->tx_packets);
	atomic64_add(tx_len, &odev->tx_bytes);
	return 0;

free_skb:
	atomic64_inc(&odev->tx_errors);
	dev_kfree_skb_any(tx_skb);
	return ret;
}

static int omci_cmd_tx(struct sk_buff *skb, struct genl_info *info)
{
	struct nlattr *pdu_attr = info->attrs[OMCI_ATTR_PDU];
	struct omci_device *odev;
	struct sk_buff *tx_skb;
	u16 gem_port_id;
	bool channel_up;
	u32 tx_len;
	int ret;

	if (!pdu_attr)
		return -EINVAL;

	mutex_lock(&omci_devices_lock);
	odev = omci_get_from_info(info);
	if (!odev) {
		ret = -ENODEV;
		goto out_unlock_devices;
	}

	mutex_lock(&odev->owner_lock);
	if (odev->owner_portid != info->snd_portid ||
	    odev->owner_net != genl_info_net(info)) {
		ret = -EPERM;
		goto out_unlock_owner;
	}
	mutex_unlock(&odev->owner_lock);

	spin_lock_bh(&odev->state_lock);
	gem_port_id = odev->gem_port_id;
	channel_up = odev->channel_up;
	spin_unlock_bh(&odev->state_lock);
	if (!channel_up) {
		ret = -ENOLINK;
		goto out_unlock_devices;
	}

	tx_skb = alloc_skb(nla_len(pdu_attr), GFP_KERNEL);
	if (!tx_skb) {
		ret = -ENOMEM;
		goto out_unlock_devices;
	}

	skb_put_data(tx_skb, nla_data(pdu_attr), nla_len(pdu_attr));
	ret = omci_validate_tx(odev, tx_skb);
	if (ret)
		goto free_tx_skb;

	tx_len = tx_skb->len;
	ret = odev->ops->xmit(odev, tx_skb, gem_port_id);
	if (ret)
		goto free_tx_skb;

	atomic64_inc(&odev->tx_packets);
	atomic64_add(tx_len, &odev->tx_bytes);
	mutex_unlock(&omci_devices_lock);
	return 0;

free_tx_skb:
	atomic64_inc(&odev->tx_errors);
	dev_kfree_skb_any(tx_skb);
	goto out_unlock_devices;

out_unlock_owner:
	mutex_unlock(&odev->owner_lock);
out_unlock_devices:
	mutex_unlock(&omci_devices_lock);
	return ret;
}

static int omci_put_vlan_filter(struct sk_buff *msg,
				const struct omci_mib_object *object)
{
	const struct omci_vlan_tagging_filter *filter = &object->vlan_filter;
	struct nlattr *entries;
	struct nlattr *nested;
	unsigned int i;

	if (object->class_id != OMCI_CLASS_VLAN_TAGGING_FILTER_DATA || !filter->valid)
		return 0;

	nested = nla_nest_start(msg, OMCI_ATTR_VLAN_TAGGING_FILTER);
	if (!nested)
		return -EMSGSIZE;
	if (nla_put_u8(msg, OMCI_VLAN_FILTER_ATTR_FORWARD_OPERATION,
		       filter->forward_operation) ||
	    nla_put_u8(msg, OMCI_VLAN_FILTER_ATTR_NUMBER_OF_ENTRIES,
		       filter->num_entries))
		goto cancel;

	entries = nla_nest_start(msg, OMCI_VLAN_FILTER_ATTR_ENTRIES);
	if (!entries)
		goto cancel;
	for (i = 0; i < filter->num_entries; i++) {
		const struct omci_vlan_filter_entry *entry = &filter->entries[i];
		struct nlattr *item;

		item = nla_nest_start(msg, i + 1);
		if (!item)
			goto cancel_entries;
		if (nla_put_u8(msg, OMCI_VLAN_FILTER_ENTRY_ATTR_INDEX, i) ||
		    nla_put_u16(msg, OMCI_VLAN_FILTER_ENTRY_ATTR_TCI,
				entry->tci) ||
		    nla_put_u8(msg, OMCI_VLAN_FILTER_ENTRY_ATTR_PBIT,
			       entry->pbit) ||
		    nla_put_u8(msg, OMCI_VLAN_FILTER_ENTRY_ATTR_DEI,
			       entry->dei) ||
		    nla_put_u16(msg, OMCI_VLAN_FILTER_ENTRY_ATTR_VID,
				entry->vid)) {
			nla_nest_cancel(msg, item);
			goto cancel_entries;
		}
		nla_nest_end(msg, item);
	}
	nla_nest_end(msg, entries);
	nla_nest_end(msg, nested);

	return 0;

cancel_entries:
	nla_nest_cancel(msg, entries);
cancel:
	nla_nest_cancel(msg, nested);
	return -EMSGSIZE;
}

static int omci_put_ext_vlan_rule(struct sk_buff *msg,
				  const struct omci_extended_vlan_rule *rule,
				  unsigned int index)
{
	struct nlattr *nested;

	nested = nla_nest_start(msg, index + 1);
	if (!nested)
		return -EMSGSIZE;
	if (nla_put_u8(msg, OMCI_EXT_VLAN_RULE_ATTR_INDEX, index) ||
	    nla_put(msg, OMCI_EXT_VLAN_RULE_ATTR_RAW, sizeof(rule->raw),
		    rule->raw) ||
	    nla_put_u8(msg, OMCI_EXT_VLAN_RULE_ATTR_DELETE, rule->delete) ||
	    nla_put_u8(msg, OMCI_EXT_VLAN_RULE_ATTR_FILTER_OUTER_PBIT,
		       rule->filter_outer_pbit) ||
	    nla_put_u16(msg, OMCI_EXT_VLAN_RULE_ATTR_FILTER_OUTER_VID, rule->filter_outer_vid) ||
	    nla_put_u8(msg, OMCI_EXT_VLAN_RULE_ATTR_FILTER_OUTER_TPID_DEI,
		       rule->filter_outer_tpid_dei) ||
	    nla_put_u8(msg, OMCI_EXT_VLAN_RULE_ATTR_FILTER_INNER_PBIT,
		       rule->filter_inner_pbit) ||
	    nla_put_u16(msg, OMCI_EXT_VLAN_RULE_ATTR_FILTER_INNER_VID, rule->filter_inner_vid) ||
	    nla_put_u8(msg, OMCI_EXT_VLAN_RULE_ATTR_FILTER_INNER_TPID_DEI,
		       rule->filter_inner_tpid_dei) ||
	    nla_put_u8(msg, OMCI_EXT_VLAN_RULE_ATTR_FILTER_ETHERTYPE,
		       rule->filter_ethertype) ||
	    nla_put_u8(msg, OMCI_EXT_VLAN_RULE_ATTR_TAGS_TO_REMOVE,
		       rule->tags_to_remove) ||
	    nla_put_u8(msg, OMCI_EXT_VLAN_RULE_ATTR_TREAT_OUTER_PBIT,
		       rule->treat_outer_pbit) ||
	    nla_put_u16(msg, OMCI_EXT_VLAN_RULE_ATTR_TREAT_OUTER_VID, rule->treat_outer_vid) ||
	    nla_put_u8(msg, OMCI_EXT_VLAN_RULE_ATTR_TREAT_OUTER_TPID_DEI,
		       rule->treat_outer_tpid_dei) ||
	    nla_put_u8(msg, OMCI_EXT_VLAN_RULE_ATTR_TREAT_INNER_PBIT,
		       rule->treat_inner_pbit) ||
	    nla_put_u16(msg, OMCI_EXT_VLAN_RULE_ATTR_TREAT_INNER_VID, rule->treat_inner_vid) ||
	    nla_put_u8(msg, OMCI_EXT_VLAN_RULE_ATTR_TREAT_INNER_TPID_DEI,
		       rule->treat_inner_tpid_dei)) {
		nla_nest_cancel(msg, nested);
		return -EMSGSIZE;
	}
	nla_nest_end(msg, nested);

	return 0;
}

static int omci_put_extended_vlan(struct sk_buff *msg,
				  const struct omci_mib_object *object)
{
	const struct omci_extended_vlan *vlan = &object->extended_vlan;
	struct nlattr *rules;
	struct nlattr *nested;
	unsigned int i;

	if (object->class_id != OMCI_CLASS_EXTENDED_VLAN || !vlan->valid)
		return 0;

	nested = nla_nest_start(msg, OMCI_ATTR_EXTENDED_VLAN);
	if (!nested)
		return -EMSGSIZE;
	if (nla_put_u8(msg, OMCI_EXT_VLAN_ATTR_ASSOCIATION_TYPE,
		       vlan->association_type) ||
	    nla_put_u16(msg, OMCI_EXT_VLAN_ATTR_MAX_TABLE_SIZE, vlan->max_table_size) ||
	    nla_put_u16(msg, OMCI_EXT_VLAN_ATTR_INPUT_TPID, vlan->input_tpid) ||
	    nla_put_u16(msg, OMCI_EXT_VLAN_ATTR_OUTPUT_TPID, vlan->output_tpid) ||
	    nla_put_u8(msg, OMCI_EXT_VLAN_ATTR_DOWNSTREAM_MODE,
		       vlan->downstream_mode) ||
	    nla_put_u16(msg, OMCI_EXT_VLAN_ATTR_ASSOCIATED_ME, vlan->associated_me) ||
	    nla_put(msg, OMCI_EXT_VLAN_ATTR_DSCP_TO_PBIT,
		    sizeof(vlan->dscp_to_pbit), vlan->dscp_to_pbit))
		goto cancel;

	rules = nla_nest_start(msg, OMCI_EXT_VLAN_ATTR_RULES);
	if (!rules)
		goto cancel;
	for (i = 0; i < vlan->rule_count; i++)
		if (omci_put_ext_vlan_rule(msg, &vlan->rules[i], i))
			goto cancel_rules;
	nla_nest_end(msg, rules);
	nla_nest_end(msg, nested);

	return 0;

cancel_rules:
	nla_nest_cancel(msg, rules);
cancel:
	nla_nest_cancel(msg, nested);
	return -EMSGSIZE;
}

static int omci_put_mib_object(struct sk_buff *msg,
			       const struct omci_mib_object *object,
			       u32 next_index, const char *name)
{
	if (nla_put_u16(msg, OMCI_ATTR_CLASS_ID, object->class_id) ||
	    nla_put_u16(msg, OMCI_ATTR_ENTITY_ID, object->entity_id) ||
	    nla_put_u16(msg, OMCI_ATTR_ATTR_MASK, object->attr_mask) ||
	    nla_put(msg, OMCI_ATTR_ATTR_DATA, sizeof(object->data),
		    object->data) ||
	    nla_put_u8(msg, OMCI_ATTR_ORIGIN, object->origin) ||
	    nla_put_u32(msg, OMCI_ATTR_INDEX, next_index) ||
	    (name && nla_put_string(msg, OMCI_ATTR_NAME, name)))
		return -EMSGSIZE;
	if (omci_put_vlan_filter(msg, object) ||
	    omci_put_extended_vlan(msg, object))
		return -EMSGSIZE;
	return 0;
}

static int omci_reply_mib(struct genl_info *info, u8 command,
			  struct omci_device *odev,
			  const struct omci_mib_object *object,
			  u32 next_index, const char *name)
{
	struct sk_buff *msg;
	void *hdr;
	int ret;

	msg = genlmsg_new(NLMSG_GOODSIZE, GFP_KERNEL);
	if (!msg)
		return -ENOMEM;
	hdr = genlmsg_put_reply(msg, info, &omci_genl_family, 0, command);
	if (!hdr) {
		nlmsg_free(msg);
		return -EMSGSIZE;
	}
	if (nla_put_u32(msg, OMCI_ATTR_DEV_ID, odev->id)) {
		ret = -EMSGSIZE;
		goto cancel;
	}
	ret = omci_put_mib_object(msg, object, next_index, name);
	if (ret)
		goto cancel;
	genlmsg_end(msg, hdr);
	return genlmsg_reply(msg, info);

cancel:
	genlmsg_cancel(msg, hdr);
	nlmsg_free(msg);
	return ret;
}

static int omci_cmd_agent_set(struct sk_buff *skb, struct genl_info *info)
{
	struct omci_device *odev;
	u8 value;
	int ret = 0;

	mutex_lock(&omci_devices_lock);
	odev = omci_get_from_info(info);
	if (!odev) {
		ret = -ENODEV;
		goto out;
	}
	if (info->attrs[OMCI_ATTR_AGENT_ENABLED]) {
		value = nla_get_u8(info->attrs[OMCI_ATTR_AGENT_ENABLED]);
		ret = omci_agent_config_set(odev, OMCI_CONFIG_AGENT_ENABLED,
					    &value, sizeof(value));
		if (ret)
			goto out;
	}
	if (info->attrs[OMCI_ATTR_AGENT_PERMISSIVE]) {
		value = nla_get_u8(info->attrs[OMCI_ATTR_AGENT_PERMISSIVE]);
		ret = omci_agent_config_set(odev, OMCI_CONFIG_AGENT_PERMISSIVE,
					    &value, sizeof(value));
	}
	if (!ret)
		omci_device_notify(odev, OMCI_EVENT_CONFIG_CHANGE);
out:
	mutex_unlock(&omci_devices_lock);
	return ret;
}

static int omci_cmd_config_get(struct sk_buff *skb, struct genl_info *info)
{
	u8 value[OMCI_MAX_CONFIG_VALUE];
	struct omci_device *odev;
	struct sk_buff *msg;
	size_t len = sizeof(value);
	u16 key;
	void *hdr;
	int ret;

	if (!info->attrs[OMCI_ATTR_CONFIG_KEY])
		return -EINVAL;
	key = nla_get_u16(info->attrs[OMCI_ATTR_CONFIG_KEY]);

	mutex_lock(&omci_devices_lock);
	odev = omci_get_from_info(info);
	if (!odev) {
		ret = -ENODEV;
		goto out_unlock;
	}
	ret = omci_agent_config_get(odev, key, value, &len);
	if (ret)
		goto out_unlock;

	msg = genlmsg_new(NLMSG_DEFAULT_SIZE + len, GFP_KERNEL);
	if (!msg) {
		ret = -ENOMEM;
		goto out_unlock;
	}
	hdr = genlmsg_put_reply(msg, info, &omci_genl_family, 0,
				OMCI_CMD_CONFIG_GET);
	if (!hdr || nla_put_u32(msg, OMCI_ATTR_DEV_ID, odev->id) ||
	    nla_put_u16(msg, OMCI_ATTR_CONFIG_KEY, key) ||
	    nla_put(msg, OMCI_ATTR_CONFIG_VALUE, len, value)) {
		nlmsg_free(msg);
		ret = -EMSGSIZE;
		goto out_unlock;
	}
	genlmsg_end(msg, hdr);
	mutex_unlock(&omci_devices_lock);
	return genlmsg_reply(msg, info);

out_unlock:
	mutex_unlock(&omci_devices_lock);
	return ret;
}

static int omci_cmd_config_set(struct sk_buff *skb, struct genl_info *info)
{
	struct nlattr *value = info->attrs[OMCI_ATTR_CONFIG_VALUE];
	struct omci_device *odev;
	u16 key;
	int ret;

	if (!info->attrs[OMCI_ATTR_CONFIG_KEY] || !value)
		return -EINVAL;
	key = nla_get_u16(info->attrs[OMCI_ATTR_CONFIG_KEY]);

	mutex_lock(&omci_devices_lock);
	odev = omci_get_from_info(info);
	if (!odev) {
		ret = -ENODEV;
		goto out;
	}
	ret = omci_agent_config_set(odev, key, nla_data(value),
				    nla_len(value));
	if (!ret)
		omci_device_notify(odev, OMCI_EVENT_CONFIG_CHANGE);
out:
	mutex_unlock(&omci_devices_lock);
	return ret;
}

static int omci_cmd_mib_get(struct sk_buff *skb, struct genl_info *info)
{
	struct omci_mib_object object;
	struct omci_device *odev;
	u16 class_id;
	u16 entity_id;
	int ret;

	if (!info->attrs[OMCI_ATTR_CLASS_ID] ||
	    !info->attrs[OMCI_ATTR_ENTITY_ID])
		return -EINVAL;
	class_id = nla_get_u16(info->attrs[OMCI_ATTR_CLASS_ID]);
	entity_id = nla_get_u16(info->attrs[OMCI_ATTR_ENTITY_ID]);

	mutex_lock(&omci_devices_lock);
	odev = omci_get_from_info(info);
	if (!odev) {
		ret = -ENODEV;
		goto out;
	}
	ret = omci_agent_mib_get(odev, class_id, entity_id, &object);
	if (!ret)
		ret = omci_reply_mib(info, OMCI_CMD_MIB_GET, odev, &object,
				     0, omci_agent_class_name(class_id));
out:
	mutex_unlock(&omci_devices_lock);
	return ret;
}

static int omci_cmd_mib_set(struct sk_buff *skb, struct genl_info *info)
{
	struct nlattr *data = info->attrs[OMCI_ATTR_ATTR_DATA];
	struct omci_mib_object object = {
		.origin = OMCI_MIB_ORIGIN_LOCAL,
	};
	struct omci_device *odev;
	int ret;

	if (!info->attrs[OMCI_ATTR_CLASS_ID] ||
	    !info->attrs[OMCI_ATTR_ENTITY_ID])
		return -EINVAL;
	object.class_id = nla_get_u16(info->attrs[OMCI_ATTR_CLASS_ID]);
	object.entity_id = nla_get_u16(info->attrs[OMCI_ATTR_ENTITY_ID]);
	object.attr_mask = info->attrs[OMCI_ATTR_ATTR_MASK] ?
		nla_get_u16(info->attrs[OMCI_ATTR_ATTR_MASK]) : 0xffff;
	if (data)
		memcpy(object.data, nla_data(data),
		       min_t(size_t, nla_len(data), sizeof(object.data)));

	mutex_lock(&omci_devices_lock);
	odev = omci_get_from_info(info);
	if (!odev) {
		ret = -ENODEV;
		goto out;
	}
	ret = omci_agent_mib_set(odev, &object);
	if (!ret)
		omci_device_notify(odev, OMCI_EVENT_MIB_CHANGE);
out:
	mutex_unlock(&omci_devices_lock);
	return ret;
}

static int omci_cmd_mib_delete(struct sk_buff *skb, struct genl_info *info)
{
	struct omci_device *odev;
	u16 class_id;
	u16 entity_id;
	int ret;

	if (!info->attrs[OMCI_ATTR_CLASS_ID] ||
	    !info->attrs[OMCI_ATTR_ENTITY_ID])
		return -EINVAL;
	class_id = nla_get_u16(info->attrs[OMCI_ATTR_CLASS_ID]);
	entity_id = nla_get_u16(info->attrs[OMCI_ATTR_ENTITY_ID]);

	mutex_lock(&omci_devices_lock);
	odev = omci_get_from_info(info);
	if (!odev) {
		ret = -ENODEV;
		goto out;
	}
	ret = omci_agent_mib_delete(odev, class_id, entity_id);
	if (!ret)
		omci_device_notify(odev, OMCI_EVENT_MIB_CHANGE);
out:
	mutex_unlock(&omci_devices_lock);
	return ret;
}

static int omci_cmd_mib_reset(struct sk_buff *skb, struct genl_info *info)
{
	struct omci_device *odev;
	int ret = 0;

	mutex_lock(&omci_devices_lock);
	odev = omci_get_from_info(info);
	if (!odev) {
		ret = -ENODEV;
		goto out;
	}
	omci_agent_mib_reset(odev, true);
	omci_device_notify(odev, OMCI_EVENT_MIB_CHANGE);
out:
	mutex_unlock(&omci_devices_lock);
	return ret;
}

static int omci_cmd_mib_next(struct sk_buff *skb, struct genl_info *info)
{
	struct omci_mib_object object;
	struct omci_device *odev;
	const char *name = NULL;
	u32 index = 0;
	u32 next_index;
	int ret;

	if (info->attrs[OMCI_ATTR_INDEX])
		index = nla_get_u32(info->attrs[OMCI_ATTR_INDEX]);

	mutex_lock(&omci_devices_lock);
	odev = omci_get_from_info(info);
	if (!odev) {
		ret = -ENODEV;
		goto out;
	}
	ret = omci_agent_mib_next(odev, index, &object, &next_index, &name);
	if (!ret)
		ret = omci_reply_mib(info, OMCI_CMD_MIB_NEXT, odev, &object,
				     next_index, name);
out:
	mutex_unlock(&omci_devices_lock);
	return ret;
}

static const struct genl_ops omci_genl_ops[] = {
	{
		.cmd = OMCI_CMD_GET,
		.policy = omci_policy,
		.doit = omci_cmd_get,
	},
	{
		.cmd = OMCI_CMD_BIND,
		.flags = GENL_ADMIN_PERM,
		.policy = omci_policy,
		.doit = omci_cmd_bind,
	},
	{
		.cmd = OMCI_CMD_UNBIND,
		.flags = GENL_ADMIN_PERM,
		.policy = omci_policy,
		.doit = omci_cmd_unbind,
	},
	{
		.cmd = OMCI_CMD_TX,
		.flags = GENL_ADMIN_PERM,
		.policy = omci_policy,
		.doit = omci_cmd_tx,
	},
	{
		.cmd = OMCI_CMD_AGENT_SET,
		.flags = GENL_ADMIN_PERM,
		.policy = omci_policy,
		.doit = omci_cmd_agent_set,
	},
	{
		.cmd = OMCI_CMD_CONFIG_GET,
		.policy = omci_policy,
		.doit = omci_cmd_config_get,
	},
	{
		.cmd = OMCI_CMD_CONFIG_SET,
		.flags = GENL_ADMIN_PERM,
		.policy = omci_policy,
		.doit = omci_cmd_config_set,
	},
	{
		.cmd = OMCI_CMD_MIB_GET,
		.policy = omci_policy,
		.doit = omci_cmd_mib_get,
	},
	{
		.cmd = OMCI_CMD_MIB_SET,
		.flags = GENL_ADMIN_PERM,
		.policy = omci_policy,
		.doit = omci_cmd_mib_set,
	},
	{
		.cmd = OMCI_CMD_MIB_DELETE,
		.flags = GENL_ADMIN_PERM,
		.policy = omci_policy,
		.doit = omci_cmd_mib_delete,
	},
	{
		.cmd = OMCI_CMD_MIB_RESET,
		.flags = GENL_ADMIN_PERM,
		.policy = omci_policy,
		.doit = omci_cmd_mib_reset,
	},
	{
		.cmd = OMCI_CMD_MIB_NEXT,
		.policy = omci_policy,
		.doit = omci_cmd_mib_next,
	},
};

static struct genl_family omci_genl_family __ro_after_init = {
	.name = OMCI_GENL_NAME,
	.version = OMCI_GENL_VERSION,
	.maxattr = OMCI_ATTR_MAX,
	.module = THIS_MODULE,
	.ops = omci_genl_ops,
	.n_ops = ARRAY_SIZE(omci_genl_ops),
};

static int omci_send(struct omci_device *odev, struct sk_buff *msg,
		     u32 portid, struct net *net)
{
	int ret;

	ret = genlmsg_unicast(net, msg, portid);
	if (ret == -ESRCH || ret == -ECONNREFUSED) {
		struct net *owner_net = NULL;

		mutex_lock(&odev->owner_lock);
		if (odev->owner_portid == portid && odev->owner_net == net) {
			owner_net = odev->owner_net;
			odev->owner_net = NULL;
			odev->owner_portid = 0;
		}
		mutex_unlock(&odev->owner_lock);
		if (owner_net)
			put_net(owner_net);
	}

	return ret;
}

static void omci_rx_work(struct work_struct *work)
{
	struct omci_device *odev;
	struct sk_buff *skb;

	odev = container_of(work, struct omci_device, rx_work);
	while ((skb = skb_dequeue(&odev->rx_queue))) {
		struct omci_skb_cb *cb = OMCI_SKB_CB(skb);
		struct net *net = NULL;
		struct sk_buff *msg;
		u32 portid = 0;
		u16 onu_id;
		void *hdr;
		int ret;

		spin_lock_bh(&odev->state_lock);
		onu_id = odev->onu_id;
		if (cb->generation != odev->generation) {
			spin_unlock_bh(&odev->state_lock);
			atomic64_inc(&odev->rx_dropped);
			dev_kfree_skb_any(skb);
			continue;
		}
		spin_unlock_bh(&odev->state_lock);

		omci_agent_receive(odev, skb);

		mutex_lock(&odev->owner_lock);
		if (odev->owner_portid && odev->owner_net) {
			portid = odev->owner_portid;
			net = get_net(odev->owner_net);
		}
		mutex_unlock(&odev->owner_lock);

		if (!net) {
			dev_kfree_skb_any(skb);
			continue;
		}

		msg = genlmsg_new(NLMSG_DEFAULT_SIZE + skb->len, GFP_KERNEL);
		if (!msg) {
			atomic64_inc(&odev->rx_dropped);
			put_net(net);
			dev_kfree_skb_any(skb);
			continue;
		}

		hdr = genlmsg_put(msg, 0, 0, &omci_genl_family, 0,
				  OMCI_CMD_RX);
		if (!hdr ||
		    nla_put_u32(msg, OMCI_ATTR_DEV_ID, odev->id) ||
		    nla_put_u16(msg, OMCI_ATTR_ONU_ID, onu_id) ||
		    nla_put_u16(msg, OMCI_ATTR_GEM_PORT_ID, cb->gem_port_id) ||
		    nla_put_u32(msg, OMCI_ATTR_FLAGS, cb->flags) ||
		    nla_put_u64_64bit(msg, OMCI_ATTR_SEQUENCE, cb->sequence,
				      OMCI_ATTR_PAD) ||
		    nla_put(msg, OMCI_ATTR_PDU, skb->len, skb->data)) {
			nlmsg_free(msg);
			atomic64_inc(&odev->rx_dropped);
			put_net(net);
			dev_kfree_skb_any(skb);
			continue;
		}

		genlmsg_end(msg, hdr);
		ret = omci_send(odev, msg, portid, net);
		if (ret)
			atomic64_inc(&odev->rx_dropped);
		put_net(net);
		dev_kfree_skb_any(skb);
	}
}

void omci_device_notify(struct omci_device *odev, u8 event)
{
	struct net *net = NULL;
	struct sk_buff *msg;
	u32 portid = 0;
	void *hdr;

	mutex_lock(&odev->owner_lock);
	if (odev->owner_portid && odev->owner_net) {
		portid = odev->owner_portid;
		net = get_net(odev->owner_net);
	}
	mutex_unlock(&odev->owner_lock);
	if (!net)
		return;

	msg = genlmsg_new(NLMSG_DEFAULT_SIZE, GFP_KERNEL);
	if (!msg)
		goto out_put_net;

	hdr = genlmsg_put(msg, 0, 0, &omci_genl_family, 0,
			  OMCI_CMD_EVENT);
	if (!hdr || nla_put_u8(msg, OMCI_ATTR_EVENT, event) ||
	    omci_put_status(msg, odev)) {
		nlmsg_free(msg);
		goto out_put_net;
	}

	genlmsg_end(msg, hdr);
	omci_send(odev, msg, portid, net);

out_put_net:
	put_net(net);
}

static int omci_netlink_notify(struct notifier_block *nb, unsigned long state,
			       void *data)
{
	struct netlink_notify *notify = data;
	struct omci_device *odev;

	if (state != NETLINK_URELEASE || notify->protocol != NETLINK_GENERIC)
		return NOTIFY_DONE;

	mutex_lock(&omci_devices_lock);
	list_for_each_entry(odev, &omci_devices, list) {
		struct net *owner_net = NULL;

		mutex_lock(&odev->owner_lock);
		if (odev->owner_portid == notify->portid &&
		    odev->owner_net == notify->net) {
			owner_net = odev->owner_net;
			odev->owner_net = NULL;
			odev->owner_portid = 0;
		}
		mutex_unlock(&odev->owner_lock);
		if (owner_net)
			put_net(owner_net);
	}
	mutex_unlock(&omci_devices_lock);

	return NOTIFY_DONE;
}

static struct notifier_block omci_netlink_nb = {
	.notifier_call = omci_netlink_notify,
};

struct omci_device *
omci_device_register(struct device *parent, u32 ifindex, u32 capabilities,
		     const struct omci_device_ops *ops, void *priv)
{
	struct omci_device *odev;

	if (!parent || !ops || !ops->xmit)
		return ERR_PTR(-EINVAL);

	odev = kzalloc(sizeof(*odev), GFP_KERNEL);
	if (!odev)
		return ERR_PTR(-ENOMEM);

	INIT_LIST_HEAD(&odev->list);
	mutex_init(&odev->owner_lock);
	spin_lock_init(&odev->state_lock);
	skb_queue_head_init(&odev->rx_queue);
	INIT_WORK(&odev->rx_work, omci_rx_work);
	odev->parent = parent;
	odev->ifindex = ifindex;
	odev->capabilities = capabilities | OMCI_CAP_BASELINE_AGENT;
	odev->ops = ops;
	odev->priv = priv;
	odev->onu_id = 0xffff;
	odev->gem_port_id = 0xffff;
	odev->id = atomic_inc_return(&omci_next_id) - 1;

	if (omci_agent_init(odev)) {
		kfree(odev);
		return ERR_PTR(-ENOMEM);
	}

	mutex_lock(&omci_devices_lock);
	list_add_tail(&odev->list, &omci_devices);
	mutex_unlock(&omci_devices_lock);

	dev_info(parent, "registered OMCI agent device %u\n", odev->id);
	return odev;
}
EXPORT_SYMBOL_GPL(omci_device_register);

void omci_device_unregister(struct omci_device *odev)
{
	struct net *owner_net;

	if (!odev)
		return;

	mutex_lock(&omci_devices_lock);
	list_del_init(&odev->list);
	mutex_unlock(&omci_devices_lock);

	cancel_work_sync(&odev->rx_work);
	skb_queue_purge(&odev->rx_queue);

	mutex_lock(&odev->owner_lock);
	owner_net = odev->owner_net;
	odev->owner_net = NULL;
	odev->owner_portid = 0;
	mutex_unlock(&odev->owner_lock);
	if (owner_net)
		put_net(owner_net);
	omci_agent_cleanup(odev);
	kfree(odev);
}
EXPORT_SYMBOL_GPL(omci_device_unregister);

void *omci_device_priv(const struct omci_device *odev)
{
	return odev->priv;
}
EXPORT_SYMBOL_GPL(omci_device_priv);

u32 omci_device_id(const struct omci_device *odev)
{
	return odev->id;
}
EXPORT_SYMBOL_GPL(omci_device_id);

void omci_device_set_identity(struct omci_device *odev,
			      const u8 serial_number[8],
			      const u8 password[10])
{
	if (serial_number) {
		omci_agent_config_set(odev, OMCI_CONFIG_SERIAL_NUMBER,
				      serial_number, 8);
		omci_agent_config_set(odev, OMCI_CONFIG_VENDOR_ID,
				      serial_number, 4);
	}
	if (password)
		omci_agent_config_set(odev, OMCI_CONFIG_PASSWORD, password, 10);
}
EXPORT_SYMBOL_GPL(omci_device_set_identity);

void omci_device_set_onu_id(struct omci_device *odev, u16 onu_id)
{
	spin_lock_bh(&odev->state_lock);
	odev->onu_id = onu_id;
	spin_unlock_bh(&odev->state_lock);
}
EXPORT_SYMBOL_GPL(omci_device_set_onu_id);

void omci_device_set_channel(struct omci_device *odev, u16 gem_port_id,
			     bool valid)
{
	u8 event = valid ? OMCI_EVENT_CHANNEL_UP : OMCI_EVENT_CHANNEL_DOWN;
	bool changed;

	spin_lock_bh(&odev->state_lock);
	changed = odev->channel_up != valid ||
		  (valid && odev->gem_port_id != gem_port_id);
	if (changed)
		odev->generation++;
	odev->channel_up = valid;
	odev->gem_port_id = valid ? gem_port_id : 0xffff;
	spin_unlock_bh(&odev->state_lock);

	if (!valid)
		skb_queue_purge(&odev->rx_queue);
	if (changed) {
		omci_agent_channel_changed(odev, valid);
		omci_device_notify(odev, event);
	}
}
EXPORT_SYMBOL_GPL(omci_device_set_channel);

void omci_device_set_state(struct omci_device *odev, u8 state)
{
	bool changed;

	spin_lock_bh(&odev->state_lock);
	changed = odev->state != state;
	odev->state = state;
	spin_unlock_bh(&odev->state_lock);

	if (changed)
		omci_device_notify(odev, OMCI_EVENT_STATE_CHANGE);
}
EXPORT_SYMBOL_GPL(omci_device_set_state);

void omci_device_receive(struct omci_device *odev, struct sk_buff *skb,
			 u16 gem_port_id, u32 flags)
{
	struct omci_skb_cb *cb;
	bool channel_up;
	u16 configured_gem;
	u32 generation;

	if (!skb)
		return;
	if (!odev) {
		dev_kfree_skb_any(skb);
		return;
	}

	spin_lock_bh(&odev->state_lock);
	channel_up = odev->channel_up;
	configured_gem = odev->gem_port_id;
	generation = odev->generation;
	spin_unlock_bh(&odev->state_lock);

	if (!channel_up || gem_port_id != configured_gem || !skb->len ||
	    skb->len > OMCI_MAX_PDU_LEN) {
		atomic64_inc(&odev->rx_dropped);
		dev_kfree_skb_any(skb);
		return;
	}

	if (skb_queue_len(&odev->rx_queue) >= OMCI_RX_QUEUE_LEN) {
		atomic64_inc(&odev->rx_dropped);
		dev_kfree_skb_any(skb);
		return;
	}

	cb = OMCI_SKB_CB(skb);
	memset(cb, 0, sizeof(*cb));
	cb->sequence = atomic64_inc_return(&odev->sequence);
	cb->flags = flags;
	cb->generation = generation;
	cb->gem_port_id = gem_port_id;
	atomic64_inc(&odev->rx_packets);
	atomic64_add(skb->len, &odev->rx_bytes);
	skb_queue_tail(&odev->rx_queue, skb);
	schedule_work(&odev->rx_work);
}
EXPORT_SYMBOL_GPL(omci_device_receive);

static int __init omci_init(void)
{
	int ret;

	ret = genl_register_family(&omci_genl_family);
	if (ret)
		return ret;

	ret = netlink_register_notifier(&omci_netlink_nb);
	if (ret)
		genl_unregister_family(&omci_genl_family);

	return ret;
}
module_init(omci_init);

static void __exit omci_exit(void)
{
	netlink_unregister_notifier(&omci_netlink_nb);
	genl_unregister_family(&omci_genl_family);
}
module_exit(omci_exit);

MODULE_DESCRIPTION("Generic in-kernel OMCI agent");
MODULE_LICENSE("GPL");
