// SPDX-License-Identifier: GPL-2.0-only
/*
 * Shared helpers for Airoha and EcoNet frame-engine Ethernet drivers.
 */

#include <linux/bits.h>
#include <linux/dma-mapping.h>
#include <linux/etherdevice.h>
#include <linux/ethtool.h>
#include <linux/export.h>
#include <linux/netdevice.h>
#include <linux/skbuff.h>
#include <linux/unaligned.h>
#include <linux/of.h>
#include <linux/of_net.h>
#include <linux/phylink.h>
#include <net/dsa.h>

#include "airoha_common.h"

#define AIROHA_MTK_HDR_LEN			4
#define AIROHA_MTK_STAG_PORT_MASK		GENMASK(5, 0)
#define AIROHA_MTK_HDR_XMIT_TAGGED_TPID_8100	1
#define AIROHA_MTK_HDR_XMIT_TAGGED_TPID_88A8	2

static void airoha_qdma_skb_meta_init(struct airoha_qdma_skb_meta *meta)
{
	memset(meta, 0, sizeof(*meta));
	meta->channel = AIROHA_MTK_INVALID_CHANNEL;
}

void airoha_qdma_skb_get_mtk_meta(struct sk_buff *skb,
				  struct net_device *netdev,
				  enum airoha_mtk_tag_mode mode,
				  struct airoha_qdma_skb_meta *meta)
{
#if IS_ENABLED(CONFIG_NET_DSA)
	struct ethhdr *ehdr;
	u8 xmit_tpid;
	u16 tag;

	airoha_qdma_skb_meta_init(meta);

	if (!netdev_uses_dsa(netdev) ||
	    netdev->dsa_ptr->tag_ops->proto != DSA_TAG_PROTO_MTK ||
	    skb_headlen(skb) < ETH_HLEN + AIROHA_MTK_HDR_LEN)
		return;

	tag = get_unaligned_be16(skb->data + 2 * ETH_ALEN);
	meta->has_mtk_tag = true;
	meta->mtk_tag = tag;
	meta->port_mask = tag & AIROHA_MTK_STAG_PORT_MASK;
	if (meta->port_mask)
		meta->channel = __ffs(meta->port_mask);

	if (mode == AIROHA_MTK_TAG_IN_SKB)
		return;

	if (skb_cow_head(skb, 0)) {
		airoha_qdma_skb_meta_init(meta);
		return;
	}

	ehdr = (struct ethhdr *)skb->data;
	xmit_tpid = tag >> 8;

	switch (xmit_tpid) {
	case AIROHA_MTK_HDR_XMIT_TAGGED_TPID_8100:
		ehdr->h_proto = cpu_to_be16(ETH_P_8021Q);
		tag &= ~(AIROHA_MTK_HDR_XMIT_TAGGED_TPID_8100 << 8);
		break;
	case AIROHA_MTK_HDR_XMIT_TAGGED_TPID_88A8:
		ehdr->h_proto = cpu_to_be16(ETH_P_8021AD);
		tag &= ~(AIROHA_MTK_HDR_XMIT_TAGGED_TPID_88A8 << 8);
		break;
	default:
		/* Newer Airoha QDMA carries an untagged MTK DSA header in
		 * descriptor metadata so the PPE sees the original Ethernet
		 * header. EcoNet selects AIROHA_MTK_TAG_IN_SKB instead.
		 */
		memmove(skb->data + AIROHA_MTK_HDR_LEN, skb->data,
			2 * ETH_ALEN);
		__skb_pull(skb, AIROHA_MTK_HDR_LEN);
		break;
	}

	meta->mtk_tag = tag;
#else
	airoha_qdma_skb_meta_init(meta);
#endif
}
EXPORT_SYMBOL_GPL(airoha_qdma_skb_get_mtk_meta);

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

static void airoha_gdm_mac_config(struct phylink_config *config,
				  unsigned int mode,
				  const struct phylink_link_state *state)
{
	struct airoha_gdm_common *gdm;

	gdm = container_of(config, struct airoha_gdm_common, phylink_config);
	if (gdm->mac_ops && gdm->mac_ops->mac_config)
		gdm->mac_ops->mac_config(gdm->priv, mode, state);
}

static void airoha_gdm_mac_link_up(struct phylink_config *config,
				   struct phy_device *phy,
				   unsigned int mode,
				   phy_interface_t interface,
				   int speed, int duplex,
				   bool tx_pause, bool rx_pause)
{
	struct airoha_gdm_common *gdm;

	gdm = container_of(config, struct airoha_gdm_common, phylink_config);
	if (gdm->mac_ops && gdm->mac_ops->mac_link_up)
		gdm->mac_ops->mac_link_up(gdm->priv, phy, mode, interface,
					  speed, duplex, tx_pause, rx_pause);
}

static void airoha_gdm_mac_link_down(struct phylink_config *config,
				     unsigned int mode,
				     phy_interface_t interface)
{
	struct airoha_gdm_common *gdm;

	gdm = container_of(config, struct airoha_gdm_common, phylink_config);
	if (gdm->mac_ops && gdm->mac_ops->mac_link_down)
		gdm->mac_ops->mac_link_down(gdm->priv, mode, interface);
}

static const struct phylink_mac_ops airoha_gdm_phylink_ops = {
	.mac_config = airoha_gdm_mac_config,
	.mac_link_up = airoha_gdm_mac_link_up,
	.mac_link_down = airoha_gdm_mac_link_down,
};

void airoha_gdm_common_init(struct airoha_gdm_common *gdm,
			    struct net_device *netdev,
			    enum airoha_eth_family family, u8 id,
			    u8 pse_port, void *priv,
			    const struct airoha_gdm_mac_ops *mac_ops)
{
	memset(gdm, 0, sizeof(*gdm));
	gdm->magic = AIROHA_GDM_COMMON_MAGIC;
	gdm->family = family;
	gdm->id = id;
	gdm->pse_port = pse_port;
	gdm->netdev = netdev;
	gdm->priv = priv;
	gdm->mac_ops = mac_ops;
	gdm->phylink_config.dev = &netdev->dev;
	gdm->phylink_config.type = PHYLINK_NETDEV;
}
EXPORT_SYMBOL_GPL(airoha_gdm_common_init);

int airoha_gdm_phylink_create(struct airoha_gdm_common *gdm,
			      struct device_node *np,
			      phy_interface_t phy_mode)
{
	struct phylink *phylink;

	phylink = phylink_create(&gdm->phylink_config,
				 of_fwnode_handle(np), phy_mode,
				 &airoha_gdm_phylink_ops);
	if (IS_ERR(phylink))
		return PTR_ERR(phylink);

	gdm->phylink = phylink;
	return 0;
}
EXPORT_SYMBOL_GPL(airoha_gdm_phylink_create);

int airoha_gdm_phylink_connect(struct airoha_gdm_common *gdm,
			       bool allow_no_phy)
{
	int err;

	if (!gdm->phylink)
		return -ENODEV;

	err = phylink_of_phy_connect(gdm->phylink,
				     gdm->netdev->dev.of_node, 0);
	if (err) {
		if (allow_no_phy && err == -ENODEV)
			return 0;
		return err;
	}

	phylink_start(gdm->phylink);
	gdm->phylink_started = true;
	return 0;
}
EXPORT_SYMBOL_GPL(airoha_gdm_phylink_connect);

void airoha_gdm_phylink_disconnect(struct airoha_gdm_common *gdm)
{
	if (!gdm->phylink_started)
		return;

	phylink_stop(gdm->phylink);
	phylink_disconnect_phy(gdm->phylink);
	gdm->phylink_started = false;
}
EXPORT_SYMBOL_GPL(airoha_gdm_phylink_disconnect);

void airoha_gdm_phylink_destroy(struct airoha_gdm_common *gdm)
{
	airoha_gdm_phylink_disconnect(gdm);
	if (!gdm->phylink)
		return;

	phylink_destroy(gdm->phylink);
	gdm->phylink = NULL;
}
EXPORT_SYMBOL_GPL(airoha_gdm_phylink_destroy);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Shared helpers for Airoha and EcoNet Ethernet drivers");
