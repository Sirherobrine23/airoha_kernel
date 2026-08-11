// SPDX-License-Identifier: GPL-2.0-only
/*
 * Shared Airoha/EcoNet QDMA helpers and generation-1 QDMA backend.
 */
#include <linux/bitmap.h>
#include <linux/dma-mapping.h>
#include <linux/etherdevice.h>
#include <linux/interrupt.h>
#include <linux/netdevice.h>
#include <linux/if_vlan.h>
#include <linux/ipv6.h>
#include <linux/tcp.h>
#include <linux/of_reserved_mem.h>
#include <linux/skbuff.h>
#include <linux/slab.h>
#include <linux/unaligned.h>
#include <net/dsa.h>
#include <net/dst_metadata.h>
#include <net/ip6_checksum.h>
#include <net/tcp.h>
#include <net/page_pool/helpers.h>

#include "airoha_eth.h"
#include "airoha_eth_gen1.h"
#include "airoha_regs.h"

void airoha_qdma_common_init(struct airoha_qdma_common *qdma,
			     struct airoha_eth *eth, void __iomem *regs, u8 id)
{
	qdma->eth = eth;
	qdma->regs = regs;
	qdma->id = id;
}

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

/* QDMA datapath. */
/* The non-dma part of RX packet descriptor */
struct econet_q_rx_ent {
	void				*buf;
	dma_addr_t			dma_addr;
	u16				dma_len;
};

struct econet_q_rx {
	/* No lock, access only in NAPI, or else when NAPI is disabled
	 * and qdma->lock is held */
	struct econet_q_rx_ent		*entry;
	struct desc			*desc;
	u16				cpu_i;

	/* Not modified after init */
	struct econet_qdma		*qdma;
	struct qchain_regs __iomem	*qchain_regs;
	int				ndesc;
	int				buf_size;
	struct napi_struct		napi;
	struct page_pool		*page_pool;
};

/* The non-dma part of TX packet descriptor */
struct econet_q_tx_ent {
	struct sk_buff			*skb;
	dma_addr_t			dma_addr;
	u16				dma_len;
	u16				freelist_next;
};

struct econet_q_tx {
	/* protect concurrent queue accesses
	 * use _bh unless in napi poll */
	spinlock_t			lock_bh;
	struct econet_q_tx_ent		*entry;
	struct desc			*desc;

	/* FIFO of free entries because they complete out of order. */
	u16				freelist_head;
	u16				freelist_tail;

	/* Not modified after init */
	struct econet_qdma		*qdma;
	struct qchain_regs __iomem	*qchain_regs;
	int				ndesc;
	struct napi_struct		napi;
};

struct econet_irq {
	/* protect concurrent irqmask accesses
	 * use _irqsave unless in irq handler */
	spinlock_t 			lock_irq;
	u32 				irqmask[QDMA_REGS_PER_IRQ];
	u32 __iomem 			*mask_reg[QDMA_REGS_PER_IRQ];
	u32 __iomem 			*status_reg[QDMA_REGS_PER_IRQ];

	/* Not modified after init */
	struct econet_qdma 		*qdma;
	int 				irq;
};

struct econet_tx_doneq {
	/* No lock, access only in NAPI */
	u32 				*q;
	struct qregs_doneq __iomem	*regs;

	/* Not modified after init */
	struct econet_qdma 		*qdma;
	int 				size;
	struct napi_struct 		napi;
};

struct econet_qdma {
	struct airoha_qdma_common	common;

	/* Protects register programming and users. */
	struct mutex			lock;
	/* Typed generation-1 view of common.regs. */
	struct qregs __iomem		*regs;
	int				users;

	/* Synchronized inside of the structure */
	struct econet_irq 		irqs[QDMA_NUM_IRQS];
	struct econet_tx_doneq 		q_tx_done[QDMA_NUM_TX_DONE];
	struct econet_q_tx 		q_tx[QDMA_NUM_CHAINS];
	struct econet_q_rx 		q_rx[QDMA_NUM_CHAINS];

	/* Not modified after init */
	struct device 			*dev;
	struct fwdesc 			*hwf_desc;
	int				num_fwd_descs;
	u32				fwd_buf_size;
	struct net_device 		*napi_dev;
	struct econet_qdma_cfg 		cfg;
	const struct airoha_eth_soc_data	*soc;
};

static void econet_fill_rx_queue(struct econet_q_rx *q, u32 end_i)
{
	u32 ndesc = q->ndesc;
	u32 cpu_i = (q->cpu_i + 1) % ndesc;
	int rx_offset = q->qdma->cfg.rx_2b_offset ? NET_IP_ALIGN : 0;

	for (; cpu_i != end_i; cpu_i = (cpu_i + 1) % ndesc) {
		struct econet_q_rx_ent *e = &q->entry[cpu_i];
		struct desc *pdesc = &q->desc[cpu_i];
		struct page *page;
		u16 pkt_len;
		int offset;
		int i;

		page = page_pool_dev_alloc_frag(q->page_pool, &offset,
						q->buf_size);

		if (!page)
			break;

		WARN_ON_ONCE(e->dma_addr);
		e->buf = page_address(page) + offset;
		e->dma_addr = page_pool_get_dma_addr(page) + offset;
		pkt_len = SKB_WITH_OVERHEAD(q->buf_size) - rx_offset;
		e->dma_len = pkt_len + rx_offset;

		WRITE_ONCE(pdesc->info, ((struct desc_info) {
			.word = FIELD_PREP(DESC_INFO_PKT_LEN_MASK, pkt_len)
		}));
		WRITE_ONCE(pdesc->pkt_addr, e->dma_addr);
		for (i = 0; i < ARRAY_SIZE(pdesc->msg.raw); i++)
			WRITE_ONCE(pdesc->msg.raw[i], 0);
	}

	cpu_i = (cpu_i - 1) % ndesc;

	q->cpu_i = cpu_i;
	/* Publish every refilled descriptor before returning ownership to DMA. */
	dma_wmb();
	econet_wreg(cpu_i, &q->qchain_regs->rx_cpui);
}

static void econet_qdma_rx_process_one(struct econet_q_rx *q, u32 cpu_i,
				     enum dma_data_direction dir)
{
	struct econet_q_rx_ent *e = &q->entry[cpu_i];
	struct sk_buff *skb;
	struct page *page;
	struct desc desc;
	u32 hash;
	u8 sport;
	int len;

	memcpy(&desc, &q->desc[cpu_i], sizeof(desc));

	dma_sync_single_for_cpu(q->qdma->dev, e->dma_addr, e->dma_len, dir);

	/* Not needed but a couple of WARN_ON_ONCE() check this later */
	e->dma_addr = 0;

	/* Make debug more readable */
	WRITE_ONCE(q->desc[cpu_i].pkt_addr, 0);

	len = get_desc_info_pkt_len(&desc.info);
	if (!len || len > SKB_WITH_OVERHEAD(q->buf_size) -
				(q->qdma->cfg.rx_2b_offset ? NET_IP_ALIGN : 0))
		goto return_page;

	if (WARN_ON_ONCE(is_desc_info_nls(&desc.info)))
		goto return_page;

	skb = napi_build_skb(e->buf, q->buf_size);
	if (!skb)
		goto return_page;

	if (q->qdma->cfg.rx_2b_offset)
		skb_reserve(skb, NET_IP_ALIGN);
	__skb_put(skb, len);
	skb_mark_for_recycle(skb);
	skb->ip_summed = CHECKSUM_NONE;

	/* EN751221 QDMA1 uses PWAN_FERxMsg_T for GPON/EPON OAM.  Word 0
	 * carries OAM/channel/GEM metadata while words 1..3 retain the regular
	 * FE RX layout.  Divert management frames before interpreting them as
	 * Ethernet/PPE traffic.
	 */
	if (airoha_eth_gen1_rx_xpon_oam(q->qdma->common.eth,
					q->qdma->common.id, skb, &desc.msg))
		return;

	hash = get_erx_ppe_entry(&desc.msg.erx);
	skb_set_hash(skb, jhash_1word(hash, 0),
		     PKT_HASH_TYPE_L4);
	airoha_ppe_dev_check_skb_reason(q->qdma->common.eth->ppe_dev, skb, hash,
					get_erx_crsn(&desc.msg.erx));

	sport = get_erx_sport(&desc.msg.erx);
	if (econet_rx_before_recv(q->qdma->common.eth, skb, sport)) {
		dev_kfree_skb(skb);
	} else {
		if ((skb->dev->features & NETIF_F_RXCSUM) &&
		    sport != ETX_FPORT_QDMA0_CPU &&
		    sport != ETX_FPORT_QDMA1_CPU &&
		    (is_erx_ip4(&desc.msg.erx) ||
		     is_erx_ip6(&desc.msg.erx)) &&
		    !is_erx_ip4f(&desc.msg.erx) &&
		    !is_erx_l4f(&desc.msg.erx))
			skb->ip_summed = CHECKSUM_UNNECESSARY;

		napi_gro_receive(&q->napi, skb);
	}

	return;

return_page:
	page = virt_to_head_page(e->buf);
	page_pool_put_full_page(q->page_pool, page, false);
}

static int econet_qdma_rx_process(struct econet_q_rx *q, int budget)
{
	enum dma_data_direction dir = page_pool_get_dma_dir(q->page_pool);
	u32 hardware_i = econet_rreg(&q->qchain_regs->rx_hwi);
	int ndesc = q->ndesc;
	u32 cpu_i;
	int done;

	/*
	 * The device publishes descriptors before advancing RX_HWI. Pair the
	 * producer-side ordering with a DMA read barrier before consuming them.
	 */
	dma_rmb();

	/* The stored value of cpu_i is the last entry actually handled.
	 * Whereas hardware_i is the next entry that has not yet been received.
	 */
	cpu_i = (q->cpu_i + 1) % ndesc;

	for (done = 0; done < budget && cpu_i != hardware_i; done++) {
		econet_qdma_rx_process_one(q, cpu_i, dir);
		cpu_i = (cpu_i + 1) % ndesc;
	}

	econet_fill_rx_queue(q, cpu_i);

	return done;
}

#define IRQ_PURPOSE(t, s, c) \
	((union econet_irq_purpose){ .type = IPS_ ## t, .source = IPSC_ ## s, .chain = (c) })

union irq_bit {
	struct {
		int irq_idx 	: 8;
		int reg_idx 	: 8;
		int bit_idx 	: 8;
		int _pad 	: 8;
	};
	u32 word;
};
static_assert(sizeof(union irq_bit) == 4, "irq_bit size");

/* IRQ functions relevant to the EN751221 IRQ layout */

static const union econet_irq_purpose EN751221_IRQ_MAP[] = {
	[0]  = IRQ_PURPOSE(DONE,		TX,	0),
	[1]  = IRQ_PURPOSE(DONE,		RX,	0),
	[2]  = IRQ_PURPOSE(NO_DSCP,		TX,	0),
	[3]  = IRQ_PURPOSE(NO_DSCP,		RX,	0),
	[4]  = IRQ_PURPOSE(DONE,		TX,	1),
	[5]  = IRQ_PURPOSE(DONE,		RX,	1),
	[6]  = IRQ_PURPOSE(NO_DSCP,		TX,	1),
	[7]  = IRQ_PURPOSE(NO_DSCP,		RX,	1),
	[8]  = IRQ_PURPOSE(NO_DSCP,		FWD,	-1),
	[9]  = IRQ_PURPOSE(NO_DSCP,		DONE,	0),
	[10] = IRQ_PURPOSE(LOW_DSCP,		FWD,	0),
	[11] = IRQ_PURPOSE(OVERFLOW,		UNSPEC,	-1),
	[12] = IRQ_PURPOSE(ERR_COHERENT,	TX,	0),
	[13] = IRQ_PURPOSE(ERR_COHERENT,	RX,	0),
	[14] = IRQ_PURPOSE(ERR_COHERENT,	TX,	1),
	[15] = IRQ_PURPOSE(ERR_COHERENT,	RX,	1),
	[16] = IRQ_PURPOSE(GPON_INT,		UNSPEC,	-1),
	[17] = IRQ_PURPOSE(EPON_INT,		UNSPEC,	-1),
	[18] = IRQ_PURPOSE(XPON_INT,		UNSPEC,	-1),
};

static bool en751221_valid_irq_bit(union irq_bit bit)
{
	return bit.irq_idx == 0 && bit.reg_idx == 0 &&
		bit.bit_idx < ARRAY_SIZE(EN751221_IRQ_MAP);
}

static union econet_irq_purpose en751221_irq_purpose(union irq_bit bit)
{
	if (WARN_ON_ONCE(!en751221_valid_irq_bit(bit)))
		return (union econet_irq_purpose){0};

	return EN751221_IRQ_MAP[bit.bit_idx];
}

static union irq_bit en751221_irq_bit(union econet_irq_purpose purpose)
{
	union irq_bit bit = {0};
	for (int i = 0; i < ARRAY_SIZE(EN751221_IRQ_MAP); i++) {
		if (EN751221_IRQ_MAP[i].word == purpose.word) {
			bit.bit_idx = i;
			return bit;
		}
	}
	return bit;
}

static u32 __iomem *en751221_irq_status_reg(struct qregs __iomem *regs, int irqn)
{
	if (irqn > 0)
		return ERR_PTR(-EINVAL);

	return &regs->int_status;
}

static u32 __iomem *en751221_irq_enable_reg(struct qregs __iomem *regs, int irqn)
{
	if (irqn > 0)
		return ERR_PTR(-EINVAL);

	return &regs->int_enable;
}

/* End EN751221 IRQ functions */

static void econet_qdma_set_irqmask(struct econet_qdma *qdma, union irq_bit b, bool enable)
{
	struct econet_irq *irq;

	if (WARN_ON_ONCE(b.irq_idx >= ARRAY_SIZE(qdma->irqs)))
		return;

	irq = &qdma->irqs[b.irq_idx];

	if (WARN_ON_ONCE(b.reg_idx >= ARRAY_SIZE(irq->irqmask)))
		return;

	if (WARN_ON_ONCE(b.bit_idx >= 32))
		return;

	guard(spinlock_irqsave)(&irq->lock_irq);

	if (enable)
		irq->irqmask[b.reg_idx] |= BIT(b.bit_idx);
	else
		irq->irqmask[b.reg_idx] &= ~BIT(b.bit_idx);

	econet_wreg(irq->irqmask[b.reg_idx], irq->mask_reg[b.reg_idx]);

	/* Read irq_enable register in order to guarantee the update above
	 * completes in the spinlock critical section.
	 */
	econet_rreg(irq->mask_reg[b.reg_idx]);
}

int airoha_qdma_gen1_set_xpon_irq(struct econet_qdma *qdma,
				  enum airoha_xpon_mode mode, bool enable)
{
	union econet_irq_purpose purpose;
	union irq_bit bit;

	if (!qdma || qdma->common.id != 1)
		return -EINVAL;

	switch (mode) {
	case AIROHA_XPON_MODE_GPON:
		purpose = IRQ_PURPOSE(GPON_INT, UNSPEC, -1);
		break;
	case AIROHA_XPON_MODE_EPON:
		purpose = IRQ_PURPOSE(EPON_INT, UNSPEC, -1);
		break;
	default:
		return -EINVAL;
	}

	bit = en751221_irq_bit(purpose);
	if (!en751221_valid_irq_bit(bit))
		return -EINVAL;

	econet_qdma_set_irqmask(qdma, bit, enable);
	if (!enable)
		synchronize_irq(qdma->irqs[bit.irq_idx].irq);

	return 0;
}

static int econet_qdma_rx_napi_poll(struct napi_struct *napi, int budget)
{
	struct econet_q_rx *q = container_of(napi, struct econet_q_rx, napi);
	struct econet_qdma *qdma = q->qdma;
	int cur, done = 0;

	do {
		cur = econet_qdma_rx_process(q, budget - done);
		done += cur;
	} while (cur && done < budget);

	if (done < budget && napi_complete(napi)) {
		union econet_irq_purpose purpose;
		union irq_bit b;

		purpose = IRQ_PURPOSE(DONE, RX, q - &qdma->q_rx[0]);
		b = en751221_irq_bit(purpose);
		econet_qdma_set_irqmask(qdma, b, true);
	}

	return done;
}

static irqreturn_t econet_irq_handler(int irq_num, void *dev_instance)
{
	struct econet_irq *irq = dev_instance;
	struct econet_qdma *qdma = irq->qdma;
	unsigned long xpon_pending = 0;
	int i;

	{
		guard(spinlock)(&irq->lock_irq);

		for (i = 0; i < ARRAY_SIZE(irq->irqmask); i++) {
			unsigned long regval;
			u32 disable_int = 0;
			u8 bit;

			regval = econet_rreg(irq->status_reg[i]);
			regval &= irq->irqmask[i];

			/* You must write the bits back to the status register
			 * or you will keep receiving the same interrupt.
			 */
			econet_wreg((u32)regval, irq->status_reg[i]);

			for_each_set_bit(bit, &regval, 32) {
				union econet_irq_purpose p;

				p = en751221_irq_purpose((union irq_bit){
					.irq_idx = irq - &qdma->irqs[0],
					.reg_idx = i,
					.bit_idx = bit,
				});

				if (p.type == IPS_DONE && p.source == IPSC_RX) {
					napi_schedule_irqoff(&qdma->q_rx[p.chain].napi);
					disable_int |= BIT(bit);
					continue;
				}
				if (p.type == IPS_DONE && p.source == IPSC_TX) {
					napi_schedule_irqoff(&qdma->q_tx_done[i].napi);
					disable_int |= BIT(bit);
					continue;
				}
				if (p.type == IPS_GPON_INT) {
					xpon_pending |= BIT(AIROHA_XPON_MODE_GPON);
					continue;
				}
				if (p.type == IPS_EPON_INT) {
					xpon_pending |= BIT(AIROHA_XPON_MODE_EPON);
					continue;
				}
				dev_dbg_ratelimited(qdma->dev,
						    "%s IRQ from %s[%d]\n",
						    econet_irq_purpose_type_str(p.type),
						    econet_irq_purpose_source_str(p.source),
						    p.chain);
			}

			irq->irqmask[i] &= ~disable_int;
			econet_wreg(irq->irqmask[i], irq->mask_reg[i]);
		}
	}

	/* The EN751221 vendor stack registers GPON/EPON MAC handlers through
	 * QDMA_WAN.  Invoke the MAC after acknowledging the QDMA aggregator and
	 * after dropping irq->lock_irq; the MAC ISR performs its own W1C and FIFO
	 * drain operations.
	 */
	if (xpon_pending & BIT(AIROHA_XPON_MODE_GPON))
		airoha_eth_gen1_xpon_irq(qdma->common.eth, qdma->common.id,
					 AIROHA_XPON_MODE_GPON);
	if (xpon_pending & BIT(AIROHA_XPON_MODE_EPON))
		airoha_eth_gen1_xpon_irq(qdma->common.eth, qdma->common.id,
					 AIROHA_XPON_MODE_EPON);

	return IRQ_HANDLED;
}

static int econet_poll_tx_complete(struct napi_struct *napi, int budget)
{
	struct qregs_doneq_state state;
	struct econet_tx_doneq *done_q;
	struct econet_qdma *qdma;
	int irq_queued;
	u32 done = 0, head;

	done_q = container_of(napi, struct econet_tx_doneq, napi);
	qdma = done_q->qdma;

	state = econet_rreg(&done_q->regs->state);
	head = get_qregs_doneq_state_head_index(&state);
	head = head % done_q->size;
	irq_queued = get_qregs_doneq_state_length(&state);
	/* The done queue entries/descriptors precede the producer state update. */
	dma_rmb();

	while (irq_queued > 0 && done < budget) {
		u32 index, qid, val = done_q->q[head];
		struct econet_q_tx_ent *e;
		struct econet_q_tx *q;
		struct netdev_queue *txq;
		struct sk_buff *skb;
		struct desc desc;

		if (val == 0xffffffffU)
			break;

		done_q->q[head] = 0xffffffffU; /* mark as done */
		head = (head + 1) % done_q->size;
		irq_queued--;
		done++;

		qid = FIELD_GET(EN751221_QDMA_IRQ_RING_IDX_MASK, val);
		if (WARN_ON_ONCE(qid >= ARRAY_SIZE(qdma->q_tx)))
			continue;

		q = &qdma->q_tx[qid];
		if (WARN_ON_ONCE(!q->ndesc))
			continue;

		index = FIELD_GET(EN751221_QDMA_IRQ_DESC_IDX_MASK, val);
		if (WARN_ON_ONCE(index >= q->ndesc))
			continue;

		guard(spinlock)(&q->lock_bh);

		desc = q->desc[index];

		if (WARN_ON_ONCE(!is_desc_info_done(&desc.info) &&
				 !is_desc_info_dropped(&desc.info)))
			continue;

		e = &q->entry[index];
		skb = e->skb;

		dma_unmap_single(qdma->dev, e->dma_addr, e->dma_len,
				 DMA_TO_DEVICE);
		memset(e, 0, sizeof(*e));

		/* Completion ring can report out of order when hw QoS is
		 * enabled and packets with different priority are queued
		 * to same DMA ring. So we use a linked list to maintain free
		 * entries.
		 */
		e->freelist_next = 0xffff;
		q->entry[q->freelist_tail].freelist_next = index;
		q->freelist_tail = index;

		txq = netdev_get_tx_queue(skb->dev,
					  skb_get_queue_mapping(skb));
		netdev_tx_completed_queue(txq, 1, skb->len);
		if (netif_tx_queue_stopped(txq))
			netif_tx_wake_queue(txq);

		dev_kfree_skb_any(skb);
	}

	if (done) {
		int i, len = done >> 7;

		for (i = 0; i < len; i++) {
			econet_rreg(&qdma->regs->done_queue.pop_back);
			econet_wreg(0x80U, &qdma->regs->done_queue.pop_back);
		}
		econet_rreg(&qdma->regs->done_queue.pop_back);
		econet_wreg(done & 0x7f, &qdma->regs->done_queue.pop_back);
	}

	if (done < budget && napi_complete(napi)) {
		int chain;

		/*
		 * EN751221 has a single completion list shared by TX0 and TX1.
		 * Either DONE source can therefore schedule this NAPI instance.
		 * Re-arm both sources after draining it; re-arming only TX0 leaves
		 * TX1 permanently masked after its first interrupt.
		 */
		for (chain = 0; chain < QDMA_NUM_CHAINS; chain++) {
			union econet_irq_purpose purpose;
			union irq_bit b;

			purpose = IRQ_PURPOSE(DONE, TX, chain);
			b = en751221_irq_bit(purpose);
			econet_qdma_set_irqmask(qdma, b, true);
		}
	}

	return done;
}

int airoha_qdma_gen1_xmit(struct econet_qdma *qdma, struct sk_buff *skb,
		   union desc_msg *msg, int qid)
{
	struct econet_q_tx *q = &qdma->q_tx[qid];
	int len = skb_headlen(skb);
	struct econet_q_tx_ent *e;
	u16 index, next_index;
	struct desc *desc;
	dma_addr_t addr;
	int ret;

	guard(spinlock_bh)(&q->lock_bh);

	index = q->freelist_head;
	if (index == 0xffff)
		return -EBUSY;

	e = &q->entry[index];
	next_index = e->freelist_next;
	if (next_index == 0xffff)
		return -EBUSY;

	addr = dma_map_single(qdma->dev, skb->data, len, DMA_TO_DEVICE);
	ret = dma_mapping_error(qdma->dev, addr);
	if (unlikely(ret))
		return ret;

	desc = &q->desc[index];
	WRITE_ONCE(desc->pkt_addr, addr);
	WRITE_ONCE(desc->info, ((struct desc_info) {
		.word = FIELD_PREP(DESC_INFO_PKT_LEN_MASK, len)
	}));
	WRITE_ONCE(desc->next_idx, next_index);
	for (int i = 0; i < ARRAY_SIZE(desc->msg.raw); i++)
		WRITE_ONCE(desc->msg.raw[i], msg->raw[i]);

	e->skb = skb;
	e->dma_addr = addr;
	e->dma_len = len;
	q->freelist_head = next_index;

	skb_tx_timestamp(skb);

	/*
	 * Match the vendor QDMA handoff: the descriptor must be globally visible
	 * before TX_CPUI gives it to hardware. This is required on non-coherent
	 * MIPS even though the descriptor ring itself is dma_alloc_coherent().
	 */
	dma_wmb();
	econet_wreg((u32)next_index, &q->qchain_regs->tx_cpui);

	return q->entry[next_index].freelist_next == 0xffff ?
		EBUSY : 0;
}

/* Init functions, no locks, assumed non-concurrent */

static int econet_init_rx_queue(struct econet_q_rx *q,
			      struct econet_qdma *qdma, int ndesc)
{
	const struct page_pool_params pp_params = {
		.order = 0,
		.pool_size = ndesc,
		.flags = PP_FLAG_DMA_MAP | PP_FLAG_DMA_SYNC_DEV,
		.dma_dir = DMA_FROM_DEVICE,
		.max_len = PAGE_SIZE,
		.nid = NUMA_NO_NODE,
		.dev = qdma->dev,
		.napi = &q->napi,
	};
	int threshold;
	struct qregs_rxring_size rrs;
	struct qregs_rxring_low rrl;
	dma_addr_t dma_addr;

	q->buf_size = ECONET_MAX_PACKET_SIZE;
	q->ndesc = ndesc;
	q->qdma = qdma;

	if (airoha_is(qdma->common.eth, econet_en751221))
		/* QDMA_LAN uses 32 for both rings; QDMA_WAN uses ring_size / 4. */
		threshold = qdma->common.id == 0 ? 32 : ndesc >> 2;
	else
		threshold = clamp(ndesc >> 3, 1, 32);
	q->qchain_regs = (q == &qdma->q_rx[0]) ?
			 &qdma->regs->qchain0 :
			 &qdma->regs->qchain1;

	q->entry = devm_kzalloc(qdma->dev, q->ndesc * sizeof(*q->entry),
				GFP_KERNEL);
	if (!q->entry)
		return -ENOMEM;

	q->page_pool = page_pool_create(&pp_params);
	if (IS_ERR(q->page_pool)) {
		int err = PTR_ERR(q->page_pool);

		q->page_pool = NULL;
		return err;
	}

	q->desc = dmam_alloc_coherent(qdma->dev, q->ndesc * sizeof(*q->desc),
				      &dma_addr, GFP_KERNEL);
	if (!q->desc)
		return -ENOMEM;

	memset(q->desc, 0, q->ndesc * sizeof(*q->desc));

	netif_napi_add(qdma->napi_dev, &q->napi, econet_qdma_rx_napi_poll);

	econet_wreg(lower_32_bits(dma_addr), &q->qchain_regs->rxbase);

	/* econet_fill_rx_queue fills everything from current cpu index + 1 up to
	 * but excluding end_i, so to fill every entry we run it with 0 to do
	 * 1,2,3,[...],n, then we run it with 1 to do 0. */
	econet_fill_rx_queue(q, 0);
	econet_fill_rx_queue(q, 1);
	for (int i = 0; i < q->ndesc; i++)
		if (!q->entry[i].dma_addr)
			return -ENOMEM;

	/* The RX hardware side considers that hwi == cpui means the queue is
	 * full, not empty. Since cpui starts at 0, we initialize hwi to 1. */
	econet_wreg(1U, &q->qchain_regs->rx_hwi);

	rrs = econet_rreg(&qdma->regs->rxring_size);
	rrl = econet_rreg(&qdma->regs->rxring_low);

	if (q == &qdma->q_rx[0]) {
		set_qregs_rxring_size_ring0(&rrs, ndesc);
		set_qregs_rxring_low_ring0(&rrl, threshold);
	} else {
		set_qregs_rxring_size_ring1(&rrs, ndesc);
		set_qregs_rxring_low_ring1(&rrl, threshold);
	}

	econet_wreg(rrs, &qdma->regs->rxring_size);
	econet_wreg(rrl, &qdma->regs->rxring_low);

	return 0;
}

static int econet_init_irqs(struct device *dev, struct econet_qdma *qdma,
			  int *irqs, int num_irqs)
{
	int i;

	if (num_irqs < ARRAY_SIZE(qdma->irqs))
		return -EINVAL;

	for (i = 0; i < ARRAY_SIZE(qdma->irqs); i++) {
		struct econet_irq *irq = &qdma->irqs[i];
		const char *name;
		int err;
		int j;

		spin_lock_init(&irq->lock_irq);
		irq->qdma = qdma;

		irq->irq = irqs[i];
		if (irq->irq < 0)
			return irq->irq;

		for (j = 0; j < QDMA_REGS_PER_IRQ; j++) {
			irq->status_reg[j] = en751221_irq_status_reg(qdma->regs, j);
			irq->mask_reg[j] = en751221_irq_enable_reg(qdma->regs, j);
		}

		name = devm_kasprintf(dev, GFP_KERNEL,
				      KBUILD_MODNAME "-%d.%d", qdma->common.id, i);
		if (!name)
			return -ENOMEM;

		err = devm_request_irq(dev, irq->irq,
				       econet_irq_handler, IRQF_SHARED, name,
				       irq);
		if (err)
			return err;
	}

	return 0;
}

static int econet_init_tx_doneq(struct econet_tx_doneq *done_q,
			      struct econet_qdma *qdma, int size,
			      int irq_threshold)
{
	dma_addr_t dma_addr;

	netif_napi_add_tx(qdma->napi_dev, &done_q->napi,
			  econet_poll_tx_complete);
	done_q->q = dmam_alloc_coherent(qdma->dev, size * sizeof(u32),
				       &dma_addr, GFP_KERNEL);
	if (!done_q->q)
		return -ENOMEM;

	memset(done_q->q, 0xff, size * sizeof(u32));
	done_q->size = size;
	done_q->qdma = qdma;
	done_q->regs = &qdma->regs->done_queue;

	econet_wreg(lower_32_bits(dma_addr), &qdma->regs->done_queue.address);
	struct qregs_doneq_cfg cfg = econet_rreg(&qdma->regs->done_queue.config);
	set_qregs_doneq_cfg_size(&cfg, size);
	set_qregs_doneq_cfg_int_threshold(&cfg, irq_threshold);
	econet_wreg(cfg, &qdma->regs->done_queue.config);

	return 0;
}

static int econet_init_tx_queue(struct econet_q_tx *q,
			      struct econet_qdma *qdma, int size)
{
	int i, qid = q - &qdma->q_tx[0];
	dma_addr_t dma_addr;

	spin_lock_init(&q->lock_bh);
	q->ndesc = size;
	q->qdma = qdma;
	q->qchain_regs = (qid == 0) ?
			 &qdma->regs->qchain0 :
			 &qdma->regs->qchain1;

	q->entry = devm_kzalloc(qdma->dev, q->ndesc * sizeof(*q->entry),
				GFP_KERNEL);
	if (!q->entry)
		return -ENOMEM;

	q->desc = dmam_alloc_coherent(qdma->dev, q->ndesc * sizeof(*q->desc),
				      &dma_addr, GFP_KERNEL);
	if (!q->desc)
		return -ENOMEM;

	memset(q->desc, 0, q->ndesc * sizeof(*q->desc));

	for (i = 0; i < q->ndesc - 1; i++) {
		q->entry[i].freelist_next = i + 1;
	}
	q->entry[q->ndesc - 1].freelist_next = 0xffff;
	q->freelist_tail = q->ndesc - 1;
	q->freelist_head = 0;

	econet_wreg(lower_32_bits(dma_addr), &q->qchain_regs->txbase);

	/* On TX, hardware advances hwi until it is equal to cpui. */
	econet_wreg(0U, &q->qchain_regs->tx_cpui);
	econet_wreg(0U, &q->qchain_regs->tx_hwi);

	return 0;
}

static int econet_init_hw_fwd(struct econet_qdma *qdma)
{
	enum qregs_hwf_cfg_pkt_sz buf_size_cfg = QREGS_HWF_CFG_PKT_SZ_2048;
	int ret, size, index, num_desc = qdma->cfg.num_fwd_descs;
	struct qregs_hwf_cfg1 cfg1;
	dma_addr_t dma_addr;
	u32 buf_size = 2048;
	struct hwf_cfg cfg;
	const char *name;

	name = devm_kasprintf(qdma->dev, GFP_KERNEL, "qdma%d-buf", qdma->common.id);
	if (!name)
		return -ENOMEM;

	while (buf_size < qdma->cfg.fwd_max_packet_size) {
		buf_size_cfg++;
		buf_size = 2048 << buf_size_cfg;
		if (buf_size > 16384) {
			dev_err(qdma->dev, "Unsupported hw forwarding max packet size %d\n",
				qdma->cfg.fwd_max_packet_size);
			return -EINVAL;
		}
	}

	index = of_property_match_string(qdma->dev->of_node,
					 "memory-region-names", name);
	if (index >= 0) {
		struct reserved_mem *rmem;
		struct device_node *np;

		/* Consume reserved memory for hw forwarding buffers queue if
		 * available in the DTS
		 */
		np = of_parse_phandle(qdma->dev->of_node, "memory-region",
				      index);
		if (!np)
			return -ENODEV;

		rmem = of_reserved_mem_lookup(np);
		of_node_put(np);
		if (!rmem)
			return dev_err_probe(qdma->dev, -EINVAL,
					     "invalid %s memory-region\n", name);
		if (!rmem->size || upper_32_bits(rmem->base) ||
		    upper_32_bits(rmem->base + rmem->size - 1))
			return dev_err_probe(qdma->dev, -ERANGE,
					     "%s memory-region must be below 4 GiB\n",
					     name);

		dma_addr = rmem->base;
		/* Compute the number of hw descriptors according to the
		 * reserved memory size and the payload buffer size
		 */
		num_desc = div_u64(rmem->size, buf_size);
		if (!num_desc)
			return dev_err_probe(qdma->dev, -EINVAL,
					     "%s memory-region is too small\n", name);

		if (num_desc < qdma->cfg.num_fwd_descs)
			dev_warn(qdma->dev,
				 "Reserved memory %pa too small for %d "
				 "hw forwarding descriptors with %d bytes"
				 "payload, reducing to %d descriptors.\n",
				 &rmem->size, qdma->cfg.num_fwd_descs,
				 buf_size, num_desc);
	} else {
		/*
		 * The vendor QDMA_WAN reserves 8 MiB for 4096 x 2 KiB HWF
		 * buffers. Do not require such a large contiguous coherent
		 * allocation on boards that omit the reserved qdma1-buf region;
		 * keep the exact 4096-entry profile whenever the region exists,
		 * otherwise use a conservative fallback pool.
		 */
		if (airoha_is(qdma->common.eth, econet_en751221) && qdma->common.id == 1 &&
		    num_desc > 256)
			num_desc = 256;

		size = buf_size * num_desc;
		if (!dmam_alloc_coherent(qdma->dev, size, &dma_addr,
					 GFP_KERNEL))
			return -ENOMEM;
	}

	econet_wreg(lower_32_bits(dma_addr), &qdma->regs->hwf_data_addr);

	size = num_desc * sizeof(*qdma->hwf_desc);
	qdma->hwf_desc = dmam_alloc_coherent(qdma->dev, size, &dma_addr, GFP_KERNEL);
	if (!qdma->hwf_desc)
		return -ENOMEM;

	econet_wreg(lower_32_bits(dma_addr), &qdma->regs->hwf_desc_addr);

	qdma->num_fwd_descs = num_desc;
	qdma->fwd_buf_size = buf_size;

	cfg = econet_rreg(&qdma->regs->hwf_cfg);
	set_qregs_hwf_cfg_pkt_sz(&cfg, buf_size_cfg);
	set_qregs_hwf_cfg_low_th(&cfg,
				 min_t(int, qdma->cfg.fwd_low_threshold,
				       max_t(int, 1, num_desc >> 3)));
	econet_wreg(cfg, &qdma->regs->hwf_cfg);

	cfg1 = econet_rreg(&qdma->regs->hwf_cfg1);
	set_qregs_hwf_cfg1_fwd_desc_n(&cfg1, num_desc);
	set_qregs_hwf_cfg1_overhead_en(&cfg1, true);
	set_qregs_hwf_cfg1_overhead(&cfg1, qdma->common.id == 0 ? 0x14 : 0x18);
	set_qregs_hwf_cfg1_start(&cfg1, true);
	econet_wreg(cfg1, &qdma->regs->hwf_cfg1);

	ret = read_poll_timeout(econet_rreg, cfg1,
				!is_qregs_hwf_cfg1_start(&cfg1), USEC_PER_MSEC,
				30 * USEC_PER_MSEC, true,
				&qdma->regs->hwf_cfg1);
	if (ret)
		dev_err(qdma->dev,
			"Error %pe waiting for HW forwarding engine to start",
			ERR_PTR(ret));
	return ret;
}

static void econet_init_en751221_qdma(struct econet_qdma *qdma)
{
	struct qregs_tx_congest_cfg cngst_cfg;
	struct wrr_mode wrr = { 0 };
	u32 total_min, channel_min, queue_min;
	u32 total_max, channel_max, queue_max;
	u32 physical_size, val;

	/*
	 * Match the final PSE buffer policy observed on the production EN751221
	 * firmware.  Userspace retunes the initial SDK values before the system
	 * reaches its steady state: buffer control and estimation are enabled,
	 * DMA prefetch stays disabled, and LAN/WAN use distinct thresholds.
	 */
	val = econet_rreg(&qdma->regs->buf_usage_cfg);
	val &= ~(EN751221_PSE_BUF_CTRL_EN |
		 EN751221_PSE_BUF_PREFETCH_EN |
		 EN751221_PSE_BUF_ESTIMATE_EN |
		 EN751221_PSE_BUF_CH_THR_MASK |
		 EN751221_PSE_BUF_TOTAL_THR_MASK);
	val |= EN751221_PSE_BUF_CTRL_EN | EN751221_PSE_BUF_ESTIMATE_EN;
	if (qdma->common.id == 0) {
		val |= FIELD_PREP(EN751221_PSE_BUF_CH_THR_MASK, 16) |
		       FIELD_PREP(EN751221_PSE_BUF_TOTAL_THR_MASK, 128);
	} else {
		val |= FIELD_PREP(EN751221_PSE_BUF_CH_THR_MASK, 20) |
		       FIELD_PREP(EN751221_PSE_BUF_TOTAL_THR_MASK, 192);
	}
	econet_wreg(val, &qdma->regs->buf_usage_cfg);

	/* Match qdma_dev.c: 16-byte WRR scale, accounting weights by byte. */
	set_qregs_wrr_mode_use_16b(&wrr, true);
	set_qregs_wrr_mode_by_byte(&wrr, true);
	econet_wreg(wrr, &qdma->regs->wrr_mode);

	/* The normal dynamic congestion profile uses the TX rate meter. */
	val = EN751221_TX_RATE_METER_EN |
	      FIELD_PREP(EN751221_TX_RATE_METER_DIV_MASK, 2) |
	      FIELD_PREP(EN751221_TX_RATE_METER_SLICE_MASK, 4000);
	econet_wreg(val, &qdma->regs->tx_meter_cfg);

	/*
	 * Start from qdma_set_txq_cngst_auto_config() so reduced fallback
	 * pools remain usable.  The production firmware retunes the total
	 * threshold after this initial auto profile, however, and the final
	 * values are materially larger than CONFIG_HWFWD_DSCP_NUM / 5.
	 */
	if (qdma->num_fwd_descs <= 256) {
		total_min = 48;
		channel_min = 2;
		queue_min = 2;
	} else {
		total_min = qdma->num_fwd_descs / 5;
		channel_min = qdma->num_fwd_descs / 14;
		queue_min = qdma->num_fwd_descs / 170;
	}

	physical_size = qdma->num_fwd_descs * qdma->fwd_buf_size;
	total_max = (physical_size - (physical_size >> 4)) >> 8;
	channel_max = total_max;
	queue_max = total_min;

	/* Final steady-state values captured from the shipping EN751221
	 * firmware.  Keep the auto profile for reduced/no-reserved-memory
	 * fallback pools, where these full-size thresholds would be invalid.
	 */
	if (qdma->fwd_buf_size == 2048 && qdma->common.id == 0 &&
	    qdma->num_fwd_descs == 1024) {
		total_min = 5120;
		total_max = 7680;
		channel_max = 7680;
		queue_max = 204;
	} else if (qdma->fwd_buf_size == 2048 && qdma->common.id == 1 &&
		   qdma->num_fwd_descs == 4096) {
		total_min = 2048;
		total_max = 15360;
		channel_max = 15360;
		queue_max = 819;
	}

	airoha_wr((void __iomem *)&qdma->regs->tx_congest_thr, 0,
		  FIELD_PREP(EN751221_TXQ_MAX_THR_MASK, total_max) |
		  FIELD_PREP(EN751221_TXQ_MIN_THR_MASK, total_min));
	econet_wreg((u32)(FIELD_PREP(EN751221_TXQ_MAX_THR_MASK, channel_max) |
		     FIELD_PREP(EN751221_TXQ_MIN_THR_MASK, channel_min)),
		     &qdma->regs->tx_per_ch_dthr);
	econet_wreg((u32)(FIELD_PREP(EN751221_TXQ_MAX_THR_MASK, queue_max) |
		     FIELD_PREP(EN751221_TXQ_MIN_THR_MASK, queue_min)),
		     &qdma->regs->tx_per_q_dthr);

	/*
	 * Match the final production dynamic-congestion state: normal and DEI
	 * drops enabled, all three update triggers, 250 us tick and a 1/2 DEI
	 * threshold. The four TX-ring blocking bits stay clear.
	 */
	cngst_cfg = econet_rreg(&qdma->regs->tx_congest_cfg);
	set_qregs_tx_congest_cfg_tail_drop_en(&cngst_cfg, true);
	set_qregs_tx_congest_cfg_dei_drop_en(&cngst_cfg, true);
	set_qregs_tx_congest_cfg_dyncong_en(&cngst_cfg, true);
	set_qregs_tx_congest_cfg_max_thr_blk_tx1(&cngst_cfg, false);
	set_qregs_tx_congest_cfg_min_thr_blk_tx1(&cngst_cfg, false);
	set_qregs_tx_congest_cfg_max_thr_blk_tx0(&cngst_cfg, false);
	set_qregs_tx_congest_cfg_min_thr_blk_tx0(&cngst_cfg, false);
	set_qregs_tx_congest_cfg_dyncong_dei_scale(&cngst_cfg,
						   QREGS_TX_CONGEST_CFG_DYNCONG_DEI_SCALE_HALF);
	set_qregs_tx_congest_cfg_dyncong_upd_wrr(&cngst_cfg, true);
	set_qregs_tx_congest_cfg_dyncong_upd_txrx(&cngst_cfg, true);
	set_qregs_tx_congest_cfg_dyncong_upd_tick(&cngst_cfg, true);
	cngst_cfg.dyncong_tick = 250;
	econet_wreg(cngst_cfg, &qdma->regs->tx_congest_cfg);
}

static int econet_init_final(struct econet_qdma *qdma)
{
	struct qregs_qcfg qcfg;
	int i;

	for (i = 0; i < ARRAY_SIZE(qdma->irqs); i++) {
		int j;

		/* clear pending irqs */
		for (j = 0; j < ARRAY_SIZE(qdma->irqs[i].status_reg); j++)
			econet_wreg(0xffffffffU, qdma->irqs[i].status_reg[j]);

		/* enable IRQs */
		for (j = 0; j < ARRAY_SIZE(qdma->irqs[i].irqmask); j++) {
			u32 mask = 0;

			for (int k = 0; k < 32; k++) {
				union econet_irq_purpose p;
				union irq_bit bit = {
					.irq_idx = i,
					.reg_idx = j,
					.bit_idx = k,
				};
				u32 en = 0;

				if (!en751221_valid_irq_bit(bit))
					continue;

				p = en751221_irq_purpose(bit);

				if (airoha_is(qdma->common.eth, econet_en751221)) {
					/*
					 * Match qdma_dev.c's production mask. HWF
					 * EMPTY/LOW are status/debug conditions, not
					 * runtime interrupts; keeping them enabled while
					 * PPE consumes LMGR descriptors can cause a hard
					 * IRQ storm. Bit 9 is the vendor IRQ_FULL event,
					 * represented as NO_DSCP/DONE in our map.
					 */
					en |= p.type == IPS_DONE;
					en |= p.type == IPS_NO_DSCP &&
					      (p.source == IPSC_RX ||
					       p.source == IPSC_DONE);
					en |= p.type == IPS_ERR_COHERENT;
					en |= p.type == IPS_OVERFLOW;
				} else {
					/* RX and TX done */
					en |= (p.type == IPS_DONE);

					/* Running out of resources */
					en |= (p.type == IPS_NO_DSCP &&
					       p.source != IPSC_TX);
					en |= (p.type == IPS_LOW_DSCP &&
					       p.source != IPSC_TX);

					/* Error conditions */
					en |= (p.type == IPS_ERR_COHERENT);
					en |= (p.type == IPS_OVERFLOW);

					/* External hardware */
					en |= (p.type == IPS_GPON_INT);
					en |= (p.type == IPS_EPON_INT);
					en |= (p.type == IPS_XPON_INT);
				}

				mask |= en << k;
			}

			qdma->irqs[i].irqmask[j] = mask;
			econet_wreg(mask, qdma->irqs[i].mask_reg[j]);
		}
	}

	qcfg = (struct qregs_qcfg) { 0 };
	set_qregs_qcfg_msg_word_swap(&qcfg, true);
	set_qregs_qcfg_dscp_byte_swap(&qcfg, airoha_is(qdma->common.eth, econet_en751221));
	set_qregs_qcfg_payload_byte_sw(&qcfg, true);
	set_qregs_qcfg_dma_pref(&qcfg, QREGS_QCFG_DMA_PREF_TX1_FRX_TX0);
	set_qregs_qcfg_rx_2b_offset(&qcfg, qdma->cfg.rx_2b_offset);
	set_qregs_qcfg_irq_en(&qcfg, true);
	set_qregs_qcfg_check_done(&qcfg, !airoha_is(qdma->common.eth, econet_en751221));
	set_qregs_qcfg_tx_wb_done(&qcfg, true);
	if (airoha_is(qdma->common.eth, econet_en751221) && qdma->common.id == 0)
		set_qregs_qcfg_tx_immediate_done(&qcfg, true);
	set_qregs_qcfg_burst_size(&qcfg, QREGS_QCFG_BURST_SIZE_128_BYTES);
	econet_wreg(qcfg, &qdma->regs->qdma_cfg);

	econet_wreg(0U, &qdma->regs->rx_int_delay);

	if (airoha_is(qdma->common.eth, econet_en751221)) {
		econet_init_en751221_qdma(qdma);
	} else {
		struct qregs_tx_congest_cfg cngst_cfg = { 0 };

		set_qregs_tx_congest_cfg_tail_drop_en(&cngst_cfg, true);
		set_qregs_tx_congest_cfg_dei_drop_en(&cngst_cfg, true);
		econet_wreg(cngst_cfg, &qdma->regs->tx_congest_cfg);
	}

	/*
	 * The vendor EN751221 LAN driver disables per-channel QDMA TX rate
	 * limiting for channels 0..4 when the external MT7530 is used.  The
	 * firmware/bootloader may leave these bits enabled, so explicitly clear
	 * them instead of inheriting a stale hardware shaper configuration.
	 */
	{
		u32 ch_lim_en = econet_rreg(&qdma->regs->ch_lim_en);

		ch_lim_en &= ~GENMASK(4, 0);
		econet_wreg(ch_lim_en, &qdma->regs->ch_lim_en);
	}

	return 0;
}

static int econet_init(struct device *dev,
		     struct econet_qdma *qdma,
		     int *irqs,
		     int num_irqs)
{
	int err;
	int i;

	/* Make sure RX and TX are shutdown */
	econet_wreg((struct qregs_qcfg) { 0 }, &qdma->regs->qdma_cfg);

	err = econet_init_irqs(dev, qdma, irqs, num_irqs);
	if (err)
		return err;

	for (i = 0; i < ARRAY_SIZE(qdma->q_rx); i++) {
		err = econet_init_rx_queue(&qdma->q_rx[i], qdma,
					 qdma->cfg.num_rx_descs[i]);
		if (err)
			return err;
	}

	for (i = 0; i < ARRAY_SIZE(qdma->q_tx_done); i++) {
		err = econet_init_tx_doneq(&qdma->q_tx_done[i], qdma,
					 qdma->cfg.done_list_size[i],
					 qdma->cfg.done_list_irq_threshold[i]);
		if (err)
			return err;
	}

	for (i = 0; i < ARRAY_SIZE(qdma->q_tx); i++) {
		err = econet_init_tx_queue(&qdma->q_tx[i], qdma,
					 qdma->cfg.num_tx_descs[i]);
		if (err)
			return err;
	}

	err = econet_init_hw_fwd(qdma);
	if (err)
		return err;

	return econet_init_final(qdma);
}

/* End init functions */

static void airoha_qdma_gen1_destroy_rxq_locked(struct econet_q_rx *q)
{
	int i;

	if (!q->page_pool)
		return;

	for (i = 0; i < q->ndesc; i++) {
		struct econet_q_rx_ent *e = &q->entry[i];
		struct page *page;

		if (!e->dma_addr)
			continue;

		page = virt_to_head_page(e->buf);
		dma_sync_single_for_cpu(q->qdma->dev, e->dma_addr, e->dma_len,
					page_pool_get_dma_dir(q->page_pool));
		page_pool_put_full_page(q->page_pool, page, false);
		e->dma_addr = 0;
		e->buf = NULL;
	}

	page_pool_destroy(q->page_pool);
	q->page_pool = NULL;
}

static int airoha_qdma_gen1_destroy_locked(struct econet_qdma *qdma)
{
	struct qregs_qcfg qcfg;
	int i;

	if (WARN_ON_ONCE(qdma->users))
		return -EBUSY;

	qcfg = econet_rreg(&qdma->regs->qdma_cfg);
	set_qregs_qcfg_irq_en(&qcfg, false);
	set_qregs_qcfg_rx_dma_en(&qcfg, false);
	set_qregs_qcfg_tx_dma_en(&qcfg, false);
	econet_wreg(qcfg, &qdma->regs->qdma_cfg);

	for (i = 0; i < ARRAY_SIZE(qdma->irqs); i++) {
		struct econet_irq *irq = &qdma->irqs[i];
		int j;

		for (j = 0; j < ARRAY_SIZE(irq->irqmask); j++) {
			irq->irqmask[j] = 0;
			if (irq->mask_reg[j])
				econet_wreg(0U, irq->mask_reg[j]);
		}

		if (irq->irq > 0)
			synchronize_irq(irq->irq);
	}

	for (i = 0; i < ARRAY_SIZE(qdma->q_rx); i++) {
		struct econet_q_rx *q = &qdma->q_rx[i];

		airoha_qdma_gen1_destroy_rxq_locked(q);
		if (q->napi.dev)
			netif_napi_del(&q->napi);
	}

	for (i = 0; i < ARRAY_SIZE(qdma->q_tx_done); i++) {
		struct econet_tx_doneq *q = &qdma->q_tx_done[i];

		if (q->napi.dev)
			netif_napi_del(&q->napi);
	}

	if (qdma->napi_dev) {
		free_netdev(qdma->napi_dev);
		qdma->napi_dev = NULL;
	}

	return 0;
}

int airoha_qdma_gen1_destroy(struct econet_qdma *qdma)
{
	guard(mutex)(&qdma->lock);

	return airoha_qdma_gen1_destroy_locked(qdma);
}

int airoha_qdma_gen1_use(struct econet_qdma *qdma)
{
	struct qregs_qcfg qcfg;
	int i;

	guard(mutex)(&qdma->lock);

	if (qdma->users++ > 0)
		return 0;

	for (i = 0; i < ARRAY_SIZE(qdma->q_rx); i++)
		napi_enable(&qdma->q_rx[i].napi);

	for (i = 0; i < ARRAY_SIZE(qdma->q_tx_done); i++)
		napi_enable(&qdma->q_tx_done[i].napi);

	/* NAPI must be ready before enabling a DMA engine that can raise IRQs. */
	qcfg = econet_rreg(&qdma->regs->qdma_cfg);
	set_qregs_qcfg_rx_dma_en(&qcfg, true);
	set_qregs_qcfg_tx_dma_en(&qcfg, true);
	econet_wreg(qcfg, &qdma->regs->qdma_cfg);

	return 0;
}

int airoha_qdma_gen1_unuse(struct econet_qdma *qdma)
{
	struct qregs_qcfg qcfg;
	int i, j;

	guard(mutex)(&qdma->lock);

	if (WARN_ON_ONCE(qdma->users <= 0))
		return -EINVAL;
	if (--qdma->users > 0)
		return 0;

	qcfg = econet_rreg(&qdma->regs->qdma_cfg);
	set_qregs_qcfg_rx_dma_en(&qcfg, false);
	set_qregs_qcfg_tx_dma_en(&qcfg, false);
	econet_wreg(qcfg, &qdma->regs->qdma_cfg);

	for (i = 0; i < ARRAY_SIZE(qdma->q_rx); i++)
		napi_disable(&qdma->q_rx[i].napi);

	for (i = 0; i < ARRAY_SIZE(qdma->q_tx_done); i++)
		napi_disable(&qdma->q_tx_done[i].napi);

	for (i = 0; i < ARRAY_SIZE(qdma->q_tx); i++) {
		struct econet_q_tx *q = &qdma->q_tx[i];

		guard(spinlock_bh)(&q->lock_bh);
		for (j = 0; j < q->ndesc; j++) {
			struct econet_q_tx_ent *e = &q->entry[j];

			/* In the free list already */
			if (!e->dma_addr)
				continue;

			dma_unmap_single(qdma->dev, e->dma_addr, e->dma_len,
					DMA_TO_DEVICE);
			dev_kfree_skb_any(e->skb);
			memset(e, 0, sizeof(*e));

			e->freelist_next = 0xffff;
			q->entry[q->freelist_tail].freelist_next = j;
			q->freelist_tail = j;
		}
	}

	return 0;
}

struct airoha_qdma_common *airoha_qdma_gen1_common(struct econet_qdma *qdma)
{
	return &qdma->common;
}

struct econet_qdma *airoha_qdma_gen1_new(struct airoha_eth *eth,
				void __iomem *qdma_regs,
				int id,
				int *irqs,
				int num_irqs,
				struct econet_qdma_cfg *cfg)
{
	struct econet_qdma *qdma;
	int err;

	qdma = devm_kzalloc(eth->dev, sizeof(*qdma), GFP_KERNEL);
	if (!qdma)
		return ERR_PTR(-ENOMEM);

	qdma->dev = eth->dev;
	airoha_qdma_common_init(&qdma->common, eth, qdma_regs, id);
	qdma->common.num_channels = cfg->num_channels;
	mutex_init(&qdma->lock);
	qdma->regs = qdma_regs;
	memcpy(&qdma->cfg, cfg, sizeof(*cfg));
	qdma->soc = cfg->soc;

	{
		char name[IFNAMSIZ];

		snprintf(name, sizeof(name), "qdma%d_eth", id);
		qdma->napi_dev = airoha_eth_alloc_napi_dev(name);
	}
	if (!qdma->napi_dev)
		return ERR_PTR(-ENOMEM);

	err = econet_init(eth->dev, qdma, irqs, num_irqs);
	if (err) {
		airoha_qdma_gen1_destroy(qdma);
		return ERR_PTR(err);
	}

	return qdma;
}

/* Generation-2 QDMA backend. */
static void airoha_qdma_set_irqmask(struct airoha_irq_bank *irq_bank,
				    int index, u32 clear, u32 set)
{
	struct airoha_qdma *qdma = irq_bank->qdma;
	int bank = irq_bank - &qdma->irq_banks[0];
	unsigned long flags;

	if (WARN_ON_ONCE(index >= ARRAY_SIZE(irq_bank->irqmask)))
		return;

	spin_lock_irqsave(&irq_bank->irq_lock, flags);

	irq_bank->irqmask[index] &= ~clear;
	irq_bank->irqmask[index] |= set;
	airoha_qdma_wr(qdma, REG_INT_ENABLE(bank, index),
		       irq_bank->irqmask[index]);
	/* Read irq_enable register in order to guarantee the update above
	 * completes in the spinlock critical section.
	 */
	airoha_qdma_rr(qdma, REG_INT_ENABLE(bank, index));

	spin_unlock_irqrestore(&irq_bank->irq_lock, flags);
}

static void airoha_qdma_irq_enable(struct airoha_irq_bank *irq_bank,
				   int index, u32 mask)
{
	airoha_qdma_set_irqmask(irq_bank, index, 0, mask);
}

static void airoha_qdma_irq_disable(struct airoha_irq_bank *irq_bank,
				    int index, u32 mask)
{
	airoha_qdma_set_irqmask(irq_bank, index, mask, 0);
}

static int airoha_qdma_fill_rx_queue(struct airoha_queue *q)
{
	struct airoha_qdma *qdma = q->qdma;
	int qid = q - &qdma->q_rx[0];
	int nframes = 0;

	while (q->queued < q->ndesc - 1) {
		struct airoha_queue_entry *e = &q->entry[q->head];
		struct airoha_qdma_desc *desc = &q->desc[q->head];
		struct page *page;
		int offset;
		u32 val;

		page = page_pool_dev_alloc_frag(q->page_pool, &offset,
						q->buf_size);
		if (!page)
			break;

		q->head = (q->head + 1) % q->ndesc;
		q->queued++;
		nframes++;

		e->buf = page_address(page) + offset;
		e->dma_addr = page_pool_get_dma_addr(page) + offset;
		e->dma_len = SKB_WITH_OVERHEAD(q->buf_size);

		WRITE_ONCE(desc->tcp_ts_reply, 0);
		val = airoha_is(qdma->common.eth, airoha_en7523) ?
			FIELD_PREP(EN7523_QDMA_DESC_LEN_MASK, e->dma_len) :
			FIELD_PREP(QDMA_DESC_LEN_MASK, e->dma_len);
		WRITE_ONCE(desc->ctrl, cpu_to_le32(val));
		WRITE_ONCE(desc->addr, cpu_to_le32(e->dma_addr));
		val = FIELD_PREP(QDMA_DESC_NEXT_ID_MASK, q->head);
		WRITE_ONCE(desc->data, cpu_to_le32(val));
		WRITE_ONCE(desc->msg0, 0);
		WRITE_ONCE(desc->msg1, 0);
		WRITE_ONCE(desc->msg2, 0);
		WRITE_ONCE(desc->msg3, 0);
	}

	if (nframes) {
		/* Publish descriptor contents before handing ownership to DMA. */
		dma_wmb();
		airoha_qdma_rmw(qdma, REG_RX_CPU_IDX(qid),
				RX_RING_CPU_IDX_MASK,
				FIELD_PREP(RX_RING_CPU_IDX_MASK, q->head));
	}

	return nframes;
}

static struct airoha_gdm_dev *
airoha_qdma_get_xpon_dev(struct airoha_eth *eth)
{
	int i, j;

	for (i = 0; i < eth->soc->max_gdm_ports; i++) {
		struct airoha_gdm_port *port = eth->ports[i];

		if (!port || port->id != AIROHA_GDM2_IDX)
			continue;

		for (j = 0; j < ARRAY_SIZE(port->devs); j++) {
			struct airoha_gdm_dev *dev = port->devs[j];

			if (dev &&
			    (dev->flags & AIROHA_PRIV_F_XPON_MANAGED))
				return dev;
		}
	}

	return ERR_PTR(-ENODEV);
}

static struct airoha_gdm_dev *
airoha_qdma_get_gdm_dev(struct airoha_eth *eth, struct airoha_qdma_desc *desc)
{
	struct airoha_gdm_port *port;
	u16 p, d;

	if (eth->soc->ops.get_dev_from_sport(desc, &p, &d))
		return ERR_PTR(-ENODEV);

	if (p >= eth->soc->max_gdm_ports)
		return ERR_PTR(-ENODEV);

	port = eth->ports[p];
	if (!port)
		return ERR_PTR(-ENODEV);

	if (d >= ARRAY_SIZE(port->devs))
		return ERR_PTR(-ENODEV);

	return port->devs[d] ? port->devs[d] : ERR_PTR(-ENODEV);
}

static struct sk_buff *airoha_qdma_lro_rx_skb(struct airoha_queue *q,
					      struct airoha_qdma_desc *desc,
					      struct airoha_queue_entry *e)
{
	u32 len, th_off, tcp_ack_seq, agg_count, data_off, data_len;
	u32 desc_ctrl = le32_to_cpu(READ_ONCE(desc->ctrl));
	u32 msg1 = le32_to_cpu(READ_ONCE(desc->msg1));
	u32 msg2 = le32_to_cpu(READ_ONCE(desc->msg2));
	u32 msg3 = le32_to_cpu(READ_ONCE(desc->msg3));
	struct skb_shared_info *shinfo;
	u16 tcp_win, l2_len;
	struct sk_buff *skb;
	struct tcphdr *th;
	struct page *page;
	bool ipv4, ipv6;

	switch (q->qdma->common.eth->soc->version) {
	case econet_en751221:
	case econet_en7528:
		return NULL;
	case airoha_en7523:
		ipv4 = FIELD_GET(EN7523_QDMA_ETH_RXMSG_IP4_MASK, msg1);
		ipv6 = FIELD_GET(EN7523_QDMA_ETH_RXMSG_IP6_MASK, msg1);
		break;
	case airoha_en7581:
	case airoha_an7583:
		ipv4 = FIELD_GET(QDMA_ETH_RXMSG_IP4_MASK, msg1);
		ipv6 = FIELD_GET(QDMA_ETH_RXMSG_IP6_MASK, msg1);
		break;
	}
	if (!ipv4 && !ipv6)
		return NULL;

	l2_len = FIELD_GET(QDMA_ETH_RXMSG_L2_LEN_MASK, msg2);
	len = FIELD_GET(QDMA_DESC_LEN_MASK, desc_ctrl);

	if (ipv4) {
		struct iphdr *iph;

		if (len < l2_len + sizeof(*iph))
			return NULL;

		iph = (struct iphdr *)(e->buf + l2_len);
		if (iph->protocol != IPPROTO_TCP)
			return NULL;

		if (iph->ihl < 5)
			return NULL;

		th_off = l2_len + (iph->ihl << 2);
		if (len < th_off)
			return NULL;

		iph->tot_len = cpu_to_be16(len - l2_len);
		iph->check = 0;
		iph->check = ip_fast_csum((void *)iph, iph->ihl);
	} else {
		struct ipv6hdr *ip6h;

		th_off = l2_len + sizeof(*ip6h);
		if (len < th_off)
			return NULL;

		ip6h = (struct ipv6hdr *)(e->buf + l2_len);
		if (ip6h->nexthdr != NEXTHDR_TCP)
			return NULL;

		ip6h->payload_len = cpu_to_be16(len - th_off);
	}

	if (len < th_off + sizeof(*th))
		return NULL;

	th = (struct tcphdr *)(e->buf + th_off);
	if (th->doff < 5)
		return NULL;

	data_off = th_off + (th->doff << 2);
	if (len < data_off)
		return NULL;

	tcp_win = FIELD_GET(QDMA_ETH_RXMSG_TCP_WIN_MASK, msg3);
	tcp_ack_seq = le32_to_cpu(READ_ONCE(desc->data));
	th->ack_seq = cpu_to_be32(tcp_ack_seq);
	th->window = cpu_to_be16(tcp_win);

	/* Check tcp timestamp option */
	if (th->doff == (sizeof(*th) + TCPOLEN_TSTAMP_ALIGNED) / 4) {
		u32 topt = get_unaligned_be32(th + 1);

		if (topt == ((TCPOPT_NOP << 24) | (TCPOPT_NOP << 16) |
			     (TCPOPT_TIMESTAMP << 8) | TCPOLEN_TIMESTAMP)) {
			u8 *ptr = (u8 *)th + sizeof(*th) + 2 * sizeof(__be32);
			__le32 tcp_ts_reply = READ_ONCE(desc->tcp_ts_reply);

			put_unaligned_be32(le32_to_cpu(tcp_ts_reply), ptr);
		}
	}

	if (ipv4) {
		struct iphdr *iph = (struct iphdr *)(e->buf + l2_len);

		th->check = ~tcp_v4_check(len - th_off, iph->saddr,
					  iph->daddr, 0);
	} else {
		struct ipv6hdr *ip6h = (struct ipv6hdr *)(e->buf + l2_len);

		th->check = ~tcp_v6_check(len - th_off, &ip6h->saddr,
					  &ip6h->daddr, 0);
	}

	skb = napi_alloc_skb(&q->napi, data_off);
	if (!skb)
		return NULL;

	__skb_put(skb, data_off);
	memcpy(skb->data, e->buf, data_off);

	page = virt_to_head_page(e->buf);
	data_len = len - data_off;
	shinfo = skb_shinfo(skb);
	skb_add_rx_frag(skb, shinfo->nr_frags, page,
			e->buf + data_off - page_address(page), data_len,
			q->buf_size);

	shinfo->gso_type = ipv4 ? SKB_GSO_TCPV4 : SKB_GSO_TCPV6;
	agg_count = FIELD_GET(QDMA_ETH_RXMSG_AGG_COUNT_MASK, msg2);
	shinfo->gso_size = DIV_ROUND_UP(data_len, agg_count);
	shinfo->gso_segs = agg_count;

	skb->csum_start = skb_headroom(skb) + th_off;
	skb->csum_offset = offsetof(struct tcphdr, check);
	skb->ip_summed = CHECKSUM_PARTIAL;

	return skb;
}

static bool airoha_qdma_rx_checksum_ok(struct airoha_eth *eth,
				       struct airoha_qdma_desc *desc)
{
	u32 msg1 = le32_to_cpu(READ_ONCE(desc->msg1));
	u32 ip4, ip6, ip4_fault, l4_valid, l4_fault;

	if (airoha_is(eth, airoha_en7523)) {
		ip4 = msg1 & EN7523_QDMA_ETH_RXMSG_IP4_MASK;
		ip6 = msg1 & EN7523_QDMA_ETH_RXMSG_IP6_MASK;
		ip4_fault = msg1 & EN7523_QDMA_ETH_RXMSG_IP4F_MASK;
		l4_valid = msg1 & EN7523_QDMA_ETH_RXMSG_L4_VALID_MASK;
		l4_fault = msg1 & EN7523_QDMA_ETH_RXMSG_L4F_MASK;
	} else {
		ip4 = msg1 & QDMA_ETH_RXMSG_IP4_MASK;
		ip6 = msg1 & QDMA_ETH_RXMSG_IP6_MASK;
		ip4_fault = msg1 & QDMA_ETH_RXMSG_IP4F_MASK;
		l4_valid = msg1 & QDMA_ETH_RXMSG_L4_VALID_MASK;
		l4_fault = msg1 & QDMA_ETH_RXMSG_L4F_MASK;
	}

	return (ip4 || ip6) && l4_valid && !ip4_fault && !l4_fault;
}

static struct sk_buff *airoha_qdma_build_rx_skb(struct airoha_queue *q,
						struct airoha_qdma_desc *desc,
						struct airoha_queue_entry *e,
						struct net_device *netdev,
						bool raw)
{
	u32 msg2 = le32_to_cpu(READ_ONCE(desc->msg2));
	int qid = q - &q->qdma->q_rx[0];
	struct sk_buff *skb;

	if (!raw && FIELD_GET(QDMA_ETH_RXMSG_AGG_COUNT_MASK, msg2) > 1) { /* LRO */
		skb = airoha_qdma_lro_rx_skb(q, desc, e);
		if (!skb)
			return NULL;
	} else {
		u32 desc_ctrl = le32_to_cpu(READ_ONCE(desc->ctrl));
		u32 len = FIELD_GET(QDMA_DESC_LEN_MASK, desc_ctrl);

		skb = napi_build_skb(e->buf, q->buf_size);
		if (!skb)
			return NULL;

		__skb_put(skb, len);
		if ((netdev->features & NETIF_F_RXCSUM) &&
		    airoha_qdma_rx_checksum_ok(q->qdma->common.eth, desc))
			skb->ip_summed = CHECKSUM_UNNECESSARY;
		else
			skb->ip_summed = CHECKSUM_NONE;
	}

	skb_mark_for_recycle(skb);
	skb->dev = netdev;
	skb_record_rx_queue(skb, qid);
	if (raw) {
		skb_reset_mac_header(skb);
		skb->protocol = 0;
	} else {
		skb->protocol = eth_type_trans(skb, netdev);
	}

	return skb;
}

#define AIROHA_XPON_DATA_DUMP_LEN	128

static bool airoha_qdma_foe_entry_is_valid(struct airoha_eth *eth, u32 hash)
{
	return airoha_is(eth, airoha_en7523) ?
	       hash != EN7523_AIROHA_RXD4_FOE_ENTRY_INVALID :
	       hash != AN7581_AIROHA_RXD4_FOE_ENTRY_INVALID;
}

static bool airoha_qdma_should_check_ppe_skb(struct airoha_eth *eth,
					     u32 reason)
{
	if (reason == AIROHA_PPE_CPU_REASON_HIT_UNBIND_RATE_REACHED)
		return true;

	if (!airoha_is(eth, airoha_en7523))
		return false;

	return reason == AIROHA_PPE_CPU_REASON_HIT_UNBIND ||
	       reason == AIROHA_PPE_CPU_REASON_FOE_UNHIT;
}

static int airoha_qdma_rx_process(struct airoha_queue *q, int budget)
{
	enum dma_data_direction dir = page_pool_get_dma_dir(q->page_pool);
	struct airoha_eth *eth = q->qdma->common.eth;
	int done = 0;

	while (done < budget) {
		struct airoha_queue_entry *e = &q->entry[q->tail];
		struct airoha_qdma_desc *desc = &q->desc[q->tail];
		u32 hash, reason, msg0, msg1, desc_ctrl;
		struct airoha_gdm_dev *dev;
		struct net_device *netdev;
		bool xpon_oam;
		int data_len, len;
		struct page *page;

		desc_ctrl = le32_to_cpu(READ_ONCE(desc->ctrl));
		if (!(desc_ctrl & QDMA_DESC_DONE_MASK))
			break;

		dma_rmb();

		q->tail = (q->tail + 1) % q->ndesc;
		q->queued--;

		dma_sync_single_for_cpu(eth->dev, e->dma_addr,
					SKB_WITH_OVERHEAD(q->buf_size), dir);

		page = virt_to_head_page(e->buf);
		len = airoha_is(eth, airoha_en7523) ?
			FIELD_GET(EN7523_QDMA_DESC_LEN_MASK, desc_ctrl) :
			FIELD_GET(QDMA_DESC_LEN_MASK, desc_ctrl);
		data_len = q->skb ? q->buf_size
				  : SKB_WITH_OVERHEAD(q->buf_size);
		if (!len || data_len < len)
			goto free_frag;

		msg0 = le32_to_cpu(READ_ONCE(desc->msg0));
		if (airoha_is(eth, airoha_en7523) &&
		    q - &q->qdma->q_rx[0] == 15) {
			u32 msg2 = le32_to_cpu(READ_ONCE(desc->msg2));
			u32 msg3 = le32_to_cpu(READ_ONCE(desc->msg3));
			unsigned int channel, gem_port_id;

			msg1 = le32_to_cpu(READ_ONCE(desc->msg1));
			channel = FIELD_GET(EN7523_QDMA_ETH_RXMSG_CHAN_MASK, msg0);
			gem_port_id = FIELD_GET(EN7523_QDMA_ETH_RXMSG_GEM_MASK, msg0);
			dev_dbg_ratelimited(eth->dev,
					    "QDMA RX15 descriptor: len=%d ctrl=%#010x msg=%#010x/%#010x/%#010x/%#010x oam=%u channel=%u gem=%u no-mic=%u\n",
					     len, desc_ctrl, msg0, msg1, msg2, msg3,
					     !!(msg0 & EN7523_QDMA_ETH_RXMSG_OAM_MASK),
					     channel, gem_port_id,
					     !!(msg0 & EN7523_QDMA_ETH_RXMSG_NO_MIC_MASK));
		}
		xpon_oam = airoha_is(eth, airoha_en7523) &&
			   (msg0 & EN7523_QDMA_ETH_RXMSG_OAM_MASK);

		/* GPON OAM descriptors are not required to carry an Ethernet
		 * source port. Resolve them through the registered xPON GDM2
		 * provider before applying the normal source-port decoder.
		 */
		dev = xpon_oam ? airoha_qdma_get_xpon_dev(eth) :
				 airoha_qdma_get_gdm_dev(eth, desc);
		if (IS_ERR(dev)) {
			if (xpon_oam)
				dev_warn_ratelimited(eth->dev,
						     "xPON OAM RX dropped: no managed GDM2 ring=%td len=%d msg0=%#010x msg1=%#010x\n",
					q - &q->qdma->q_rx[0], len, msg0,
					le32_to_cpu(READ_ONCE(desc->msg1)));
			goto free_frag;
		}

		netdev = netdev_from_priv(dev);
		if (!q->skb) { /* first buffer */
			q->skb = airoha_qdma_build_rx_skb(q, desc, e, netdev, xpon_oam);
			if (!q->skb)
				goto free_frag;
		} else { /* scattered frame */
			struct skb_shared_info *shinfo = skb_shinfo(q->skb);
			int nr_frags = shinfo->nr_frags;

			if (nr_frags >= ARRAY_SIZE(shinfo->frags))
				goto free_frag;

			skb_add_rx_frag(q->skb, nr_frags, page,
					e->buf - page_address(page), len,
					q->buf_size);
		}

		if (FIELD_GET(QDMA_DESC_MORE_MASK, desc_ctrl))
			continue;

		if (airoha_is(eth, airoha_en7523)) {
			struct airoha_xpon_oam_handler *handler;

			if (xpon_oam) {
				u32 msg2 = le32_to_cpu(READ_ONCE(desc->msg2));
				u32 msg3 = le32_to_cpu(READ_ONCE(desc->msg3));
				u16 gem_port_id, channel, sport;
				u32 flags = 0, skb_len;
				bool consumed = false;

				gem_port_id = FIELD_GET(EN7523_QDMA_ETH_RXMSG_GEM_MASK,
							msg0);
				channel = FIELD_GET(EN7523_QDMA_ETH_RXMSG_CHAN_MASK,
						    msg0);
				msg1 = le32_to_cpu(READ_ONCE(desc->msg1));
				sport = FIELD_GET(EN7523_QDMA_ETH_RXMSG_SPORT_MASK,
						  msg1);
				if (!(msg0 & EN7523_QDMA_ETH_RXMSG_NO_MIC_MASK))
					flags |= AIROHA_XPON_OAM_RX_F_MIC_PRESENT;
				if (!(msg0 & EN7523_QDMA_ETH_RXMSG_CRC_ERR_MASK))
					flags |= AIROHA_XPON_OAM_RX_F_MIC_VALID;
				else
					flags |= AIROHA_XPON_OAM_RX_F_CRC_ERROR;

				skb_len = q->skb->len;
				atomic64_inc(&dev->xpon_oam_rx_packets);
				atomic64_add(skb_len, &dev->xpon_oam_rx_bytes);

				rcu_read_lock();
				handler = rcu_dereference(dev->xpon_oam);
				if (handler && handler->rx)
					consumed = handler->rx(handler->priv,
							       q->skb,
							       gem_port_id,
							       flags);
				else
					atomic64_inc(&dev->xpon_oam_rx_no_handler);
				rcu_read_unlock();

				if (consumed) {
					atomic64_inc(&dev->xpon_oam_rx_delivered);
				} else {
					atomic64_inc(&dev->xpon_oam_rx_dropped);
					dev_kfree_skb_any(q->skb);
				}

				dev_dbg_ratelimited(&netdev->dev,
						    "xPON OAM RX: ring=%td len=%u msg=%#010x/%#010x/%#010x/%#010x sport=%u channel=%u gem=%u no-mic=%u crc=%u runt=%u long=%u consumed=%u totals=%lld/%lld/%lld\n",
					q - &q->qdma->q_rx[0], skb_len,
					msg0, msg1, msg2, msg3, sport, channel,
					gem_port_id,
					!!(msg0 & EN7523_QDMA_ETH_RXMSG_NO_MIC_MASK),
					!!(msg0 & EN7523_QDMA_ETH_RXMSG_CRC_ERR_MASK),
					!!(msg0 & EN7523_QDMA_ETH_RXMSG_RUNT_MASK),
					!!(msg0 & EN7523_QDMA_ETH_RXMSG_LONG_MASK),
					consumed,
					(long long)atomic64_read(&dev->xpon_oam_rx_packets),
					(long long)atomic64_read(&dev->xpon_oam_rx_delivered),
					(long long)atomic64_read(&dev->xpon_oam_rx_dropped));

				q->skb = NULL;
				done++;
				continue;
			}
		}

		if (netdev_uses_dsa(netdev)) {
			struct airoha_gdm_port *port = dev->port;

			/* PPE module requires untagged packets to work
			 * properly and it provides DSA port index via the
			 * DMA descriptor. Report DSA tag to the DSA stack
			 * via skb dst info.
			 */
			u32 msg0 = le32_to_cpu(READ_ONCE(desc->msg0));
			u32 sptag = FIELD_GET(QDMA_ETH_RXMSG_SPTAG, msg0);

			if (sptag < ARRAY_SIZE(port->dsa_meta) &&
			    port->dsa_meta[sptag])
				skb_dst_set_noref(q->skb,
						  &port->dsa_meta[sptag]->dst);
		}

		msg1 = le32_to_cpu(READ_ONCE(desc->msg1));
		hash = airoha_is(eth, airoha_en7523) ?
		       FIELD_GET(EN7523_QDMA_ETH_RXMSG_PPE_ENTRY_MASK, msg1) :
		       FIELD_GET(AIROHA_RXD4_FOE_ENTRY, msg1);
		reason = airoha_is(eth, airoha_en7523) ?
			 FIELD_GET(EN7523_QDMA_ETH_RXMSG_CRSN_MASK, msg1) :
			 FIELD_GET(AIROHA_RXD4_PPE_CPU_REASON, msg1);
		if (airoha_qdma_foe_entry_is_valid(eth, hash))
			skb_set_hash(q->skb, jhash_1word(hash, 0), PKT_HASH_TYPE_L4);

		if (airoha_qdma_foe_entry_is_valid(eth, hash) &&
		    airoha_qdma_should_check_ppe_skb(eth, reason))
			airoha_ppe_check_skb(&eth->ppe->common.dev, q->skb, hash,
					     false);

		done++;
		napi_gro_receive(&q->napi, q->skb);
		q->skb = NULL;
		continue;
free_frag:
		if (q->skb) {
			dev_kfree_skb(q->skb);
			q->skb = NULL;
		}
		page_pool_put_full_page(q->page_pool, page, true);
	}
	airoha_qdma_fill_rx_queue(q);

	return done;
}

static int airoha_qdma_rx_napi_poll(struct napi_struct *napi, int budget)
{
	struct airoha_queue *q = container_of(napi, struct airoha_queue, napi);
	int cur, done = 0;

	do {
		cur = airoha_qdma_rx_process(q, budget - done);
		done += cur;
	} while (cur && done < budget);

	if (done < budget && napi_complete(napi)) {
		struct airoha_qdma *qdma = q->qdma;
		int i, qid = q - &qdma->q_rx[0];
		int intr_reg = qid < RX_DONE_HIGH_OFFSET ? QDMA_INT_REG_IDX1
							 : QDMA_INT_REG_IDX2;

		for (i = 0; i < qdma->common.eth->soc->irq_banks; i++) {
			if (!(BIT(qid) & RX_IRQ_BANK_PIN_MASK(i)))
				continue;

			airoha_qdma_irq_enable(&qdma->irq_banks[i], intr_reg,
					       BIT(qid % RX_DONE_HIGH_OFFSET));
		}
	}

	return done;
}

static int airoha_qdma_init_rx_queue(struct airoha_queue *q,
				     struct airoha_qdma *qdma, int ndesc)
{
	struct page_pool_params pp_params = {
		.pool_size = 256,
		.flags = PP_FLAG_DMA_MAP | PP_FLAG_DMA_SYNC_DEV,
		.dma_dir = DMA_FROM_DEVICE,
		.nid = NUMA_NO_NODE,
		.dev = qdma->common.eth->dev,
		.napi = &q->napi,
	};
	struct airoha_eth *eth = qdma->common.eth;
	int qid = q - &qdma->q_rx[0], thr;
	dma_addr_t dma_addr;
	bool lro_q;

	q->qdma = qdma;
	lro_q = airoha_qdma_is_lro_queue(q);

	q->entry = devm_kzalloc(eth->dev, ndesc * sizeof(*q->entry),
				GFP_KERNEL);
	if (!q->entry)
		return -ENOMEM;

	q->desc = dmam_alloc_coherent(eth->dev, ndesc * sizeof(*q->desc),
				      &dma_addr, GFP_KERNEL);
	if (!q->desc)
		return -ENOMEM;

	pp_params.order = lro_q ?
		(airoha_is(eth, airoha_en7523) ? EN7523_AIROHA_LRO_PAGE_ORDER :
		 AIROHA_LRO_PAGE_ORDER) : 0;
	pp_params.max_len = PAGE_SIZE << pp_params.order;

	q->page_pool = page_pool_create(&pp_params);
	if (IS_ERR(q->page_pool)) {
		int err = PTR_ERR(q->page_pool);

		q->page_pool = NULL;
		return err;
	}

	if (lro_q && airoha_is(eth, airoha_en7523))
		q->buf_size = SKB_HEAD_ALIGN(EN7523_AIROHA_RXQ_LRO_MAX_AGG_SIZE);
	else
		q->buf_size = lro_q ? pp_params.max_len : pp_params.max_len / 2;
	q->ndesc = ndesc;
	netif_napi_add(eth->napi_dev, &q->napi, airoha_qdma_rx_napi_poll);

	airoha_qdma_wr(qdma, REG_RX_RING_BASE(qid), dma_addr);
	airoha_qdma_rmw(qdma, REG_RX_RING_SIZE(qid),
			RX_RING_SIZE_MASK,
			FIELD_PREP(RX_RING_SIZE_MASK, ndesc));

	thr = clamp(ndesc >> 3, 1, 32);
	airoha_qdma_rmw(qdma, REG_RX_RING_SIZE(qid), RX_RING_THR_MASK,
			FIELD_PREP(RX_RING_THR_MASK, thr));
	airoha_qdma_rmw(qdma, REG_RX_DMA_IDX(qid), RX_RING_DMA_IDX_MASK,
			FIELD_PREP(RX_RING_DMA_IDX_MASK, q->head));
	if (lro_q || (airoha_is(eth, airoha_en7523) && qid == 15))
		airoha_qdma_clear(qdma, REG_RX_SCATTER_CFG(qid),
				  RX_RING_SG_EN_MASK);
	else
		airoha_qdma_set(qdma, REG_RX_SCATTER_CFG(qid),
				RX_RING_SG_EN_MASK);

	airoha_qdma_fill_rx_queue(q);

	return 0;
}

static void airoha_qdma_cleanup_rx_queue(struct airoha_queue *q)
{
	struct airoha_qdma *qdma = q->qdma;
	struct airoha_eth *eth = qdma->common.eth;
	int qid = q - &qdma->q_rx[0];

	while (q->queued) {
		struct airoha_queue_entry *e = &q->entry[q->tail];
		struct airoha_qdma_desc *desc = &q->desc[q->tail];
		struct page *page = virt_to_head_page(e->buf);

		dma_sync_single_for_cpu(eth->dev, e->dma_addr, e->dma_len,
					page_pool_get_dma_dir(q->page_pool));
		page_pool_put_full_page(q->page_pool, page, false);
		/* Reset DMA descriptor */
		WRITE_ONCE(desc->tcp_ts_reply, 0);
		WRITE_ONCE(desc->ctrl, 0);
		WRITE_ONCE(desc->addr, 0);
		WRITE_ONCE(desc->data, 0);
		WRITE_ONCE(desc->msg0, 0);
		WRITE_ONCE(desc->msg1, 0);
		WRITE_ONCE(desc->msg2, 0);
		WRITE_ONCE(desc->msg3, 0);

		q->tail = (q->tail + 1) % q->ndesc;
		q->queued--;
	}

	q->head = q->tail;
	/* Set RX_DMA_IDX to RX_CPU_IDX to notify the hw the QDMA RX ring is
	 * empty.
	 */
	airoha_qdma_rmw(qdma, REG_RX_CPU_IDX(qid), RX_RING_CPU_IDX_MASK,
			FIELD_PREP(RX_RING_CPU_IDX_MASK, q->head));
	airoha_qdma_rmw(qdma, REG_RX_DMA_IDX(qid), RX_RING_DMA_IDX_MASK,
			FIELD_PREP(RX_RING_DMA_IDX_MASK, q->tail));
}

static int airoha_qdma_init_rx(struct airoha_qdma *qdma)
{
	int i;

	for (i = 0; i < qdma->common.eth->soc->rx_ring; i++) {
		int err;

		if (!(RX_DONE_INT_MASK & BIT(i))) {
			/* rx-queue not binded to irq */
			continue;
		}

		err = airoha_qdma_init_rx_queue(&qdma->q_rx[i], qdma,
						RX_DSCP_NUM(i));
		if (err)
			return err;
	}

	return 0;
}

static void airoha_qdma_wake_netdev_txqs(struct airoha_queue *q)
{
	struct airoha_qdma *qdma = q->qdma;
	struct airoha_eth *eth = qdma->common.eth;
	int i, qid = q - &qdma->q_tx[0];

	for (i = 0; i < eth->soc->max_gdm_ports; i++) {
		struct airoha_gdm_port *port = eth->ports[i];
		int d;

		if (!port)
			continue;

		for (d = 0; d < ARRAY_SIZE(port->devs); d++) {
			struct airoha_gdm_dev *dev = port->devs[d];
			struct net_device *netdev;
			int j;

			if (!dev)
				continue;

			if (rcu_access_pointer(dev->qdma) != qdma)
				continue;

			netdev = netdev_from_priv(dev);
			for (j = 0; j < netdev->num_tx_queues; j++) {
				if (airoha_qdma_get_txq(qdma, j) != qid)
					continue;

				netif_wake_subqueue(netdev, j);
			}
		}
	}
	q->txq_stopped = false;
}

void
airoha_qdma_unmap_tx_entry(struct airoha_eth *eth,
			   struct airoha_queue_entry *e)
{
	if (e->dma_map_page)
		dma_unmap_page(eth->dev, e->dma_addr, e->dma_len,
			       DMA_TO_DEVICE);
	else
		dma_unmap_single(eth->dev, e->dma_addr, e->dma_len,
				 DMA_TO_DEVICE);

	e->dma_addr = 0;
	e->dma_len = 0;
	e->dma_map_page = false;
}

static int airoha_qdma_tx_napi_poll(struct napi_struct *napi, int budget)
{
	struct airoha_tx_irq_queue *irq_q;
	int id, done = 0, irq_queued;
	struct airoha_qdma *qdma;
	struct airoha_eth *eth;
	u32 status, head;

	irq_q = container_of(napi, struct airoha_tx_irq_queue, napi);
	qdma = irq_q->qdma;
	id = irq_q - &qdma->q_tx_irq[0];
	eth = qdma->common.eth;

	status = airoha_qdma_rr(qdma, REG_IRQ_STATUS(id));
	head = FIELD_GET(IRQ_HEAD_IDX_MASK, status);
	head = head % irq_q->size;
	irq_queued = FIELD_GET(IRQ_ENTRY_LEN_MASK, status);

	while (irq_queued > 0 && done < budget) {
		struct airoha_qdma_desc *desc;
		struct airoha_queue_entry *e;
		struct airoha_queue *q;
		u32 qid, val, index, desc_ctrl;
		struct sk_buff *skb;
		int retry;

		/*
		 * The IRQ status can become visible before the corresponding
		 * completion entry reaches coherent memory.  The vendor driver
		 * retries this read for the same reason.
		 */
		for (retry = 0; retry < 16; retry++) {
			val = READ_ONCE(irq_q->q[head]);
			if (val != U32_MAX)
				break;

			dma_rmb();
			cpu_relax();
		}
		if (val == U32_MAX)
			break;

		/*
		 * The completion entry is published after the descriptor writeback.
		 * Order the descriptor read after the queue entry read.
		 */
		dma_rmb();

		qid = FIELD_GET(IRQ_RING_IDX_MASK, val);
		if (qid >= eth->soc->tx_ring)
			goto consume;

		q = &qdma->q_tx[qid];
		if (!q->ndesc)
			goto consume;

		index = FIELD_GET(IRQ_DESC_IDX_MASK, val);
		if (index >= q->ndesc)
			goto consume;

		spin_lock_bh(&q->lock);

		if (!q->queued)
			goto unlock_consume;

		e = &q->entry[index];
		if (!e->dma_addr)
			goto unlock_consume;

		desc = &q->desc[index];
		desc_ctrl = le32_to_cpu(READ_ONCE(desc->ctrl));

		if (!(desc_ctrl & QDMA_DESC_DONE_MASK) &&
		    !(desc_ctrl & QDMA_DESC_DROP_MASK)) {
			spin_unlock_bh(&q->lock);
			break;
		}

		skb = e->skb;
		airoha_qdma_unmap_tx_entry(eth, e);
		e->skb = NULL;
		list_add_tail(&e->list, &q->tx_list);

		WRITE_ONCE(desc->ctrl, 0);
		WRITE_ONCE(desc->addr, 0);
		WRITE_ONCE(desc->data, 0);
		WRITE_ONCE(desc->msg0, 0);
		WRITE_ONCE(desc->msg1, 0);
		WRITE_ONCE(desc->msg2, 0);
		q->queued--;

		if (skb) {
			struct netdev_queue *txq;

			txq = skb_get_tx_queue(skb->dev, skb);
			netdev_tx_completed_queue(txq, 1, skb->len);
			dev_kfree_skb_any(skb);
		}

		if (q->txq_stopped && q->ndesc - q->queued >= q->free_thr) {
			/* Since multiple net_device TX queues can share the
			 * same hw QDMA TX queue, there is no guarantee we have
			 * inflight packets queued in hw belonging to a
			 * net_device TX queue stopped in the xmit path.
			 * In order to avoid any potential net_device TX queue
			 * stall, we need to wake all the net_device TX queues
			 * feeding the same hw QDMA TX queue.
			 */
			airoha_qdma_wake_netdev_txqs(q);
		}

unlock_consume:
		spin_unlock_bh(&q->lock);
consume:
		WRITE_ONCE(irq_q->q[head], U32_MAX);
		head = (head + 1) % irq_q->size;
		irq_queued--;
		done++;
	}

	if (done) {
		int i, len = done >> 7;

		/* Publish empty markers before returning entries to hardware. */
		dma_wmb();

		for (i = 0; i < len; i++)
			airoha_qdma_rmw(qdma, REG_IRQ_CLEAR_LEN(id),
					IRQ_CLEAR_LEN_MASK, 0x80);
		airoha_qdma_rmw(qdma, REG_IRQ_CLEAR_LEN(id),
				IRQ_CLEAR_LEN_MASK, (done & 0x7f));
	}

	if (done < budget && napi_complete(napi))
		airoha_qdma_irq_enable(&qdma->irq_banks[0], QDMA_INT_REG_IDX0,
				       TX_DONE_INT_MASK(id));

	return done;
}

static int airoha_qdma_init_tx_queue(struct airoha_queue *q,
				     struct airoha_qdma *qdma, int size)
{
	struct airoha_eth *eth = qdma->common.eth;
	int i, qid = q - &qdma->q_tx[0];
	dma_addr_t dma_addr;

	spin_lock_init(&q->lock);
	q->qdma = qdma;
	q->free_thr = 1 + MAX_SKB_FRAGS;
	INIT_LIST_HEAD(&q->tx_list);

	q->entry = devm_kzalloc(eth->dev, size * sizeof(*q->entry),
				GFP_KERNEL);
	if (!q->entry)
		return -ENOMEM;

	q->desc = dmam_alloc_coherent(eth->dev, size * sizeof(*q->desc),
				      &dma_addr, GFP_KERNEL);
	if (!q->desc)
		return -ENOMEM;

	for (i = 0; i < size; i++) {
		u32 val = FIELD_PREP(QDMA_DESC_DONE_MASK, 1);

		list_add_tail(&q->entry[i].list, &q->tx_list);
		WRITE_ONCE(q->desc[i].ctrl, cpu_to_le32(val));
	}
	q->ndesc = size;

	/* xmit ring drop default setting */
	if (!airoha_is(eth, airoha_en7523))
		airoha_qdma_set(qdma, REG_TX_RING_BLOCKING(qid),
			TX_RING_IRQ_BLOCKING_TX_DROP_EN_MASK);

	airoha_qdma_wr(qdma, REG_TX_RING_BASE(qid), dma_addr);
	airoha_qdma_rmw(qdma, REG_TX_CPU_IDX(qid), TX_RING_CPU_IDX_MASK,
			FIELD_PREP(TX_RING_CPU_IDX_MASK, 0));
	airoha_qdma_rmw(qdma, REG_TX_DMA_IDX(qid), TX_RING_DMA_IDX_MASK,
			FIELD_PREP(TX_RING_DMA_IDX_MASK, 0));

	return 0;
}

static int airoha_qdma_tx_irq_init(struct airoha_tx_irq_queue *irq_q,
				   struct airoha_qdma *qdma, int size)
{
	int id = irq_q - &qdma->q_tx_irq[0];
	struct airoha_eth *eth = qdma->common.eth;
	dma_addr_t dma_addr;

	irq_q->q = dmam_alloc_coherent(eth->dev, size * sizeof(u32),
				       &dma_addr, GFP_KERNEL);
	if (!irq_q->q)
		return -ENOMEM;

	memset(irq_q->q, 0xff, size * sizeof(u32));
	irq_q->size = size;
	irq_q->qdma = qdma;

	netif_napi_add_tx(eth->napi_dev, &irq_q->napi,
			  airoha_qdma_tx_napi_poll);

	airoha_qdma_wr(qdma, REG_TX_IRQ_BASE(id), dma_addr);
	airoha_qdma_rmw(qdma, REG_TX_IRQ_CFG(id), TX_IRQ_DEPTH_MASK,
			FIELD_PREP(TX_IRQ_DEPTH_MASK, size));
	airoha_qdma_rmw(qdma, REG_TX_IRQ_CFG(id), TX_IRQ_THR_MASK,
			FIELD_PREP(TX_IRQ_THR_MASK, 1));

	return 0;
}

static int airoha_qdma_init_tx(struct airoha_qdma *qdma)
{
	int i, err;

	for (i = 0; i < ARRAY_SIZE(qdma->q_tx_irq); i++) {
		err = airoha_qdma_tx_irq_init(&qdma->q_tx_irq[i], qdma,
					      IRQ_QUEUE_LEN(i));
		if (err)
			return err;
	}

	for (i = 0; i < qdma->common.eth->soc->tx_ring; i++) {
		err = airoha_qdma_init_tx_queue(&qdma->q_tx[i], qdma,
						TX_DSCP_NUM(i));
		if (err)
			return err;
	}

	return 0;
}

void airoha_qdma_cleanup_tx_queue(struct airoha_queue *q)
{
	struct airoha_qdma *qdma = q->qdma;
	struct airoha_eth *eth = qdma->common.eth;
	int i, qid = q - &qdma->q_tx[0];
	u16 index = 0;

	spin_lock_bh(&q->lock);
	for (i = 0; i < q->ndesc; i++) {
		struct airoha_queue_entry *e = &q->entry[i];
		struct airoha_qdma_desc *desc = &q->desc[i];

		if (!e->dma_addr)
			continue;

		airoha_qdma_unmap_tx_entry(eth, e);
		dev_kfree_skb_any(e->skb);
		e->skb = NULL;
		list_add_tail(&e->list, &q->tx_list);

		/* Reset DMA descriptor */
		WRITE_ONCE(desc->ctrl, 0);
		WRITE_ONCE(desc->addr, 0);
		WRITE_ONCE(desc->data, 0);
		WRITE_ONCE(desc->msg0, 0);
		WRITE_ONCE(desc->msg1, 0);
		WRITE_ONCE(desc->msg2, 0);

		q->queued--;
	}

	if (!list_empty(&q->tx_list)) {
		struct airoha_queue_entry *e;

		e = list_first_entry(&q->tx_list, struct airoha_queue_entry,
				     list);
		index = e - q->entry;
	}
	/* Set TX_DMA_IDX to TX_CPU_IDX to notify the hw the QDMA TX ring is
	 * empty.
	 */
	airoha_qdma_rmw(qdma, REG_TX_CPU_IDX(qid), TX_RING_CPU_IDX_MASK,
			FIELD_PREP(TX_RING_CPU_IDX_MASK, index));
	airoha_qdma_rmw(qdma, REG_TX_DMA_IDX(qid), TX_RING_DMA_IDX_MASK,
			FIELD_PREP(TX_RING_DMA_IDX_MASK, index));

	spin_unlock_bh(&q->lock);
}

static int airoha_qdma_init_hfwd_queues(struct airoha_qdma *qdma)
{
	int size, index, num_desc = HW_DSCP_NUM;
	struct airoha_eth *eth = qdma->common.eth;
	int id = qdma - &eth->qdma[0];
	u32 status, buf_size;
	dma_addr_t dma_addr;
	const char *name;

	name = devm_kasprintf(eth->dev, GFP_KERNEL, "qdma%d-buf", id);
	if (!name)
		return -ENOMEM;

	/* EN7523 reserves 32 MiB for QDMA0/LAN and 16 MiB for QDMA1/WAN.
	 * Keep the vendor 16K HWFWD descriptors on both instances by using
	 * 2 KiB payloads on LAN and 1 KiB payloads on WAN.
	 */
	buf_size = id ? AIROHA_MAX_PACKET_SIZE / 2 :
		   AIROHA_MAX_PACKET_SIZE;
	index = of_property_match_string(eth->dev->of_node,
					 "memory-region-names", name);
	if (index >= 0) {
		struct reserved_mem *rmem;
		struct device_node *np;

		/* Consume reserved memory for hw forwarding buffers queue if
		 * available in the DTS
		 */
		np = of_parse_phandle(eth->dev->of_node, "memory-region",
				      index);
		if (!np)
			return -ENODEV;

		rmem = of_reserved_mem_lookup(np);
		of_node_put(np);
		if (!rmem)
			return -ENODEV;

		dma_addr = rmem->base;
		/* Compute the number of hw descriptors according to the
		 * reserved memory size and the payload buffer size
		 */
		num_desc = div_u64(rmem->size, buf_size);
	} else {
		size = buf_size * num_desc;
		if (!dmam_alloc_coherent(eth->dev, size, &dma_addr,
					 GFP_KERNEL))
			return -ENOMEM;
	}

	airoha_qdma_wr(qdma, REG_FWD_BUF_BASE, dma_addr);

	size = num_desc * sizeof(struct airoha_qdma_fwd_desc);
	if (!dmam_alloc_coherent(eth->dev, size, &dma_addr, GFP_KERNEL))
		return -ENOMEM;

	airoha_qdma_wr(qdma, REG_FWD_DSCP_BASE, dma_addr);
	dev_info(eth->dev,
		 "QDMA%d HWFWD: payload=%u bytes descriptors=%d\n",
		 id, buf_size, num_desc);
	airoha_qdma_rmw(qdma, REG_HW_FWD_DSCP_CFG,
			HW_FWD_DSCP_PAYLOAD_SIZE_MASK,
			FIELD_PREP(HW_FWD_DSCP_PAYLOAD_SIZE_MASK,
				   buf_size != AIROHA_MAX_PACKET_SIZE));
	airoha_qdma_rmw(qdma, REG_FWD_DSCP_LOW_THR, FWD_DSCP_LOW_THR_MASK,
			FIELD_PREP(FWD_DSCP_LOW_THR_MASK, 128));
	airoha_qdma_rmw(qdma, REG_LMGR_INIT_CFG,
			LMGR_INIT_START | LMGR_SRAM_MODE_MASK |
			HW_FWD_DESC_NUM_MASK,
			FIELD_PREP(HW_FWD_DESC_NUM_MASK, num_desc) |
			LMGR_INIT_START | LMGR_SRAM_MODE_MASK);

	return read_poll_timeout(airoha_qdma_rr, status,
				 !(status & LMGR_INIT_START), USEC_PER_MSEC,
				 30 * USEC_PER_MSEC, true, qdma,
				 REG_LMGR_INIT_CFG);
}

static void airoha_qdma_init_qos(struct airoha_qdma *qdma)
{
	struct airoha_eth *eth = qdma->common.eth;
	u32 meter_cfg, meter_window, meter_timeslice;
	int id = qdma - &eth->qdma[0];

	airoha_qdma_clear(qdma, REG_TXWRR_MODE_CFG, TWRR_WEIGHT_SCALE_MASK);
	airoha_qdma_set(qdma, REG_TXWRR_MODE_CFG, TWRR_WEIGHT_BASE_MASK);

	/* The EN7523 SDK enables PSE buffer estimation only on QDMA WAN.
	 * QDMA1 is the WAN instance in this driver. Later SoCs explicitly
	 * keep this estimator disabled, as does EN7523 QDMA LAN.
	 */
	if (airoha_is(eth, airoha_en7523) && id == 1)
		airoha_qdma_set(qdma, REG_PSE_BUF_USAGE_CFG,
				PSE_BUF_ESTIMATE_EN_MASK);
	else
		airoha_qdma_clear(qdma, REG_PSE_BUF_USAGE_CFG,
				  PSE_BUF_ESTIMATE_EN_MASK);

	meter_cfg = EGRESS_RATE_METER_EN_MASK |
		    EGRESS_RATE_METER_EQ_RATE_EN_MASK;
	meter_window = 0x1f;
	meter_timeslice = 0x7ff;

	/* The EN7523 SDK uses a shorter sampling interval on QDMA WAN and
	 * leaves equal-rate mode disabled there. QDMA LAN uses the generic
	 * 2047us x 31 = 63.457ms interval.
	 */
	if (airoha_is(eth, airoha_en7523) && id == 1) {
		meter_cfg &= ~EGRESS_RATE_METER_EQ_RATE_EN_MASK;
		meter_window = 20;
		meter_timeslice = 200; /* 200us x 20 = 4ms */
	}

	meter_cfg |= FIELD_PREP(EGRESS_RATE_METER_WINDOW_SZ_MASK,
				meter_window) |
		     FIELD_PREP(EGRESS_RATE_METER_TIMESLICE_MASK,
				meter_timeslice);
	airoha_qdma_rmw(qdma, REG_EGRESS_RATE_METER_CFG,
			EGRESS_RATE_METER_EN_MASK |
			EGRESS_RATE_METER_EQ_RATE_EN_MASK |
			EGRESS_RATE_METER_WINDOW_SZ_MASK |
			EGRESS_RATE_METER_TIMESLICE_MASK,
			meter_cfg);

	/* ratelimit init */
	airoha_qdma_set(qdma, REG_GLB_TRTCM_CFG, GLB_TRTCM_EN_MASK);
	/* fast-tick 25us */
	airoha_qdma_rmw(qdma, REG_GLB_TRTCM_CFG, GLB_FAST_TICK_MASK,
			FIELD_PREP(GLB_FAST_TICK_MASK, 25));
	airoha_qdma_rmw(qdma, REG_GLB_TRTCM_CFG, GLB_SLOW_TICK_RATIO_MASK,
			FIELD_PREP(GLB_SLOW_TICK_RATIO_MASK, 40));

	airoha_qdma_set(qdma, REG_EGRESS_TRTCM_CFG, EGRESS_TRTCM_EN_MASK);
	airoha_qdma_rmw(qdma, REG_EGRESS_TRTCM_CFG, EGRESS_FAST_TICK_MASK,
			FIELD_PREP(EGRESS_FAST_TICK_MASK, 25));
	airoha_qdma_rmw(qdma, REG_EGRESS_TRTCM_CFG,
			EGRESS_SLOW_TICK_RATIO_MASK,
			FIELD_PREP(EGRESS_SLOW_TICK_RATIO_MASK, 40));

	airoha_qdma_set(qdma, REG_INGRESS_TRTCM_CFG, INGRESS_TRTCM_EN_MASK);
	airoha_qdma_clear(qdma, REG_INGRESS_TRTCM_CFG,
			  INGRESS_TRTCM_MODE_MASK);
	airoha_qdma_rmw(qdma, REG_INGRESS_TRTCM_CFG, INGRESS_FAST_TICK_MASK,
			FIELD_PREP(INGRESS_FAST_TICK_MASK, 125));
	airoha_qdma_rmw(qdma, REG_INGRESS_TRTCM_CFG,
			INGRESS_SLOW_TICK_RATIO_MASK,
			FIELD_PREP(INGRESS_SLOW_TICK_RATIO_MASK, 8));

	airoha_qdma_set(qdma, REG_SLA_TRTCM_CFG, SLA_TRTCM_EN_MASK);
	airoha_qdma_rmw(qdma, REG_SLA_TRTCM_CFG, SLA_FAST_TICK_MASK,
			FIELD_PREP(SLA_FAST_TICK_MASK, 25));
	airoha_qdma_rmw(qdma, REG_SLA_TRTCM_CFG, SLA_SLOW_TICK_RATIO_MASK,
			FIELD_PREP(SLA_SLOW_TICK_RATIO_MASK, 40));
}

static void airoha_qdma_init_qos_stats(struct airoha_qdma *qdma)
{
	int i;

	for (i = 0; i < AIROHA_NUM_QOS_CHANNELS; i++) {
		/* Tx-cpu transferred count */
		airoha_qdma_wr(qdma, REG_CNTR_VAL(i << 1), 0);
		airoha_qdma_wr(qdma, REG_CNTR_CFG(i << 1),
			       CNTR_EN_MASK | CNTR_ALL_QUEUE_EN_MASK |
			       CNTR_ALL_DSCP_RING_EN_MASK |
			       FIELD_PREP(CNTR_CHAN_MASK, i));
		/* Tx-fwd transferred count */
		airoha_qdma_wr(qdma, REG_CNTR_VAL((i << 1) + 1), 0);
		airoha_qdma_wr(qdma, REG_CNTR_CFG((i << 1) + 1),
			       CNTR_EN_MASK | CNTR_ALL_QUEUE_EN_MASK |
			       CNTR_ALL_DSCP_RING_EN_MASK |
			       FIELD_PREP(CNTR_SRC_MASK, 1) |
			       FIELD_PREP(CNTR_CHAN_MASK, i));
	}
}

static int airoha_qdma_hw_init(struct airoha_qdma *qdma)
{
	int i;

	for (i = 0; i < qdma->common.eth->soc->irq_banks; i++) {
		/* clear pending irqs */
		airoha_qdma_wr(qdma, REG_INT_STATUS(i), 0xffffffff);
		/* setup rx irqs */
		airoha_qdma_irq_enable(&qdma->irq_banks[i], QDMA_INT_REG_IDX0,
				       INT_RX0_MASK(RX_IRQ_BANK_PIN_MASK(i)));
		airoha_qdma_irq_enable(&qdma->irq_banks[i], QDMA_INT_REG_IDX1,
				       INT_RX1_MASK(RX_IRQ_BANK_PIN_MASK(i)));
		airoha_qdma_irq_enable(&qdma->irq_banks[i], QDMA_INT_REG_IDX2,
				       INT_RX2_MASK(RX_IRQ_BANK_PIN_MASK(i)));
		airoha_qdma_irq_enable(&qdma->irq_banks[i], QDMA_INT_REG_IDX3,
				       INT_RX3_MASK(RX_IRQ_BANK_PIN_MASK(i)));
	}
	/* setup tx irqs */
	airoha_qdma_irq_enable(&qdma->irq_banks[0], QDMA_INT_REG_IDX0,
			       TX_COHERENT_LOW_INT_MASK | INT_TX_MASK);
	airoha_qdma_irq_enable(&qdma->irq_banks[0], QDMA_INT_REG_IDX4,
			       TX_COHERENT_HIGH_INT_MASK);

	if (airoha_is(qdma->common.eth, airoha_en7523)) {
		airoha_qdma_wr(qdma, 0x30, 0x7C000000);
		airoha_qdma_wr(qdma, 0x34, 0x7C007C00);
		airoha_qdma_wr(qdma, 0x38, 0x00200000);
		airoha_qdma_wr(qdma, 0x3C, 0x00200020);
		airoha_qdma_wr(qdma, 0x40, 0x00000030);
		airoha_qdma_wr(qdma, 0x6C, 0x00000000);
	}

	/* setup irq binding */
	for (i = 0; i < qdma->common.eth->soc->tx_ring; i++) {
		if (!qdma->q_tx[i].ndesc)
			continue;

		if (TX_RING_IRQ_BLOCKING_MAP_MASK & BIT(i))
			airoha_qdma_set(qdma, REG_TX_RING_BLOCKING(i),
					TX_RING_IRQ_BLOCKING_CFG_MASK);
		else
			airoha_qdma_clear(qdma, REG_TX_RING_BLOCKING(i),
					  TX_RING_IRQ_BLOCKING_CFG_MASK);

		if (airoha_is(qdma->common.eth, airoha_en7523)) {
			if (i == 0)
				airoha_qdma_set(qdma, REG_TX_RING_BLOCKING(i),
						TX_RING_IRQ_BLOCKING_TX_DROP_EN_MASK);
			else
				airoha_qdma_clear(qdma, REG_TX_RING_BLOCKING(i),
						TX_RING_IRQ_BLOCKING_TX_DROP_EN_MASK);
		}
	}

	airoha_qdma_wr(qdma, REG_QDMA_GLOBAL_CFG,
		       FIELD_PREP(GLOBAL_CFG_DMA_PREFERENCE_MASK, 3) |
		       GLOBAL_CFG_CPU_TXR_RR_MASK |
		       GLOBAL_CFG_PAYLOAD_BYTE_SWAP_MASK |
		       GLOBAL_CFG_MULTICAST_MODIFY_FP_MASK |
		       GLOBAL_CFG_MULTICAST_EN_MASK |
		       GLOBAL_CFG_IRQ0_EN_MASK | GLOBAL_CFG_IRQ1_EN_MASK |
		       GLOBAL_CFG_TX_WB_DONE_MASK |
		       FIELD_PREP(GLOBAL_CFG_MAX_ISSUE_NUM_MASK, 2));

	airoha_qdma_init_qos(qdma);

	/* disable qdma rx delay interrupt */
	for (i = 0; i < qdma->common.eth->soc->rx_ring; i++) {
		if (!qdma->q_rx[i].ndesc)
			continue;

		airoha_qdma_clear(qdma, REG_RX_DELAY_INT_IDX(i),
				  RX_DELAY_INT_MASK);
	}

	airoha_qdma_set(qdma, REG_TXQ_CNGST_CFG,
			TXQ_CNGST_DROP_EN | TXQ_CNGST_DEI_DROP_EN);
	airoha_qdma_init_qos_stats(qdma);

	return 0;
}

static irqreturn_t airoha_irq_handler(int irq, void *dev_instance)
{
	struct airoha_irq_bank *irq_bank = dev_instance;
	struct airoha_qdma *qdma = irq_bank->qdma;
	u32 rx_intr_mask = 0, rx_intr1, rx_intr2;
	u32 intr[ARRAY_SIZE(irq_bank->irqmask)];
	int i;

	for (i = 0; i < ARRAY_SIZE(intr); i++) {
		intr[i] = airoha_qdma_rr(qdma, REG_INT_STATUS(i));
		intr[i] &= irq_bank->irqmask[i];
		airoha_qdma_wr(qdma, REG_INT_STATUS(i), intr[i]);
	}

	if (!test_bit(DEV_STATE_INITIALIZED, &qdma->common.eth->state))
		return IRQ_NONE;

	rx_intr1 = intr[1] & RX_DONE_LOW_INT_MASK;
	if (rx_intr1) {
		airoha_qdma_irq_disable(irq_bank, QDMA_INT_REG_IDX1, rx_intr1);
		rx_intr_mask |= rx_intr1;
	}

	rx_intr2 = intr[2] & RX_DONE_HIGH_INT_MASK;
	if (rx_intr2) {
		airoha_qdma_irq_disable(irq_bank, QDMA_INT_REG_IDX2, rx_intr2);
		rx_intr_mask |= (rx_intr2 << 16);
	}

	for (i = 0; rx_intr_mask && i < qdma->common.eth->soc->rx_ring; i++) {
		if (!qdma->q_rx[i].ndesc)
			continue;

		if (rx_intr_mask & BIT(i))
			napi_schedule(&qdma->q_rx[i].napi);
	}

	if (intr[0] & INT_TX_MASK) {
		for (i = 0; i < ARRAY_SIZE(qdma->q_tx_irq); i++) {
			if (!(intr[0] & TX_DONE_INT_MASK(i)))
				continue;

			airoha_qdma_irq_disable(irq_bank, QDMA_INT_REG_IDX0,
						TX_DONE_INT_MASK(i));
			napi_schedule(&qdma->q_tx_irq[i].napi);
		}
	}

	return IRQ_HANDLED;
}

static int airoha_qdma_init_irq_banks(struct platform_device *pdev,
				      struct airoha_qdma *qdma)
{
	struct airoha_eth *eth = qdma->common.eth;
	int i, id = qdma - &eth->qdma[0];
	qdma->irq_banks = devm_kzalloc(&pdev->dev,
		sizeof(*qdma->irq_banks) * eth->soc->irq_banks, GFP_KERNEL);
	if (!qdma->irq_banks)
			return -ENOMEM;

	for (i = 0; i < eth->soc->irq_banks; i++) {
		struct airoha_irq_bank *irq_bank = &qdma->irq_banks[i];
		int err, irq_index = 4 * id + i;
		const char *name;

		spin_lock_init(&irq_bank->irq_lock);
		irq_bank->qdma = qdma;

		irq_bank->irq = platform_get_irq(pdev, irq_index);
		if (irq_bank->irq < 0)
			return irq_bank->irq;

		name = devm_kasprintf(eth->dev, GFP_KERNEL,
				      KBUILD_MODNAME ".%d", irq_index);
		if (!name)
			return -ENOMEM;

		err = devm_request_irq(eth->dev, irq_bank->irq,
				       airoha_irq_handler, IRQF_SHARED, name,
				       irq_bank);
		if (err)
			return err;
	}

	return 0;
}

int airoha_qdma_init(struct platform_device *pdev,
			    struct airoha_eth *eth,
			    struct airoha_qdma *qdma)
{
	int err, id = qdma - &eth->qdma[0];
	const char *res;

	res = devm_kasprintf(eth->dev, GFP_KERNEL, "qdma%d", id);
	if (!res)
		return -ENOMEM;

	qdma->common.regs = devm_platform_ioremap_resource_byname(pdev, res);
	if (IS_ERR(qdma->common.regs))
		return dev_err_probe(eth->dev, PTR_ERR(qdma->common.regs),
				     "failed to iomap qdma%d regs\n", id);

	airoha_qdma_common_init(&qdma->common, eth, qdma->common.regs, id);
	eth->qdma_common[id] = &qdma->common;

	err = airoha_qdma_init_irq_banks(pdev, qdma);
	if (err)
		return err;

	err = airoha_qdma_init_rx(qdma);
	if (err)
		return err;

	err = airoha_qdma_init_tx(qdma);
	if (err)
		return err;

	err = airoha_qdma_init_hfwd_queues(qdma);
	if (err)
		return err;

	return airoha_qdma_hw_init(qdma);
}

void airoha_qdma_cleanup(struct airoha_qdma *qdma)
{
	struct airoha_eth *eth = qdma->common.eth;
	int i;

	for (i = 0; i < qdma->common.eth->soc->rx_ring; i++) {
		if (!qdma->q_rx[i].ndesc)
			continue;

		netif_napi_del(&qdma->q_rx[i].napi);
		airoha_qdma_cleanup_rx_queue(&qdma->q_rx[i]);
		if (qdma->q_rx[i].page_pool) {
			page_pool_destroy(qdma->q_rx[i].page_pool);
			qdma->q_rx[i].page_pool = NULL;
		}
	}

	for (i = 0; i < ARRAY_SIZE(qdma->q_tx_irq); i++) {
		if (!qdma->q_tx_irq[i].size)
			continue;

		netif_napi_del(&qdma->q_tx_irq[i].napi);
	}

	for (i = 0; i < qdma->common.eth->soc->tx_ring; i++) {
		if (!qdma->q_tx[i].ndesc)
			continue;

		airoha_qdma_cleanup_tx_queue(&qdma->q_tx[i]);
	}

	if (eth && qdma->common.id < ARRAY_SIZE(eth->qdma_common))
		eth->qdma_common[qdma->common.id] = NULL;
}

/* Generation-2 QDMA runtime lifecycle. */
void airoha_qdma_start_napi(struct airoha_qdma *qdma)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(qdma->q_tx_irq); i++)
		napi_enable(&qdma->q_tx_irq[i].napi);

	for (i = 0; i < qdma->common.eth->soc->rx_ring; i++) {
		if (!qdma->q_rx[i].ndesc)
			continue;

		napi_enable(&qdma->q_rx[i].napi);
	}
}

void airoha_qdma_stop_napi(struct airoha_qdma *qdma)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(qdma->q_tx_irq); i++)
		napi_disable(&qdma->q_tx_irq[i].napi);

	for (i = 0; i < qdma->common.eth->soc->rx_ring; i++) {
		if (!qdma->q_rx[i].ndesc)
			continue;

		napi_disable(&qdma->q_rx[i].napi);
	}
}

void airoha_qdma_start(struct airoha_qdma *qdma)
{
	airoha_qdma_set(qdma, REG_QDMA_GLOBAL_CFG,
			GLOBAL_CFG_TX_DMA_EN_MASK |
			GLOBAL_CFG_RX_DMA_EN_MASK);
	qdma->users++;
}

void airoha_qdma_stop(struct airoha_qdma *qdma)
{
	u32 status;

	if (--qdma->users)
		return;

	airoha_qdma_clear(qdma, REG_QDMA_GLOBAL_CFG,
			  GLOBAL_CFG_TX_DMA_EN_MASK |
			  GLOBAL_CFG_RX_DMA_EN_MASK);

	if (read_poll_timeout(airoha_qdma_rr, status,
			      !(status & (GLOBAL_CFG_TX_DMA_BUSY_MASK |
					  GLOBAL_CFG_RX_DMA_BUSY_MASK)),
			      USEC_PER_MSEC, 50 * USEC_PER_MSEC, true,
			      qdma, REG_QDMA_GLOBAL_CFG))
		dev_warn(qdma->common.eth->dev, "QDMA DMA engine busy timeout\n");

	for (int i = 0; i < qdma->common.eth->soc->tx_ring; i++) {
		if (!qdma->q_tx[i].ndesc)
			continue;

		airoha_qdma_cleanup_tx_queue(&qdma->q_tx[i]);
	}
}
