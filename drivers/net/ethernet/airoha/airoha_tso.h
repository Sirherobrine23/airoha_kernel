/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _AIROHA_TSO_H
#define _AIROHA_TSO_H

#include <linux/errno.h>

#include "airoha_eth.h"

/* Build airoha_tso.o only when CONFIG_NET_AIROHA_TSO is enabled. */
#ifdef CONFIG_NET_AIROHA_TSO
enum airoha_tso_version airoha_tso_version(struct airoha_eth *eth);
int airoha_tso_get_sset_count(struct airoha_eth *eth);
void airoha_tso_get_strings(struct airoha_eth *eth, u8 *data);
void airoha_tso_get_stats(struct airoha_eth *eth, u64 *data);
netdev_features_t airoha_tso_features_check(struct sk_buff *skb,
					    struct net_device *dev,
					    netdev_features_t features);
int airoha_tso_prepare(struct sk_buff *skb);
void airoha_tx_complete(struct sk_buff *skb);
int airoha_tso_init(struct airoha_eth *eth);
void airoha_tso_cleanup(struct airoha_eth *eth);
void airoha_tso_flush(struct airoha_eth *eth, struct net_device *stopping);
netdev_tx_t airoha_tso_xmit(struct sk_buff *skb, struct net_device *dev,
			    unsigned int ring, u32 msg0, u32 msg1);
#else
static inline int airoha_tso_get_sset_count(struct airoha_eth *eth)
{
	return 0;
}

static inline void airoha_tso_get_strings(struct airoha_eth *eth, u8 *data)
{
}

static inline void airoha_tso_get_stats(struct airoha_eth *eth, u64 *data)
{
}

static inline enum airoha_tso_version
airoha_tso_version(struct airoha_eth *eth)
{
	return AIROHA_TSO_NONE;
}

static inline netdev_features_t
airoha_tso_features_check(struct sk_buff *skb, struct net_device *dev,
			  netdev_features_t features)
{
	return features;
}

static inline int airoha_tso_prepare(struct sk_buff *skb)
{
	return skb_is_gso(skb) ? -EOPNOTSUPP : 0;
}

/* QDMA still needs BQL completion accounting with TSO disabled. */
static inline void airoha_tx_complete(struct sk_buff *skb)
{
	struct airoha_gdm_dev *dev = netdev_priv(skb->dev);

	spin_lock_bh(&dev->tx_complete_lock);
	netdev_tx_completed_queue(skb_get_tx_queue(skb->dev, skb), 1, skb->len);
	spin_unlock_bh(&dev->tx_complete_lock);
}

static inline int airoha_tso_init(struct airoha_eth *eth)
{
	return 0;
}

static inline void airoha_tso_cleanup(struct airoha_eth *eth)
{
}

static inline void airoha_tso_flush(struct airoha_eth *eth,
				    struct net_device *stopping)
{
}

static inline netdev_tx_t
airoha_tso_xmit(struct sk_buff *skb, struct net_device *dev,
		unsigned int ring, u32 msg0, u32 msg1)
{
	/* Unreachable with AIROHA_TSO_NONE; leave skb ownership to the caller. */
	return NETDEV_TX_BUSY;
}
#endif

#endif /* _AIROHA_TSO_H */
