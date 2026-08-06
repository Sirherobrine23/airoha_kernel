/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef AIROHA_COMMON_H
#define AIROHA_COMMON_H

#include <linux/io.h>
#include <linux/types.h>

struct device;
struct device_node;
struct ethtool_drvinfo;
struct net_device;

u32 airoha_rr(void __iomem *base, u32 offset);
void airoha_wr(void __iomem *base, u32 offset, u32 val);
u32 airoha_rmw(void __iomem *base, u32 offset, u32 mask, u32 val);

int airoha_eth_set_dma_mask(struct device *dev);
int airoha_eth_get_port_id(struct device *dev, struct device_node *np,
			   u32 min, u32 max, u32 *id);
struct net_device *airoha_eth_alloc_napi_dev(const char *name);
int airoha_eth_init_mac_address(struct device *dev, struct device_node *np,
				struct net_device *netdev);
void airoha_eth_get_drvinfo(struct net_device *netdev,
			    struct ethtool_drvinfo *info);

#endif /* AIROHA_COMMON_H */
