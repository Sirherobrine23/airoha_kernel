// SPDX-License-Identifier: GPL-2.0-only
/*
 * EN751221 WHNAT: PPE flow offload for WiFi netdevs without WED support.
 *
 * MT7603 and MT76x2 radios have no WED, so the vendor driver lets the PPE
 * handle their NAT flows through the CPU instead:
 *
 * - Upstream, mt76 offers each received Ethernet frame to
 *   en75_whnat_upstream_early() before GRO. The frame gets an 802.1Q tag
 *   carrying the index of its vif and goes to the PPE through the LAN QDMA.
 *   A bound flow leaves through its GDM port. An unbound one comes back to
 *   the CPU still tagged, which lets the PPE bind it, and is handed to GRO
 *   on its vif as if mt76 had never offered it.
 * - Downstream, a flow towards a vif is bound with the CPU as destination and
 *   the vif index in its L2 tag. The PPE returns the rewritten frames with
 *   CRSN_22, and they are queued straight to the vif.
 *
 * mt76 registers the vif netdevs with en75_whnat_register_vif(), which gives
 * each one of the AIROHA_WHNAT_VIFS slots. The offload is on whenever it is
 * built and a PPE is active; clearing the whnat_enable module parameter hands
 * nothing more to the PPE.
 */

#include <linux/etherdevice.h>
#include <linux/if_vlan.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/rcupdate.h>
#include <linux/unaligned.h>
#include <net/sch_generic.h>

#include "airoha_eth.h"
#include "airoha_whnat.h"

static struct net_device __rcu *vifs[AIROHA_WHNAT_VIFS];
static struct airoha_ppe __rcu *active_ppe;
static struct airoha_qdma __rcu *active_qdma;
static DEFINE_MUTEX(vif_lock);

static bool whnat_enable = true;
module_param(whnat_enable, bool, 0644);
MODULE_PARM_DESC(whnat_enable, "Hand WiFi frames to the EN751221 PPE");

static atomic_t handed = ATOMIC_INIT(0);
static atomic_t completed = ATOMIC_INIT(0);
static atomic_t returned = ATOMIC_INIT(0);
static atomic_t downstream = ATOMIC_INIT(0);
static atomic_t fallback = ATOMIC_INIT(0);

static int counter_get(char *buf, const struct kernel_param *kp)
{
	return sysfs_emit(buf, "%u\n", (unsigned int)atomic_read(kp->arg));
}

static const struct kernel_param_ops counter_ops = {
	.get = counter_get,
};

module_param_cb(whnat_handed, &counter_ops, &handed, 0444);
module_param_cb(whnat_completed, &counter_ops, &completed, 0444);
module_param_cb(whnat_returned, &counter_ops, &returned, 0444);
module_param_cb(whnat_downstream, &counter_ops, &downstream, 0444);
module_param_cb(whnat_fallback, &counter_ops, &fallback, 0444);

bool airoha_whnat_active(void)
{
	return READ_ONCE(whnat_enable) && rcu_access_pointer(active_ppe);
}

void airoha_whnat_complete(void)
{
	atomic_inc(&completed);
}

void airoha_whnat_set_ppe(struct airoha_ppe *ppe)
{
	rcu_assign_pointer(active_ppe, ppe);
}

/* Free a slot. The caller holds vif_lock. */
static void airoha_whnat_release_slot(int idx, bool invalidate)
{
	struct net_device *dev;
	struct airoha_ppe *ppe;

	dev = rcu_dereference_protected(vifs[idx], lockdep_is_held(&vif_lock));
	if (!dev)
		return;

	RCU_INIT_POINTER(vifs[idx], NULL);
	synchronize_net();

	if (invalidate) {
		rcu_read_lock_bh();
		ppe = rcu_dereference_bh(active_ppe);
		if (ppe)
			airoha_whnat_invalidate_vif(ppe, idx);
		rcu_read_unlock_bh();
	}

	dev_put(dev);
}

void airoha_whnat_clear_ppe(struct airoha_ppe *ppe)
{
	int i;

	if (rcu_access_pointer(active_ppe) != ppe)
		return;

	RCU_INIT_POINTER(active_ppe, NULL);
	synchronize_net();

	/* The PPE flushes its flows itself; only drop the netdev references. */
	mutex_lock(&vif_lock);
	for (i = 0; i < AIROHA_WHNAT_VIFS; i++)
		airoha_whnat_release_slot(i, false);
	mutex_unlock(&vif_lock);
}

void airoha_whnat_set_qdma(struct airoha_qdma *qdma)
{
	rcu_assign_pointer(active_qdma, qdma);
}

void airoha_whnat_clear_qdma(struct airoha_qdma *qdma)
{
	if (rcu_access_pointer(active_qdma) != qdma)
		return;

	RCU_INIT_POINTER(active_qdma, NULL);
	synchronize_net();
}

/* Give a WiFi netdev a slot; the slot index tags its frames in the PPE. */
int en75_whnat_register_vif(struct net_device *dev)
{
	int i, idx = -ENOSPC;

	if (!dev || !rcu_access_pointer(active_ppe))
		return -ENODEV;

	mutex_lock(&vif_lock);
	for (i = 0; i < AIROHA_WHNAT_VIFS; i++) {
		struct net_device *cur;

		cur = rcu_dereference_protected(vifs[i],
						lockdep_is_held(&vif_lock));
		if (cur == dev) {
			idx = i;
			goto out;
		}
		if (!cur && idx < 0)
			idx = i;
	}

	if (idx >= 0) {
		dev_hold(dev);
		rcu_assign_pointer(vifs[idx], dev);
	}
out:
	mutex_unlock(&vif_lock);

	return idx;
}
EXPORT_SYMBOL_GPL(en75_whnat_register_vif);

void en75_whnat_unregister_vif(struct net_device *dev)
{
	int i;

	if (!dev)
		return;

	mutex_lock(&vif_lock);
	for (i = 0; i < AIROHA_WHNAT_VIFS; i++) {
		if (rcu_access_pointer(vifs[i]) == dev)
			airoha_whnat_release_slot(i, true);
	}
	mutex_unlock(&vif_lock);
}
EXPORT_SYMBOL_GPL(en75_whnat_unregister_vif);

int airoha_whnat_vif_index(struct net_device *dev)
{
	int i, idx = -1;

	if (!dev)
		return -1;

	rcu_read_lock();
	for (i = 0; i < AIROHA_WHNAT_VIFS; i++) {
		if (rcu_dereference(vifs[i]) == dev) {
			idx = i;
			break;
		}
	}
	rcu_read_unlock();

	return idx;
}

bool en75_whnat_upstream_early(struct sk_buff *skb)
{
	struct net_device *vif_dev;
	struct airoha_qdma *qdma;
	struct airoha_ppe *ppe;
	bool taken = false;
	int idx;

	if (!READ_ONCE(whnat_enable) || !skb || !skb->dev ||
	    skb->protocol != htons(ETH_P_IP) || skb_is_gso(skb) ||
	    skb_vlan_tag_present(skb))
		return false;

	idx = airoha_whnat_vif_index(skb->dev);
	if (idx < 0 || !skb_mac_header_was_set(skb) ||
	    skb->data - skb_mac_header(skb) != ETH_HLEN)
		return false;

	rcu_read_lock_bh();
	ppe = rcu_dereference_bh(active_ppe);
	qdma = rcu_dereference_bh(active_qdma);
	if (!ppe || !qdma || !READ_ONCE(ppe->v1.armed))
		goto out;

	if ((skb_is_nonlinear(skb) && skb_linearize(skb)) ||
	    skb_cow_head(skb, ETH_HLEN + VLAN_HLEN))
		goto out;

	/* Put the Ethernet header back and tag the frame with its vif. */
	vif_dev = skb->dev;
	skb_push(skb, ETH_HLEN + VLAN_HLEN);
	memmove(skb->data, skb->data + VLAN_HLEN, 2 * ETH_ALEN);
	put_unaligned_be16(ETH_P_8021Q, skb->data + 12);
	put_unaligned_be16(idx, skb->data + 14);

	/* The frame no longer belongs to a netdev queue (no BQL accounting). */
	skb->dev = NULL;
	if (airoha_whnat_qdma_xmit(qdma, skb) >= 0) {
		atomic_inc(&handed);
		taken = true;
	} else {
		memmove(skb->data + VLAN_HLEN, skb->data, 2 * ETH_ALEN);
		skb_pull(skb, VLAN_HLEN + ETH_HLEN);
		skb->dev = vif_dev;
	}
out:
	if (!taken)
		atomic_inc(&fallback);
	rcu_read_unlock_bh();

	return taken;
}
EXPORT_SYMBOL_GPL(en75_whnat_upstream_early);

void airoha_whnat_batch_init(struct airoha_whnat_batch *batch)
{
	int i;

	for (i = 0; i < AIROHA_WHNAT_VIFS; i++) {
		__skb_queue_head_init(&batch->queue[i]);
		batch->dev[i] = NULL;
	}
}

/*
 * dev_queue_xmit() hands a list of frames to the driver as is only when the
 * TX queue has no qdisc that queues; a qdisc enqueues the first frame alone
 * and the rest of the list is lost. IFF_NO_QUEUE only selects the default
 * qdisc: a vif keeps it when one is attached (SQM, fq_codel), so look at the
 * qdiscs themselves.
 */
static bool airoha_whnat_dev_queueless(struct net_device *dev)
{
	unsigned int i;

	for (i = 0; i < dev->real_num_tx_queues; i++) {
		struct netdev_queue *txq = netdev_get_tx_queue(dev, i);

		if (rcu_dereference_bh(txq->qdisc)->enqueue)
			return false;
	}

	return true;
}

void airoha_whnat_batch_flush(struct airoha_whnat_batch *batch)
{
	int i;

	for (i = 0; i < AIROHA_WHNAT_VIFS; i++) {
		struct sk_buff *skb, *head = NULL, *tail = NULL;
		struct net_device *dev = batch->dev[i];
		struct sk_buff_head *q = &batch->queue[i];

		if (!dev)
			continue;

		/* A queueless vif with no taps takes the whole batch as one
		 * list; anything else goes through the qdisc frame by frame.
		 */
		if (q->qlen > 1 && airoha_whnat_dev_queueless(dev) &&
		    !dev_nit_active(dev)) {
			while ((skb = __skb_dequeue(q))) {
				if (!head)
					head = skb;
				else
					tail->next = skb;
				tail = skb;
			}
			tail->next = NULL;
			dev_queue_xmit(head);
		} else {
			while ((skb = __skb_dequeue(q)))
				dev_queue_xmit(skb);
		}

		dev_put(dev);
		batch->dev[i] = NULL;
	}
}

bool airoha_whnat_rx(struct sk_buff *skb, struct napi_struct *napi,
		     u8 qdma_id, u8 sport, u8 reason,
		     struct airoha_whnat_batch *batch)
{
	bool bound = reason == CRSN_22;
	struct net_device *dev;
	u16 proto, idx;

	if (!rcu_access_pointer(active_ppe) ||
	    skb_headlen(skb) < ETH_HLEN + VLAN_HLEN)
		return false;

	proto = get_unaligned_be16(skb->data + 12);
	if (bound) {
		if (proto != ETH_P_8021Q && proto != AIROHA_WHNAT_ETYPE)
			return false;
	} else if (qdma_id != 0 || sport != 0 || proto != ETH_P_8021Q) {
		return false;
	}

	idx = get_unaligned_be16(skb->data + 14) & VLAN_VID_MASK;
	if (idx >= AIROHA_WHNAT_VIFS)
		return false;

	rcu_read_lock_bh();
	dev = batch && batch->dev[idx] ? batch->dev[idx] :
					 rcu_dereference_bh(vifs[idx]);
	if (!dev || !netif_running(dev)) {
		rcu_read_unlock_bh();
		return false;
	}

	/* Drop the tag. */
	memmove(skb->data + VLAN_HLEN, skb->data, 2 * ETH_ALEN);
	skb_pull(skb, VLAN_HLEN);
	skb->dev = dev;

	if (bound) {
		skb_reset_mac_header(skb);
		skb_set_network_header(skb, ETH_HLEN);
		skb->protocol = eth_hdr(skb)->h_proto;
		atomic_inc(&downstream);
		if (batch) {
			if (!batch->dev[idx]) {
				dev_hold(dev);
				batch->dev[idx] = dev;
			}
			__skb_queue_tail(&batch->queue[idx], skb);
		} else {
			dev_queue_xmit(skb);
		}
	} else {
		skb->protocol = eth_type_trans(skb, dev);
		atomic_inc(&returned);
		/* Below the mt76 hook: this frame is never offered again. */
		napi_gro_receive(napi, skb);
	}
	rcu_read_unlock_bh();

	return true;
}
