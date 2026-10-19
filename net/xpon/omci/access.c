// SPDX-License-Identifier: GPL-2.0-only
/* Local access management shared by OMCC providers and copper frontends. */

#include <linux/device.h>
#include <linux/err.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/rtnetlink.h>
#include <linux/slab.h>
#include <linux/xarray.h>
#include <net/genetlink.h>
#include <net/xpon.h>

#include "internal.h"

struct omci_line {
	struct device *parent;
	struct net_device *netdev;
	const struct omci_line_ops *ops;
	void *priv;
	u32 id;
};

static DEFINE_XARRAY_ALLOC(omci_lines);
/* Held across provider callbacks; unregister waits for all readers. */
static DEFINE_MUTEX(omci_lines_lock);

struct omci_line *omci_line_register(struct device *parent,
				     struct net_device *netdev,
				     const struct omci_line_ops *ops,
				     void *priv)
{
	struct omci_line *line, *existing;
	unsigned long index;
	int ret;

	if (!parent || !netdev || !ops || !ops->get_status)
		return ERR_PTR(-EINVAL);
	/* The existing OMCI family is only visible in the initial namespace. */
	if (!net_eq(dev_net(netdev), &init_net))
		return ERR_PTR(-EOPNOTSUPP);

	line = kzalloc(sizeof(*line), GFP_KERNEL);
	if (!line)
		return ERR_PTR(-ENOMEM);
	line->parent = get_device(parent);
	line->netdev = netdev;
	dev_hold(netdev);
	line->ops = ops;
	line->priv = priv;

	mutex_lock(&omci_lines_lock);
	xa_for_each(&omci_lines, index, existing) {
		if (existing->netdev == netdev) {
			ret = -EBUSY;
			goto out_unlock;
		}
	}
	/* Leave room for a dump cursor one past the largest ID on 32-bit. */
	ret = xa_alloc(&omci_lines, &line->id, line,
		       XA_LIMIT(1, INT_MAX - 1), GFP_KERNEL);
out_unlock:
	mutex_unlock(&omci_lines_lock);
	if (ret) {
		dev_put(netdev);
		put_device(line->parent);
		kfree(line);
		return ERR_PTR(ret);
	}
	return line;
}
EXPORT_SYMBOL_GPL(omci_line_register);

void omci_line_unregister(struct omci_line *line)
{
	if (!line)
		return;
	mutex_lock(&omci_lines_lock);
	xa_erase(&omci_lines, line->id);
	mutex_unlock(&omci_lines_lock);
	dev_put(line->netdev);
	put_device(line->parent);
	kfree(line);
}
EXPORT_SYMBOL_GPL(omci_line_unregister);

u32 omci_line_id(const struct omci_line *line)
{
	return line->id;
}
EXPORT_SYMBOL_GPL(omci_line_id);

static int omci_line_snapshot(struct omci_line *line,
			      struct omci_line_status *status)
{
	int ret;

	memset(status, 0, sizeof(*status));
	if (!net_eq(dev_net(line->netdev), &init_net))
		return -ENODEV;
	ret = line->ops->get_status(line->priv, status);
	if (ret)
		return ret;
	if (status->access > OMCI_ACCESS_XDSL ||
	    status->state > OMCI_LINE_UP ||
	    status->flags & ~(OMCI_LINE_F_STARTED | OMCI_LINE_F_CARRIER |
			      OMCI_LINE_F_RATES_VALID))
		return -EINVAL;
	return 0;
}

int omci_line_read(u32 id, struct omci_line_status *status)
{
	struct omci_line_status snapshot;
	struct omci_line *line;
	int ret = -ENODEV;

	if (!status)
		return -EINVAL;
	memset(status, 0, sizeof(*status));
	mutex_lock(&omci_lines_lock);
	line = xa_load(&omci_lines, id);
	if (line) {
		ret = omci_line_snapshot(line, &snapshot);
		if (!ret)
			*status = snapshot;
	}
	mutex_unlock(&omci_lines_lock);
	return ret;
}

int omci_line_retrain(u32 id)
{
	struct omci_line_status status;
	struct omci_line *line;
	int ret = -ENODEV;

	mutex_lock(&omci_lines_lock);
	line = xa_load(&omci_lines, id);
	if (!line)
		goto out;
	ret = omci_line_snapshot(line, &status);
	if (ret)
		goto out;
	if (status.access != OMCI_ACCESS_XDSL || !line->ops->retrain) {
		ret = -EOPNOTSUPP;
		goto out;
	}
	ret = line->ops->retrain(line->priv);
out:
	mutex_unlock(&omci_lines_lock);
	return ret;
}

void omci_line_status_from_xpon(const struct xpon_state *state,
				bool started, struct omci_line_status *status)
{
	memset(status, 0, sizeof(*status));
	switch (state->mode) {
	case XPON_MODE_GPON:
		status->access = OMCI_ACCESS_GPON;
		break;
	case XPON_MODE_XGPON:
		status->access = OMCI_ACCESS_XGPON;
		break;
	case XPON_MODE_XGSPON:
		status->access = OMCI_ACCESS_XGSPON;
		break;
	default:
		/* A mode switch must not leave a stale GPON label behind. */
		status->state = OMCI_LINE_UNAVAILABLE;
		return;
	}
	if (started)
		status->flags |= OMCI_LINE_F_STARTED;
	if (state->carrier)
		status->flags |= OMCI_LINE_F_CARRIER;
	switch (state->registration) {
	case XPON_REGISTRATION_OPERATIONAL:
		status->state = OMCI_LINE_UP;
		break;
	case XPON_REGISTRATION_DISCOVERY:
	case XPON_REGISTRATION_REGISTERING:
		status->state = OMCI_LINE_TRAINING;
		break;
	default:
		status->state = OMCI_LINE_DOWN;
		break;
	}
	/* Nominal PON capacity is not a measurement of the subscriber rate. */
}

int omci_line_put_status(struct sk_buff *msg,
			 const struct omci_line_status *status)
{
	if (nla_put_u8(msg, OMCI_ATTR_ACCESS_TYPE, status->access) ||
	    nla_put_u8(msg, OMCI_ATTR_LINE_STATE, status->state) ||
	    nla_put_u32(msg, OMCI_ATTR_LINE_FLAGS, status->flags))
		return -EMSGSIZE;
	if ((status->flags & OMCI_LINE_F_RATES_VALID) &&
	    (nla_put_u64_64bit(msg, OMCI_ATTR_DOWNSTREAM_BPS,
			      status->downstream_bps, OMCI_ATTR_PAD) ||
	     nla_put_u64_64bit(msg, OMCI_ATTR_UPSTREAM_BPS,
			       status->upstream_bps, OMCI_ATTR_PAD)))
		return -EMSGSIZE;
	return 0;
}

static int omci_line_fill(struct sk_buff *msg, struct omci_line *line,
			  const struct genl_family *family,
			  u32 portid, u32 seq, int flags)
{
	struct omci_line_status status;
	u32 capabilities = 0;
	void *hdr;
	int ret;

	ret = omci_line_snapshot(line, &status);
	if (ret)
		return ret;
	if (status.access == OMCI_ACCESS_XDSL && line->ops->retrain)
		capabilities |= OMCI_LINE_CAP_RETRAIN;
	if (status.access >= OMCI_ACCESS_GPON &&
	    status.access <= OMCI_ACCESS_XGSPON)
		capabilities |= OMCI_LINE_CAP_OMCC;

	hdr = genlmsg_put(msg, portid, seq, family, flags, OMCI_CMD_LINE_GET);
	if (!hdr)
		return -EMSGSIZE;
	if (nla_put_u32(msg, OMCI_ATTR_LINE_ID, line->id) ||
	    nla_put_u32(msg, OMCI_ATTR_IFINDEX, line->netdev->ifindex) ||
	    nla_put_string(msg, OMCI_ATTR_NAME, dev_name(line->parent)) ||
	    nla_put_u32(msg, OMCI_ATTR_LINE_CAPABILITIES, capabilities) ||
	    omci_line_put_status(msg, &status))
		goto cancel;
	genlmsg_end(msg, hdr);
	return 0;
cancel:
	genlmsg_cancel(msg, hdr);
	return -EMSGSIZE;
}

int omci_line_cmd_get(struct sk_buff *skb, struct genl_info *info,
		      const struct genl_family *family)
{
	struct omci_line *line;
	struct sk_buff *msg;
	int ret = -ENODEV;

	if (!info->attrs[OMCI_ATTR_LINE_ID])
		return -EINVAL;
	msg = genlmsg_new(NLMSG_DEFAULT_SIZE, GFP_KERNEL);
	if (!msg)
		return -ENOMEM;
	/* Keep netns moves and ifindex changes outside the reply snapshot. */
	rtnl_lock();
	mutex_lock(&omci_lines_lock);
	line = xa_load(&omci_lines, nla_get_u32(info->attrs[OMCI_ATTR_LINE_ID]));
	if (line)
		ret = omci_line_fill(msg, line, family, info->snd_portid,
				     info->snd_seq, 0);
	mutex_unlock(&omci_lines_lock);
	rtnl_unlock();
	if (ret) {
		nlmsg_free(msg);
		return ret;
	}
	return genlmsg_reply(msg, info);
}

int omci_line_cmd_retrain(struct sk_buff *skb, struct genl_info *info)
{
	int ret;

	if (!info->attrs[OMCI_ATTR_LINE_ID])
		return -EINVAL;
	rtnl_lock();
	ret = omci_line_retrain(nla_get_u32(info->attrs[OMCI_ATTR_LINE_ID]));
	rtnl_unlock();
	return ret;
}

int omci_line_dump(struct sk_buff *skb, struct netlink_callback *cb,
		   const struct genl_family *family)
{
	unsigned long index = cb->args[0];
	struct omci_line *line;
	int ret = 0;

	rtnl_lock();
	mutex_lock(&omci_lines_lock);
	xa_for_each_start(&omci_lines, index, line, index) {
		/* A device moved out of init_net must not leak its new ifindex. */
		if (!net_eq(dev_net(line->netdev), &init_net)) {
			cb->args[0] = index + 1;
			continue;
		}
		ret = omci_line_fill(skb, line, family,
				     NETLINK_CB(cb->skb).portid,
				     cb->nlh->nlmsg_seq, NLM_F_MULTI);
		if (ret)
			break;
		cb->args[0] = index + 1;
	}
	mutex_unlock(&omci_lines_lock);
	rtnl_unlock();
	if (ret && ret != -EMSGSIZE)
		return ret;
	return skb->len ? skb->len : ret;
}
