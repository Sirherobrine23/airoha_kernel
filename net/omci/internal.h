/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _NET_OMCI_INTERNAL_H
#define _NET_OMCI_INTERNAL_H

#include <linux/atomic.h>
#include <linux/device.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/skbuff.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/workqueue.h>
#include <linux/xarray.h>
#include <net/net_namespace.h>
#include <net/omci.h>

#define OMCI_RX_QUEUE_LEN		256
#define OMCI_BASELINE_DEV_ID		0x0a
#define OMCI_EXTENDED_DEV_ID		0x0b
#define OMCI_BASELINE_LEN		48
#define OMCI_BASELINE_LEN_NO_MIC	44
#define OMCI_EXTENDED_HEADER_LEN	10
#define OMCI_MIC_LEN			4

#define OMCI_CLASS_VLAN_TAGGING_FILTER_DATA	84
#define OMCI_CLASS_EXTENDED_VLAN		171

struct omci_skb_cb {
	u64 sequence;
	u32 flags;
	u32 generation;
	u16 gem_port_id;
};

#define OMCI_SKB_CB(_skb) ((struct omci_skb_cb *)&((_skb)->cb[0]))

struct omci_mib_object {
	u16 class_id;
	u16 entity_id;
	u16 attr_mask;
	u8 origin;
	u8 data[OMCI_MAX_ATTR_DATA];
	struct omci_vlan_tagging_filter vlan_filter;
	struct omci_extended_vlan extended_vlan;
};

struct omci_agent_config {
	u8 serial_number[8];
	u8 vendor_id[4];
	u8 version[14];
	u8 equipment_id[20];
	u8 password[10];
	u8 traffic_mgmt_option;
	u8 onu_type;
	u8 uni_count;
};

struct omci_agent {
	/* Serializes configuration, MIB and transaction state. */
	struct mutex lock;
	struct xarray mib;
	struct omci_agent_config config;
	u32 upload_index;
	u16 mib_sync;
	bool enabled;
	bool permissive;

	u8 last_request[OMCI_BASELINE_LEN_NO_MIC];
	u8 last_response[OMCI_BASELINE_LEN_NO_MIC];
	u8 last_request_len;
	u8 last_response_len;

	atomic64_t responses;
	atomic64_t duplicates;
	atomic64_t unsupported;
};

struct omci_device {
	struct list_head list;
	struct device *parent;
	const struct omci_device_ops *ops;
	void *priv;
	u32 id;
	u32 ifindex;
	u32 capabilities;

	/* Protect the optional userspace observer. */
	struct mutex owner_lock;
	struct net *owner_net;
	u32 owner_portid;

	/* Protect channel state accessed from the hardware RX path. */
	spinlock_t state_lock;
	u16 onu_id;
	u16 gem_port_id;
	u32 generation;
	u8 state;
	bool channel_up;

	struct sk_buff_head rx_queue;
	struct work_struct rx_work;
	atomic64_t sequence;
	atomic64_t rx_packets;
	atomic64_t rx_bytes;
	atomic64_t rx_dropped;
	atomic64_t tx_packets;
	atomic64_t tx_bytes;
	atomic64_t tx_errors;

	struct omci_agent agent;
};

int omci_validate_tx(struct omci_device *odev, struct sk_buff *skb);
int omci_device_xmit(struct omci_device *odev, const void *data, size_t len);
void omci_device_notify(struct omci_device *odev, u8 event);

int omci_agent_init(struct omci_device *odev);
void omci_agent_cleanup(struct omci_device *odev);
void omci_agent_receive(struct omci_device *odev, const struct sk_buff *skb);
void omci_agent_channel_changed(struct omci_device *odev, bool valid);
int omci_agent_put_status(struct sk_buff *msg, struct omci_device *odev);
int omci_agent_config_get(struct omci_device *odev, u16 key,
			  void *value, size_t *len);
int omci_agent_config_set(struct omci_device *odev, u16 key,
			  const void *value, size_t len);
int omci_agent_mib_get(struct omci_device *odev, u16 class_id, u16 entity_id,
		       struct omci_mib_object *object);
int omci_agent_mib_set(struct omci_device *odev,
		       const struct omci_mib_object *object);
int omci_agent_mib_delete(struct omci_device *odev, u16 class_id,
			  u16 entity_id);
void omci_agent_mib_reset(struct omci_device *odev, bool all);
int omci_agent_mib_next(struct omci_device *odev, u32 index,
			struct omci_mib_object *object, u32 *next_index,
			const char **name);
const char *omci_agent_class_name(u16 class_id);

#endif /* _NET_OMCI_INTERNAL_H */
