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
	enum xpon_mode requested;
};

static int xpon_test_set_mode(struct xpon_device *xpon, enum xpon_mode mode)
{
	struct xpon_mode_test_ctx *ctx = xpon_device_priv(xpon);

	ctx->calls++;
	ctx->requested = mode;
	return ctx->error;
}

static const struct xpon_device_ops xpon_test_ops = {
	.set_mode = xpon_test_set_mode,
};

static int xpon_mode_test_init(struct kunit *test)
{
	struct xpon_device_desc desc = {};
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
	desc.netdev = ctx->netdev;
	desc.modes = XPON_MODE_GPON | XPON_MODE_XGSPON |
		     XPON_MODE_50GPON | XPON_MODE_50GEPON;
	desc.mode = XPON_MODE_GPON;
	desc.ops = &xpon_test_ops;
	desc.priv = ctx;
	ctx->xpon = xpon_device_register(ctx->parent, &desc);
	if (IS_ERR(ctx->xpon)) {
		ret = PTR_ERR(ctx->xpon);
		free_netdev(ctx->netdev);
		goto unregister_parent;
	}
	return 0;

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
	ctx->error = 0;
	KUNIT_EXPECT_EQ(test, xpon_device_set_mode(ctx->xpon, XPON_MODE_XGSPON), 0);
	KUNIT_EXPECT_EQ(test, xpon_device_mode(ctx->xpon), XPON_MODE_XGSPON);
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
	struct xpon_device_desc desc = {
		.netdev = ctx->netdev,
		.modes = XPON_MODE_GPON | XPON_MODE_XGSPON,
	};
	struct xpon_device *xpon;
	const u32 modes[] = { 0, 1, XPON_MODE_GPON | XPON_MODE_XGSPON };
	int i;

	for (i = 0; i < ARRAY_SIZE(modes); i++) {
		desc.mode = modes[i];
		xpon = xpon_device_register(ctx->parent, &desc);
		if (!IS_ERR(xpon))
			xpon_device_unregister(xpon);
		KUNIT_EXPECT_EQ(test, PTR_ERR_OR_ZERO(xpon), -EINVAL);
	}
	desc.mode = XPON_MODE_GPON;
	desc.modes |= BIT(31);
	xpon = xpon_device_register(ctx->parent, &desc);
	if (!IS_ERR(xpon))
		xpon_device_unregister(xpon);
	KUNIT_EXPECT_EQ(test, PTR_ERR_OR_ZERO(xpon), -EINVAL);
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

static struct kunit_case xpon_mode_cases[] = {
	KUNIT_CASE(xpon_mode_high_bits),
	KUNIT_CASE(xpon_mode_invalid_requests),
	KUNIT_CASE(xpon_mode_unsupported),
	KUNIT_CASE(xpon_mode_backend_failure),
	KUNIT_CASE(xpon_mode_running),
	KUNIT_CASE(xpon_mode_invalid_registration),
	KUNIT_CASE(xpon_mode_sysfs),
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
