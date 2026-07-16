// SPDX-License-Identifier: GPL-2.0-only
/*
 * Generic ONU Management and Control Interface transport
 *
 * This module transports OMCI PDUs between GPON hardware drivers and one
 * userspace owner. The G.988 managed-entity database and state machine are
 * intentionally left in userspace.
 */

#include <linux/atomic.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/netdevice.h>
#include <linux/netlink.h>
#include <linux/notifier.h>
#include <linux/skbuff.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/unaligned.h>
#include <linux/workqueue.h>
#include <net/genetlink.h>
#include <net/net_namespace.h>
#include <net/netlink.h>
#include <net/omci.h>

#define OMCI_RX_QUEUE_LEN		256
#define OMCI_BASELINE_DEV_ID		0x0a
#define OMCI_EXTENDED_DEV_ID		0x0b
#define OMCI_BASELINE_LEN		48
#define OMCI_BASELINE_LEN_NO_MIC	44
#define OMCI_EXTENDED_HEADER_LEN	10
#define OMCI_MIC_LEN			4

struct omci_skb_cb {
	u64 sequence;
	u32 flags;
	u32 generation;
	u16 gem_port_id;
};

#define OMCI_SKB_CB(_skb) ((struct omci_skb_cb *)&((_skb)->cb[0]))

struct omci_device {
	struct list_head list;
	struct device *parent;
	const struct omci_device_ops *ops;
	void *priv;
	u32 id;
	u32 ifindex;
	u32 capabilities;

	/* Protect the userspace owner. */
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
};

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

	return 0;
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

static int omci_validate_tx(struct omci_device *odev, struct sk_buff *skb)
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

		mutex_lock(&odev->owner_lock);
		if (odev->owner_portid && odev->owner_net) {
			portid = odev->owner_portid;
			net = get_net(odev->owner_net);
		}
		mutex_unlock(&odev->owner_lock);

		if (!net) {
			skb_queue_head(&odev->rx_queue, skb);
			break;
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

static void omci_send_event(struct omci_device *odev, u8 event)
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
	odev->capabilities = capabilities;
	odev->ops = ops;
	odev->priv = priv;
	odev->onu_id = 0xffff;
	odev->gem_port_id = 0xffff;
	odev->id = atomic_inc_return(&omci_next_id) - 1;

	mutex_lock(&omci_devices_lock);
	list_add_tail(&odev->list, &omci_devices);
	mutex_unlock(&omci_devices_lock);

	dev_info(parent, "registered OMCI transport device %u\n", odev->id);
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
	if (changed)
		omci_send_event(odev, event);
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
		omci_send_event(odev, OMCI_EVENT_STATE_CHANGE);
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

MODULE_DESCRIPTION("Generic OMCI transport");
MODULE_LICENSE("GPL");
