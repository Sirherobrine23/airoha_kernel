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
#include <linux/gpio/consumer.h>
#include <linux/idr.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/minmax.h>
#include <linux/mutex.h>
#include <linux/optical_frontend.h>
#include <linux/property.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/sysfs.h>

struct optical_frontend {
	struct device dev;
	struct device *provider;
	const struct optical_frontend_desc *desc;
	const struct optical_frontend_ops *ops;
	void *drvdata;
	struct gpio_desc *tx_disable_gpio;

	struct mutex op_lock; /* serializes provider callbacks and shared state */
	struct optical_frontend_mode mode;
	bool mode_valid;

	struct optical_frontend_telemetry telemetry_cache;
	unsigned long telemetry_expires;
	bool telemetry_cache_valid;

	int id;
};

static DEFINE_IDA(optical_frontend_ida);

static const char *optical_frontend_protocol_name(enum optical_frontend_protocol protocol)
{
	switch (protocol) {
	case OPTICAL_FRONTEND_PROTO_ETHERNET:
		return "ethernet";
	case OPTICAL_FRONTEND_PROTO_EPON:
		return "epon";
	case OPTICAL_FRONTEND_PROTO_GPON:
		return "gpon";
	case OPTICAL_FRONTEND_PROTO_XGPON:
		return "xgpon";
	case OPTICAL_FRONTEND_PROTO_XGSPON:
		return "xgspon";
	case OPTICAL_FRONTEND_PROTO_NGPON2:
		return "ngpon2";
	case OPTICAL_FRONTEND_PROTO_UNSPEC:
	default:
		return "unspecified";
	}
}

static ssize_t vendor_show(struct device *dev, struct device_attribute *attr,
			   char *buf)
{
	struct optical_frontend *frontend =
		container_of(dev, struct optical_frontend, dev);

	return sysfs_emit(buf, "%s\n",
			  frontend->desc->vendor_name ?: "unknown");
}
static DEVICE_ATTR_RO(vendor);

static ssize_t model_show(struct device *dev, struct device_attribute *attr,
			  char *buf)
{
	struct optical_frontend *frontend =
		container_of(dev, struct optical_frontend, dev);

	return sysfs_emit(buf, "%s\n",
			  frontend->desc->part_number ?: frontend->desc->name);
}
static DEVICE_ATTR_RO(model);

static ssize_t type_show(struct device *dev, struct device_attribute *attr,
			 char *buf)
{
	struct optical_frontend *frontend =
		container_of(dev, struct optical_frontend, dev);

	return sysfs_emit(buf, "%s\n", frontend->desc->type ?: "unknown");
}
static DEVICE_ATTR_RO(type);

static ssize_t capabilities_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct optical_frontend *frontend =
		container_of(dev, struct optical_frontend, dev);
	u32 capabilities = frontend->desc->capabilities;

	if (frontend->tx_disable_gpio)
		capabilities |= OPTICAL_FRONTEND_CAP_TX_DISABLE;

	return sysfs_emit(buf, "0x%08x\n", capabilities);
}
static DEVICE_ATTR_RO(capabilities);

static ssize_t supported_protocols_show(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	struct optical_frontend *frontend =
		container_of(dev, struct optical_frontend, dev);
	ssize_t len = 0;
	unsigned int protocol;

	for (protocol = OPTICAL_FRONTEND_PROTO_ETHERNET;
	     protocol <= OPTICAL_FRONTEND_PROTO_NGPON2; protocol++) {
		if (!(frontend->desc->protocols & BIT(protocol)))
			continue;
		len += sysfs_emit_at(buf, len, "%s%s", len ? " " : "",
				     optical_frontend_protocol_name(protocol));
	}

	return sysfs_emit_at(buf, len, "\n");
}
static DEVICE_ATTR_RO(supported_protocols);

static ssize_t protocol_show(struct device *dev, struct device_attribute *attr,
			     char *buf)
{
	struct optical_frontend *frontend =
		container_of(dev, struct optical_frontend, dev);
	struct optical_frontend_mode mode;
	int ret;

	ret = optical_frontend_get_mode(frontend, &mode);
	if (ret == -ENODATA)
		return sysfs_emit(buf, "unspecified\n");
	if (ret)
		return ret;

	return sysfs_emit(buf, "%s\n", optical_frontend_protocol_name(mode.protocol));
}
static DEVICE_ATTR_RO(protocol);

static int optical_frontend_sysfs_state(struct optical_frontend *frontend,
					struct optical_frontend_state *state)
{
	int ret = optical_frontend_get_state(frontend, state);

	return ret == -EOPNOTSUPP ? -ENODATA : ret;
}

#define OPTICAL_FRONTEND_STATE_ATTR(_name, _flag, _member) \
static ssize_t _name##_show(struct device *dev, \
			    struct device_attribute *attr, char *buf) \
{ \
	struct optical_frontend *frontend = \
		container_of(dev, struct optical_frontend, dev); \
	struct optical_frontend_state state; \
	int ret; \
\
	ret = optical_frontend_sysfs_state(frontend, &state); \
	if (ret) \
		return ret; \
	if (!(state.valid & (_flag))) \
		return -ENODATA; \
	return sysfs_emit(buf, "%u\n", state._member); \
} \
static DEVICE_ATTR_RO(_name)

OPTICAL_FRONTEND_STATE_ATTR(present, OPTICAL_FRONTEND_STATE_F_PRESENT, present);
OPTICAL_FRONTEND_STATE_ATTR(ready, OPTICAL_FRONTEND_STATE_F_READY, ready);
OPTICAL_FRONTEND_STATE_ATTR(rx_los, OPTICAL_FRONTEND_STATE_F_RX_LOS, rx_los);
OPTICAL_FRONTEND_STATE_ATTR(tx_fault, OPTICAL_FRONTEND_STATE_F_TX_FAULT, tx_fault);
OPTICAL_FRONTEND_STATE_ATTR(tx_enabled, OPTICAL_FRONTEND_STATE_F_TX_ENABLED,
			    tx_enabled);

static ssize_t alarms_show(struct device *dev, struct device_attribute *attr,
			   char *buf)
{
	struct optical_frontend *frontend =
		container_of(dev, struct optical_frontend, dev);
	struct optical_frontend_telemetry telemetry;
	int ret;

	ret = optical_frontend_get_telemetry(frontend, &telemetry);
	if (ret)
		return ret;
	if (!(telemetry.valid & OPTICAL_FRONTEND_TELEMETRY_F_ALARMS))
		return -ENODATA;

	return sysfs_emit(buf, "0x%08x\n", telemetry.alarms);
}
static DEVICE_ATTR_RO(alarms);

static struct attribute *optical_frontend_attrs[] = {
	&dev_attr_vendor.attr,
	&dev_attr_model.attr,
	&dev_attr_type.attr,
	&dev_attr_capabilities.attr,
	&dev_attr_supported_protocols.attr,
	&dev_attr_protocol.attr,
	&dev_attr_present.attr,
	&dev_attr_ready.attr,
	&dev_attr_rx_los.attr,
	&dev_attr_tx_fault.attr,
	&dev_attr_tx_enabled.attr,
	&dev_attr_alarms.attr,
	NULL,
};

static umode_t optical_frontend_attr_is_visible(struct kobject *kobj,
						struct attribute *attr, int n)
{
	struct device *dev = kobj_to_dev(kobj);
	struct optical_frontend *frontend =
		container_of(dev, struct optical_frontend, dev);

	if (attr == &dev_attr_present.attr || attr == &dev_attr_ready.attr)
		return frontend->ops->get_state ? 0444 : 0;
	if (attr == &dev_attr_rx_los.attr)
		return frontend->desc->capabilities & OPTICAL_FRONTEND_CAP_RX_LOS ?
			0444 : 0;
	if (attr == &dev_attr_tx_fault.attr)
		return frontend->desc->capabilities & OPTICAL_FRONTEND_CAP_TX_FAULT ?
			0444 : 0;
	if (attr == &dev_attr_tx_enabled.attr)
		return frontend->tx_disable_gpio || frontend->ops->tx_enable ||
		       (frontend->desc->capabilities & OPTICAL_FRONTEND_CAP_TX_DISABLE) ?
			0444 : 0;
	if (attr == &dev_attr_alarms.attr)
		return frontend->desc->capabilities & OPTICAL_FRONTEND_CAP_ALARMS ?
			0444 : 0;

	return 0444;
}

static const struct attribute_group optical_frontend_group = {
	.attrs = optical_frontend_attrs,
	.is_visible = optical_frontend_attr_is_visible,
};

static const struct attribute_group *optical_frontend_groups[] = {
	&optical_frontend_group,
	NULL,
};

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
	.dev_groups = optical_frontend_groups,
};

static void optical_frontend_unregister(void *data)
{
	struct optical_frontend *frontend = data;

	if (frontend->tx_disable_gpio)
		gpiod_set_value_cansleep(frontend->tx_disable_gpio, 1);
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
	struct gpio_desc *tx_disable_gpio;
	int ret;

	if (!dev || !desc || !desc->name || !ops)
		return ERR_PTR(-EINVAL);

	tx_disable_gpio = devm_gpiod_get_optional(dev, "tx-disable",
						  GPIOD_OUT_HIGH);
	if (IS_ERR(tx_disable_gpio)) {
		ret = dev_err_probe(dev, PTR_ERR(tx_disable_gpio),
				    "failed to get TX disable GPIO\n");
		return ERR_PTR(ret);
	}

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
	frontend->tx_disable_gpio = tx_disable_gpio;
	mutex_init(&frontend->op_lock);

	device_initialize(&frontend->dev);
	frontend->dev.class = &optical_frontend_class;
	frontend->dev.parent = dev;
	frontend->dev.groups = desc->groups;
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

struct optical_frontend *optical_frontend_from_dev(struct device *dev)
{
	if (!dev || dev->class != &optical_frontend_class)
		return NULL;

	return container_of(dev, struct optical_frontend, dev);
}
EXPORT_SYMBOL_GPL(optical_frontend_from_dev);

struct device *optical_frontend_get_device(struct optical_frontend *frontend)
{
	return frontend ? &frontend->dev : NULL;
}
EXPORT_SYMBOL_GPL(optical_frontend_get_device);

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

int optical_frontend_tx_enable(struct optical_frontend *frontend, bool enable)
{
	int ret = 0;

	if (!frontend)
		return -EINVAL;
	if (!frontend->tx_disable_gpio && !frontend->ops->tx_enable)
		return -EOPNOTSUPP;

	mutex_lock(&frontend->op_lock);

	/* Block the optical output before asking the provider to shut down. */
	if (!enable && frontend->tx_disable_gpio)
		gpiod_set_value_cansleep(frontend->tx_disable_gpio, 1);

	if (frontend->ops->tx_enable) {
		ret = frontend->ops->tx_enable(frontend, enable);
		if (ret)
			goto out;
	}

	/* Release the external interlock only after the provider is ready. */
	if (enable && frontend->tx_disable_gpio)
		gpiod_set_value_cansleep(frontend->tx_disable_gpio, 0);

out:
	frontend->telemetry_cache_valid = false;
	mutex_unlock(&frontend->op_lock);

	return ret;
}
EXPORT_SYMBOL_GPL(optical_frontend_tx_enable);

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
	int gpio_value;
	int ret = 0;

	if (!frontend || !state)
		return -EINVAL;

	memset(state, 0, sizeof(*state));
	mutex_lock(&frontend->op_lock);
	if (frontend->ops->get_state) {
		ret = frontend->ops->get_state(frontend, state);
		if (ret)
			goto out;
	}

	/* TX_DISABLE is a core-owned interlock, so report it even when the
	 * provider has no matching status register. GPIO values are logical:
	 * one means disabled regardless of the electrical active level.
	 */
	if (frontend->tx_disable_gpio) {
		gpio_value = gpiod_get_value_cansleep(frontend->tx_disable_gpio);
		if (gpio_value < 0) {
			ret = gpio_value;
			goto out;
		}
		state->tx_enabled = !gpio_value;
		state->valid |= OPTICAL_FRONTEND_STATE_F_TX_ENABLED;
	}

	if (!state->valid)
		ret = -ENODATA;

out:
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
