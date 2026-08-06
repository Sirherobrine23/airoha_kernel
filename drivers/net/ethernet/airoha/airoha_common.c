// SPDX-License-Identifier: GPL-2.0-only
/*
 * Shared helpers for Airoha and EcoNet frame-engine Ethernet drivers.
 */

#include <linux/dma-mapping.h>
#include <linux/etherdevice.h>
#include <linux/ethtool.h>
#include <linux/export.h>
#include <linux/netdevice.h>
#include <linux/of.h>
#include <linux/of_net.h>

#include "airoha_common.h"

u32 airoha_rr(void __iomem *base, u32 offset)
{
	return readl(base + offset);
}
EXPORT_SYMBOL_GPL(airoha_rr);

void airoha_wr(void __iomem *base, u32 offset, u32 val)
{
	writel(val, base + offset);
}
EXPORT_SYMBOL_GPL(airoha_wr);

u32 airoha_rmw(void __iomem *base, u32 offset, u32 mask, u32 val)
{
	val |= airoha_rr(base, offset) & ~mask;
	airoha_wr(base, offset, val);

	return val;
}
EXPORT_SYMBOL_GPL(airoha_rmw);

int airoha_eth_set_dma_mask(struct device *dev)
{
	int err;

	err = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (err)
		return dev_err_probe(dev, err,
				     "failed configuring 32-bit DMA mask\n");

	return 0;
}
EXPORT_SYMBOL_GPL(airoha_eth_set_dma_mask);

int airoha_eth_get_port_id(struct device *dev, struct device_node *np,
			   u32 min, u32 max, u32 *id)
{
	int err;

	err = of_property_read_u32(np, "reg", id);
	if (err)
		return dev_err_probe(dev, err, "missing port id\n");
	if (*id < min || *id > max)
		return dev_err_probe(dev, -EINVAL,
				     "invalid port id: %u\n", *id);

	return 0;
}
EXPORT_SYMBOL_GPL(airoha_eth_get_port_id);

struct net_device *airoha_eth_alloc_napi_dev(const char *name)
{
	struct net_device *netdev;

	netdev = alloc_netdev_dummy(0);
	if (!netdev)
		return NULL;

	netdev->threaded = true;
	strscpy(netdev->name, name, sizeof(netdev->name));

	return netdev;
}
EXPORT_SYMBOL_GPL(airoha_eth_alloc_napi_dev);

int airoha_eth_init_mac_address(struct device *dev, struct device_node *np,
				struct net_device *netdev)
{
	int err;

	err = of_get_ethdev_address(np, netdev);
	if (!err)
		return 0;
	if (err == -EPROBE_DEFER)
		return err;

	eth_hw_addr_random(netdev);
	dev_info(dev, "generated random MAC address %pM\n", netdev->dev_addr);

	return 0;
}
EXPORT_SYMBOL_GPL(airoha_eth_init_mac_address);

void airoha_eth_get_drvinfo(struct net_device *netdev,
			    struct ethtool_drvinfo *info)
{
	struct device *parent = netdev->dev.parent;

	if (!parent)
		return;

	if (parent->driver)
		strscpy(info->driver, parent->driver->name,
			sizeof(info->driver));
	strscpy(info->bus_info, dev_name(parent), sizeof(info->bus_info));
}
EXPORT_SYMBOL_GPL(airoha_eth_get_drvinfo);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Shared helpers for Airoha and EcoNet Ethernet drivers");
