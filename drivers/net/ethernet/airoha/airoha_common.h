/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef AIROHA_COMMON_H
#define AIROHA_COMMON_H

#include <linux/io.h>
#include <linux/netdevice.h>
#include <linux/phylink.h>
#include <linux/types.h>

struct device;
struct device_node;
struct ethtool_drvinfo;
struct airoha_ppe_dev;

enum airoha_eth_family {
	AIROHA_ETH_FAMILY_AIROHA,
	AIROHA_ETH_FAMILY_ECONET,
};

/**
 * struct airoha_gdm_mac_ops - SoC-specific MAC callbacks
 * @mac_config: optional phylink MAC configuration callback
 * @mac_link_up: optional phylink link-up callback
 * @mac_link_down: optional phylink link-down callback
 *
 * GDM1 and GDM2 are functionally equivalent on the EcoNet and Airoha frame
 * engines.  The common object owns phylink and dispatches only the hardware
 * differences to the Ethernet backend.
 */
struct airoha_gdm_mac_ops {
	void (*mac_config)(void *priv, unsigned int mode,
			   const struct phylink_link_state *state);
	void (*mac_link_up)(void *priv, struct phy_device *phy,
			    unsigned int mode, phy_interface_t interface,
			    int speed, int duplex, bool tx_pause,
			    bool rx_pause);
	void (*mac_link_down)(void *priv, unsigned int mode,
			      phy_interface_t interface);
};

#define AIROHA_GDM_COMMON_MAGIC	0x47444d43 /* "GDMC" */

/**
 * struct airoha_gdm_common - common GDM netdev/phylink state
 * @magic: identifies a common GDM private object
 * @family: frame-engine generation
 * @id: one-based GDM identifier
 * @pse_port: PPE/PSE destination port used by the backend
 * @netdev: associated Linux network device
 * @ppe: optional common PPE frontend
 * @priv: backend GDM object passed to @mac_ops
 * @mac_ops: optional SoC-specific MAC callbacks
 * @phylink: phylink instance
 * @phylink_config: phylink configuration owned by this GDM
 * @phylink_started: whether phylink_start() has been issued
 */
struct airoha_gdm_common {
	u32 magic;
	enum airoha_eth_family family;
	u8 id;
	u8 pse_port;
	struct net_device *netdev;
	struct airoha_ppe_dev *ppe;
	void *priv;
	const struct airoha_gdm_mac_ops *mac_ops;
	struct phylink *phylink;
	struct phylink_config phylink_config;
	bool phylink_started;
};

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

void airoha_gdm_common_init(struct airoha_gdm_common *gdm,
			    struct net_device *netdev,
			    enum airoha_eth_family family, u8 id,
			    u8 pse_port, void *priv,
			    const struct airoha_gdm_mac_ops *mac_ops);
int airoha_gdm_phylink_create(struct airoha_gdm_common *gdm,
			      struct device_node *np,
			      phy_interface_t phy_mode);
int airoha_gdm_phylink_connect(struct airoha_gdm_common *gdm,
			       bool allow_no_phy);
void airoha_gdm_phylink_disconnect(struct airoha_gdm_common *gdm);
void airoha_gdm_phylink_destroy(struct airoha_gdm_common *gdm);

static inline struct airoha_gdm_common *
airoha_gdm_common_from_netdev(struct net_device *netdev)
{
	struct airoha_gdm_common *gdm = netdev_priv(netdev);

	return gdm->magic == AIROHA_GDM_COMMON_MAGIC ? gdm : NULL;
}

#endif /* AIROHA_COMMON_H */
