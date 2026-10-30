// SPDX-License-Identifier: GPL-2.0-only
/* Exercise the shared registry without a line engine, modem or OLT. */

#include <kunit/test.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/etherdevice.h>
#include <linux/kthread.h>
#include <net/netlink.h>
#include <net/xpon.h>

#include "internal.h"

struct omci_access_test_ctx {
	struct device *parent;
	struct net_device *netdev;
	struct omci_line *line;
	struct omci_line_status status;
	int read_error;
	int retrain_error;
	int retrain_calls;
	bool block_read;
	struct completion read_entered;
	struct completion release_read;
	struct completion remove_entered;
	struct completion removed;
};

static int omci_access_test_get(void *priv, struct omci_line_status *status)
{
	struct omci_access_test_ctx *ctx = priv;

	if (ctx->block_read) {
		complete(&ctx->read_entered);
		wait_for_completion(&ctx->release_read);
	}
	*status = ctx->status;
	return ctx->read_error;
}

static int omci_access_test_retrain(void *priv)
{
	struct omci_access_test_ctx *ctx = priv;

	ctx->retrain_calls++;
	return ctx->retrain_error;
}

static const struct omci_line_ops omci_access_test_ops = {
	.get_status = omci_access_test_get,
	.retrain = omci_access_test_retrain,
};

static int omci_access_test_init(struct kunit *test)
{
	struct omci_access_test_ctx *ctx;
	int ret;

	ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;
	test->priv = ctx;
	ctx->parent = root_device_register("omci-access-kunit");
	if (IS_ERR(ctx->parent))
		return PTR_ERR(ctx->parent);
	ctx->netdev = alloc_etherdev(0);
	if (!ctx->netdev) {
		ret = -ENOMEM;
		goto unregister_parent;
	}
	ctx->status.access = OMCI_ACCESS_XDSL;
	ctx->status.state = OMCI_LINE_UNAVAILABLE;
	init_completion(&ctx->read_entered);
	init_completion(&ctx->release_read);
	init_completion(&ctx->remove_entered);
	init_completion(&ctx->removed);
	ctx->line = omci_line_register(ctx->parent, ctx->netdev,
				       &omci_access_test_ops, ctx);
	if (IS_ERR(ctx->line)) {
		ret = PTR_ERR(ctx->line);
		free_netdev(ctx->netdev);
		goto unregister_parent;
	}
	return 0;
unregister_parent:
	root_device_unregister(ctx->parent);
	return ret;
}

static void omci_access_test_exit(struct kunit *test)
{
	struct omci_access_test_ctx *ctx = test->priv;

	omci_line_unregister(ctx->line);
	free_netdev(ctx->netdev);
	root_device_unregister(ctx->parent);
}

static void omci_access_registration(struct kunit *test)
{
	struct omci_access_test_ctx *ctx = test->priv;
	struct omci_line *other;
	struct omci_line_status status;
	u32 id = omci_line_id(ctx->line);

	other = omci_line_register(ctx->parent, ctx->netdev,
				   &omci_access_test_ops, ctx);
	KUNIT_EXPECT_EQ(test, PTR_ERR(other), -EBUSY);
	if (!IS_ERR(other))
		omci_line_unregister(other);
	other = omci_line_register(ctx->parent, ctx->netdev, NULL, ctx);
	KUNIT_EXPECT_EQ(test, PTR_ERR(other), -EINVAL);
	if (!IS_ERR(other))
		omci_line_unregister(other);
	KUNIT_ASSERT_EQ(test, omci_line_read(id, &status), 0);
	KUNIT_EXPECT_EQ(test, status.access, (u8)OMCI_ACCESS_XDSL);
	KUNIT_EXPECT_EQ(test, status.state, (u8)OMCI_LINE_UNAVAILABLE);
	KUNIT_EXPECT_EQ(test, status.flags, 0U);
	omci_line_unregister(ctx->line);
	ctx->line = NULL;
	KUNIT_EXPECT_EQ(test, omci_line_read(id, &status), -ENODEV);
	KUNIT_EXPECT_EQ(test, omci_line_retrain(id), -ENODEV);
}

static void omci_access_provider_errors(struct kunit *test)
{
	struct omci_access_test_ctx *ctx = test->priv;
	struct omci_line_status status;
	u32 id = omci_line_id(ctx->line);

	ctx->status.state = OMCI_LINE_UP;
	ctx->status.flags = OMCI_LINE_F_RATES_VALID | OMCI_LINE_F_CARRIER;
	ctx->status.downstream_bps = 10000000000ULL;
	ctx->status.upstream_bps = 2500000000ULL;
	KUNIT_ASSERT_EQ(test, omci_line_read(id, &status), 0);
	KUNIT_EXPECT_EQ(test, status.downstream_bps, 10000000000ULL);
	KUNIT_EXPECT_EQ(test, status.upstream_bps, 2500000000ULL);
	ctx->read_error = -EIO;
	KUNIT_EXPECT_EQ(test, omci_line_read(id, &status), -EIO);
	KUNIT_EXPECT_EQ(test, status.flags, 0U);
	KUNIT_EXPECT_EQ(test, omci_line_retrain(id), -EIO);
	KUNIT_EXPECT_EQ(test, ctx->retrain_calls, 0);
	ctx->read_error = 0;
	ctx->status.access = 0xff;
	KUNIT_EXPECT_EQ(test, omci_line_read(id, &status), -EINVAL);
}

static void omci_access_retrain_dispatch(struct kunit *test)
{
	struct omci_access_test_ctx *ctx = test->priv;
	u32 id = omci_line_id(ctx->line);

	ctx->retrain_error = -ENETDOWN;
	KUNIT_EXPECT_EQ(test, omci_line_retrain(id), -ENETDOWN);
	KUNIT_EXPECT_EQ(test, ctx->retrain_calls, 1);
	ctx->status.access = OMCI_ACCESS_XGSPON;
	KUNIT_EXPECT_EQ(test, omci_line_retrain(id), -EOPNOTSUPP);
	KUNIT_EXPECT_EQ(test, ctx->retrain_calls, 1);
}

static void omci_access_no_engine(struct kunit *test)
{
	static const struct omci_line_ops no_engine_ops = {
		.get_status = omci_access_test_get,
	};
	struct omci_access_test_ctx *ctx = test->priv;

	omci_line_unregister(ctx->line);
	ctx->line = omci_line_register(ctx->parent, ctx->netdev,
				       &no_engine_ops, ctx);
	if (IS_ERR(ctx->line)) {
		ctx->line = NULL;
		KUNIT_FAIL(test, "failed to register no-engine provider");
		return;
	}
	KUNIT_EXPECT_EQ(test, omci_line_retrain(omci_line_id(ctx->line)),
			-EOPNOTSUPP);
}

static void omci_access_pon_modes(struct kunit *test)
{
	static const struct {
		enum xpon_mode mode;
		u8 access;
	} cases[] = {
		{ XPON_MODE_GPON, OMCI_ACCESS_GPON },
		{ XPON_MODE_XGPON, OMCI_ACCESS_XGPON },
		{ XPON_MODE_XGSPON, OMCI_ACCESS_XGSPON },
	};
	struct xpon_state state = {
		.registration = XPON_REGISTRATION_OPERATIONAL,
		.carrier = true,
	};
	struct omci_line_status status;
	int i;

	for (i = 0; i < ARRAY_SIZE(cases); i++) {
		state.mode = cases[i].mode;
		omci_line_status_from_xpon(&state, true, &status);
		KUNIT_EXPECT_EQ(test, status.access, cases[i].access);
		KUNIT_EXPECT_EQ(test, status.state, (u8)OMCI_LINE_UP);
		KUNIT_EXPECT_TRUE(test, status.flags & OMCI_LINE_F_CARRIER);
		KUNIT_EXPECT_FALSE(test, status.flags & OMCI_LINE_F_RATES_VALID);
	}
	state.registration = XPON_REGISTRATION_REGISTERING;
	state.carrier = false;
	omci_line_status_from_xpon(&state, true, &status);
	KUNIT_EXPECT_EQ(test, status.state, (u8)OMCI_LINE_TRAINING);
	state.mode = XPON_MODE_EPON;
	omci_line_status_from_xpon(&state, true, &status);
	KUNIT_EXPECT_EQ(test, status.access, (u8)OMCI_ACCESS_UNSPEC);
	KUNIT_EXPECT_EQ(test, status.state, (u8)OMCI_LINE_UNAVAILABLE);
	KUNIT_EXPECT_EQ(test, status.flags, 0U);
}

static void omci_access_netlink_rates(struct kunit *test)
{
	struct omci_line_status status = {
		.access = OMCI_ACCESS_XGSPON,
		.state = OMCI_LINE_UP,
		.downstream_bps = 10000000000ULL,
		.upstream_bps = 10000000000ULL,
	};
	struct nlattr *attr;
	struct sk_buff *msg;

	msg = alloc_skb(256, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, msg);
	KUNIT_EXPECT_EQ(test, omci_line_put_status(msg, &status), 0);
	attr = nla_find((struct nlattr *)msg->data, msg->len,
			OMCI_ATTR_DOWNSTREAM_BPS);
	KUNIT_EXPECT_PTR_EQ(test, attr, NULL);
	skb_trim(msg, 0);
	status.flags = OMCI_LINE_F_RATES_VALID;
	KUNIT_EXPECT_EQ(test, omci_line_put_status(msg, &status), 0);
	attr = nla_find((struct nlattr *)msg->data, msg->len,
			OMCI_ATTR_DOWNSTREAM_BPS);
	KUNIT_EXPECT_NOT_NULL(test, attr);
	if (attr)
		KUNIT_EXPECT_EQ(test, nla_get_u64(attr), 10000000000ULL);
	attr = nla_find((struct nlattr *)msg->data, msg->len,
			OMCI_ATTR_UPSTREAM_BPS);
	KUNIT_EXPECT_NOT_NULL(test, attr);
	if (attr)
		KUNIT_EXPECT_EQ(test, nla_get_u64(attr), 10000000000ULL);
	kfree_skb(msg);
}

static int omci_access_test_xmit(struct omci_device *odev,
				 struct sk_buff *skb, u16 gem_port_id)
{
	kfree_skb(skb);
	return 0;
}

static void omci_access_pon_registration(struct kunit *test)
{
	static const struct omci_device_ops transport_ops = {
		.xmit = omci_access_test_xmit,
	};
	static const struct xpon_device_ops xpon_ops = {};
	struct omci_access_test_ctx *ctx = test->priv;
	struct xpon_device_desc desc = {
		.netdev = ctx->netdev,
		.ops = &xpon_ops,
		.modes = XPON_MODE_XGSPON,
		.mode = XPON_MODE_XGSPON,
	};
	struct omci_line_status status;
	struct omci_device *odev;
	struct xpon_device *xpon;
	u32 id;

	/* Replace the copper registration with a real OMCI core registration. */
	omci_line_unregister(ctx->line);
	ctx->line = NULL;
	xpon = xpon_device_register(ctx->parent, &desc);
	KUNIT_ASSERT_FALSE(test, IS_ERR(xpon));
	odev = omci_device_register(xpon, 0, &transport_ops, NULL);
	KUNIT_EXPECT_FALSE(test, IS_ERR(odev));
	if (IS_ERR(odev))
		goto unregister_xpon;
	id = odev->line_id;
	KUNIT_EXPECT_NE(test, id, 0U);
	KUNIT_EXPECT_EQ(test, omci_line_read(id, &status), 0);
	KUNIT_EXPECT_EQ(test, status.access, (u8)OMCI_ACCESS_XGSPON);
	KUNIT_EXPECT_EQ(test, omci_line_retrain(id), -EOPNOTSUPP);
	omci_device_unregister(odev);
	KUNIT_EXPECT_EQ(test, omci_line_read(id, &status), -ENODEV);
unregister_xpon:
	xpon_device_unregister(xpon);
}

static int omci_access_read_thread(void *priv)
{
	struct omci_access_test_ctx *ctx = priv;
	struct omci_line_status status;

	return omci_line_read(omci_line_id(ctx->line), &status);
}

static int omci_access_remove_thread(void *priv)
{
	struct omci_access_test_ctx *ctx = priv;

	complete(&ctx->remove_entered);
	omci_line_unregister(ctx->line);
	ctx->line = NULL;
	complete(&ctx->removed);
	return 0;
}

static void omci_access_remove_waits(struct kunit *test)
{
	struct omci_access_test_ctx *ctx = test->priv;
	struct task_struct *reader, *remover;
	unsigned long ready;

	ctx->block_read = true;
	reader = kthread_create(omci_access_read_thread, ctx, "omci-read-test");
	KUNIT_ASSERT_FALSE(test, IS_ERR(reader));
	get_task_struct(reader);
	wake_up_process(reader);
	ready = wait_for_completion_timeout(&ctx->read_entered, 5 * HZ);
	KUNIT_EXPECT_NE(test, ready, 0UL);
	if (!ready)
		goto release_reader;
	remover = kthread_create(omci_access_remove_thread, ctx, "omci-remove-test");
	KUNIT_EXPECT_FALSE(test, IS_ERR(remover));
	if (IS_ERR(remover))
		goto release_reader;
	get_task_struct(remover);
	wake_up_process(remover);
	ready = wait_for_completion_timeout(&ctx->remove_entered, 5 * HZ);
	KUNIT_EXPECT_NE(test, ready, 0UL);
	msleep(20);
	KUNIT_EXPECT_FALSE(test, completion_done(&ctx->removed));
	complete(&ctx->release_read);
	kthread_stop(reader);
	kthread_stop(remover);
	put_task_struct(reader);
	put_task_struct(remover);
	KUNIT_EXPECT_TRUE(test, completion_done(&ctx->removed));
	return;
release_reader:
	complete(&ctx->release_read);
	kthread_stop(reader);
	put_task_struct(reader);
}

static struct kunit_case omci_access_test_cases[] = {
	KUNIT_CASE(omci_access_registration),
	KUNIT_CASE(omci_access_provider_errors),
	KUNIT_CASE(omci_access_retrain_dispatch),
	KUNIT_CASE(omci_access_no_engine),
	KUNIT_CASE(omci_access_pon_modes),
	KUNIT_CASE(omci_access_netlink_rates),
	KUNIT_CASE(omci_access_pon_registration),
	KUNIT_CASE(omci_access_remove_waits),
	{}
};

static struct kunit_suite omci_access_test_suite = {
	.name = "omci-access",
	.init = omci_access_test_init,
	.exit = omci_access_test_exit,
	.test_cases = omci_access_test_cases,
};

kunit_test_suite(omci_access_test_suite);
