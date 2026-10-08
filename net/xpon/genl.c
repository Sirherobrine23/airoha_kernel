// SPDX-License-Identifier: GPL-2.0-only

#include <linux/etherdevice.h>
#include <linux/module.h>
#include <linux/rtnetlink.h>
#include <net/genetlink.h>
#include <net/net_namespace.h>
#include <net/xpon/oam.h>

#include "internal.h"

static const struct nla_policy xpon_genl_policy[XPON_ATTR_MAX + 1] = {
	[XPON_ATTR_IFINDEX] = { .type = NLA_U32 },
	[XPON_ATTR_MODE] = { .type = NLA_U32 },
	[XPON_ATTR_ID] = { .type = NLA_U32 },
	[XPON_ATTR_TYPE] = NLA_POLICY_MAX(NLA_U8, XPON_ENDPOINT_LLID),
	[XPON_ATTR_WIRE_ID] = { .type = NLA_U16 },
	[XPON_ATTR_SCHEDULER_ID] = { .type = NLA_U16 },
	[XPON_ATTR_LINK_ID] = { .type = NLA_U32 },
	[XPON_ATTR_QUEUE] = { .type = NLA_U8 },
	[XPON_ATTR_ENABLED] = NLA_POLICY_MAX(NLA_U8, 1),
	[XPON_ATTR_FRAME] = { .type = NLA_BINARY, .len = XPON_OAM_FRAME_MAX },
	[XPON_ATTR_REPORT_SETS] = { .type = NLA_U8 },
	[XPON_ATTR_REPORT_SET] = { .type = NLA_U8 },
	[XPON_ATTR_THRESHOLD_TQ] = { .type = NLA_U32 },
};

static struct genl_family xpon_genl_family;

static int xpon_genl_put_state(struct sk_buff *msg, struct xpon_device *xpon)
{
	struct xpon_state state;
	u32 caps = 0;

	spin_lock_bh(&xpon->state_lock);
	state = xpon->state;
	spin_unlock_bh(&xpon->state_lock);
	if (xpon->ops) {
		if (xpon->ops->xmit)
			caps |= XPON_CAP_ENDPOINTS;
		if (xpon->ops->set_link)
			caps |= XPON_CAP_LINK_ADMIN;
		if (xpon->ops->get_report && xpon->ops->set_report)
			caps |= XPON_CAP_REPORT;
	}
	if (xpon->oam)
		caps |= XPON_CAP_OAM;
	if (nla_put_u32(msg, XPON_ATTR_IFINDEX, xpon->netdev->ifindex) ||
	    nla_put_u32(msg, XPON_ATTR_MODE, state.mode) ||
	    nla_put_u32(msg, XPON_ATTR_AVAILABLE_MODES, xpon->modes) ||
	    nla_put_u8(msg, XPON_ATTR_REGISTRATION, state.registration) ||
	    nla_put_u8(msg, XPON_ATTR_CARRIER, state.carrier) ||
	    ((state.valid & XPON_STATE_F_SIGNAL) &&
	     nla_put_u8(msg, XPON_ATTR_SIGNAL_DETECT, state.signal_detect)) ||
	    ((state.valid & XPON_STATE_F_LOS) &&
	     nla_put_u8(msg, XPON_ATTR_LOS, state.los)) ||
	    nla_put_u32(msg, XPON_ATTR_CAPABILITIES, caps) ||
	    nla_put_u16(msg, XPON_ATTR_MAX_LINKS, xpon->max_links) ||
	    nla_put_u16(msg, XPON_ATTR_MAX_ENDPOINTS, xpon->max_endpoints) ||
	    nla_put_u8(msg, XPON_ATTR_MAX_QUEUES, xpon->max_queues) ||
	    nla_put_u8(msg, XPON_ATTR_MAX_REPORT_SETS, xpon->report_sets) ||
	    nla_put_u8(msg, XPON_ATTR_ONU_TYPE, xpon->onu_type) ||
	    nla_put_u8(msg, XPON_ATTR_UNI_COUNT, xpon->uni_count))
		return -EMSGSIZE;
	return 0;
}

static int xpon_genl_put_object(struct sk_buff *msg, struct xpon_device *xpon,
				u8 cmd, u32 id)
{
	struct xpon_endpoint *ep;
	struct net_device *dev;
	struct xpon_link_info link;
	int ret;

	if (nla_put_u32(msg, XPON_ATTR_IFINDEX, xpon->netdev->ifindex) ||
	    nla_put_u32(msg, XPON_ATTR_ID, id))
		return -EMSGSIZE;
	switch (cmd) {
	case XPON_CMD_UNI_GET:
		if (!id || id > xpon->uni_count)
			return -ENOENT;
		dev = xpon_uni_get_netdev(xpon, id);
		if (!dev)
			return 0; /* Inventory exists before the Ethernet driver probes. */
		ret = nla_put_u32(msg, XPON_ATTR_NETDEV_IFINDEX,
				  dev->ifindex) ||
		      nla_put_string(msg, XPON_ATTR_NAME, dev->name) ||
		      nla_put_u8(msg, XPON_ATTR_ENABLED,
				 !!(dev->flags & IFF_UP)) ||
		      nla_put_u8(msg, XPON_ATTR_CARRIER, netif_carrier_ok(dev));
		put_device(&dev->dev);
		return ret ? -EMSGSIZE : 0;
	case XPON_CMD_LINK_GET:
		if (!id || id > xpon->max_links)
			return -ENOENT;
		spin_lock_bh(&xpon->state_lock);
		link = xpon->links[id - 1];
		spin_unlock_bh(&xpon->state_lock);
		if (nla_put_u8(msg, XPON_ATTR_LINK_STATE, link.state) ||
		    nla_put_u8(msg, XPON_ATTR_ENABLED, link.enabled) ||
		    nla_put_u16(msg, XPON_ATTR_WIRE_ID, link.llid) ||
		    nla_put_u64_64bit(msg, XPON_ATTR_RX_PACKETS, link.rx_oam,
				      XPON_ATTR_PAD) ||
		    nla_put_u64_64bit(msg, XPON_ATTR_TX_PACKETS, link.tx_oam,
				      XPON_ATTR_PAD))
			return -EMSGSIZE;
		return 0;
	case XPON_CMD_ENDPOINT_GET:
		ep = xa_load(&xpon->endpoints, id);
		if (!ep)
			return -ENOENT;
		if (nla_put_u8(msg, XPON_ATTR_TYPE, ep->config.type) ||
		    nla_put_u16(msg, XPON_ATTR_WIRE_ID, ep->config.wire_id) ||
		    nla_put_u16(msg, XPON_ATTR_SCHEDULER_ID,
				ep->config.scheduler_id) ||
		    nla_put_u32(msg, XPON_ATTR_LINK_ID, ep->config.link_id) ||
		    nla_put_u8(msg, XPON_ATTR_QUEUE, ep->config.queue) ||
		    nla_put_u8(msg, XPON_ATTR_ENABLED, ep->config.enabled) ||
		    nla_put_u32(msg, XPON_ATTR_NETDEV_IFINDEX,
				ep->netdev->ifindex) ||
		    nla_put_string(msg, XPON_ATTR_NAME, ep->netdev->name))
			return -EMSGSIZE;
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

static int xpon_genl_doit(struct sk_buff *skb, struct genl_info *info)
{
	struct nlattr **a = info->attrs;
	struct xpon_endpoint_config config = {};
	struct xpon_report_config report = {};
	struct xpon_device *xpon;
	struct sk_buff *msg = NULL;
	u32 id = a[XPON_ATTR_ID] ? nla_get_u32(a[XPON_ATTR_ID]) : 0;
	u8 cmd = info->genlhdr->cmd;
	bool exists;
	void *hdr;
	int ret = 0;

	if (info->genlhdr->version != XPON_GENL_VERSION)
		return -EPROTONOSUPPORT;
	if (!net_eq(genl_info_net(info), &init_net))
		return -EOPNOTSUPP;
	if (!a[XPON_ATTR_IFINDEX])
		return -EINVAL;
	rtnl_lock();
	xpon = xpon_device_find_by_ifindex(nla_get_u32(a[XPON_ATTR_IFINDEX]));
	if (!xpon) {
		ret = -ENODEV;
		goto unlock;
	}
	switch (cmd) {
	case XPON_CMD_SET_MODE:
		ret = a[XPON_ATTR_MODE] ?
			      xpon_device_set_mode(
				      xpon, nla_get_u32(a[XPON_ATTR_MODE])) :
			      -EINVAL;
		goto out;
	case XPON_CMD_LINK_SET:
		if (!id || id > xpon->max_links || !a[XPON_ATTR_ENABLED]) {
			ret = -EINVAL;
			goto out;
		}
		ret = xpon->ops && xpon->ops->set_link ?
			      xpon->ops->set_link(
				      xpon, id,
				      nla_get_u8(a[XPON_ATTR_ENABLED])) :
			      -EOPNOTSUPP;
		goto out;
	case XPON_CMD_ENDPOINT_NEW:
	case XPON_CMD_ENDPOINT_SET:
		if (!id || !a[XPON_ATTR_TYPE] || !a[XPON_ATTR_WIRE_ID] ||
		    !a[XPON_ATTR_QUEUE] || !a[XPON_ATTR_ENABLED]) {
			ret = -EINVAL;
			goto out;
		}
		exists = !!xa_load(&xpon->endpoints, id);
		if ((cmd == XPON_CMD_ENDPOINT_NEW && exists) ||
		    (cmd == XPON_CMD_ENDPOINT_SET && !exists)) {
			ret = cmd == XPON_CMD_ENDPOINT_NEW ? -EEXIST : -ENOENT;
			goto out;
		}
		config.id = id;
		config.type = nla_get_u8(a[XPON_ATTR_TYPE]);
		config.wire_id = nla_get_u16(a[XPON_ATTR_WIRE_ID]);
		if (config.type == XPON_ENDPOINT_GEM) {
			if (!a[XPON_ATTR_SCHEDULER_ID]) {
				ret = -EINVAL;
				goto out;
			}
			config.scheduler_id =
				nla_get_u16(a[XPON_ATTR_SCHEDULER_ID]);
		} else {
			if (!a[XPON_ATTR_LINK_ID]) {
				ret = -EINVAL;
				goto out;
			}
			config.link_id = nla_get_u32(a[XPON_ATTR_LINK_ID]);
		}
		config.queue = nla_get_u8(a[XPON_ATTR_QUEUE]);
		config.enabled = nla_get_u8(a[XPON_ATTR_ENABLED]);
		ret = xpon_endpoint_set(xpon, &config);
		goto out;
	case XPON_CMD_ENDPOINT_DEL:
		ret = xpon_endpoint_delete(xpon, id);
		goto out;
	case XPON_CMD_OAM_TX:
		ret = a[XPON_ATTR_FRAME] ?
			      xpon_oam_transmit(xpon, id,
						nla_data(a[XPON_ATTR_FRAME]),
						nla_len(a[XPON_ATTR_FRAME])) :
			      -EINVAL;
		goto out;
	case XPON_CMD_REPORT_GET:
	case XPON_CMD_REPORT_SET:
		if (!id || id > xpon->max_links || !a[XPON_ATTR_QUEUE] ||
		    !a[XPON_ATTR_REPORT_SET]) {
			ret = -EINVAL;
			goto out;
		}
		report.queue = nla_get_u8(a[XPON_ATTR_QUEUE]);
		report.set = nla_get_u8(a[XPON_ATTR_REPORT_SET]);
		if (report.queue >= xpon->max_queues ||
		    report.set >= xpon->report_sets) {
			ret = -EINVAL;
			goto out;
		}
		if (cmd == XPON_CMD_REPORT_SET) {
			if (!a[XPON_ATTR_REPORT_SETS] ||
			    !a[XPON_ATTR_THRESHOLD_TQ]) {
				ret = -EINVAL;
				goto out;
			}
			report.sets = nla_get_u8(a[XPON_ATTR_REPORT_SETS]);
			report.threshold_tq =
				nla_get_u32(a[XPON_ATTR_THRESHOLD_TQ]);
			if (!report.sets || report.sets > xpon->report_sets ||
			    report.set >= report.sets) {
				ret = -EINVAL;
				goto out;
			}
			ret = xpon->ops && xpon->ops->set_report ?
				      xpon->ops->set_report(xpon, id, &report) :
				      -EOPNOTSUPP;
			goto out;
		}
		ret = xpon->ops && xpon->ops->get_report ?
			      xpon->ops->get_report(xpon, id, &report) :
			      -EOPNOTSUPP;
		if (ret)
			goto out;
		break;
	}
	msg = genlmsg_new(NLMSG_DEFAULT_SIZE, GFP_KERNEL);
	if (!msg) {
		ret = -ENOMEM;
		goto out;
	}
	hdr = genlmsg_put_reply(msg, info, &xpon_genl_family, 0, cmd);
	if (!hdr) {
		ret = -EMSGSIZE;
		goto free;
	}
	if (cmd == XPON_CMD_UNI_GET || cmd == XPON_CMD_LINK_GET ||
	    cmd == XPON_CMD_ENDPOINT_GET)
		ret = xpon_genl_put_object(msg, xpon, cmd, id);
	else
		ret = xpon_genl_put_state(msg, xpon);
	if (!ret && cmd == XPON_CMD_REPORT_GET &&
	    (nla_put_u32(msg, XPON_ATTR_ID, id) ||
	     nla_put_u8(msg, XPON_ATTR_QUEUE, report.queue) ||
	     nla_put_u8(msg, XPON_ATTR_REPORT_SET, report.set) ||
	     nla_put_u8(msg, XPON_ATTR_REPORT_SETS, report.sets) ||
	     nla_put_u32(msg, XPON_ATTR_THRESHOLD_TQ, report.threshold_tq)))
		ret = -EMSGSIZE;
	if (ret)
		goto free;
	genlmsg_end(msg, hdr);
	ret = genlmsg_reply(msg, info);
	goto out;
free:
	nlmsg_free(msg);
out:
	xpon_device_put(xpon);
unlock:
	rtnl_unlock();
	return ret;
}

static int xpon_genl_dump(struct sk_buff *msg, struct netlink_callback *cb)
{
	const struct genl_info *info = genl_info_dump(cb);
	struct xpon_device *xpon;
	unsigned long id = cb->args[0] ?: 1;
	u8 cmd = info->genlhdr->cmd;
	u32 last;
	void *hdr;
	int ret = 0;

	if (info->genlhdr->version != XPON_GENL_VERSION)
		return -EPROTONOSUPPORT;
	if (!net_eq(genl_info_net(info), &init_net))
		return -EOPNOTSUPP;
	if (!info->attrs[XPON_ATTR_IFINDEX])
		return -EINVAL;
	rtnl_lock();
	xpon = xpon_device_find_by_ifindex(
		nla_get_u32(info->attrs[XPON_ATTR_IFINDEX]));
	if (!xpon) {
		ret = -ENODEV;
		goto unlock;
	}
	last = cmd == XPON_CMD_UNI_GET ? xpon->uni_count : xpon->max_links;
	for (;;) {
		if (cmd == XPON_CMD_ENDPOINT_GET) {
			if (!xa_find(&xpon->endpoints, &id, U32_MAX,
				     XA_PRESENT))
				break;
		} else if (id > last) {
			break;
		}
		hdr = genlmsg_put(msg, NETLINK_CB(cb->skb).portid,
				  cb->nlh->nlmsg_seq, &xpon_genl_family,
				  NLM_F_MULTI, cmd);
		if (!hdr)
			break;
		ret = xpon_genl_put_object(msg, xpon, cmd, id);
		if (ret) {
			genlmsg_cancel(msg, hdr);
			break;
		}
		genlmsg_end(msg, hdr);
		id++;
	}
	cb->args[0] = id;
	xpon_device_put(xpon);
unlock:
	rtnl_unlock();
	return msg->len ?: ret;
}

#define XPON_OP(_cmd, _flags)         \
	{ .cmd = _cmd,                \
	  .flags = _flags,            \
	  .policy = xpon_genl_policy, \
	  .doit = xpon_genl_doit }
#define XPON_DUMP_OP(_cmd)            \
	{ .cmd = _cmd,                \
	  .policy = xpon_genl_policy, \
	  .doit = xpon_genl_doit,     \
	  .dumpit = xpon_genl_dump }

static const struct genl_ops xpon_genl_ops[] = {
	XPON_OP(XPON_CMD_GET, 0),
	XPON_OP(XPON_CMD_GET_CAPS, 0),
	XPON_OP(XPON_CMD_GET_STATE, 0),
	XPON_OP(XPON_CMD_SET_MODE, GENL_ADMIN_PERM),
	XPON_DUMP_OP(XPON_CMD_UNI_GET),
	XPON_DUMP_OP(XPON_CMD_LINK_GET),
	XPON_DUMP_OP(XPON_CMD_ENDPOINT_GET),
	XPON_OP(XPON_CMD_LINK_SET, GENL_ADMIN_PERM),
	XPON_OP(XPON_CMD_ENDPOINT_NEW, GENL_ADMIN_PERM),
	XPON_OP(XPON_CMD_ENDPOINT_SET, GENL_ADMIN_PERM),
	XPON_OP(XPON_CMD_ENDPOINT_DEL, GENL_ADMIN_PERM),
	XPON_OP(XPON_CMD_OAM_TX, GENL_ADMIN_PERM),
	XPON_OP(XPON_CMD_REPORT_GET, 0),
	XPON_OP(XPON_CMD_REPORT_SET, GENL_ADMIN_PERM),
};

static const struct genl_multicast_group xpon_mcgrps[] = {
	{ .name = "events" },
	{ .name = "oam", .flags = GENL_MCAST_CAP_NET_ADMIN },
};

static struct genl_family xpon_genl_family = {
	.name = XPON_GENL_NAME,
	.version = XPON_GENL_VERSION,
	.maxattr = XPON_ATTR_MAX,
	.module = THIS_MODULE,
	.ops = xpon_genl_ops,
	.n_ops = ARRAY_SIZE(xpon_genl_ops),
	.mcgrps = xpon_mcgrps,
	.n_mcgrps = ARRAY_SIZE(xpon_mcgrps),
};

void xpon_genl_notify(struct xpon_device *xpon, unsigned long changed)
{
	struct sk_buff *msg;
	void *hdr;

	msg = genlmsg_new(NLMSG_DEFAULT_SIZE, GFP_KERNEL);
	if (!msg)
		return;
	hdr = genlmsg_put(msg, 0, 0, &xpon_genl_family, 0, XPON_CMD_EVENT);
	if (!hdr || xpon_genl_put_state(msg, xpon) ||
	    nla_put_u32(msg, XPON_ATTR_CHANGED, changed))
		goto free;
	genlmsg_end(msg, hdr);
	genlmsg_multicast(&xpon_genl_family, msg, 0, 0, GFP_KERNEL);
	return;
free:
	nlmsg_free(msg);
}

void xpon_genl_oam_receive(struct xpon_device *xpon, u32 id,
			   struct sk_buff *skb)
{
	struct sk_buff *msg;
	struct nlattr *frame;
	void *hdr;

	msg = genlmsg_new(skb->len + 128, GFP_ATOMIC);
	if (!msg)
		return;
	hdr = genlmsg_put(msg, 0, 0, &xpon_genl_family, 0, XPON_CMD_OAM_RX);
	if (!hdr ||
	    nla_put_u32(msg, XPON_ATTR_IFINDEX, xpon->netdev->ifindex) ||
	    nla_put_u32(msg, XPON_ATTR_ID, id))
		goto free;
	frame = nla_reserve(msg, XPON_ATTR_FRAME, skb->len);
	if (!frame || skb_copy_bits(skb, 0, nla_data(frame), skb->len))
		goto free;
	genlmsg_end(msg, hdr);
	genlmsg_multicast(&xpon_genl_family, msg, 0, 1, GFP_ATOMIC);
	return;
free:
	nlmsg_free(msg);
}
EXPORT_SYMBOL_GPL(xpon_genl_oam_receive);

int xpon_genl_init(void)
{
	return genl_register_family(&xpon_genl_family);
}

void xpon_genl_exit(void)
{
	genl_unregister_family(&xpon_genl_family);
}
