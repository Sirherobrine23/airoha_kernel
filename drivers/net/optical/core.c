// SPDX-License-Identifier: GPL-2.0-only
/*
 * Generic optical frontend provider/consumer core.
 *
 * Optical frontends are analog components in front of a digital PHY/MAC:
 * laser drivers, limiting amplifiers, APD controllers and similar devices.
 * They are intentionally modelled independently from PHYLIB, phylink and the
 * generic PHY framework so the same physical frontend can be shared by the
 * MAC, protocol management (for example OMCI) and diagnostic consumers.
 */

#include <linux/device.h>
#include <linux/device/class.h>
#include <linux/device/devres.h>
#include <linux/err.h>
#include <linux/idr.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/minmax.h>
#include <linux/mutex.h>
#include <linux/optical_frontend.h>
#include <linux/property.h>
#include <linux/slab.h>
#include <linux/string.h>

struct optical_frontend {
	struct device dev;
	struct device *provider;
	const struct optical_frontend_desc *desc;
	const struct optical_frontend_ops *ops;
	void *drvdata;

	struct mutex op_lock; /* serializes provider callbacks and shared state */
	struct optical_frontend_mode mode;
	bool mode_valid;

	struct optical_frontend_telemetry telemetry_cache;
	unsigned long telemetry_expires;
	bool telemetry_cache_valid;

	int id;
};

static DEFINE_IDA(optical_frontend_ida);

static void optical_frontend_dev_release(struct device *dev)
{
	struct optical_frontend *frontend =
		container_of(dev, struct optical_frontend, dev);

	ida_free(&optical_frontend_ida, frontend->id);
	kfree(frontend);
}

static const struct class optical_frontend_class = {
	.name = "optical_frontend",
	.dev_release = optical_frontend_dev_release,
};

static void optical_frontend_unregister(void *data)
{
	struct optical_frontend *frontend = data;

	device_unregister(&frontend->dev);
}

/**
 * devm_optical_frontend_register() - register one optical frontend provider
 * @dev: physical provider device
 * @desc: immutable provider description
 * @ops: provider operations
 * @drvdata: provider-private context returned by optical_frontend_get_drvdata()
 */
struct optical_frontend *
devm_optical_frontend_register(struct device *dev,
			       const struct optical_frontend_desc *desc,
			       const struct optical_frontend_ops *ops,
			       void *drvdata)
{
	struct optical_frontend *frontend;
	int ret;

	if (!dev || !desc || !desc->name || !ops)
		return ERR_PTR(-EINVAL);

	frontend = kzalloc(sizeof(*frontend), GFP_KERNEL);
	if (!frontend)
		return ERR_PTR(-ENOMEM);

	frontend->id = ida_alloc(&optical_frontend_ida, GFP_KERNEL);
	if (frontend->id < 0) {
		ret = frontend->id;
		kfree(frontend);
		return ERR_PTR(ret);
	}

	frontend->provider = dev;
	frontend->desc = desc;
	frontend->ops = ops;
	frontend->drvdata = drvdata;
	mutex_init(&frontend->op_lock);

	device_initialize(&frontend->dev);
	frontend->dev.class = &optical_frontend_class;
	frontend->dev.parent = dev;
	device_set_node(&frontend->dev, dev_fwnode(dev));
	dev_set_name(&frontend->dev, "frontend%d", frontend->id);

	ret = device_add(&frontend->dev);
	if (ret) {
		put_device(&frontend->dev);
		return ERR_PTR(ret);
	}

	ret = devm_add_action_or_reset(dev, optical_frontend_unregister,
				       frontend);
	if (ret)
		return ERR_PTR(ret);

	dev_dbg(dev, "registered optical frontend %s as %s\n",
		desc->name, dev_name(&frontend->dev));

	return frontend;
}
EXPORT_SYMBOL_GPL(devm_optical_frontend_register);

static void optical_frontend_put_action(void *data)
{
	put_device(data);
}

struct optical_frontend *
devm_optical_frontend_get_by_fwnode(struct device *dev,
				    const struct fwnode_handle *fwnode)
{
	struct optical_frontend *frontend;
	struct device *frontend_dev;
	struct device_link *link;
	int ret;

	if (!dev || !fwnode)
		return ERR_PTR(-EINVAL);

	frontend_dev = class_find_device_by_fwnode(&optical_frontend_class,
						   fwnode);
	if (!frontend_dev)
		return ERR_PTR(-EPROBE_DEFER);

	frontend = container_of(frontend_dev, struct optical_frontend, dev);
	link = device_link_add(dev, frontend->provider,
			       DL_FLAG_AUTOREMOVE_CONSUMER);
	if (!link) {
		put_device(frontend_dev);
		return ERR_PTR(-ENOMEM);
	}

	ret = devm_add_action_or_reset(dev, optical_frontend_put_action,
				       frontend_dev);
	if (ret)
		return ERR_PTR(ret);

	return frontend;
}
EXPORT_SYMBOL_GPL(devm_optical_frontend_get_by_fwnode);

/**
 * devm_optical_frontend_get_optional() - obtain a named frontend consumer
 * @dev: consumer device
 * @name: entry in optical-frontend-names, or NULL for entry zero
 *
 * Firmware binding:
 *
 *   optical-frontends = <&frontend>;
 *   optical-frontend-names = "pon";
 *
 * Providers expose #optical-frontend-cells = <0>.
 */
struct optical_frontend *
devm_optical_frontend_get_optional(struct device *dev, const char *name)
{
	struct fwnode_reference_args args;
	struct optical_frontend *frontend;
	struct fwnode_handle *fwnode;
	int index = 0;
	int ret;

	if (!dev)
		return ERR_PTR(-EINVAL);

	fwnode = dev_fwnode(dev);
	if (!fwnode)
		return NULL;

	if (name) {
		index = fwnode_property_match_string(
			fwnode, "optical-frontend-names", name);
		if (index < 0) {
			if (index == -EINVAL || index == -ENODATA ||
			    index == -ENOENT)
				return NULL;
			return ERR_PTR(index);
		}
	}

	ret = fwnode_property_get_reference_args(fwnode, "optical-frontends",
						 "#optical-frontend-cells",
						 0, index, &args);
	if (ret) {
		if (ret == -ENOENT)
			return NULL;
		return ERR_PTR(ret);
	}

	if (args.nargs) {
		fwnode_handle_put(args.fwnode);
		return ERR_PTR(-EINVAL);
	}

	frontend = devm_optical_frontend_get_by_fwnode(dev, args.fwnode);
	fwnode_handle_put(args.fwnode);

	return frontend;
}
EXPORT_SYMBOL_GPL(devm_optical_frontend_get_optional);

void *optical_frontend_get_drvdata(struct optical_frontend *frontend)
{
	return frontend ? frontend->drvdata : NULL;
}
EXPORT_SYMBOL_GPL(optical_frontend_get_drvdata);

struct device *optical_frontend_get_provider(struct optical_frontend *frontend)
{
	return frontend ? frontend->provider : NULL;
}
EXPORT_SYMBOL_GPL(optical_frontend_get_provider);

const struct optical_frontend_desc *
optical_frontend_get_desc(struct optical_frontend *frontend)
{
	return frontend ? frontend->desc : NULL;
}
EXPORT_SYMBOL_GPL(optical_frontend_get_desc);

int optical_frontend_set_mode(struct optical_frontend *frontend,
			      const struct optical_frontend_mode *mode)
{
	int ret = 0;

	if (!frontend || !mode)
		return -EINVAL;

	if (mode->protocol >= 32 ||
	    !(frontend->desc->protocols & BIT(mode->protocol)))
		return -EOPNOTSUPP;

	mutex_lock(&frontend->op_lock);
	if (frontend->ops->set_mode)
		ret = frontend->ops->set_mode(frontend, mode);
	if (!ret) {
		frontend->mode = *mode;
		frontend->mode_valid = true;
		frontend->telemetry_cache_valid = false;
	}
	mutex_unlock(&frontend->op_lock);

	return ret;
}
EXPORT_SYMBOL_GPL(optical_frontend_set_mode);

int optical_frontend_get_mode(struct optical_frontend *frontend,
			      struct optical_frontend_mode *mode)
{
	if (!frontend || !mode)
		return -EINVAL;

	mutex_lock(&frontend->op_lock);
	if (!frontend->mode_valid) {
		mutex_unlock(&frontend->op_lock);
		return -ENODATA;
	}
	*mode = frontend->mode;
	mutex_unlock(&frontend->op_lock);

	return 0;
}
EXPORT_SYMBOL_GPL(optical_frontend_get_mode);

int optical_frontend_tx_rearm(struct optical_frontend *frontend)
{
	int ret;

	if (!frontend)
		return -EINVAL;
	if (!frontend->ops->tx_rearm)
		return -EOPNOTSUPP;

	mutex_lock(&frontend->op_lock);
	ret = frontend->ops->tx_rearm(frontend);
	frontend->telemetry_cache_valid = false;
	mutex_unlock(&frontend->op_lock);

	return ret;
}
EXPORT_SYMBOL_GPL(optical_frontend_tx_rearm);

int optical_frontend_get_state(struct optical_frontend *frontend,
			       struct optical_frontend_state *state)
{
	int ret;

	if (!frontend || !state)
		return -EINVAL;
	if (!frontend->ops->get_state)
		return -EOPNOTSUPP;

	memset(state, 0, sizeof(*state));
	mutex_lock(&frontend->op_lock);
	ret = frontend->ops->get_state(frontend, state);
	mutex_unlock(&frontend->op_lock);

	return ret;
}
EXPORT_SYMBOL_GPL(optical_frontend_get_state);

int optical_frontend_get_telemetry(struct optical_frontend *frontend,
				   struct optical_frontend_telemetry *telemetry)
{
	unsigned long cache_jiffies;
	int ret;

	if (!frontend || !telemetry)
		return -EINVAL;
	if (!frontend->ops->get_telemetry)
		return -EOPNOTSUPP;

	mutex_lock(&frontend->op_lock);
	if (frontend->telemetry_cache_valid && frontend->desc->telemetry_cache_ms &&
	    time_before(jiffies, frontend->telemetry_expires)) {
		*telemetry = frontend->telemetry_cache;
		mutex_unlock(&frontend->op_lock);
		return telemetry->valid ? 0 : -ENODATA;
	}

	memset(telemetry, 0, sizeof(*telemetry));
	ret = frontend->ops->get_telemetry(frontend, telemetry);
	if (!ret && telemetry->valid && frontend->desc->telemetry_cache_ms) {
		frontend->telemetry_cache = *telemetry;
		cache_jiffies = msecs_to_jiffies(frontend->desc->telemetry_cache_ms);
		frontend->telemetry_expires = jiffies + max_t(unsigned long, 1,
							       cache_jiffies);
		frontend->telemetry_cache_valid = true;
	}
	mutex_unlock(&frontend->op_lock);

	if (ret)
		return ret;
	return telemetry->valid ? 0 : -ENODATA;
}
EXPORT_SYMBOL_GPL(optical_frontend_get_telemetry);

void optical_frontend_invalidate_telemetry(struct optical_frontend *frontend)
{
	if (!frontend)
		return;

	mutex_lock(&frontend->op_lock);
	frontend->telemetry_cache_valid = false;
	mutex_unlock(&frontend->op_lock);
}
EXPORT_SYMBOL_GPL(optical_frontend_invalidate_telemetry);

static int __init optical_frontend_init(void)
{
	return class_register(&optical_frontend_class);
}
subsys_initcall(optical_frontend_init);

static void __exit optical_frontend_exit(void)
{
	class_unregister(&optical_frontend_class);
	ida_destroy(&optical_frontend_ida);
}
module_exit(optical_frontend_exit);

MODULE_DESCRIPTION("Generic optical frontend provider/consumer core");
MODULE_LICENSE("GPL");
