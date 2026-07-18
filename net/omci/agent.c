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
#include <linux/kernel.h>
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
#define OMCI_MSG_TYPE_TEST		18
#define OMCI_MSG_TYPE_START_SW_DOWNLOAD 19
#define OMCI_MSG_TYPE_DOWNLOAD_SECTION	20
#define OMCI_MSG_TYPE_END_SW_DOWNLOAD	21
#define OMCI_MSG_TYPE_ACTIVATE_SW	22
#define OMCI_MSG_TYPE_COMMIT_SW	23
#define OMCI_MSG_TYPE_SYNC_TIME	24
#define OMCI_MSG_TYPE_REBOOT		25
#define OMCI_MSG_TYPE_GET_NEXT		26
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

#define OMCI_CLASS_ONU_DATA		2
#define OMCI_CLASS_SOFTWARE_IMAGE	7
#define OMCI_CLASS_PPTP_ETHERNET_UNI	11
#define OMCI_CLASS_MAC_BRIDGE_SERVICE_PROFILE 45
#define OMCI_CLASS_MAC_BRIDGE_CONFIG_DATA 46
#define OMCI_CLASS_MAC_BRIDGE_PORT_CONFIG_DATA 47
#define OMCI_CLASS_VLAN_TAGGING_FILTER_DATA 84
#define OMCI_CLASS_OLT_G		131
#define OMCI_CLASS_NETWORK_ADDRESS	137
#define OMCI_CLASS_AUTH_METHOD		148
#define OMCI_CLASS_EXTENDED_VLAN	171
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

struct omci_class_info {
	u16 class_id;
	const char *name;
};

static const struct omci_class_info omci_classes[] = {
	{ OMCI_CLASS_ONU_DATA, "ONU data" },
	{ OMCI_CLASS_SOFTWARE_IMAGE, "Software image" },
	{ OMCI_CLASS_PPTP_ETHERNET_UNI, "PPTP Ethernet UNI" },
	{ OMCI_CLASS_MAC_BRIDGE_SERVICE_PROFILE, "MAC bridge service profile" },
	{ OMCI_CLASS_MAC_BRIDGE_CONFIG_DATA, "MAC bridge configuration data" },
	{ OMCI_CLASS_MAC_BRIDGE_PORT_CONFIG_DATA, "MAC bridge port configuration data" },
	{ OMCI_CLASS_VLAN_TAGGING_FILTER_DATA, "VLAN tagging filter data" },
	{ OMCI_CLASS_OLT_G, "OLT-G" },
	{ OMCI_CLASS_NETWORK_ADDRESS, "Network address" },
	{ OMCI_CLASS_AUTH_METHOD, "Authentication security method" },
	{ OMCI_CLASS_EXTENDED_VLAN, "Extended VLAN tagging operation configuration data" },
	{ OMCI_CLASS_ONU_G, "ONU-G" },
	{ OMCI_CLASS_ONU2_G, "ONU2-G" },
	{ OMCI_CLASS_TCONT, "T-CONT" },
	{ OMCI_CLASS_ANI_G, "ANI-G" },
	{ OMCI_CLASS_GEM_IWTP, "GEM interworking termination point" },
	{ OMCI_CLASS_GEM_PORT_CTP, "GEM port network CTP" },
	{ OMCI_CLASS_GAL_ETHERNET_PROFILE, "GAL Ethernet profile" },
	{ OMCI_CLASS_PRIORITY_QUEUE, "Priority queue" },
	{ OMCI_CLASS_TRAFFIC_SCHEDULER, "Traffic scheduler" },
	{ OMCI_CLASS_VEIP, "Virtual Ethernet interface point" },
};

static unsigned long omci_mib_key(u16 class_id, u16 entity_id)
{
	return ((unsigned long)class_id << 16) | entity_id;
}

const char *omci_agent_class_name(u16 class_id)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(omci_classes); i++)
		if (omci_classes[i].class_id == class_id)
			return omci_classes[i].name;

	return "Dynamic managed entity";
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

static int omci_mib_add_default(struct omci_agent *agent, u16 class_id,
				u16 entity_id, const void *data, size_t len)
{
	struct omci_mib_object object = {
		.class_id = class_id,
		.entity_id = entity_id,
		.attr_mask = 0xffff,
		.origin = OMCI_MIB_ORIGIN_DEFAULT,
	};

	if (data)
		memcpy(object.data, data, min(len, sizeof(object.data)));
	return omci_mib_store_locked(agent, &object);
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
	ret = omci_mib_add_default(agent, OMCI_CLASS_ANI_G, 0, data,
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
	int ret;

	mutex_init(&agent->lock);
	xa_init(&agent->mib);
	agent->enabled = true;
	agent->permissive = true;
	agent->config.uni_count = 4;
	agent->config.onu_type = 2;
	agent->config.traffic_mgmt_option = 0;
	memcpy(agent->config.serial_number, "OPEN0001", 8);
	memcpy(agent->config.vendor_id, "OPEN", 4);
	strscpy(agent->config.version, "OpenWrt", sizeof(agent->config.version));
	strscpy(agent->config.equipment_id, "Airoha EN7523",
		sizeof(agent->config.equipment_id));

	mutex_lock(&agent->lock);
	ret = omci_agent_populate_defaults(agent);
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
	u16 value;

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
		return ops->set_uni(odev, object->entity_id,
				    action != OMCI_MSG_TYPE_DELETE);
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
	struct omci_mib_object object = {
		.class_id = class_id,
		.entity_id = entity_id,
		.attr_mask = 0,
		.origin = OMCI_MIB_ORIGIN_OLT,
	};
	struct omci_mib_object *stored;

	stored = omci_mib_lookup(agent, class_id, entity_id);
	if (stored || !create)
		return stored;
	if (omci_mib_store_locked(agent, &object))
		return NULL;
	return omci_mib_lookup(agent, class_id, entity_id);
}

static u8 omci_agent_create_locked(struct omci_device *odev,
				   u16 class_id, u16 entity_id,
				   const u8 *content)
{
	struct omci_agent *agent = &odev->agent;
	struct omci_mib_object object = {
		.class_id = class_id,
		.entity_id = entity_id,
		.attr_mask = 0xffff,
		.origin = OMCI_MIB_ORIGIN_OLT,
	};
	int ret;

	if (omci_mib_lookup(agent, class_id, entity_id))
		return OMCI_RESULT_INSTANCE_EXISTS;
	memcpy(object.data, content, sizeof(object.data));
	ret = omci_agent_hw_update(odev, &object, OMCI_MSG_TYPE_CREATE,
				   content);
	if (ret)
		return OMCI_RESULT_PROCESSING_ERROR;
	ret = omci_mib_store_locked(agent, &object);
	if (ret)
		return OMCI_RESULT_DEVICE_BUSY;
	agent->mib_sync++;
	return OMCI_RESULT_SUCCESS;
}

static u8 omci_agent_set_locked(struct omci_device *odev, u16 class_id,
				u16 entity_id, const u8 *content)
{
	struct omci_agent *agent = &odev->agent;
	struct omci_mib_object *object;
	u16 mask = get_unaligned_be16(content);
	int ret;

	object = omci_get_or_create_locked(agent, class_id, entity_id,
					   agent->permissive);
	if (!object)
		return agent->permissive ? OMCI_RESULT_DEVICE_BUSY :
					   OMCI_RESULT_UNKNOWN_INSTANCE;
	object->attr_mask |= mask;
	memcpy(object->data, content + 2, sizeof(object->data) - 2);
	object->origin = OMCI_MIB_ORIGIN_OLT;
	ret = omci_agent_hw_update(odev, object, OMCI_MSG_TYPE_SET, content);
	if (ret)
		return OMCI_RESULT_PROCESSING_ERROR;
	agent->mib_sync++;
	return OMCI_RESULT_SUCCESS;
}

static u8 omci_agent_delete_locked(struct omci_device *odev, u16 class_id,
				   u16 entity_id)
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
	agent->mib_sync++;
	return OMCI_RESULT_SUCCESS;
}

static u8 omci_agent_get_locked(struct omci_agent *agent, u16 class_id,
				u16 entity_id, const u8 *content,
				u8 *response_content)
{
	struct omci_mib_object *object;
	u16 mask = get_unaligned_be16(content);

	object = omci_get_or_create_locked(agent, class_id, entity_id,
					   agent->permissive);
	if (!object)
		return OMCI_RESULT_UNKNOWN_INSTANCE;
	response_content[0] = OMCI_RESULT_SUCCESS;
	put_unaligned_be16(mask, response_content + 1);
	memcpy(response_content + 3, object->data,
	       OMCI_BASELINE_LEN_NO_MIC - 8 - 3 - 4);
	return OMCI_RESULT_SUCCESS;
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
					      bool *unsupported)
{
	struct omci_agent *agent = &odev->agent;
	u8 *content = response + 8;
	u8 action = request[2] & 0x1f;
	u16 class_id = get_unaligned_be16(request + 4);
	u16 entity_id = get_unaligned_be16(request + 6);
	u16 sequence;
	u8 result = OMCI_RESULT_SUCCESS;

	omci_response_init(response, request, action);
	*unsupported = false;

	switch (action) {
	case OMCI_MSG_TYPE_CREATE:
		result = omci_agent_create_locked(odev, class_id, entity_id,
						  request + 8);
		content[0] = result;
		break;
	case OMCI_MSG_TYPE_DELETE:
		result = omci_agent_delete_locked(odev, class_id, entity_id);
		content[0] = result;
		break;
	case OMCI_MSG_TYPE_SET:
	case OMCI_MSG_TYPE_SET_TABLE:
		result = omci_agent_set_locked(odev, class_id, entity_id,
					       request + 8);
		content[0] = result;
		put_unaligned_be16(0, content + 1);
		put_unaligned_be16(result ? get_unaligned_be16(request + 8) : 0,
				   content + 3);
		break;
	case OMCI_MSG_TYPE_GET:
	case OMCI_MSG_TYPE_GET_NEXT:
	case OMCI_MSG_TYPE_GET_CURRENT_DATA:
		result = omci_agent_get_locked(agent, class_id, entity_id,
					       request + 8, content);
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
		content[0] = OMCI_RESULT_NOT_SUPPORTED;
		*unsupported = true;
		break;
	default:
		content[0] = OMCI_RESULT_NOT_SUPPORTED;
		*unsupported = true;
		break;
	}

	return true;
}

void omci_agent_receive(struct omci_device *odev, const struct sk_buff *skb)
{
	struct omci_agent *agent = &odev->agent;
	u8 response[OMCI_BASELINE_LEN_NO_MIC];
	bool unsupported = false;
	bool duplicate = false;
	int ret;

	if (skb->len != OMCI_BASELINE_LEN_NO_MIC &&
	    skb->len != OMCI_BASELINE_LEN)
		return;
	if (skb->data[3] != OMCI_BASELINE_DEV_ID)
		return;

	omci_agent_log_baseline(odev, "RX", skb->data);

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
		duplicate = true;
	} else {
		omci_agent_build_response_locked(odev, skb->data, response,
						 &unsupported);
		memcpy(agent->last_request, skb->data,
		       OMCI_BASELINE_LEN_NO_MIC);
		memcpy(agent->last_response, response, sizeof(response));
		agent->last_request_len = OMCI_BASELINE_LEN_NO_MIC;
		agent->last_response_len = sizeof(response);
	}
	mutex_unlock(&agent->lock);

	omci_agent_log_baseline(odev, "TX", response);
	ret = omci_device_xmit(odev, response, sizeof(response));
	if (!ret)
		atomic64_inc(&agent->responses);
	if (duplicate)
		atomic64_inc(&agent->duplicates);
	if (unsupported) {
		atomic64_inc(&agent->unsupported);
		omci_device_notify(odev, OMCI_EVENT_UNSUPPORTED);
	}
}

void omci_agent_channel_changed(struct omci_device *odev, bool valid)
{
	struct omci_agent *agent = &odev->agent;

	mutex_lock(&agent->lock);
	agent->last_request_len = 0;
	agent->last_response_len = 0;
	agent->upload_index = 0;
	mutex_unlock(&agent->lock);
}

int omci_agent_put_status(struct sk_buff *msg, struct omci_device *odev)
{
	struct omci_agent *agent = &odev->agent;
	u32 count;
	u16 sync;
	bool enabled;
	bool permissive;

	mutex_lock(&agent->lock);
	count = omci_mib_count_locked(agent);
	sync = agent->mib_sync;
	enabled = agent->enabled;
	permissive = agent->permissive;
	mutex_unlock(&agent->lock);

	if (nla_put_u8(msg, OMCI_ATTR_AGENT_ENABLED, enabled) ||
	    nla_put_u8(msg, OMCI_ATTR_AGENT_PERMISSIVE, permissive) ||
	    nla_put_u16(msg, OMCI_ATTR_MIB_SYNC, sync) ||
	    nla_put_u32(msg, OMCI_ATTR_MIB_OBJECTS, count) ||
	    nla_put_u64_64bit(msg, OMCI_ATTR_AGENT_RESPONSES,
			      atomic64_read(&agent->responses), OMCI_ATTR_PAD) ||
	    nla_put_u64_64bit(msg, OMCI_ATTR_AGENT_DUPLICATES,
			      atomic64_read(&agent->duplicates), OMCI_ATTR_PAD) ||
	    nla_put_u64_64bit(msg, OMCI_ATTR_AGENT_UNSUPPORTED,
			      atomic64_read(&agent->unsupported), OMCI_ATTR_PAD))
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

int omci_agent_config_set(struct omci_device *odev, u16 key,
			  const void *value, size_t len)
{
	struct omci_agent *agent = &odev->agent;
	u8 scalar;
	int ret = 0;

	mutex_lock(&agent->lock);
	switch (key) {
	case OMCI_CONFIG_SERIAL_NUMBER:
		if (len != sizeof(agent->config.serial_number)) {
			ret = -EINVAL;
			break;
		}
		memcpy(agent->config.serial_number, value, len);
		break;
	case OMCI_CONFIG_VENDOR_ID:
		if (len != sizeof(agent->config.vendor_id)) {
			ret = -EINVAL;
			break;
		}
		memcpy(agent->config.vendor_id, value, len);
		break;
	case OMCI_CONFIG_VERSION:
		if (!len || len >= sizeof(agent->config.version)) {
			ret = -EINVAL;
			break;
		}
		memset(agent->config.version, 0, sizeof(agent->config.version));
		memcpy(agent->config.version, value, len);
		break;
	case OMCI_CONFIG_EQUIPMENT_ID:
		if (!len || len >= sizeof(agent->config.equipment_id)) {
			ret = -EINVAL;
			break;
		}
		memset(agent->config.equipment_id, 0,
		       sizeof(agent->config.equipment_id));
		memcpy(agent->config.equipment_id, value, len);
		break;
	case OMCI_CONFIG_PASSWORD:
		if (len != sizeof(agent->config.password)) {
			ret = -EINVAL;
			break;
		}
		memcpy(agent->config.password, value, len);
		break;
	case OMCI_CONFIG_TRAFFIC_MGMT_OPTION:
	case OMCI_CONFIG_ONU_TYPE:
	case OMCI_CONFIG_UNI_COUNT:
	case OMCI_CONFIG_AGENT_ENABLED:
	case OMCI_CONFIG_AGENT_PERMISSIVE:
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
		else if (key == OMCI_CONFIG_AGENT_ENABLED)
			agent->enabled = !!scalar;
		else
			agent->permissive = !!scalar;
		break;
	default:
		ret = -EINVAL;
		break;
	}
	if (!ret) {
		omci_agent_refresh_identity_locked(agent);
		agent->mib_sync++;
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
	struct omci_mib_object local = *object;
	int ret;

	local.origin = OMCI_MIB_ORIGIN_LOCAL;
	mutex_lock(&agent->lock);
	ret = omci_mib_store_locked(agent, &local);
	if (!ret)
		agent->mib_sync++;
	mutex_unlock(&agent->lock);
	return ret;
}

int omci_agent_mib_delete(struct omci_device *odev, u16 class_id,
			  u16 entity_id)
{
	struct omci_agent *agent = &odev->agent;
	struct omci_mib_object *object;

	mutex_lock(&agent->lock);
	object = xa_erase(&agent->mib, omci_mib_key(class_id, entity_id));
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
