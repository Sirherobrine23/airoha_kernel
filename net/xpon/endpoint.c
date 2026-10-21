// SPDX-License-Identifier: GPL-2.0-only
/* Ethernet endpoints bound to PON protocol objects, independent of DSA. */

#include <linux/etherdevice.h>
#include <linux/rtnetlink.h>
#include <net/net_namespace.h>
#include <net/pkt_cls.h>

#include "internal.h"

static unsigned long xpon_rx_key(u8 type, u16 wire_id)
{
	return ((unsigned long)type << 16) | wire_id;
}

static bool xpon_endpoint_ready(struct xpon_endpoint *ep)
{
	struct xpon_device *xpon = ep->xpon;
	struct xpon_endpoint_config cfg;
	bool ready;

	spin_lock_bh(&xpon->state_lock);
	cfg = ep->config;
	ready = cfg.enabled && !xpon->state.los && xpon->state.carrier;
	if (cfg.type == XPON_ENDPOINT_LLID)
		ready = cfg.enabled && cfg.channel < xpon->max_links &&
			xpon->links[cfg.channel].state ==
				XPON_LINK_REGISTERED &&
			xpon->links[cfg.channel].llid == cfg.wire_id;
	spin_unlock_bh(&xpon->state_lock);
	return ready;
}

static netdev_tx_t xpon_endpoint_xmit(struct sk_buff *skb,
				      struct net_device *dev)
{
	struct xpon_endpoint *ep = netdev_priv(dev);
	struct xpon_endpoint_config config;
	unsigned int len = skb->len;
	netdev_tx_t ret;

	if (!xpon_endpoint_ready(ep)) {
		dev_kfree_skb_any(skb);
		dev->stats.tx_dropped++;
		return NETDEV_TX_OK;
	}
	spin_lock_bh(&ep->xpon->state_lock);
	config = ep->config;
	spin_unlock_bh(&ep->xpon->state_lock);
	ret = ep->xpon->ops->xmit(ep->xpon, skb, &config);
	if (ret == NETDEV_TX_OK)
		dev_sw_netstats_tx_add(dev, 1, len);
	return ret;
}

static int xpon_endpoint_open(struct net_device *dev)
{
	struct xpon_endpoint *ep = netdev_priv(dev);
	int ret;

	/* Keep the shared datapath alive through ordinary netdev semantics. */
	ret = dev_open(ep->xpon->netdev, NULL);
	if (ret)
		return ret;
	if (xpon_endpoint_ready(ep))
		netif_carrier_on(dev);
	else
		netif_carrier_off(dev);
	netif_start_queue(dev);
	return 0;
}

static int xpon_endpoint_stop(struct net_device *dev)
{
	netif_stop_queue(dev);
	return 0;
}

static int xpon_endpoint_setup_tc(struct net_device *dev,
				  enum tc_setup_type type, void *type_data)
{
	struct xpon_endpoint *ep = netdev_priv(dev);
	struct net_device *lower = ep->xpon->netdev;

	if (type != TC_SETUP_FT || !lower->netdev_ops->ndo_setup_tc)
		return -EOPNOTSUPP;

	return lower->netdev_ops->ndo_setup_tc(lower, type, type_data);
}

static const struct net_device_ops xpon_endpoint_ops = {
	.ndo_open = xpon_endpoint_open,
	.ndo_stop = xpon_endpoint_stop,
	.ndo_start_xmit = xpon_endpoint_xmit,
	.ndo_get_stats64 = dev_get_tstats64,
	.ndo_set_mac_address = eth_mac_addr,
	.ndo_validate_addr = eth_validate_addr,
	.ndo_setup_tc = xpon_endpoint_setup_tc,
};

int xpon_endpoint_get_path(struct net_device *dev, struct net_device **lower,
			   struct xpon_endpoint_config *config)
{
	struct xpon_endpoint *ep;
	bool ready;

	if (dev->netdev_ops != &xpon_endpoint_ops)
		return -EOPNOTSUPP;
	ep = netdev_priv(dev);
	spin_lock_bh(&ep->xpon->state_lock);
	*config = ep->config;
	ready = config->enabled && !READ_ONCE(ep->xpon->unregistering);
	*lower = ep->xpon->netdev;
	spin_unlock_bh(&ep->xpon->state_lock);

	return ready && netif_running(dev) ? 0 : -ENETDOWN;
}
EXPORT_SYMBOL_GPL(xpon_endpoint_get_path);

static void xpon_endpoint_destructor(struct net_device *dev)
{
	struct xpon_endpoint *ep = netdev_priv(dev);

	xpon_device_put(ep->xpon);
}

static void xpon_endpoint_setup(struct net_device *dev)
{
	ether_setup(dev);
	dev->netdev_ops = &xpon_endpoint_ops;
	dev->needs_free_netdev = true;
	dev->pcpu_stat_type = NETDEV_PCPU_STAT_TSTATS;
	/* No hardware checksum/GSO offload is assumed across providers. */
	dev->lltx = true;
	dev->netns_immutable = true;
}

int xpon_endpoint_set(struct xpon_device *xpon,
		      const struct xpon_endpoint_config *requested)
{
	struct xpon_endpoint_config resolved = *requested;
	const struct xpon_endpoint_config *config = &resolved;
	struct net_device *dev = NULL;
	struct xpon_endpoint *ep, *other;
	unsigned long key, old_key = 0;
	bool changed = false;
	int ret;

	ASSERT_RTNL();
	if (!xpon->ops || !xpon->ops->xmit)
		return -EOPNOTSUPP;
	if (!config->id || config->id == U32_MAX ||
	    config->type > XPON_ENDPOINT_LLID ||
	    config->queue >= xpon->max_queues)
		return -EINVAL;
	if (config->type == XPON_ENDPOINT_LLID) {
		if (xpon->state.mode != XPON_MODE_EPON ||
		    (!config->link_id || config->link_id > xpon->max_links))
			return -EINVAL;
	} else if (xpon->state.mode == XPON_MODE_EPON) {
		return -EINVAL;
	}
	if (xpon->ops->resolve_endpoint) {
		ret = xpon->ops->resolve_endpoint(xpon, &resolved);
		if (ret)
			return ret;
	}
	key = xpon_rx_key(config->type, config->wire_id);
	ep = xa_load(&xpon->endpoints, config->id);
	other = xa_load(&xpon->rx_endpoints, key);
	if (other && other != ep)
		return -EEXIST;
	if (!ep) {
		unsigned long index;
		unsigned int count = 0;

		xa_for_each(&xpon->endpoints, index, other)
			count++;
		if (count >= xpon->max_endpoints)
			return -ENOSPC;
		dev = alloc_netdev(sizeof(*ep), "pon%d", NET_NAME_ENUM,
				   xpon_endpoint_setup);
		if (!dev)
			return -ENOMEM;
		ep = netdev_priv(dev);
		ep->xpon = xpon;
		ep->netdev = dev;
		kref_get(&xpon->ref);
		dev_net_set(dev, dev_net(xpon->netdev));
		SET_NETDEV_DEV(dev, xpon->parent);
		eth_hw_addr_inherit(dev, xpon->netdev);
		dev->min_mtu = xpon->netdev->min_mtu;
		dev->max_mtu = xpon->netdev->mtu;
		dev->mtu = xpon->netdev->mtu;
		netif_carrier_off(dev);
	} else {
		old_key = xpon_rx_key(ep->config.type, ep->config.wire_id);
		changed = !xpon_endpoint_same_binding(&ep->config, config);
	}
	ret = xa_err(xa_store(&xpon->rx_endpoints, key, ep, GFP_KERNEL));
	if (ret)
		goto free;
	spin_lock_bh(&xpon->state_lock);
	ep->config = *config;
	spin_unlock_bh(&xpon->state_lock);
	if (!dev) {
		if (old_key != key)
			xa_erase(&xpon->rx_endpoints, old_key);
		if (xpon_endpoint_ready(ep) && netif_running(xpon->netdev))
			netif_carrier_on(ep->netdev);
		else
			netif_carrier_off(ep->netdev);
		/* Flush provider offloads which captured the previous binding. */
		if (changed)
			call_netdevice_notifiers(NETDEV_CHANGE, ep->netdev);
		xpon_device_report_event(xpon, XPON_EVENT_ENDPOINT);
		return 0;
	}
	ret = xa_err(xa_store(&xpon->endpoints, config->id, ep, GFP_KERNEL));
	if (ret)
		goto erase;
	ret = register_netdevice(dev);
	if (ret) {
		xa_erase(&xpon->endpoints, config->id);
		goto erase;
	}
	dev->priv_destructor = xpon_endpoint_destructor;
	ret = netdev_upper_dev_link(xpon->netdev, dev, NULL);
	if (ret) {
		xpon_endpoint_delete(xpon, config->id);
		return ret;
	}
	xpon_device_report_event(xpon, XPON_EVENT_ENDPOINT);
	return 0;
erase:
	xa_erase(&xpon->rx_endpoints, key);
	synchronize_net();
free:
	if (dev) {
		xpon_device_put(xpon);
		free_netdev(dev);
	}
	return ret;
}
EXPORT_SYMBOL_GPL(xpon_endpoint_set);

int xpon_endpoint_get_config(struct xpon_device *xpon, u32 id,
			     struct xpon_endpoint_config *config)
{
	struct xpon_endpoint *ep;

	ASSERT_RTNL();
	ep = xa_load(&xpon->endpoints, id);
	if (!ep)
		return -ENOENT;
	*config = ep->config;
	return 0;
}
EXPORT_SYMBOL_GPL(xpon_endpoint_get_config);

int xpon_endpoint_delete(struct xpon_device *xpon, u32 id)
{
	struct xpon_endpoint *ep;

	ASSERT_RTNL();
	ep = xa_erase(&xpon->endpoints, id);
	if (!ep)
		return -ENOENT;
	xa_erase(&xpon->rx_endpoints,
		 xpon_rx_key(ep->config.type, ep->config.wire_id));
	synchronize_net();
	netdev_upper_dev_unlink(xpon->netdev, ep->netdev);
	unregister_netdevice(ep->netdev);
	xpon_device_report_event(xpon, XPON_EVENT_ENDPOINT);
	return 0;
}
EXPORT_SYMBOL_GPL(xpon_endpoint_delete);

void xpon_endpoints_cleanup(struct xpon_device *xpon)
{
	struct xpon_endpoint *ep;
	unsigned long id;

	xa_for_each(&xpon->endpoints, id, ep)
		xpon_endpoint_delete(xpon, id);
}

void xpon_endpoints_update_carrier(struct xpon_device *xpon)
{
	struct xpon_endpoint *ep;
	unsigned long id;

	xa_for_each(&xpon->endpoints, id, ep) {
		if (ep->config.type == XPON_ENDPOINT_LLID) {
			struct xpon_endpoint_config cfg = ep->config;
			struct xpon_link_info link;

			spin_lock_bh(&xpon->state_lock);
			link = xpon->links[cfg.link_id - 1];
			spin_unlock_bh(&xpon->state_lock);
			if (link.state == XPON_LINK_REGISTERED &&
			    link.llid != cfg.wire_id) {
				cfg.wire_id = link.llid;
				if (xpon_endpoint_set(xpon, &cfg)) {
					netif_carrier_off(ep->netdev);
					continue;
				}
			}
		}
		if (xpon_endpoint_ready(ep) && netif_running(xpon->netdev))
			netif_carrier_on(ep->netdev);
		else
			netif_carrier_off(ep->netdev);
	}
}

bool xpon_endpoint_receive(struct xpon_device *xpon, u8 type, u16 wire_id,
			   struct sk_buff *skb)
{
	struct xpon_endpoint *ep;
	bool found = false;

	rcu_read_lock();
	ep = xa_load(&xpon->rx_endpoints, xpon_rx_key(type, wire_id));
	if (!ep)
		goto out;
	found = true;
	if (READ_ONCE(ep->config.type) != type ||
	    READ_ONCE(ep->config.wire_id) != wire_id ||
	    !netif_running(ep->netdev) || !xpon_endpoint_ready(ep) ||
	    !pskb_may_pull(skb, ETH_HLEN)) {
		dev_kfree_skb_any(skb);
		goto out;
	}
	skb->dev = ep->netdev;
	skb->pkt_type = PACKET_HOST;
	skb_record_rx_queue(skb, 0);
	skb->protocol = eth_type_trans(skb, ep->netdev);
	dev_sw_netstats_rx_add(ep->netdev, skb->len);
	netif_receive_skb(skb);
out:
	rcu_read_unlock();
	return found;
}
EXPORT_SYMBOL_GPL(xpon_endpoint_receive);

static void xpon_endpoint_work(struct work_struct *work)
{
	struct xpon_device *xpon =
		container_of(work, struct xpon_device, endpoint_work);
	struct xpon_endpoint_request *request;
	unsigned long id = 0;
	int ret;

	rtnl_lock();
	for (;;) {
		mutex_lock(&xpon->pending_lock);
		request = xa_find(&xpon->pending_endpoints, &id, U32_MAX,
				  XA_PRESENT);
		if (request)
			xa_erase(&xpon->pending_endpoints, id);
		mutex_unlock(&xpon->pending_lock);
		if (!request)
			break;
		if (!xpon->unregistering) {
			ret = request->remove ?
				      xpon_endpoint_delete(xpon,
							   request->config.id) :
				      xpon_endpoint_set(xpon, &request->config);
			if (ret && ret != -ENOENT)
				dev_warn(xpon->parent,
					 "endpoint %u update failed: %d\n",
					 request->config.id, ret);
		}
		kfree(request);
		id = 0;
	}
	rtnl_unlock();
}

int xpon_endpoint_queue(struct xpon_device *xpon,
			const struct xpon_endpoint_config *config, bool remove)
{
	struct xpon_endpoint_request *request, *old;
	int ret = 0;

	request = kmalloc(sizeof(*request), GFP_KERNEL);
	if (!request)
		return -ENOMEM;
	request->config = *config;
	request->remove = remove;
	mutex_lock(&xpon->pending_lock);
	if (READ_ONCE(xpon->unregistering)) {
		ret = -ENODEV;
		goto free;
	}
	old = xa_store(&xpon->pending_endpoints, config->id, request,
		       GFP_KERNEL);
	ret = xa_err(old);
	if (ret)
		goto free;
	kfree(old);
	schedule_work(&xpon->endpoint_work);
	mutex_unlock(&xpon->pending_lock);
	return 0;
free:
	mutex_unlock(&xpon->pending_lock);
	kfree(request);
	return ret;
}
EXPORT_SYMBOL_GPL(xpon_endpoint_queue);

void xpon_endpoint_work_init(struct xpon_device *xpon)
{
	xa_init(&xpon->pending_endpoints);
	mutex_init(&xpon->pending_lock);
	INIT_WORK(&xpon->endpoint_work, xpon_endpoint_work);
}

void xpon_endpoint_work_cleanup(struct xpon_device *xpon)
{
	struct xpon_endpoint_request *request;
	unsigned long id;

	cancel_work_sync(&xpon->endpoint_work);
	mutex_lock(&xpon->pending_lock);
	xa_for_each(&xpon->pending_endpoints, id, request) {
		xa_erase(&xpon->pending_endpoints, id);
		kfree(request);
	}
	mutex_unlock(&xpon->pending_lock);
}
