// SPDX-License-Identifier: GPL-2.0-only
/* Service transactions against a capacity-limited, hardware-free provider. */

#include <kunit/test.h>

#include "internal.h"

#define OMCI_TEST_CAPACITY 2

struct omci_services_test_context {
	struct omci_device odev;
	struct xarray desired;
	struct omci_service_config hardware[OMCI_TEST_CAPACITY];
	bool used[OMCI_TEST_CAPACITY];
	u16 fail_gem;
	u16 fail_restore_gem;
	u32 fail_delete_cookie;
	unsigned int replace_calls;
};

static int omci_services_test_validate(struct omci_device *odev,
				       const struct omci_service_config *services,
				       size_t count)
{
	return count > OMCI_TEST_CAPACITY ? -ENOSPC : 0;
}

static int omci_services_test_replace(struct omci_device *odev,
				      const struct omci_service_config *config)
{
	struct omci_services_test_context *ctx = odev->priv;
	int i, slot = -1;

	ctx->replace_calls++;
	if (config->gem_port_id == ctx->fail_gem) {
		ctx->fail_gem = 0;
		return -EINVAL;
	}
	if (config->gem_port_id == ctx->fail_restore_gem)
		return -EAGAIN;
	for (i = 0; i < OMCI_TEST_CAPACITY; i++) {
		if (ctx->used[i] && ctx->hardware[i].cookie == config->cookie) {
			slot = i;
			break;
		}
		if (!ctx->used[i] && slot < 0)
			slot = i;
	}
	if (slot < 0)
		return -ENOSPC;
	ctx->hardware[slot] = *config;
	ctx->used[slot] = true;
	return 0;
}

static int omci_services_test_delete(struct omci_device *odev, u32 cookie)
{
	struct omci_services_test_context *ctx = odev->priv;
	int i;

	if (cookie == ctx->fail_delete_cookie) {
		ctx->fail_delete_cookie = 0;
		return -EIO;
	}
	for (i = 0; i < OMCI_TEST_CAPACITY; i++)
		if (ctx->used[i] && ctx->hardware[i].cookie == cookie) {
			ctx->used[i] = false;
			return 0;
		}
	return -ENOENT;
}

static const struct omci_device_ops omci_services_test_ops = {
	.validate_services = omci_services_test_validate,
	.replace_service = omci_services_test_replace,
	.delete_service = omci_services_test_delete,
};

static void omci_services_test_free(struct xarray *array)
{
	struct omci_service_state *state;
	unsigned long index;

	xa_for_each(array, index, state) {
		xa_erase(array, index);
		kfree(state);
	}
	xa_destroy(array);
}

static int omci_services_test_init(struct kunit *test)
{
	struct omci_services_test_context *ctx;

	ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;
	ctx->odev.parent = root_device_register("omci-services-kunit");
	if (IS_ERR(ctx->odev.parent))
		return PTR_ERR(ctx->odev.parent);
	ctx->odev.priv = ctx;
	ctx->odev.ops = &omci_services_test_ops;
	mutex_init(&ctx->odev.agent.lock);
	xa_init(&ctx->odev.agent.services);
	xa_init(&ctx->desired);
	test->priv = ctx;
	return 0;
}

static void omci_services_test_exit(struct kunit *test)
{
	struct omci_services_test_context *ctx = test->priv;

	omci_services_test_free(&ctx->desired);
	omci_services_test_free(&ctx->odev.agent.services);
	root_device_unregister(ctx->odev.parent);
}

static void omci_services_test_add(struct kunit *test, struct xarray *array,
				   u32 cookie, u16 gem)
{
	struct omci_service_state *state;

	state = kzalloc(sizeof(*state), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, state);
	state->config.cookie = cookie;
	state->config.gem_port_id = gem;
	state->config.tcont_entity_id = 0x8000;
	if (xa_err(xa_store(array, cookie, state, GFP_KERNEL))) {
		kfree(state);
		KUNIT_FAIL(test, "could not store service");
	}
}

static void omci_services_test_seed(struct kunit *test)
{
	struct omci_services_test_context *ctx = test->priv;
	struct omci_service_state *state;
	unsigned long index;

	omci_services_test_add(test, &ctx->odev.agent.services, 1, 10);
	omci_services_test_add(test, &ctx->odev.agent.services, 2, 20);
	xa_for_each(&ctx->odev.agent.services, index, state) {
		int ret = omci_services_test_replace(&ctx->odev, &state->config);

		KUNIT_ASSERT_EQ(test, ret, 0);
	}
	ctx->replace_calls = 0;
}

static int omci_services_test_apply(struct omci_services_test_context *ctx)
{
	int ret;

	mutex_lock(&ctx->odev.agent.lock);
	ret = omci_agent_apply_services_locked(&ctx->odev, &ctx->desired);
	mutex_unlock(&ctx->odev.agent.lock);
	return ret;
}

static bool omci_services_test_has(struct omci_services_test_context *ctx,
				   u32 cookie, u16 gem)
{
	int i;

	for (i = 0; i < OMCI_TEST_CAPACITY; i++)
		if (ctx->used[i] && ctx->hardware[i].cookie == cookie &&
		    ctx->hardware[i].gem_port_id == gem)
			return true;
	return false;
}

static void omci_services_full_reprovision_test(struct kunit *test)
{
	struct omci_services_test_context *ctx = test->priv;

	omci_services_test_seed(test);
	omci_services_test_add(test, &ctx->desired, 3, 30);
	omci_services_test_add(test, &ctx->desired, 4, 40);
	KUNIT_ASSERT_EQ(test, omci_services_test_apply(ctx), 0);
	KUNIT_EXPECT_TRUE(test, omci_services_test_has(ctx, 3, 30));
	KUNIT_EXPECT_TRUE(test, omci_services_test_has(ctx, 4, 40));
	KUNIT_EXPECT_PTR_EQ(test, xa_load(&ctx->odev.agent.services, 1), NULL);
	KUNIT_EXPECT_NOT_NULL(test, xa_load(&ctx->odev.agent.services, 4));
}

static void omci_services_changed_cookie_test(struct kunit *test)
{
	struct omci_services_test_context *ctx = test->priv;

	omci_services_test_seed(test);
	omci_services_test_add(test, &ctx->desired, 1, 30);
	omci_services_test_add(test, &ctx->desired, 2, 20);
	KUNIT_ASSERT_EQ(test, omci_services_test_apply(ctx), 0);
	KUNIT_EXPECT_TRUE(test, omci_services_test_has(ctx, 1, 30));
	KUNIT_EXPECT_TRUE(test, omci_services_test_has(ctx, 2, 20));
}

static void omci_services_preflight_test(struct kunit *test)
{
	struct omci_services_test_context *ctx = test->priv;

	omci_services_test_seed(test);
	omci_services_test_add(test, &ctx->desired, 3, 30);
	omci_services_test_add(test, &ctx->desired, 4, 40);
	omci_services_test_add(test, &ctx->desired, 5, 50);
	KUNIT_EXPECT_EQ(test, omci_services_test_apply(ctx), -ENOSPC);
	KUNIT_EXPECT_EQ(test, ctx->replace_calls, 0U);
	KUNIT_EXPECT_TRUE(test, omci_services_test_has(ctx, 1, 10));
	KUNIT_EXPECT_TRUE(test, omci_services_test_has(ctx, 2, 20));
}

static void omci_services_rollback_test(struct kunit *test)
{
	struct omci_services_test_context *ctx = test->priv;

	omci_services_test_seed(test);
	omci_services_test_add(test, &ctx->desired, 3, 30);
	omci_services_test_add(test, &ctx->desired, 4, 40);
	ctx->fail_gem = 40;
	KUNIT_EXPECT_EQ(test, omci_services_test_apply(ctx), -EINVAL);
	KUNIT_EXPECT_TRUE(test, omci_services_test_has(ctx, 1, 10));
	KUNIT_EXPECT_TRUE(test, omci_services_test_has(ctx, 2, 20));
	KUNIT_EXPECT_PTR_EQ(test, xa_load(&ctx->odev.agent.services, 3), NULL);
}

static void omci_services_restore_failure_test(struct kunit *test)
{
	struct omci_services_test_context *ctx = test->priv;

	omci_services_test_seed(test);
	omci_services_test_add(test, &ctx->desired, 3, 30);
	omci_services_test_add(test, &ctx->desired, 4, 40);
	ctx->fail_gem = 40;
	ctx->fail_restore_gem = 10;
	KUNIT_EXPECT_EQ(test, omci_services_test_apply(ctx), -EIO);
	KUNIT_EXPECT_NOT_NULL(test, xa_load(&ctx->odev.agent.services, 1));
}

static void omci_services_restart_replay_test(struct kunit *test)
{
	struct omci_services_test_context *ctx = test->priv;

	omci_services_test_seed(test);
	memset(ctx->used, 0, sizeof(ctx->used));
	omci_services_test_add(test, &ctx->desired, 1, 10);
	omci_services_test_add(test, &ctx->desired, 2, 20);
	KUNIT_ASSERT_EQ(test, omci_services_test_apply(ctx), 0);
	KUNIT_EXPECT_TRUE(test, omci_services_test_has(ctx, 1, 10));
	KUNIT_EXPECT_TRUE(test, omci_services_test_has(ctx, 2, 20));
	KUNIT_EXPECT_EQ(test, ctx->replace_calls, 2U);
}

static void omci_services_changed_rollback_test(struct kunit *test)
{
	struct omci_services_test_context *ctx = test->priv;

	omci_services_test_seed(test);
	omci_services_test_add(test, &ctx->desired, 1, 30);
	omci_services_test_add(test, &ctx->desired, 2, 40);
	ctx->fail_gem = 40;
	KUNIT_EXPECT_EQ(test, omci_services_test_apply(ctx), -EINVAL);
	KUNIT_EXPECT_TRUE(test, omci_services_test_has(ctx, 1, 10));
	KUNIT_EXPECT_TRUE(test, omci_services_test_has(ctx, 2, 20));
}

static void omci_services_delete_failure_test(struct kunit *test)
{
	struct omci_services_test_context *ctx = test->priv;

	omci_services_test_seed(test);
	omci_services_test_add(test, &ctx->desired, 3, 30);
	omci_services_test_add(test, &ctx->desired, 4, 40);
	ctx->fail_delete_cookie = 2;
	KUNIT_EXPECT_EQ(test, omci_services_test_apply(ctx), -EIO);
	KUNIT_EXPECT_TRUE(test, omci_services_test_has(ctx, 1, 10));
	KUNIT_EXPECT_TRUE(test, omci_services_test_has(ctx, 2, 20));
}

static void omci_services_clear_test(struct kunit *test)
{
	struct omci_services_test_context *ctx = test->priv;

	omci_services_test_seed(test);
	KUNIT_ASSERT_EQ(test, omci_services_test_apply(ctx), 0);
	KUNIT_EXPECT_TRUE(test, xa_empty(&ctx->odev.agent.services));
	KUNIT_EXPECT_FALSE(test, ctx->used[0]);
	KUNIT_EXPECT_FALSE(test, ctx->used[1]);
}

static struct kunit_case omci_services_test_cases[] = {
	KUNIT_CASE(omci_services_full_reprovision_test),
	KUNIT_CASE(omci_services_changed_cookie_test),
	KUNIT_CASE(omci_services_preflight_test),
	KUNIT_CASE(omci_services_rollback_test),
	KUNIT_CASE(omci_services_restore_failure_test),
	KUNIT_CASE(omci_services_restart_replay_test),
	KUNIT_CASE(omci_services_changed_rollback_test),
	KUNIT_CASE(omci_services_delete_failure_test),
	KUNIT_CASE(omci_services_clear_test),
	{}
};

static struct kunit_suite omci_services_test_suite = {
	.name = "omci-services",
	.init = omci_services_test_init,
	.exit = omci_services_test_exit,
	.test_cases = omci_services_test_cases,
};

kunit_test_suite(omci_services_test_suite);
