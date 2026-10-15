// SPDX-License-Identifier: GPL-2.0-only
/* Lifecycle and identity tests using a provider without hardware. */

#include <kunit/test.h>
#include <linux/etherdevice.h>
#include <linux/rtnetlink.h>

#include "internal.h"

struct xpon_test_context {
	struct device *parent;
	struct net_device *netdev;
	struct xpon_device *xpon;
};

static netdev_tx_t xpon_test_xmit(struct xpon_device *xpon, struct sk_buff *skb,
				  const struct xpon_endpoint_config *config)
{
	kfree_skb(skb);
	return NETDEV_TX_OK;
}

static netdev_tx_t xpon_test_parent_xmit(struct sk_buff *skb,
					 struct net_device *dev)
{
	kfree_skb(skb);
	return NETDEV_TX_OK;
}

static const struct net_device_ops xpon_test_netdev_ops = {
	.ndo_start_xmit = xpon_test_parent_xmit,
};

static const struct xpon_device_ops xpon_test_ops = {
	.xmit = xpon_test_xmit,
};

static int xpon_test_init(struct kunit *test)
{
	struct xpon_test_context *ctx;
	struct xpon_device_desc desc = {
		.mode = XPON_MODE_EPON,
		.modes = XPON_MODE_CAP(XPON_MODE_EPON) |
			 XPON_MODE_CAP(XPON_MODE_GPON),
		.max_links = 2,
		.max_endpoints = 2,
		.max_queues = 2,
		.ops = &xpon_test_ops,
	};
	int ret;

	ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;
	test->priv = ctx;
	ctx->parent = root_device_register("xpon-kunit");
	if (IS_ERR(ctx->parent))
		return PTR_ERR(ctx->parent);
	ctx->netdev =
		alloc_netdev(0, "xpon-test%d", NET_NAME_ENUM, ether_setup);
	if (!ctx->netdev) {
		ret = -ENOMEM;
		goto parent;
	}
	ctx->netdev->netdev_ops = &xpon_test_netdev_ops;
	eth_hw_addr_random(ctx->netdev);
	ret = register_netdev(ctx->netdev);
	if (ret)
		goto free;
	desc.netdev = ctx->netdev;
	ctx->xpon = xpon_device_register(ctx->parent, &desc);
	if (!IS_ERR(ctx->xpon))
		return 0;
	ret = PTR_ERR(ctx->xpon);
	unregister_netdev(ctx->netdev);
free:
	free_netdev(ctx->netdev);
parent:
	root_device_unregister(ctx->parent);
	return ret;
}

static void xpon_test_exit(struct kunit *test)
{
	struct xpon_test_context *ctx = test->priv;

	xpon_device_unregister(ctx->xpon);
	unregister_netdev(ctx->netdev);
	free_netdev(ctx->netdev);
	root_device_unregister(ctx->parent);
}

static struct xpon_endpoint_config xpon_test_config(u32 id, u16 wire)
{
	return (struct xpon_endpoint_config){
		.id = id,
		.type = XPON_ENDPOINT_LLID,
		.wire_id = wire,
		.link_id = 1,
		.enabled = true,
	};
}

static void xpon_test_duplicate_wire(struct kunit *test)
{
	struct xpon_test_context *ctx = test->priv;
	struct xpon_endpoint_config first = xpon_test_config(1, 7);
	struct xpon_endpoint_config second = xpon_test_config(2, 7);
	int a, b;

	rtnl_lock();
	a = xpon_endpoint_set(ctx->xpon, &first);
	b = xpon_endpoint_set(ctx->xpon, &second);
	rtnl_unlock();
	KUNIT_EXPECT_EQ(test, a, 0);
	KUNIT_EXPECT_EQ(test, b, -EEXIST);
}

static void xpon_test_capacity(struct kunit *test)
{
	struct xpon_test_context *ctx = test->priv;
	int ret[3], i;

	rtnl_lock();
	for (i = 0; i < 3; i++) {
		struct xpon_endpoint_config cfg =
			xpon_test_config(i + 1, i + 7);

		ret[i] = xpon_endpoint_set(ctx->xpon, &cfg);
	}
	rtnl_unlock();
	KUNIT_EXPECT_EQ(test, ret[0], 0);
	KUNIT_EXPECT_EQ(test, ret[1], 0);
	KUNIT_EXPECT_EQ(test, ret[2], -ENOSPC);
}

static void xpon_test_rebind_preserves_interface(struct kunit *test)
{
	struct xpon_test_context *ctx = test->priv;
	struct xpon_endpoint_config cfg = xpon_test_config(1, 7);
	struct xpon_endpoint *ep;
	int create, update, first = 0, second = 0;

	rtnl_lock();
	create = xpon_endpoint_set(ctx->xpon, &cfg);
	ep = xa_load(&ctx->xpon->endpoints, cfg.id);
	if (ep)
		first = ep->netdev->ifindex;
	cfg.queue = 1;
	update = xpon_endpoint_set(ctx->xpon, &cfg);
	ep = xa_load(&ctx->xpon->endpoints, cfg.id);
	if (ep)
		second = ep->netdev->ifindex;
	rtnl_unlock();
	KUNIT_EXPECT_EQ(test, create, 0);
	KUNIT_EXPECT_EQ(test, update, 0);
	KUNIT_EXPECT_GT(test, first, 0);
	KUNIT_EXPECT_EQ(test, first, second);
}

static void xpon_test_failed_update_preserves_binding(struct kunit *test)
{
	struct xpon_test_context *ctx = test->priv;
	struct xpon_endpoint_config cfg = xpon_test_config(1, 7), observed;
	int create, update, get;

	rtnl_lock();
	create = xpon_endpoint_set(ctx->xpon, &cfg);
	cfg.wire_id = 9;
	cfg.queue = 2;
	update = xpon_endpoint_set(ctx->xpon, &cfg);
	get = xpon_endpoint_get_config(ctx->xpon, 1, &observed);
	rtnl_unlock();
	KUNIT_EXPECT_EQ(test, create, 0);
	KUNIT_EXPECT_EQ(test, update, -EINVAL);
	KUNIT_EXPECT_EQ(test, get, 0);
	KUNIT_EXPECT_EQ(test, observed.wire_id, (u16)7);
	KUNIT_EXPECT_EQ(test, observed.queue, (u8)0);
}

static void xpon_test_remove(struct kunit *test)
{
	struct xpon_test_context *ctx = test->priv;
	struct xpon_endpoint_config cfg = xpon_test_config(1, 7), observed;
	int create, remove, get;
	bool empty;

	rtnl_lock();
	create = xpon_endpoint_set(ctx->xpon, &cfg);
	remove = xpon_endpoint_delete(ctx->xpon, 1);
	get = xpon_endpoint_get_config(ctx->xpon, 1, &observed);
	empty = xa_empty(&ctx->xpon->rx_endpoints);
	rtnl_unlock();
	KUNIT_EXPECT_EQ(test, create, 0);
	KUNIT_EXPECT_EQ(test, remove, 0);
	KUNIT_EXPECT_EQ(test, get, -ENOENT);
	KUNIT_EXPECT_TRUE(test, empty);
}

static void xpon_test_mode_with_endpoint(struct kunit *test)
{
	struct xpon_test_context *ctx = test->priv;
	struct xpon_endpoint_config cfg = xpon_test_config(1, 7);
	int create, change;

	rtnl_lock();
	create = xpon_endpoint_set(ctx->xpon, &cfg);
	change = xpon_device_set_mode(ctx->xpon, XPON_MODE_GPON);
	rtnl_unlock();
	KUNIT_EXPECT_EQ(test, create, 0);
	KUNIT_EXPECT_EQ(test, change, -EBUSY);
}

static void xpon_test_queued_delete_wins(struct kunit *test)
{
	struct xpon_test_context *ctx = test->priv;
	struct xpon_endpoint_config cfg = xpon_test_config(1, 7), observed;
	int create, remove, get;

	/* The worker cannot acquire RTNL until both desired states are queued. */
	rtnl_lock();
	create = xpon_endpoint_queue(ctx->xpon, &cfg, false);
	remove = xpon_endpoint_queue(ctx->xpon, &cfg, true);
	rtnl_unlock();
	flush_work(&ctx->xpon->endpoint_work);
	rtnl_lock();
	get = xpon_endpoint_get_config(ctx->xpon, cfg.id, &observed);
	rtnl_unlock();
	KUNIT_EXPECT_EQ(test, create, 0);
	KUNIT_EXPECT_EQ(test, remove, 0);
	KUNIT_EXPECT_EQ(test, get, -ENOENT);
}

static void xpon_test_queued_config_is_copied(struct kunit *test)
{
	struct xpon_test_context *ctx = test->priv;
	struct xpon_endpoint_config cfg = xpon_test_config(1, 7), observed;
	int queue, get;

	rtnl_lock();
	queue = xpon_endpoint_queue(ctx->xpon, &cfg, false);
	cfg.wire_id = 9;
	rtnl_unlock();
	flush_work(&ctx->xpon->endpoint_work);
	rtnl_lock();
	get = xpon_endpoint_get_config(ctx->xpon, cfg.id, &observed);
	rtnl_unlock();
	KUNIT_EXPECT_EQ(test, queue, 0);
	KUNIT_EXPECT_EQ(test, get, 0);
	KUNIT_EXPECT_EQ(test, observed.wire_id, (u16)7);
}

static struct kunit_case xpon_test_cases[] = {
	KUNIT_CASE(xpon_test_duplicate_wire),
	KUNIT_CASE(xpon_test_capacity),
	KUNIT_CASE(xpon_test_rebind_preserves_interface),
	KUNIT_CASE(xpon_test_failed_update_preserves_binding),
	KUNIT_CASE(xpon_test_remove),
	KUNIT_CASE(xpon_test_mode_with_endpoint),
	KUNIT_CASE(xpon_test_queued_delete_wins),
	KUNIT_CASE(xpon_test_queued_config_is_copied),
	{}
};

static struct kunit_suite xpon_test_suite = {
	.name = "xpon-management",
	.init = xpon_test_init,
	.exit = xpon_test_exit,
	.test_cases = xpon_test_cases,
};

kunit_test_suite(xpon_test_suite);
