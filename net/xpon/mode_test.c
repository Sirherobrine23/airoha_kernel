// SPDX-License-Identifier: GPL-2.0-only

#include <kunit/test.h>
#include <linux/device.h>
#include <linux/etherdevice.h>
#include <net/xpon.h>

struct xpon_mode_test_ctx {
	struct device *parent;
	struct net_device *netdev;
	struct xpon_device *xpon;
	int calls;
	int error;
	int stop_error;
	int fallback_error;
	int stop_calls;
	int fallback_calls;
	int limits_calls;
	enum xpon_mode requested;
};

static int xpon_test_set_mode(struct xpon_device *xpon, enum xpon_mode mode)
{
	struct xpon_mode_test_ctx *ctx = xpon_device_priv(xpon);

	ctx->calls++;
	ctx->requested = mode;
	return ctx->error;
}

static int xpon_test_hw_pre_shutdown(struct xpon_device *xpon,
				      enum xpon_mode old, enum xpon_mode next)
{
	struct xpon_mode_test_ctx *ctx = xpon_device_priv(xpon);

	ctx->stop_calls++;
	return ctx->stop_error;
}

static int xpon_test_hw_fallback(struct xpon_device *xpon,
				 enum xpon_mode old, enum xpon_mode attempted,
				 enum xpon_mode *recovered)
{
	struct xpon_mode_test_ctx *ctx = xpon_device_priv(xpon);

	ctx->fallback_calls++;
	*recovered = old;
	return ctx->fallback_error;
}

static int xpon_test_get_limits(struct xpon_device *xpon,
				 enum xpon_mode mode,
				 struct xpon_device_limits *limits)
{
	struct xpon_mode_test_ctx *ctx = xpon_device_priv(xpon);

	ctx->limits_calls++;
	switch (mode) {
	case XPON_MODE_GPON:
		limits->max_endpoints = 2;
		limits->max_queues = 8;
		return 0;
	case XPON_MODE_XGSPON:
		return 0;
	case XPON_MODE_50GPON:
		limits->max_endpoints = 4;
		limits->max_queues = 8;
		return 0;
	case XPON_MODE_50GEPON:
		limits->max_links = 4;
		limits->max_endpoints = 4;
		limits->max_queues = 4;
		limits->report_sets = 3;
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

static const struct xpon_device_ops xpon_test_ops = {
	.get_limits = xpon_test_get_limits,
	.hw_pre_shutdown = xpon_test_hw_pre_shutdown,
	.hw_fallback = xpon_test_hw_fallback,
	.set_mode = xpon_test_set_mode,
};

static int xpon_mode_test_init(struct kunit *test)
{
	struct xpon_mode_test_ctx *ctx;
	int ret;

	ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;
	test->priv = ctx;
	ctx->parent = root_device_register("xpon-kunit");
	if (IS_ERR(ctx->parent))
		return PTR_ERR(ctx->parent);
	ctx->netdev = alloc_etherdev(0);
	if (!ctx->netdev) {
		ret = -ENOMEM;
		goto unregister_parent;
	}
	strscpy(ctx->netdev->name, "xpon-test");
	ctx->xpon = xpon_device_alloc(ctx->parent, 0);
	if (IS_ERR(ctx->xpon)) {
		ret = PTR_ERR(ctx->xpon);
		goto free_netdev;
	}
	ret = xpon_device_setup(ctx->xpon, ctx->netdev, &xpon_test_ops,
				ctx, XPON_MODE_GPON,
				XPON_MODE_GPON | XPON_MODE_XGSPON |
				XPON_MODE_50GPON | XPON_MODE_50GEPON);
	if (ret)
		goto free_xpon;
	ret = xpon_device_register_allocated(ctx->xpon);
	if (ret)
		goto free_xpon;
	return 0;

free_xpon:
	xpon_device_free(ctx->xpon);
free_netdev:
	free_netdev(ctx->netdev);
unregister_parent:
	root_device_unregister(ctx->parent);
	return ret;
}

static void xpon_mode_test_exit(struct kunit *test)
{
	struct xpon_mode_test_ctx *ctx = test->priv;

	xpon_device_unregister(ctx->xpon);
	free_netdev(ctx->netdev);
	root_device_unregister(ctx->parent);
}

static void xpon_mode_high_bits(struct kunit *test)
{
	struct xpon_mode_test_ctx *ctx = test->priv;
	const enum xpon_mode modes[] = {
		XPON_MODE_XGSPON, XPON_MODE_50GPON, XPON_MODE_50GEPON,
	};
	int i;

	for (i = 0; i < ARRAY_SIZE(modes); i++) {
		KUNIT_ASSERT_EQ(test, xpon_device_set_mode(ctx->xpon, modes[i]), 0);
		KUNIT_EXPECT_EQ(test, xpon_device_mode(ctx->xpon), modes[i]);
		KUNIT_EXPECT_EQ(test, ctx->requested, modes[i]);
	}
	KUNIT_EXPECT_EQ(test, ctx->calls, (int)ARRAY_SIZE(modes));
	KUNIT_EXPECT_TRUE(test, !!(xpon_device_modes(ctx->xpon) &
				  XPON_MODE_50GEPON));
}

/* The core preallocates the largest LLID table at registration, but only
 * exposes the effective limits for the current mode.
 */
static void xpon_mode_limits_switch(struct kunit *test)
{
	struct xpon_mode_test_ctx *ctx = test->priv;
	struct xpon_device_limits limits;
	int calls = ctx->limits_calls;

	KUNIT_ASSERT_EQ(test, xpon_device_get_limits(ctx->xpon, &limits), 0);
	KUNIT_EXPECT_EQ(test, limits.max_links, (u16)0);
	KUNIT_EXPECT_EQ(test, limits.max_endpoints, (u16)2);
	KUNIT_ASSERT_EQ(test,
			xpon_device_set_mode(ctx->xpon, XPON_MODE_50GEPON), 0);
	KUNIT_ASSERT_EQ(test, xpon_device_get_limits(ctx->xpon, &limits), 0);
	KUNIT_EXPECT_EQ(test, limits.max_links, (u16)4);
	KUNIT_EXPECT_EQ(test, limits.max_endpoints, (u16)4);
	KUNIT_EXPECT_EQ(test, limits.max_queues, (u8)4);
	KUNIT_EXPECT_EQ(test, limits.report_sets, (u8)3);
	KUNIT_EXPECT_EQ(test, ctx->limits_calls, calls);
}

static void xpon_mode_limits_fallback(struct kunit *test)
{
	struct xpon_mode_test_ctx *ctx = test->priv;
	struct xpon_device_limits limits;

	ctx->error = -EIO;
	KUNIT_EXPECT_EQ(test,
		xpon_device_set_mode(ctx->xpon, XPON_MODE_50GEPON), -EIO);
	KUNIT_ASSERT_EQ(test, xpon_device_get_limits(ctx->xpon, &limits), 0);
	KUNIT_EXPECT_EQ(test, limits.max_links, (u16)0);
	KUNIT_EXPECT_EQ(test, limits.max_endpoints, (u16)2);
	KUNIT_EXPECT_EQ(test, xpon_device_mode(ctx->xpon), XPON_MODE_GPON);
}

static void xpon_mode_invalid_requests(struct kunit *test)
{
	struct xpon_mode_test_ctx *ctx = test->priv;
	const u32 modes[] = {
		0, 1, XPON_MODE_GPON | XPON_MODE_XGSPON,
		XPON_MODE_50GEPON << 1, U32_MAX,
	};
	int i;

	for (i = 0; i < ARRAY_SIZE(modes); i++)
		KUNIT_EXPECT_EQ(test, xpon_device_set_mode(ctx->xpon, modes[i]),
				-EINVAL);
	KUNIT_EXPECT_EQ(test, ctx->calls, 0);
	KUNIT_EXPECT_EQ(test, xpon_device_mode(ctx->xpon), XPON_MODE_GPON);
}

static void xpon_mode_unsupported(struct kunit *test)
{
	struct xpon_mode_test_ctx *ctx = test->priv;

	KUNIT_EXPECT_EQ(test, xpon_device_set_mode(ctx->xpon, XPON_MODE_EPON),
			-EOPNOTSUPP);
	KUNIT_EXPECT_EQ(test, ctx->calls, 0);
}

static void xpon_mode_backend_failure(struct kunit *test)
{
	struct xpon_mode_test_ctx *ctx = test->priv;

	ctx->error = -EIO;
	KUNIT_EXPECT_EQ(test, xpon_device_set_mode(ctx->xpon, XPON_MODE_XGSPON),
			-EIO);
	KUNIT_EXPECT_EQ(test, xpon_device_mode(ctx->xpon), XPON_MODE_GPON);
	KUNIT_EXPECT_EQ(test, ctx->fallback_calls, 1);
	KUNIT_EXPECT_FALSE(test, xpon_device_mode_faulted(ctx->xpon));
	ctx->error = 0;
	KUNIT_EXPECT_EQ(test, xpon_device_set_mode(ctx->xpon, XPON_MODE_XGSPON), 0);
	KUNIT_EXPECT_EQ(test, xpon_device_mode(ctx->xpon), XPON_MODE_XGSPON);
}

static void xpon_mode_fallback_fault(struct kunit *test)
{
	struct xpon_mode_test_ctx *ctx = test->priv;

	ctx->error = -EIO;
	ctx->fallback_error = -EIO;
	KUNIT_EXPECT_EQ(test, xpon_device_set_mode(ctx->xpon, XPON_MODE_XGSPON),
			-EIO);
	KUNIT_EXPECT_TRUE(test, xpon_device_mode_faulted(ctx->xpon));
	ctx->error = 0;
	KUNIT_EXPECT_EQ(test, xpon_device_set_mode(ctx->xpon, XPON_MODE_XGSPON),
			-EBUSY);
}

static void xpon_mode_stop_failure(struct kunit *test)
{
	struct xpon_mode_test_ctx *ctx = test->priv;

	ctx->stop_error = -ETIMEDOUT;
	KUNIT_EXPECT_EQ(test, xpon_device_set_mode(ctx->xpon, XPON_MODE_XGSPON),
			-ETIMEDOUT);
	KUNIT_EXPECT_EQ(test, ctx->calls, 0);
	KUNIT_EXPECT_EQ(test, ctx->fallback_calls, 1);
	KUNIT_EXPECT_EQ(test, xpon_device_mode(ctx->xpon), XPON_MODE_GPON);
}

static void xpon_mode_running(struct kunit *test)
{
	struct xpon_mode_test_ctx *ctx = test->priv;

	set_bit(__LINK_STATE_START, &ctx->netdev->state);
	KUNIT_EXPECT_EQ(test, xpon_device_set_mode(ctx->xpon, XPON_MODE_XGSPON),
			-EBUSY);
	clear_bit(__LINK_STATE_START, &ctx->netdev->state);
	KUNIT_EXPECT_EQ(test, ctx->calls, 0);
}

static void xpon_mode_invalid_registration(struct kunit *test)
{
	struct xpon_mode_test_ctx *ctx = test->priv;
	struct xpon_device *xpon;
	const u32 modes[] = { 0, 1, XPON_MODE_GPON | XPON_MODE_XGSPON };
	int i, ret;

	for (i = 0; i < ARRAY_SIZE(modes); i++) {
		xpon = xpon_device_alloc(ctx->parent, 0);
		KUNIT_ASSERT_FALSE(test, IS_ERR(xpon));
		ret = xpon_device_setup(xpon, ctx->netdev, &xpon_test_ops,
				ctx, modes[i], XPON_MODE_GPON | XPON_MODE_XGSPON);
		KUNIT_EXPECT_EQ(test, ret, -EINVAL);
		xpon_device_free(xpon);
	}
	xpon = xpon_device_alloc(ctx->parent, 0);
	KUNIT_ASSERT_FALSE(test, IS_ERR(xpon));
	ret = xpon_device_setup(xpon, ctx->netdev, &xpon_test_ops,
				ctx, XPON_MODE_GPON,
				XPON_MODE_GPON | XPON_MODE_XGSPON | BIT(31));
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
	xpon_device_free(xpon);
}

static struct device_attribute *xpon_test_attr(struct device *dev,
					       const char *name)
{
	const struct attribute_group **group;
	struct attribute **attr;

	for (group = dev->groups; group && *group; group++)
		for (attr = (*group)->attrs; attr && *attr; attr++)
			if (!strcmp((*attr)->name, name))
				return container_of(*attr, struct device_attribute, attr);
	return NULL;
}

static void xpon_mode_sysfs(struct kunit *test)
{
	struct xpon_mode_test_ctx *ctx = test->priv;
	struct device *dev = xpon_device_dev(ctx->xpon);
	struct device_attribute *attr;
	char *buf;
	ssize_t ret;

	buf = kunit_kzalloc(test, PAGE_SIZE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, buf);
	attr = xpon_test_attr(dev, "mode_available");
	KUNIT_ASSERT_NOT_NULL(test, attr);
	ret = attr->show(dev, attr, buf);
	KUNIT_ASSERT_GT(test, ret, 0);
	KUNIT_EXPECT_STREQ(test, buf, "gpon xgspon 50gpon 50gepon\n");
	attr = xpon_test_attr(dev, "mode_current");
	KUNIT_ASSERT_NOT_NULL(test, attr);
	KUNIT_ASSERT_EQ(test, attr->store(dev, attr, "50gepon\n", 8), (ssize_t)8);
	KUNIT_EXPECT_EQ(test, xpon_device_mode(ctx->xpon), XPON_MODE_50GEPON);
	ret = attr->show(dev, attr, buf);
	KUNIT_ASSERT_GT(test, ret, 0);
	KUNIT_EXPECT_STREQ(test, buf, "50gepon\n");
}

static void xpon_mode_alloc_private(struct kunit *test)
{
	struct xpon_mode_test_ctx *ctx = test->priv;
	struct xpon_device *xpon;
	u64 *data;

	xpon = xpon_device_alloc(ctx->parent, sizeof(*data));
	KUNIT_ASSERT_FALSE(test, IS_ERR(xpon));
	data = xpon_device_priv(xpon);
	KUNIT_ASSERT_NOT_NULL(test, data);
	KUNIT_EXPECT_EQ(test, *data, 0ULL);
	*data = 0x123456789abcdefULL;
	KUNIT_EXPECT_EQ(test, *(u64 *)xpon_device_priv(xpon), *data);
	xpon_device_free(xpon);

	xpon = xpon_device_alloc(ctx->parent, -1);
	KUNIT_EXPECT_EQ(test, PTR_ERR(xpon), -EINVAL);
}

static struct kunit_case xpon_mode_cases[] = {
	KUNIT_CASE(xpon_mode_high_bits),
	KUNIT_CASE(xpon_mode_limits_switch),
	KUNIT_CASE(xpon_mode_limits_fallback),
	KUNIT_CASE(xpon_mode_invalid_requests),
	KUNIT_CASE(xpon_mode_unsupported),
	KUNIT_CASE(xpon_mode_backend_failure),
	KUNIT_CASE(xpon_mode_fallback_fault),
	KUNIT_CASE(xpon_mode_stop_failure),
	KUNIT_CASE(xpon_mode_running),
	KUNIT_CASE(xpon_mode_invalid_registration),
	KUNIT_CASE(xpon_mode_sysfs),
	KUNIT_CASE(xpon_mode_alloc_private),
	{}
};

static struct kunit_suite xpon_mode_suite = {
	.name = "xpon_mode",
	.init = xpon_mode_test_init,
	.exit = xpon_mode_test_exit,
	.test_cases = xpon_mode_cases,
};

kunit_test_suite(xpon_mode_suite);

MODULE_DESCRIPTION("xPON mode and capability tests");
MODULE_LICENSE("GPL");
