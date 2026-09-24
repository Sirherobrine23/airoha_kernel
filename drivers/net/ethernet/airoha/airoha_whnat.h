/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * EN751221 WHNAT: PPE flow offload for WiFi netdevs without WED support.
 */
#ifndef AIROHA_WHNAT_H
#define AIROHA_WHNAT_H

#include <linux/netdevice.h>
#include <linux/skbuff.h>

struct airoha_ppe;
struct airoha_qdma;

#define AIROHA_WHNAT_VIFS		16
/* Marker the PPE writes in the L2 tag of flows bound to a WiFi vif. */
#define AIROHA_WHNAT_ETYPE		0x5678
/* LAN QDMA RX headroom, so mt76 can push its TX descriptor in place. */
#define AIROHA_WHNAT_RX_HEADROOM	64
/* TX descriptors a handed-off frame may never take from the LAN netdevs. */
#define AIROHA_WHNAT_TX_RESERVE		32

/* Bound frames collected in one NAPI poll, flushed per vif at its end. */
struct airoha_whnat_batch {
	struct sk_buff_head queue[AIROHA_WHNAT_VIFS];
	struct net_device *dev[AIROHA_WHNAT_VIFS];
};

#if IS_ENABLED(CONFIG_NET_AIROHA_WHNAT)
bool airoha_whnat_active(void);
int airoha_whnat_vif_index(struct net_device *dev);
void airoha_whnat_set_ppe(struct airoha_ppe *ppe);
void airoha_whnat_clear_ppe(struct airoha_ppe *ppe);
void airoha_whnat_set_qdma(struct airoha_qdma *qdma);
void airoha_whnat_clear_qdma(struct airoha_qdma *qdma);
void airoha_whnat_batch_init(struct airoha_whnat_batch *batch);
void airoha_whnat_batch_flush(struct airoha_whnat_batch *batch);
bool airoha_whnat_rx(struct sk_buff *skb, struct napi_struct *napi,
		     u8 qdma_id, u8 sport, u8 reason,
		     struct airoha_whnat_batch *batch);
void airoha_whnat_complete(void);

/* Provided by the QDMA and PPE code. */
int airoha_whnat_qdma_xmit(struct airoha_qdma *qdma, struct sk_buff *skb);
void airoha_whnat_invalidate_vif(struct airoha_ppe *ppe, int idx);

/* Called by mt76 through symbol_get(). */
int en75_whnat_register_vif(struct net_device *dev);
void en75_whnat_unregister_vif(struct net_device *dev);
bool en75_whnat_upstream_early(struct sk_buff *skb);
#else
static inline bool airoha_whnat_active(void)
{
	return false;
}

static inline int airoha_whnat_vif_index(struct net_device *dev)
{
	return -1;
}

static inline void airoha_whnat_set_ppe(struct airoha_ppe *ppe)
{
}

static inline void airoha_whnat_clear_ppe(struct airoha_ppe *ppe)
{
}

static inline void airoha_whnat_set_qdma(struct airoha_qdma *qdma)
{
}

static inline void airoha_whnat_clear_qdma(struct airoha_qdma *qdma)
{
}

static inline void airoha_whnat_batch_init(struct airoha_whnat_batch *batch)
{
}

static inline void airoha_whnat_batch_flush(struct airoha_whnat_batch *batch)
{
}

static inline bool airoha_whnat_rx(struct sk_buff *skb,
				   struct napi_struct *napi, u8 qdma_id,
				   u8 sport, u8 reason,
				   struct airoha_whnat_batch *batch)
{
	return false;
}

static inline void airoha_whnat_complete(void)
{
}
#endif

#endif /* AIROHA_WHNAT_H */
