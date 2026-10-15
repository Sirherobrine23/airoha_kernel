// SPDX-License-Identifier: GPL-2.0-only
/* Firmware inventory of Ethernet user network interfaces. */

#include <linux/err.h>
#include <linux/netdevice.h>
#include <linux/of_net.h>
#include <linux/property.h>
#include <linux/rtnetlink.h>
#include <linux/slab.h>

#include "internal.h"

int xpon_unis_init(struct xpon_device *xpon)
{
	struct device *dev = xpon->parent;
	const char *type;
	int count, i, ret;

	xpon->onu_type = XPON_ONU_HGU;
	if (device_property_present(dev, "omci-type")) {
		ret = device_property_read_string(dev, "omci-type", &type);
		if (ret)
			return ret;
		if (!strcmp(type, "sfu"))
			xpon->onu_type = XPON_ONU_SFU;
		else if (strcmp(type, "hgu"))
			return -EINVAL;
	}
	if (!dev->of_node)
		return xpon->onu_type == XPON_ONU_SFU ? -EINVAL : 0;

	count = of_count_phandle_with_args(dev->of_node, "omci-ethernets", NULL);
	if (count == -ENOENT)
		return xpon->onu_type == XPON_ONU_SFU ? -EINVAL : 0;
	if (count <= 0 || count > 16)
		return -EINVAL;
	xpon->unis = kcalloc(count, sizeof(*xpon->unis), GFP_KERNEL);
	if (!xpon->unis)
		return -ENOMEM;
	xpon->uni_count = count;
	for (i = 0; i < count; i++) {
		struct device_node *np;
		int j;

		np = of_parse_phandle(dev->of_node, "omci-ethernets", i);
		if (!np || !of_device_is_available(np)) {
			of_node_put(np);
			goto invalid;
		}
		/* CPU ports and switch interconnects are not subscriber UNIs. */
		if (of_property_present(np, "ethernet") ||
		    of_property_present(np, "link")) {
			of_node_put(np);
			goto invalid;
		}
		for (j = 0; j < i; j++) {
			if (xpon->unis[j].node == np) {
				of_node_put(np);
				goto invalid;
			}
		}
		xpon->unis[i].node = np;
	}
	return 0;
invalid:
	xpon_unis_cleanup(xpon);
	return -EINVAL;
}

void xpon_unis_cleanup(struct xpon_device *xpon)
{
	int i;

	for (i = 0; i < xpon->uni_count; i++)
		of_node_put(xpon->unis[i].node);
	kfree(xpon->unis);
	xpon->unis = NULL;
	xpon->uni_count = 0;
}

unsigned int xpon_device_uni_count(struct xpon_device *xpon)
{
	return xpon->uni_count;
}
EXPORT_SYMBOL_GPL(xpon_device_uni_count);

enum xpon_onu_type xpon_device_onu_type(struct xpon_device *xpon)
{
	return xpon->onu_type;
}
EXPORT_SYMBOL_GPL(xpon_device_onu_type);

struct net_device *xpon_uni_get_netdev(struct xpon_device *xpon, u32 id)
{
	if (!id || id > xpon->uni_count)
		return NULL;
	/* Resolve on each operation so driver reprobe and renaming are safe. */
	return of_find_net_device_by_node(xpon->unis[id - 1].node);
}

int xpon_uni_set_admin(struct xpon_device *xpon, u32 id, bool enabled)
{
	struct net_device *dev;
	int ret;

	rtnl_lock();
	dev = xpon_uni_get_netdev(xpon, id);
	if (!dev) {
		ret = -ENODEV;
		goto out;
	}
	ret = dev_change_flags(dev, enabled ? dev->flags | IFF_UP :
			      dev->flags & ~IFF_UP, NULL);
	put_device(&dev->dev);
out:
	rtnl_unlock();
	return ret;
}
EXPORT_SYMBOL_GPL(xpon_uni_set_admin);
