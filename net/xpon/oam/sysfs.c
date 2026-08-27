// SPDX-License-Identifier: GPL-2.0-only

#include <linux/device.h>
#include <linux/sysfs.h>
#include <net/xpon.h>

#include "../internal.h"
#include "internal.h"

static ssize_t enabled_show(struct device *dev, struct device_attribute *attr,
			    char *buf)
{
	struct xpon_device *xpon = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%u\n", !!xpon->oam);
}
static DEVICE_ATTR_RO(enabled);

static struct attribute *xpon_oam_attrs[] = {
	&dev_attr_enabled.attr,
	NULL,
};

static const struct attribute_group xpon_oam_group = {
	.name = "oam",
	.attrs = xpon_oam_attrs,
};

int xpon_oam_sysfs_register(struct xpon_oam *oam)
{
	return sysfs_create_group(&oam->xpon->class_dev->kobj, &xpon_oam_group);
}

void xpon_oam_sysfs_unregister(struct xpon_oam *oam)
{
	if (oam->xpon && oam->xpon->class_dev)
		sysfs_remove_group(&oam->xpon->class_dev->kobj, &xpon_oam_group);
}
