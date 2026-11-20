// SPDX-License-Identifier: GPL-2.0-only
/* Service transactions against a capacity-limited, hardware-free provider. */

#include <kunit/test.h>
#include <linux/etherdevice.h>
#include <linux/if_vlan.h>
#include <linux/unaligned.h>

#include "internal.h"

#define OMCI_TEST_CAPACITY 2
#define OMCI_TEST_VLAN_CAPACITY 8

struct omci_services_test_context {
	struct omci_device odev;
	struct xpon_device_ops xpon_ops;
	struct xarray desired;
	struct net_device *netdev;
	struct omci_service_config hardware[OMCI_TEST_CAPACITY];
	bool used[OMCI_TEST_CAPACITY];
	struct xpon_vlan_config vlans[OMCI_TEST_VLAN_CAPACITY];
	bool vlan_used[OMCI_TEST_VLAN_CAPACITY];
	u32 fail_vlan_cookie;
	u32 fail_vlan_delete;
	int vlan_preflight_error;
	unsigned int vlan_replace_calls;
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

static int omci_services_vlan_validate(struct xpon_device *xpon,
				       const struct xpon_vlan_config *configs,
				       size_t count)
{
	struct omci_services_test_context *ctx = xpon_device_priv(xpon);

	if (ctx->vlan_preflight_error)
		return ctx->vlan_preflight_error;
	return count > OMCI_TEST_VLAN_CAPACITY ? -ENOSPC : 0;
}

static int omci_services_vlan_replace(struct xpon_device *xpon,
				      const struct xpon_vlan_config *config)
{
	struct omci_services_test_context *ctx = xpon_device_priv(xpon);
	int i, slot = -1;

	ctx->vlan_replace_calls++;
	if (config->cookie == ctx->fail_vlan_cookie) {
		ctx->fail_vlan_cookie = 0;
		return -EIO;
	}
	for (i = 0; i < OMCI_TEST_VLAN_CAPACITY; i++) {
		if (ctx->vlan_used[i] && ctx->vlans[i].cookie == config->cookie) {
			slot = i;
			break;
		}
		if (!ctx->vlan_used[i] && slot < 0)
			slot = i;
	}
	if (slot < 0)
		return -ENOSPC;
	ctx->vlans[slot] = *config;
	ctx->vlan_used[slot] = true;
	return 0;
}

static int omci_services_vlan_delete(struct xpon_device *xpon, u32 cookie)
{
	struct omci_services_test_context *ctx = xpon_device_priv(xpon);
	int i;

	if (cookie == ctx->fail_vlan_delete) {
		ctx->fail_vlan_delete = 0;
		return -EIO;
	}
	for (i = 0; i < OMCI_TEST_VLAN_CAPACITY; i++)
		if (ctx->vlan_used[i] && ctx->vlans[i].cookie == cookie) {
			ctx->vlan_used[i] = false;
			return 0;
		}
	return -ENOENT;
}

static const struct xpon_device_ops omci_services_vlan_ops = {
	.validate_vlans = omci_services_vlan_validate,
	.replace_vlan = omci_services_vlan_replace,
	.delete_vlan = omci_services_vlan_delete,
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
	int ret;

	ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;
	ctx->odev.parent = root_device_register("omci-services-kunit");
	if (IS_ERR(ctx->odev.parent))
		return PTR_ERR(ctx->odev.parent);
	ctx->netdev = alloc_etherdev(0);
	if (!ctx->netdev) {
		ret = -ENOMEM;
		goto parent;
	}
	ctx->odev.xpon = xpon_device_alloc(ctx->odev.parent, 0);
	if (IS_ERR(ctx->odev.xpon)) {
		ret = PTR_ERR(ctx->odev.xpon);
		goto netdev;
	}
	ctx->xpon_ops = omci_services_vlan_ops;
	ret = xpon_device_setup(ctx->odev.xpon, ctx->netdev,
				&ctx->xpon_ops, ctx,
				XPON_MODE_GPON, XPON_MODE_GPON);
	if (ret) {
		xpon_device_free(ctx->odev.xpon);
		goto netdev;
	}
	ctx->odev.priv = ctx;
	ctx->odev.ops = &omci_services_test_ops;
	mutex_init(&ctx->odev.agent.lock);
	xa_init(&ctx->odev.agent.mib);
	xa_init(&ctx->odev.agent.services);
	xa_init(&ctx->desired);
	test->priv = ctx;
	return 0;
netdev:
	free_netdev(ctx->netdev);
parent:
	root_device_unregister(ctx->odev.parent);
	return ret;
}

static void omci_services_test_exit(struct kunit *test)
{
	struct omci_services_test_context *ctx = test->priv;

	omci_services_test_free(&ctx->desired);
	omci_services_test_free(&ctx->odev.agent.services);
	xa_destroy(&ctx->odev.agent.mib);
	xpon_device_free(ctx->odev.xpon);
	free_netdev(ctx->netdev);
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

static void omci_vlan_test_rule(struct omci_extended_vlan_rule *rule)
{
	u8 raw[OMCI_EXT_VLAN_RULE_LEN + 1] = {};

	/* Single tag, any PCP/DEI/TPID: VID 10 -> 1894, preserve PCP/DEI/TPID. */
	put_unaligned_be32(0xf0000000, raw + 1);
	put_unaligned_be32(0x80050000, raw + 5);
	put_unaligned_be32(0x400f0000, raw + 9);
	put_unaligned_be32(0x00083b30, raw + 13);
	omci_ext_vlan_parse_rule(rule, raw + 1);
}

static void omci_vlan_rewrite_parse_test(struct kunit *test)
{
	struct omci_extended_vlan vlan = {};
	struct omci_extended_vlan_rule rule;
	struct xpon_vlan_config config;
	u16 original = (5 << VLAN_PRIO_SHIFT) | VLAN_CFI_MASK | 10;
	u16 translated, restored;

	omci_vlan_test_rule(&rule);
	KUNIT_ASSERT_EQ(test, omci_vlan_parse_rewrite(&vlan, &rule, &config), 0);
	KUNIT_EXPECT_EQ(test, rule.filter_inner_vid, 10);
	KUNIT_EXPECT_EQ(test, rule.treat_inner_vid, 1894);
	KUNIT_EXPECT_EQ(test, config.upstream.tci, 10);
	KUNIT_EXPECT_EQ(test, config.upstream.new_tci, 1894);
	KUNIT_EXPECT_EQ(test, config.upstream.new_tci_mask, VLAN_VID_MASK);
	KUNIT_EXPECT_TRUE(test, config.downstream_valid);
	KUNIT_EXPECT_EQ(test, config.downstream.tci, 1894);
	KUNIT_EXPECT_EQ(test, config.downstream.new_tci, 10);
	translated = (original & ~config.upstream.new_tci_mask) |
		     config.upstream.new_tci;
	KUNIT_EXPECT_EQ(test, translated & VLAN_VID_MASK, 1894);
	KUNIT_EXPECT_EQ(test, translated & ~VLAN_VID_MASK,
			original & ~VLAN_VID_MASK);
	restored = (translated & ~config.downstream.new_tci_mask) |
		   config.downstream.new_tci;
	KUNIT_EXPECT_EQ(test, restored, original);
}

static void omci_vlan_rewrite_only_test(struct kunit *test)
{
	struct omci_extended_vlan vlan = {};
	struct omci_extended_vlan_rule base, rule;
	struct xpon_vlan_config config = { .cookie = 123 };
	unsigned int i;

	omci_vlan_test_rule(&base);
	for (i = 0; i < 8; i++) {
		rule = base;
		switch (i) {
		case 0:
			rule.tags_to_remove = 0; /* Push. */
			break;
		case 1:
			rule.tags_to_remove = 2; /* Pop two. */
			break;
		case 2:
			rule.filter_ethertype = 1; /* EtherType filter. */
			break;
		case 3:
			rule.filter_inner_pbit = 15; /* Untagged. */
			break;
		case 4:
			rule.treat_inner_pbit = 15; /* Pop one. */
			break;
		case 5:
			rule.treat_outer_pbit = 0; /* Emit two tags. */
			break;
		case 6:
			rule.filter_outer_pbit = 0; /* Match two tags. */
			break;
		case 7:
			rule.delete = true;
			break;
		}
		KUNIT_EXPECT_EQ(test, omci_vlan_parse_rewrite(&vlan, &rule, &config),
				-EOPNOTSUPP);
		KUNIT_EXPECT_EQ(test, config.cookie, 123U);
	}
}

static void omci_vlan_inverse_information_test(struct kunit *test)
{
	struct omci_extended_vlan vlan = {};
	struct omci_extended_vlan_rule rule;
	struct xpon_vlan_config config;

	omci_vlan_test_rule(&rule);
	rule.treat_inner_pbit = 3;
	KUNIT_ASSERT_EQ(test, omci_vlan_parse_rewrite(&vlan, &rule, &config), 0);
	KUNIT_EXPECT_EQ(test, config.downstream.tci_mask, VLAN_VID_MASK | VLAN_PRIO_MASK);
	KUNIT_EXPECT_EQ(test, config.downstream.new_tci_mask, VLAN_VID_MASK);
	/* The inverse copies a wildcard input priority, as the SDK does. */
	KUNIT_EXPECT_EQ(test, config.downstream.tci >> VLAN_PRIO_SHIFT, 3);
	/* Transparent downstream does not need an inverse. */
	vlan.downstream_mode = 1;
	KUNIT_ASSERT_EQ(test, omci_vlan_parse_rewrite(&vlan, &rule, &config), 0);
	KUNIT_EXPECT_FALSE(test, config.downstream_valid);
	KUNIT_EXPECT_EQ(test, config.upstream.new_tci_mask,
			VLAN_VID_MASK | VLAN_PRIO_MASK);
	/* An exact input priority makes the inverse representable. */
	vlan.downstream_mode = 0;
	rule.filter_inner_pbit = 5;
	KUNIT_ASSERT_EQ(test, omci_vlan_parse_rewrite(&vlan, &rule, &config), 0);
	KUNIT_EXPECT_EQ(test, config.downstream.tci >> VLAN_PRIO_SHIFT, 3);
	KUNIT_EXPECT_EQ(test, config.downstream.new_tci >> VLAN_PRIO_SHIFT, 5);
}

static void omci_vlan_tpid_test(struct kunit *test)
{
	struct omci_extended_vlan vlan = {
		.input_tpid = ETH_P_8021Q,
		.output_tpid = ETH_P_8021AD,
	};
	struct omci_extended_vlan_rule rule;
	struct xpon_vlan_config config;

	omci_vlan_test_rule(&rule);
	rule.filter_inner_tpid_dei = 5;
	rule.treat_inner_tpid_dei = 2;
	KUNIT_ASSERT_EQ(test, omci_vlan_parse_rewrite(&vlan, &rule, &config), 0);
	KUNIT_EXPECT_EQ(test, config.upstream.tpid, htons(ETH_P_8021Q));
	KUNIT_EXPECT_EQ(test, config.upstream.new_tpid, htons(ETH_P_8021AD));
	KUNIT_EXPECT_EQ(test, config.downstream.tpid, htons(ETH_P_8021AD));
	KUNIT_EXPECT_EQ(test, config.downstream.new_tpid, htons(ETH_P_8021Q));
	rule.filter_inner_tpid_dei = 0;
	KUNIT_EXPECT_EQ(test, omci_vlan_parse_rewrite(&vlan, &rule, &config),
			-EOPNOTSUPP);
}

static void omci_vlan_invalid_test(struct kunit *test)
{
	struct omci_extended_vlan vlan = {};
	struct omci_extended_vlan_rule base, rule;
	struct xpon_vlan_config config;
	unsigned int i;

	omci_vlan_test_rule(&base);
	for (i = 0; i < 5; i++) {
		rule = base;
		switch (i) {
		case 0:
			rule.filter_inner_vid = 4095;
			break;
		case 1:
			rule.treat_inner_vid = 4095;
			break;
		case 2:
			rule.filter_inner_pbit = 11;
			break;
		case 3:
			rule.treat_inner_pbit = 11;
			break;
		case 4:
			rule.treat_inner_tpid_dei = 5;
			break;
		}
		KUNIT_EXPECT_EQ(test, omci_vlan_parse_rewrite(&vlan, &rule, &config),
				-EINVAL);
	}
	rule = base;
	memset(rule.raw + 8, 0xff, 8);
	omci_ext_vlan_parse_rule(&rule, rule.raw);
	KUNIT_EXPECT_TRUE(test, rule.delete);
}

static void omci_services_add_vlan(struct kunit *test, struct xarray *array,
				   u32 cookie, u16 vid)
{
	struct omci_service_state *state;
	struct omci_extended_vlan vlan = {};
	struct omci_extended_vlan_rule rule;
	int ret;

	omci_services_test_add(test, array, cookie, 256);
	state = xa_load(array, cookie);
	KUNIT_ASSERT_NOT_NULL(test, state);
	omci_vlan_test_rule(&rule);
	rule.treat_inner_vid = vid;
	ret = omci_vlan_parse_rewrite(&vlan, &rule, &state->config.vlan);
	KUNIT_ASSERT_EQ(test, ret, 0);
	state->config.vlan.cookie = cookie;
	state->config.vlan_rewrite = true;
}

static void omci_services_vlan_transaction_test(struct kunit *test)
{
	struct omci_services_test_context *ctx = test->priv;

	omci_services_add_vlan(test, &ctx->desired, 1, 1894);
	KUNIT_ASSERT_EQ(test, omci_services_test_apply(ctx), 0);
	KUNIT_EXPECT_TRUE(test, ctx->vlan_used[0]);
	KUNIT_EXPECT_EQ(test, ctx->vlans[0].upstream.new_tci, 1894);
	/* Changing only the translation must still replace the service. */
	omci_services_add_vlan(test, &ctx->desired, 1, 2000);
	ctx->fail_vlan_cookie = 1;
	KUNIT_EXPECT_EQ(test, omci_services_test_apply(ctx), -EIO);
	KUNIT_EXPECT_TRUE(test, ctx->vlan_used[0]);
	KUNIT_EXPECT_EQ(test, ctx->vlans[0].upstream.new_tci, 1894);
	KUNIT_EXPECT_TRUE(test, omci_services_test_has(ctx, 1, 256));
	/* The desired set stays owned by the caller after failure. */
	KUNIT_ASSERT_EQ(test, omci_services_test_apply(ctx), 0);
	KUNIT_EXPECT_EQ(test, ctx->vlans[0].upstream.new_tci, 2000);
	KUNIT_ASSERT_EQ(test, omci_services_test_apply(ctx), 0);
	KUNIT_EXPECT_FALSE(test, ctx->vlan_used[0]);
}

static void omci_services_vlan_preflight_test(struct kunit *test)
{
	struct omci_services_test_context *ctx = test->priv;

	omci_services_add_vlan(test, &ctx->desired, 1, 1894);
	ctx->vlan_preflight_error = -EOPNOTSUPP;
	KUNIT_EXPECT_EQ(test, omci_services_test_apply(ctx), -EOPNOTSUPP);
	KUNIT_EXPECT_EQ(test, ctx->replace_calls, 0U);
	KUNIT_EXPECT_EQ(test, ctx->vlan_replace_calls, 0U);
}

static void omci_services_vlan_delete_failure_test(struct kunit *test)
{
	struct omci_services_test_context *ctx = test->priv;

	omci_services_add_vlan(test, &ctx->desired, 1, 1894);
	KUNIT_ASSERT_EQ(test, omci_services_test_apply(ctx), 0);
	ctx->fail_vlan_delete = 1;
	KUNIT_EXPECT_EQ(test, omci_services_test_apply(ctx), -EIO);
	KUNIT_EXPECT_TRUE(test, ctx->vlan_used[0]);
	KUNIT_EXPECT_TRUE(test, omci_services_test_has(ctx, 1, 256));
}

static void omci_services_vlan_restart_test(struct kunit *test)
{
	struct omci_services_test_context *ctx = test->priv;

	omci_services_add_vlan(test, &ctx->desired, 1, 1894);
	KUNIT_ASSERT_EQ(test, omci_services_test_apply(ctx), 0);
	memset(ctx->used, 0, sizeof(ctx->used));
	memset(ctx->vlan_used, 0, sizeof(ctx->vlan_used));
	omci_services_add_vlan(test, &ctx->desired, 1, 1894);
	KUNIT_ASSERT_EQ(test, omci_services_test_apply(ctx), 0);
	KUNIT_EXPECT_TRUE(test, omci_services_test_has(ctx, 1, 256));
	KUNIT_EXPECT_TRUE(test, ctx->vlan_used[0]);
	KUNIT_EXPECT_EQ(test, ctx->vlan_replace_calls, 2U);
}

static void omci_services_vlan_no_provider_test(struct kunit *test)
{
	struct omci_services_test_context *ctx = test->priv;
	struct xpon_device *xpon = ctx->odev.xpon;

	omci_services_add_vlan(test, &ctx->desired, 1, 1894);
	ctx->odev.xpon = NULL;
	KUNIT_EXPECT_EQ(test, omci_services_test_apply(ctx), -EOPNOTSUPP);
	KUNIT_EXPECT_EQ(test, ctx->replace_calls, 0U);
	ctx->odev.xpon = xpon;
}

/* Captured from the provider's ME 171/0x0101, in table order. */
static const u8 omci_provider_rows[][OMCI_EXT_VLAN_RULE_LEN] = {
	{ 0xf8, 0, 0, 0, 0xe8, 0, 0x50, 0, 0xc0, 0x0f, 0, 0, 0, 0x0f, 0x80, 2 },
	{ 0xe8, 0, 0x50, 0, 0xe8, 0, 0, 0, 0xc0, 0x0f, 0, 0, 0, 0x0f, 0x80, 3 },
	{ 0xf8, 0, 0, 0, 0xf0, 0, 0x50, 0, 0xc0, 0x0f, 0, 0, 0, 0x0f, 0, 2 },
	{ 0xf8, 0, 0, 0, 0x80, 5, 0x50, 0, 0x40, 0x2f, 0, 0, 0, 0, 0x3b, 0x32 },
	{ 0x80, 5, 0x50, 0, 0x88, 0, 0, 0, 0x40, 0x2f, 0, 0, 0, 0, 0x3b, 0x33 },
};

static void omci_provider_config(struct kunit *test, unsigned int i,
				 struct xpon_vlan_config *config)
{
	struct omci_extended_vlan vlan = {
		.input_tpid = ETH_P_8021Q, .output_tpid = ETH_P_8021Q,
	};
	struct omci_extended_vlan_rule rule;

	omci_ext_vlan_parse_rule(&rule, omci_provider_rows[i]);
	KUNIT_ASSERT_EQ(test, omci_vlan_parse_rewrite(&vlan, &rule, config), 0);
	config->cookie = 10 + i;
	config->port_id = 0x101;
	config->wire_id = 295;
	config->priority |= i;
}

static void omci_provider_parse_test(struct kunit *test)
{
	struct xpon_vlan_config config;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(omci_provider_rows); i++) {
		omci_provider_config(test, i, &config);
		KUNIT_EXPECT_EQ(test, config.upstream.drop, i < 3);
		if (i < 3) {
			KUNIT_EXPECT_FALSE(test, config.downstream_valid);
			continue;
		}
		KUNIT_EXPECT_TRUE(test, config.downstream_valid);
		KUNIT_EXPECT_EQ(test, config.upstream.tci, 10);
		KUNIT_EXPECT_EQ(test, config.upstream.new_tci, 1894);
		KUNIT_EXPECT_EQ(test, config.upstream.new_tci_mask,
				VLAN_VID_MASK | VLAN_PRIO_MASK);
		KUNIT_EXPECT_EQ(test, config.downstream.tci, 1894);
		KUNIT_EXPECT_EQ(test, config.downstream.tci_mask,
				VLAN_VID_MASK | VLAN_PRIO_MASK);
		KUNIT_EXPECT_EQ(test, config.downstream.new_tci_mask, VLAN_VID_MASK);
		KUNIT_EXPECT_EQ(test, config.downstream.new_tci, 10);
		KUNIT_EXPECT_EQ(test, config.upstream.tag_mode,
				i == 3 ? XPON_VLAN_SINGLE_TAG : XPON_VLAN_DOUBLE_TAG);
	}
}

static void omci_provider_mapper_test(struct kunit *test)
{
	struct omci_services_test_context *ctx = test->priv;
	struct omci_mib_object *objects, *vlan, *gem, *iwtp, *tcont;
	struct omci_service_state *state;
	struct omci_mib_object lan_port = { .entity_id = 0x101 };
	unsigned int count = 0, drops = 0, rewrites = 0, filters = 0;
	bool default_installed = false;
	unsigned long index;
	unsigned int i;

	objects = kunit_kcalloc(test, 5, sizeof(*objects), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, objects);
	vlan = &objects[0];
	vlan->class_id = OMCI_CLASS_EXTENDED_VLAN;
	vlan->entity_id = 0x101;
	vlan->extended_vlan.valid = true;
	vlan->extended_vlan.associated_me = lan_port.entity_id;
	vlan->extended_vlan.input_tpid = ETH_P_8021Q;
	vlan->extended_vlan.output_tpid = ETH_P_8021Q;
	vlan->extended_vlan.rule_count = ARRAY_SIZE(omci_provider_rows);
	for (i = 0; i < ARRAY_SIZE(omci_provider_rows); i++)
		omci_ext_vlan_parse_rule(&vlan->extended_vlan.rules[i],
					 omci_provider_rows[i]);
	gem = &objects[1];
	gem->class_id = OMCI_CLASS_GEM_PORT_CTP;
	gem->entity_id = 0x101;
	gem->origin = OMCI_MIB_ORIGIN_OLT;
	put_unaligned_be16(295, gem->data);
	put_unaligned_be16(0x8000, gem->data + 2);
	gem->data[4] = OMCI_GEM_PORT_DIRECTION_BIDIRECTIONAL;
	iwtp = &objects[2];
	iwtp->class_id = OMCI_CLASS_GEM_IWTP;
	iwtp->entity_id = 0x101;
	put_unaligned_be16(gem->entity_id, iwtp->data);
	tcont = &objects[3];
	tcont->class_id = OMCI_CLASS_TCONT;
	tcont->entity_id = 0x8000;
	put_unaligned_be16(295, tcont->data);
	objects[4].class_id = OMCI_CLASS_VLAN_TAGGING_FILTER_DATA;
	objects[4].entity_id = 0x101;
	objects[4].vlan_filter.valid = true;
	objects[4].vlan_filter.num_entries = 1;
	objects[4].vlan_filter.forward_operation = 0x10;
	objects[4].vlan_filter.entries[0].vid = 1894;
	for (i = 0; i < 5; i++) {
		unsigned long key = ((unsigned long)objects[i].class_id << 16) |
				    objects[i].entity_id;
		int ret;

		ret = xa_insert(&ctx->odev.agent.mib, key, &objects[i], GFP_KERNEL);
		KUNIT_ASSERT_EQ(test, ret, 0);
	}

	/* All eight mapper priorities point at the same provider GEM. */
	mutex_lock(&ctx->odev.agent.lock);
	for (i = 0; i < 8; i++) {
		int ret = omci_agent_stage_path_locked(&ctx->odev, &ctx->desired,
			&lan_port, 0x101, 0x101, 0x101, iwtp->entity_id,
			i, true, false, 0, false, &default_installed);

		KUNIT_EXPECT_EQ(test, ret, 0);
	}
	mutex_unlock(&ctx->odev.agent.lock);
	xa_for_each(&ctx->desired, index, state) {
		const struct omci_service_config *service = &state->config;

		count++;
		if (!service->vlan_rewrite) {
			filters++;
			continue;
		}
		if (service->vlan.upstream.drop) {
			drops++;
			continue;
		}
		rewrites++;
		KUNIT_EXPECT_EQ(test, service->gem_port_id, 295);
		KUNIT_EXPECT_EQ(test, service->vlan.upstream.tci, 10);
		KUNIT_EXPECT_EQ(test, service->vlan.upstream.new_tci, 1894);
		KUNIT_EXPECT_EQ(test, service->queue, 0);
	}
	KUNIT_EXPECT_EQ(test, count, 34U);
	KUNIT_EXPECT_EQ(test, filters, 8U);
	KUNIT_EXPECT_EQ(test, drops, 24U);
	KUNIT_EXPECT_EQ(test, rewrites, 2U);
}

static void omci_provider_stage(struct kunit *test)
{
	struct omci_services_test_context *ctx = test->priv;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(omci_provider_rows); i++) {
		struct omci_service_state *state;

		state = kzalloc(sizeof(*state), GFP_KERNEL);
		KUNIT_ASSERT_NOT_NULL(test, state);
		omci_provider_config(test, i, &state->config.vlan);
		state->config.cookie = state->config.vlan.cookie;
		state->config.gem_port_id = 295;
		state->config.vlan_rewrite = true;
		KUNIT_ASSERT_EQ(test, xa_insert(&ctx->desired, state->config.cookie,
						state, GFP_KERNEL), 0);
	}
}

static void omci_provider_transaction_test(struct kunit *test)
{
	struct omci_services_test_context *ctx = test->priv;
	unsigned int i;

	omci_provider_stage(test);
	KUNIT_ASSERT_EQ(test, omci_services_test_apply(ctx), 0);
	/* Five policies/treatments consume just two forwarding slots. */
	KUNIT_EXPECT_EQ(test, ctx->replace_calls, 2U);
	KUNIT_EXPECT_EQ(test, ctx->vlan_replace_calls, 5U);
	KUNIT_EXPECT_TRUE(test, omci_services_test_has(ctx, 13, 295));
	KUNIT_EXPECT_TRUE(test, omci_services_test_has(ctx, 14, 295));
	omci_services_test_free(&ctx->desired);
	xa_init(&ctx->desired);
	KUNIT_ASSERT_EQ(test, omci_services_test_apply(ctx), 0);
	for (i = 0; i < OMCI_TEST_VLAN_CAPACITY; i++)
		KUNIT_EXPECT_FALSE(test, ctx->vlan_used[i]);
	KUNIT_EXPECT_FALSE(test, ctx->used[0]);
	KUNIT_EXPECT_FALSE(test, ctx->used[1]);
}

static void omci_provider_rollback_test(struct kunit *test)
{
	struct omci_services_test_context *ctx = test->priv;
	unsigned int i;

	omci_services_test_seed(test);
	omci_provider_stage(test);
	ctx->fail_vlan_cookie = 14;
	KUNIT_EXPECT_EQ(test, omci_services_test_apply(ctx), -EIO);
	KUNIT_EXPECT_TRUE(test, omci_services_test_has(ctx, 1, 10));
	KUNIT_EXPECT_TRUE(test, omci_services_test_has(ctx, 2, 20));
	for (i = 0; i < OMCI_TEST_VLAN_CAPACITY; i++)
		KUNIT_EXPECT_FALSE(test, ctx->vlan_used[i]);
}

/*
 * A Delete marks its object, works the services out again and only then
 * removes it (omci_agent_delete_locked()). Deleting the ANI-side port of a
 * bridge has to take its service out of the hardware in that step.
 */
static void omci_services_deleted_ani_port_test(struct kunit *test)
{
	struct omci_services_test_context *ctx = test->priv;
	struct omci_mib_object *objects, *uni, *lan, *ani, *iwtp, *gem, *tcont;
	unsigned int i, installed = 0;
	int ret;

	objects = kunit_kcalloc(test, 6, sizeof(*objects), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, objects);
	/* Administrative state 0: unlocked */
	uni = &objects[0];
	uni->class_id = OMCI_CLASS_PPTP_ETHERNET_UNI;
	uni->entity_id = 0x101;
	/* Bridge 1, TP type 1 (PPTP Ethernet UNI) */
	lan = &objects[1];
	lan->class_id = OMCI_CLASS_MAC_BRIDGE_PORT_CONFIG_DATA;
	lan->entity_id = 1;
	put_unaligned_be16(1, lan->data);
	lan->data[3] = 1;
	put_unaligned_be16(uni->entity_id, lan->data + 4);
	/* Bridge 1, TP type 5 (GEM interworking TP) */
	ani = &objects[2];
	ani->class_id = OMCI_CLASS_MAC_BRIDGE_PORT_CONFIG_DATA;
	ani->entity_id = 2;
	put_unaligned_be16(1, ani->data);
	ani->data[3] = 5;
	put_unaligned_be16(0x201, ani->data + 4);
	iwtp = &objects[3];
	iwtp->class_id = OMCI_CLASS_GEM_IWTP;
	iwtp->entity_id = 0x201;
	put_unaligned_be16(0x301, iwtp->data);
	gem = &objects[4];
	gem->class_id = OMCI_CLASS_GEM_PORT_CTP;
	gem->entity_id = 0x301;
	gem->origin = OMCI_MIB_ORIGIN_OLT;
	put_unaligned_be16(295, gem->data);
	put_unaligned_be16(0x8000, gem->data + 2);
	gem->data[4] = OMCI_GEM_PORT_DIRECTION_BIDIRECTIONAL;
	tcont = &objects[5];
	tcont->class_id = OMCI_CLASS_TCONT;
	tcont->entity_id = 0x8000;
	put_unaligned_be16(295, tcont->data);
	for (i = 0; i < 6; i++) {
		unsigned long key = ((unsigned long)objects[i].class_id << 16) |
				    objects[i].entity_id;

		ret = xa_insert(&ctx->odev.agent.mib, key, &objects[i], GFP_KERNEL);
		KUNIT_ASSERT_EQ(test, ret, 0);
	}

	mutex_lock(&ctx->odev.agent.lock);
	ret = omci_agent_reconcile_services_locked(&ctx->odev);
	mutex_unlock(&ctx->odev.agent.lock);
	KUNIT_ASSERT_EQ(test, ret, 0);
	for (i = 0; i < OMCI_TEST_CAPACITY; i++)
		installed += ctx->used[i] && ctx->hardware[i].gem_port_id == 295;
	KUNIT_ASSERT_EQ(test, installed, 1U);

	ani->pending_delete = true;
	mutex_lock(&ctx->odev.agent.lock);
	ret = omci_agent_reconcile_services_locked(&ctx->odev);
	mutex_unlock(&ctx->odev.agent.lock);
	KUNIT_ASSERT_EQ(test, ret, 0);
	for (i = 0; i < OMCI_TEST_CAPACITY; i++)
		KUNIT_EXPECT_FALSE(test, ctx->used[i]);
}

/* A UNI, mapper, IWTP and bridge port deliberately use distinct IDs. */
struct omci_ani_fixture {
	struct omci_mib_object lan;
	struct omci_mib_object gem;
	struct omci_mib_object iwtp;
	struct omci_mib_object tcont;
	struct omci_mib_object vlan;
	struct omci_mib_object filter;
};

static const u8 omci_huawei_insert_rows[][OMCI_EXT_VLAN_RULE_LEN] = {
	{ 0xf8, 0, 0, 0, 0xf8, 0, 0, 0, 0, 0x0f, 0, 0, 0, 0, 0x01, 0x18 },
	{ 0xf8, 0, 0, 0, 0xf8, 0, 0, 0, 0, 0x0f, 0, 0, 0, 3, 0x4e, 0x30 },
	{ 0xf8, 0, 0, 0, 0xf8, 0, 0, 0, 0, 0x0f, 0, 0, 0, 5, 0x0a, 0x78 },
};

static void omci_ani_insert_object(struct kunit *test,
				   struct omci_mib_object *object)
{
	struct omci_services_test_context *ctx = test->priv;
	unsigned long key = ((unsigned long)object->class_id << 16) |
			    object->entity_id;

	KUNIT_ASSERT_EQ(test, xa_insert(&ctx->odev.agent.mib, key, object,
					GFP_KERNEL), 0);
}

static struct omci_ani_fixture *omci_ani_fixture_init(struct kunit *test)
{
	struct omci_ani_fixture *f;

	f = kunit_kzalloc(test, sizeof(*f), GFP_KERNEL);
	if (!f)
		return NULL;
	f->lan.class_id = OMCI_CLASS_MAC_BRIDGE_PORT_CONFIG_DATA;
	f->lan.entity_id = 1;
	f->lan.data[3] = 1; /* PPTP Ethernet UNI */
	put_unaligned_be16(0x101, f->lan.data + 4);
	f->gem.class_id = OMCI_CLASS_GEM_PORT_CTP;
	f->gem.entity_id = 4;
	f->gem.origin = OMCI_MIB_ORIGIN_OLT;
	put_unaligned_be16(388, f->gem.data);
	put_unaligned_be16(0x8001, f->gem.data + 2);
	f->gem.data[4] = OMCI_GEM_PORT_DIRECTION_BIDIRECTIONAL;
	f->iwtp.class_id = OMCI_CLASS_GEM_IWTP;
	f->iwtp.entity_id = 3;
	put_unaligned_be16(f->gem.entity_id, f->iwtp.data);
	f->tcont.class_id = OMCI_CLASS_TCONT;
	f->tcont.entity_id = 0x8001;
	put_unaligned_be16(388, f->tcont.data);
	f->vlan.class_id = OMCI_CLASS_EXTENDED_VLAN;
	/* This ID collides with the bridge port, not with its association. */
	f->vlan.entity_id = f->lan.entity_id;
	f->vlan.extended_vlan.valid = true;
	f->vlan.extended_vlan.association_type = 2;
	f->vlan.extended_vlan.associated_me = 0x101;
	f->vlan.extended_vlan.rule_count = 1;
	f->vlan.extended_vlan.input_tpid = ETH_P_8021Q;
	f->vlan.extended_vlan.output_tpid = ETH_P_8021Q;
	omci_ext_vlan_parse_rule(&f->vlan.extended_vlan.rules[0],
				 omci_huawei_insert_rows[0]);
	omci_ani_insert_object(test, &f->gem);
	omci_ani_insert_object(test, &f->iwtp);
	omci_ani_insert_object(test, &f->tcont);
	omci_ani_insert_object(test, &f->vlan);
	return f;
}

static int omci_ani_stage(struct omci_services_test_context *ctx,
			  struct omci_ani_fixture *f, u8 pcp, bool mapper)
{
	bool default_installed = false;
	int ret;

	omci_services_test_free(&ctx->desired);
	xa_init(&ctx->desired);
	mutex_lock(&ctx->odev.agent.lock);
	ret = omci_agent_stage_path_locked(&ctx->odev, &ctx->desired,
					   &f->lan,
					   get_unaligned_be16(f->lan.data + 4),
					   6, 2, f->iwtp.entity_id, pcp, mapper,
					   false, 0, false, &default_installed);
	mutex_unlock(&ctx->odev.agent.lock);
	return ret;
}

static void omci_services_ani_insertion_test(struct kunit *test)
{
	static const u16 vids[] = { 35, 2502, 335 };
	static const u8 pcps[] = { 0, 3, 5 };
	struct omci_services_test_context *ctx = test->priv;
	struct omci_ani_fixture *f = omci_ani_fixture_init(test);
	struct omci_service_state *state;
	unsigned long index;
	unsigned int i, count;

	KUNIT_ASSERT_NOT_NULL(test, f);
	ctx->xpon_ops.vlan_domain = XPON_VLAN_DOMAIN_ANI;
	for (i = 0; i < ARRAY_SIZE(vids); i++) {
		omci_ext_vlan_parse_rule(&f->vlan.extended_vlan.rules[0],
					 omci_huawei_insert_rows[i]);
		KUNIT_ASSERT_EQ(test, omci_ani_stage(ctx, f, pcps[i], true), 0);
		count = 0;
		xa_for_each(&ctx->desired, index, state) {
			count++;
			KUNIT_EXPECT_TRUE(test, state->config.vlan_valid);
			KUNIT_EXPECT_EQ(test, state->config.vlan_id, vids[i]);
			KUNIT_EXPECT_EQ(test, state->config.pcp, pcps[i]);
			KUNIT_EXPECT_EQ(test, state->config.queue, pcps[i]);
			KUNIT_EXPECT_EQ(test, state->config.gem_port_id, 388);
			KUNIT_EXPECT_EQ(test, state->config.alloc_id, 388);
			KUNIT_EXPECT_FALSE(test, state->config.vlan_rewrite);
		}
		KUNIT_EXPECT_EQ(test, count, 1U);
		/* The output PCP belongs to its mapper branch, not all eight. */
		KUNIT_ASSERT_EQ(test, omci_ani_stage(ctx, f, (pcps[i] + 1) % 8,
						     true), 0);
		KUNIT_EXPECT_TRUE(test, xa_empty(&ctx->desired));
	}
	/* Real UNI input still needs insertion support and must report it. */
	ctx->xpon_ops.vlan_domain = XPON_VLAN_DOMAIN_UNI;
	KUNIT_EXPECT_EQ(test, omci_ani_stage(ctx, f, pcps[2], true), -EOPNOTSUPP);
}

static void omci_services_association_namespace_test(struct kunit *test)
{
	static const struct {
		u8 association, uni_type;
		u16 pointer;
		bool mapper, match;
	} cases[] = {
		{ 0, 1, 1, true, true },
		{ 0, 1, 6, true, true },
		{ 0, 1, 2, true, false },
		{ 1, 1, 2, true, true },
		{ 1, 1, 2, false, false },
		{ 2, 1, 0x101, true, true },
		{ 2, 1, 2, true, false },
		{ 2, 11, 0x101, true, false },
		{ 5, 1, 3, true, true },
		{ 5, 1, 2, true, false },
		{ 6, 1, 3, true, false },
		{ 10, 11, 0x101, true, true },
		{ 10, 1, 0x101, true, false },
		{ 10, 11, 2, true, false },
	};
	struct omci_services_test_context *ctx = test->priv;
	struct omci_ani_fixture *f = omci_ani_fixture_init(test);
	struct omci_service_state *state;
	unsigned long index;
	unsigned int i, matches;

	KUNIT_ASSERT_NOT_NULL(test, f);
	ctx->xpon_ops.vlan_domain = XPON_VLAN_DOMAIN_ANI;
	for (i = 0; i < ARRAY_SIZE(cases); i++) {
		f->vlan.extended_vlan.association_type = cases[i].association;
		f->vlan.extended_vlan.associated_me = cases[i].pointer;
		f->lan.data[3] = cases[i].uni_type;
		KUNIT_ASSERT_EQ(test, omci_ani_stage(ctx, f, 0, cases[i].mapper), 0);
		matches = 0;
		xa_for_each(&ctx->desired, index, state)
			matches += state->config.vlan_valid;
		KUNIT_EXPECT_LE(test, matches, 1U);
		KUNIT_EXPECT_EQ_MSG(test, matches == 1, cases[i].match,
				    "association case %u", i);
	}
}

static void omci_services_ani_filter_test(struct kunit *test)
{
	struct omci_services_test_context *ctx = test->priv;
	struct omci_ani_fixture *f = omci_ani_fixture_init(test);
	struct omci_service_state *state;
	unsigned long index;
	unsigned int count = 0;

	KUNIT_ASSERT_NOT_NULL(test, f);
	ctx->xpon_ops.vlan_domain = XPON_VLAN_DOMAIN_ANI;
	f->vlan.extended_vlan.rule_count = 0;
	f->filter.class_id = OMCI_CLASS_VLAN_TAGGING_FILTER_DATA;
	f->filter.entity_id = 6;
	f->filter.vlan_filter.valid = true;
	f->filter.vlan_filter.forward_operation = 0x10;
	f->filter.vlan_filter.num_entries = 1;
	f->filter.vlan_filter.entries[0].vid = 2501;
	omci_ani_insert_object(test, &f->filter);
	/* VID-only filter: preserve mapper PCP 5 despite zero TCI PCP bits. */
	KUNIT_ASSERT_EQ(test, omci_ani_stage(ctx, f, 5, true), 0);
	xa_for_each(&ctx->desired, index, state) {
		count++;
		KUNIT_EXPECT_EQ(test, state->config.vlan_id, 2501);
		KUNIT_EXPECT_EQ(test, state->config.pcp, 5);
		KUNIT_EXPECT_EQ(test, state->config.queue, 5);
	}
	KUNIT_EXPECT_EQ(test, count, 1U);
	/* A UNI's VLAN 35 insertion must not add VLAN 35 to the 2501 path. */
	f->vlan.extended_vlan.rule_count = 1;
	KUNIT_ASSERT_EQ(test, omci_ani_stage(ctx, f, 0, true), 0);
	count = 0;
	xa_for_each(&ctx->desired, index, state) {
		count++;
		KUNIT_EXPECT_EQ(test, state->config.vlan_id, 2501);
	}
	KUNIT_EXPECT_EQ(test, count, 1U);
	/* TCI filtering, unlike VID-only filtering, does constrain priority. */
	f->vlan.extended_vlan.rule_count = 0;
	f->filter.vlan_filter.forward_operation = 0x0c;
	f->filter.vlan_filter.entries[0].pbit = 3;
	KUNIT_ASSERT_EQ(test, omci_ani_stage(ctx, f, 5, true), 0);
	KUNIT_EXPECT_TRUE(test, xa_empty(&ctx->desired));
	KUNIT_ASSERT_EQ(test, omci_ani_stage(ctx, f, 3, true), 0);
	KUNIT_EXPECT_FALSE(test, xa_empty(&ctx->desired));
	f->filter.vlan_filter.entries[0].vid = 35;
	f->vlan.extended_vlan.rule_count = 1; /* Emits PCP 0, not PCP 3. */
	KUNIT_ASSERT_EQ(test, omci_ani_stage(ctx, f, 0, true), 0);
	KUNIT_EXPECT_TRUE(test, xa_empty(&ctx->desired));
	f->vlan.extended_vlan.rule_count = 0;
	/* ME 84 entity 2 is not attached to bridge port 6 or LAN port 1. */
	xa_erase(&ctx->odev.agent.mib,
		 ((unsigned long)f->filter.class_id << 16) | f->filter.entity_id);
	f->filter.entity_id = 2; /* Coincides only with the mapper ID. */
	omci_ani_insert_object(test, &f->filter);
	KUNIT_ASSERT_EQ(test, omci_ani_stage(ctx, f, 3, true), 0);
	xa_for_each(&ctx->desired, index, state)
		KUNIT_EXPECT_FALSE(test, state->config.vlan_valid);
}

static void omci_services_ani_discard_test(struct kunit *test)
{
	static const u8 discard[OMCI_EXT_VLAN_RULE_LEN] = {
		0xe8, 0, 0, 0, 0xe8, 0, 0, 0, 0xc0, 0x0f, 0, 0, 0, 0x0f, 0, 0,
	};
	struct omci_services_test_context *ctx = test->priv;
	struct omci_ani_fixture *f = omci_ani_fixture_init(test);

	KUNIT_ASSERT_NOT_NULL(test, f);
	ctx->xpon_ops.vlan_domain = XPON_VLAN_DOMAIN_ANI;
	omci_ext_vlan_parse_rule(&f->vlan.extended_vlan.rules[0], discard);
	KUNIT_ASSERT_EQ(test, omci_ani_stage(ctx, f, 0, true), 0);
	/* No global ANI drop rule, and no wildcard forwarding fallback. */
	KUNIT_EXPECT_TRUE(test, xa_empty(&ctx->desired));
	KUNIT_EXPECT_EQ(test, ctx->vlan_replace_calls, 0U);
}

/* Replay every VID/GEM pair advertised in hiedgm's old ME 84/268 dump. */
static void omci_services_ani_network_paths_test(struct kunit *test)
{
	static const u16 vids[] = { 2501, 35, 36, 2502, 335 };
	static const u16 gems[] = { 260, 388, 516, 644, 772 };
	struct omci_services_test_context *ctx = test->priv;
	struct omci_ani_fixture *f = omci_ani_fixture_init(test);
	struct omci_service_state *state;
	unsigned int i, pcp, count;
	unsigned long index;

	KUNIT_ASSERT_NOT_NULL(test, f);
	ctx->xpon_ops.vlan_domain = XPON_VLAN_DOMAIN_ANI;
	f->filter.class_id = OMCI_CLASS_VLAN_TAGGING_FILTER_DATA;
	f->filter.entity_id = 6;
	f->filter.vlan_filter.valid = true;
	f->filter.vlan_filter.forward_operation = 0x10;
	f->filter.vlan_filter.num_entries = 1;
	omci_ani_insert_object(test, &f->filter);
	for (i = 0; i < ARRAY_SIZE(vids); i++) {
		f->filter.vlan_filter.entries[0].vid = vids[i];
		put_unaligned_be16(gems[i], f->gem.data);
		put_unaligned_be16(gems[i], f->tcont.data);
		for (pcp = 0; pcp < 8; pcp++) {
			KUNIT_ASSERT_EQ(test, omci_ani_stage(ctx, f, pcp, true), 0);
			count = 0;
			xa_for_each(&ctx->desired, index, state) {
				count++;
				KUNIT_EXPECT_TRUE(test, state->config.vlan_valid);
				KUNIT_EXPECT_EQ(test, state->config.vlan_id, vids[i]);
				KUNIT_EXPECT_EQ(test, state->config.gem_port_id, gems[i]);
				KUNIT_EXPECT_EQ(test, state->config.pcp, pcp);
				KUNIT_EXPECT_EQ(test, state->config.alloc_id, gems[i]);
				KUNIT_EXPECT_FALSE(test, state->config.vlan_rewrite);
			}
			KUNIT_EXPECT_GT(test, count, 0U);
		}
	}
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
	KUNIT_CASE(omci_vlan_rewrite_parse_test),
	KUNIT_CASE(omci_vlan_rewrite_only_test),
	KUNIT_CASE(omci_vlan_inverse_information_test),
	KUNIT_CASE(omci_vlan_tpid_test),
	KUNIT_CASE(omci_vlan_invalid_test),
	KUNIT_CASE(omci_provider_parse_test),
	KUNIT_CASE(omci_provider_mapper_test),
	KUNIT_CASE(omci_provider_transaction_test),
	KUNIT_CASE(omci_provider_rollback_test),
	KUNIT_CASE(omci_services_vlan_transaction_test),
	KUNIT_CASE(omci_services_vlan_preflight_test),
	KUNIT_CASE(omci_services_vlan_delete_failure_test),
	KUNIT_CASE(omci_services_vlan_restart_test),
	KUNIT_CASE(omci_services_vlan_no_provider_test),
	KUNIT_CASE(omci_services_deleted_ani_port_test),
	KUNIT_CASE(omci_services_ani_insertion_test),
	KUNIT_CASE(omci_services_association_namespace_test),
	KUNIT_CASE(omci_services_ani_filter_test),
	KUNIT_CASE(omci_services_ani_discard_test),
	KUNIT_CASE(omci_services_ani_network_paths_test),
	{}
};

static struct kunit_suite omci_services_test_suite = {
	.name = "omci-services",
	.init = omci_services_test_init,
	.exit = omci_services_test_exit,
	.test_cases = omci_services_test_cases,
};

kunit_test_suite(omci_services_test_suite);
