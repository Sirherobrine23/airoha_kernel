// SPDX-License-Identifier: GPL-2.0-only

#include <linux/device.h>
#include <linux/netdevice.h>
#include <linux/string.h>
#include <net/xpon.h>

#include "internal.h"

struct class *xpon_class;

static const char *xpon_mode_name(enum xpon_mode mode)
{
	switch (mode) {
	case XPON_MODE_GPON:
		return "gpon";
	case XPON_MODE_EPON:
		return "epon";
	case XPON_MODE_XGSPON:
		return "xgspon";
	default:
		return "unknown";
	}
}

static const char *xpon_registration_name(enum xpon_registration_state state)
{
	switch (state) {
	case XPON_REGISTRATION_DOWN:
		return "down";
	case XPON_REGISTRATION_DISCOVERY:
		return "discovery";
	case XPON_REGISTRATION_REGISTERING:
		return "registering";
	case XPON_REGISTRATION_OPERATIONAL:
		return "operational";
	default:
		return "unknown";
	}
}

static ssize_t mode_available_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct xpon_device *xpon = dev_get_drvdata(dev);
	ssize_t len = 0;
	int mode;

	for (mode = 0; mode < __XPON_MODE_MAX; mode++) {
		if (!(xpon->modes & XPON_MODE_CAP(mode)))
			continue;
		len += sysfs_emit_at(buf, len, "%s%s", len ? " " : "",
				     xpon_mode_name(mode));
	}

	return sysfs_emit_at(buf, len, "\n") + len;
}
static DEVICE_ATTR_RO(mode_available);

static ssize_t mode_current_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct xpon_device *xpon = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%s\n", xpon_mode_name(xpon->state.mode));
}

static ssize_t mode_current_store(struct device *dev,
				  struct device_attribute *attr,
				  const char *buf, size_t count)
{
	struct xpon_device *xpon = dev_get_drvdata(dev);
	enum xpon_mode mode;
	int ret;

	if (sysfs_streq(buf, "gpon"))
		mode = XPON_MODE_GPON;
	else if (sysfs_streq(buf, "epon"))
		mode = XPON_MODE_EPON;
	else if (sysfs_streq(buf, "xgspon"))
		mode = XPON_MODE_XGSPON;
	else
		return -EINVAL;

	ret = xpon_device_set_mode(xpon, mode);
	return ret ? ret : count;
}
static DEVICE_ATTR_RW(mode_current);

static ssize_t state_show(struct device *dev, struct device_attribute *attr,
			  char *buf)
{
	struct xpon_device *xpon = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%s\n",
			  xpon_registration_name(xpon->state.registration));
}
static DEVICE_ATTR_RO(state);

static ssize_t carrier_show(struct device *dev, struct device_attribute *attr,
			    char *buf)
{
	struct xpon_device *xpon = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%u\n", xpon->state.carrier);
}
static DEVICE_ATTR_RO(carrier);

static ssize_t signal_detect_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct xpon_device *xpon = dev_get_drvdata(dev);

	if (!(xpon->state.valid & XPON_STATE_F_SIGNAL))
		return sysfs_emit(buf, "unknown\n");
	return sysfs_emit(buf, "%u\n", xpon->state.signal_detect);
}
static DEVICE_ATTR_RO(signal_detect);

static ssize_t los_show(struct device *dev, struct device_attribute *attr,
			char *buf)
{
	struct xpon_device *xpon = dev_get_drvdata(dev);

	if (!(xpon->state.valid & XPON_STATE_F_LOS))
		return sysfs_emit(buf, "unknown\n");
	return sysfs_emit(buf, "%u\n", xpon->state.los);
}
static DEVICE_ATTR_RO(los);

static struct attribute *xpon_attrs[] = {
	&dev_attr_mode_available.attr,
	&dev_attr_mode_current.attr,
	&dev_attr_state.attr,
	&dev_attr_carrier.attr,
	&dev_attr_signal_detect.attr,
	&dev_attr_los.attr,
	NULL,
};
ATTRIBUTE_GROUPS(xpon);

static int xpon_netdev_event(struct notifier_block *nb, unsigned long event,
			     void *ptr)
{
	struct net_device *netdev = netdev_notifier_info_to_dev(ptr);
	struct xpon_device *xpon;

	if (event != NETDEV_CHANGENAME)
		return NOTIFY_DONE;

	xpon = xpon_device_find_by_ifindex(netdev->ifindex);
	if (!xpon)
		return NOTIFY_DONE;

	device_rename(xpon->class_dev, netdev->name);
	xpon_device_put(xpon);
	return NOTIFY_DONE;
}

static struct notifier_block xpon_netdev_nb = {
	.notifier_call = xpon_netdev_event,
};

int xpon_sysfs_init(void)
{
	int ret;

	xpon_class = class_create("xpon");
	if (IS_ERR(xpon_class))
		return PTR_ERR(xpon_class);

	ret = register_netdevice_notifier(&xpon_netdev_nb);
	if (ret) {
		class_destroy(xpon_class);
		xpon_class = NULL;
	}
	return ret;
}

void xpon_sysfs_exit(void)
{
	unregister_netdevice_notifier(&xpon_netdev_nb);
	class_destroy(xpon_class);
	xpon_class = NULL;
}

int xpon_sysfs_register(struct xpon_device *xpon)
{
	int ret;

	xpon->class_dev = device_create_with_groups(xpon_class, xpon->parent,
					      MKDEV(0, 0), xpon,
					      xpon_groups, "%s",
					      xpon->netdev->name);
	if (IS_ERR(xpon->class_dev)) {
		ret = PTR_ERR(xpon->class_dev);
		xpon->class_dev = NULL;
		return ret;
	}

	if (xpon->optical) {
		ret = sysfs_create_link(&xpon->class_dev->kobj,
					&xpon->optical->kobj, "optical");
		if (ret)
			dev_warn(xpon->parent,
				 "failed to create optical xPON link: %d\n", ret);
	}

	return 0;
}

void xpon_sysfs_unregister(struct xpon_device *xpon)
{
	if (!xpon->class_dev)
		return;
	if (xpon->optical)
		sysfs_remove_link(&xpon->class_dev->kobj, "optical");
	device_unregister(xpon->class_dev);
	xpon->class_dev = NULL;
}

void xpon_sysfs_notify(struct xpon_device *xpon, unsigned long changed)
{
	if (!xpon->class_dev)
		return;
	if (changed & XPON_EVENT_MODE)
		sysfs_notify(&xpon->class_dev->kobj, NULL, "mode_current");
	if (changed & XPON_EVENT_REGISTRATION)
		sysfs_notify(&xpon->class_dev->kobj, NULL, "state");
	if (changed & XPON_EVENT_CARRIER)
		sysfs_notify(&xpon->class_dev->kobj, NULL, "carrier");
	if (changed & XPON_EVENT_SIGNAL)
		sysfs_notify(&xpon->class_dev->kobj, NULL, "signal_detect");
	if (changed & XPON_EVENT_LOS)
		sysfs_notify(&xpon->class_dev->kobj, NULL, "los");
}
