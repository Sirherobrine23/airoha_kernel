/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _AIROHA_TSO_H
#define _AIROHA_TSO_H

#include <linux/errno.h>

#include "airoha_eth.h"

#ifdef CONFIG_NET_AIROHA_TSO
int airoha_tso_get_sset_count(struct airoha_eth *eth);
void airoha_tso_get_strings(struct airoha_eth *eth, u8 *data);
void airoha_tso_get_stats(struct airoha_eth *eth, u64 *data);
netdev_features_t airoha_tso_features_check(struct sk_buff *skb,
					    struct net_device *dev,
					    netdev_features_t features);
int airoha_tso_prepare(struct sk_buff *skb);
int airoha_tso_init(struct airoha_eth *eth);
void airoha_tso_cleanup(struct airoha_eth *eth);
void airoha_tso_flush(struct airoha_eth *eth, struct net_device *stopping);
netdev_tx_t airoha_tso_xmit(struct sk_buff *skb, struct net_device *dev,
			    unsigned int ring, u32 msg0, u32 msg1);

static inline bool airoha_tso_is_v2(struct airoha_eth *eth)
{
	return !!eth->tso;
}
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
	return NETDEV_TX_BUSY;
}

static inline bool airoha_tso_is_v2(struct airoha_eth *eth)
{
	return false;
}
#endif

static inline void airoha_tx_complete(struct sk_buff *skb)
{
	struct airoha_gdm_dev *dev = netdev_priv(skb->dev);
	u16 qidx = skb_get_queue_mapping(skb);
	struct netdev_queue *txq;

	txq = skb_get_tx_queue(skb->dev, skb);
	spin_lock_bh(&dev->txq_lock[qidx]);
	netdev_tx_completed_queue(txq, 1, skb->len);
	spin_unlock_bh(&dev->txq_lock[qidx]);
}

#endif /* _AIROHA_TSO_H */
