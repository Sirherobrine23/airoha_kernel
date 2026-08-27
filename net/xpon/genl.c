// SPDX-License-Identifier: GPL-2.0-only

#include <linux/module.h>
#include <linux/netdevice.h>
#include <net/genetlink.h>
#include <net/xpon.h>

#include "internal.h"

static const struct nla_policy xpon_genl_policy[XPON_ATTR_MAX + 1] = {
	[XPON_ATTR_IFINDEX] = { .type = NLA_U32 },
	[XPON_ATTR_MODE] = { .type = NLA_U8 },
};

static struct genl_family xpon_genl_family;

static int xpon_genl_put_state(struct sk_buff *msg, struct xpon_device *xpon)
{
	if (nla_put_u32(msg, XPON_ATTR_IFINDEX, xpon->netdev->ifindex) ||
	    nla_put_u8(msg, XPON_ATTR_MODE, xpon->state.mode) ||
	    nla_put_u32(msg, XPON_ATTR_AVAILABLE_MODES, xpon->modes) ||
	    nla_put_u8(msg, XPON_ATTR_REGISTRATION, xpon->state.registration) ||
	    nla_put_u8(msg, XPON_ATTR_CARRIER, xpon->state.carrier) ||
	    nla_put_u8(msg, XPON_ATTR_SIGNAL_DETECT, xpon->state.signal_detect) ||
	    nla_put_u8(msg, XPON_ATTR_LOS, xpon->state.los))
		return -EMSGSIZE;

	return 0;
}

static int xpon_genl_get(struct sk_buff *skb, struct genl_info *info)
{
	struct xpon_device *xpon;
	struct sk_buff *msg;
	void *hdr;
	int ret;

	if (!info->attrs[XPON_ATTR_IFINDEX])
		return -EINVAL;

	xpon = xpon_device_find_by_ifindex(
		nla_get_u32(info->attrs[XPON_ATTR_IFINDEX]));
	if (!xpon)
		return -ENODEV;

	msg = genlmsg_new(NLMSG_DEFAULT_SIZE, GFP_KERNEL);
	if (!msg) {
		ret = -ENOMEM;
		goto out;
	}

	hdr = genlmsg_put_reply(msg, info, &xpon_genl_family, 0, XPON_CMD_GET);
	if (!hdr) {
		nlmsg_free(msg);
		ret = -EMSGSIZE;
		goto out;
	}

	ret = xpon_genl_put_state(msg, xpon);
	if (ret) {
		nlmsg_free(msg);
		goto out;
	}
	genlmsg_end(msg, hdr);
	ret = genlmsg_reply(msg, info);
out:
	xpon_device_put(xpon);
	return ret;
}

static int xpon_genl_set_mode(struct sk_buff *skb, struct genl_info *info)
{
	struct xpon_device *xpon;
	u8 mode;
	int ret;

	if (!info->attrs[XPON_ATTR_IFINDEX] || !info->attrs[XPON_ATTR_MODE])
		return -EINVAL;
	mode = nla_get_u8(info->attrs[XPON_ATTR_MODE]);

	xpon = xpon_device_find_by_ifindex(
		nla_get_u32(info->attrs[XPON_ATTR_IFINDEX]));
	if (!xpon)
		return -ENODEV;
	ret = xpon_device_set_mode(xpon, mode);
	xpon_device_put(xpon);
	return ret;
}

static const struct genl_ops xpon_genl_ops[] = {
	{
		.cmd = XPON_CMD_GET,
		.policy = xpon_genl_policy,
		.doit = xpon_genl_get,
	},
	{
		.cmd = XPON_CMD_GET_STATE,
		.policy = xpon_genl_policy,
		.doit = xpon_genl_get,
	},
	{
		.cmd = XPON_CMD_SET_MODE,
		.flags = GENL_ADMIN_PERM,
		.policy = xpon_genl_policy,
		.doit = xpon_genl_set_mode,
	},
};

static const struct genl_multicast_group xpon_mcgrps[] = {
	{ .name = "events" },
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
	if (!hdr)
		goto free;
	if (xpon_genl_put_state(msg, xpon) ||
	    nla_put_u32(msg, XPON_ATTR_CHANGED, changed))
		goto free;
	genlmsg_end(msg, hdr);
	genlmsg_multicast(&xpon_genl_family, msg, 0, 0, GFP_KERNEL);
	return;
free:
	nlmsg_free(msg);
}

int xpon_genl_init(void)
{
	return genl_register_family(&xpon_genl_family);
}

void xpon_genl_exit(void)
{
	genl_unregister_family(&xpon_genl_family);
}
