// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2025 AIROHA Inc
 *
 * Author: Caleb James DeLisle <cjd@cjdns.fr>
 * Author: Matheus Sampaio Queiroga <srherobrine20@gmail.com>
 */
#include <linux/bitmap.h>
#include <linux/dev_printk.h>
#include <linux/etherdevice.h>
#include <linux/ioport.h>
#include <linux/mdio.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/of_net.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>
#include <linux/reset.h>
#include <linux/skbuff.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <net/dsa.h>
#include <net/page_pool/helpers.h>

#include "econet_eth.h"

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
	/* Protects register programming and users. */
	struct mutex			lock;
	struct qregs __iomem		*regs;
	int				users;

	/* Synchronized inside of the structure */
	struct econet_irq 		irqs[QDMA_NUM_IRQS];
	struct econet_tx_doneq 		q_tx_done[QDMA_NUM_TX_DONE];
	struct econet_q_tx 		q_tx[QDMA_NUM_CHAINS];
	struct econet_q_rx 		q_rx[QDMA_NUM_CHAINS];

	/* Not modified after init */
	struct econet_eth 		*eth;
	struct device 			*dev;
	int 				id;
	struct fwdesc 			*hwf_desc;
	struct net_device 		*napi_dev;
	struct econet_qdma_cfg 		cfg;
	const struct econet_soc_data	*soc;
};

static void econet_fill_rx_queue(struct econet_q_rx *q, u32 end_i)
{
	u32 ndesc = q->ndesc;
	u32 cpu_i = (q->cpu_i + 1) % ndesc;

	for (; cpu_i != end_i; cpu_i = (cpu_i + 1) % ndesc) {
		struct econet_q_rx_ent *e = &q->entry[cpu_i];
		struct desc *pdesc = &q->desc[cpu_i];
		struct page *page;
		int offset;
		int i;

		page = page_pool_dev_alloc_frag(q->page_pool, &offset,
						q->buf_size);

		if (!page)
			break;

		WARN_ON_ONCE(e->dma_addr);
		e->buf = page_address(page) + offset;
		e->dma_addr = page_pool_get_dma_addr(page) + offset;
		e->dma_len = SKB_WITH_OVERHEAD(q->buf_size);

		WRITE_ONCE(pdesc->info, ((struct desc_info) {
			.word = FIELD_PREP(DESC_INFO_PKT_LEN_MASK, e->dma_len)
		}));
		WRITE_ONCE(pdesc->pkt_addr, e->dma_addr);
		for (i = 0; i < ARRAY_SIZE(pdesc->msg.raw); i++)
			WRITE_ONCE(pdesc->msg.raw[i], 0);
	}

	cpu_i = (cpu_i - 1) % ndesc;

	q->cpu_i = cpu_i;
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

	dma_sync_single_for_cpu(q->qdma->dev, e->dma_addr,
				SKB_WITH_OVERHEAD(q->buf_size), dir);

	/* Not needed but a couple of WARN_ON_ONCE() check this later */
	e->dma_addr = 0;

	/* Make debug more readable */
	WRITE_ONCE(q->desc[cpu_i].pkt_addr, 0);

	len = get_desc_info_pkt_len(&desc.info);
	if (!len || len > SKB_WITH_OVERHEAD(q->buf_size))
		goto return_page;

	if (WARN_ON_ONCE(is_desc_info_nls(&desc.info)))
		goto return_page;

	skb = napi_build_skb(e->buf, q->buf_size);
	if (!skb)
		goto return_page;

	__skb_put(skb, len);
	skb_mark_for_recycle(skb);
	skb->ip_summed = CHECKSUM_UNNECESSARY;

	hash = get_erx_ppe_entry(&desc.msg.erx);
	skb_set_hash(skb, jhash_1word(hash, 0),
		     PKT_HASH_TYPE_L4);
	airoha_ppe_dev_check_skb_reason(q->qdma->eth->ppe, skb, hash,
					get_erx_crsn(&desc.msg.erx));

	sport = get_erx_sport(&desc.msg.erx);
	if (econet_rx_before_recv(q->qdma->eth, skb, sport))
		dev_kfree_skb(skb);
	else
		napi_gro_receive(&q->napi, skb);

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
	int i;

	guard(spinlock)(&irq->lock_irq);

	for (i = 0; i < ARRAY_SIZE(irq->irqmask); i++) {
		unsigned long regval;
		u32 disable_int = 0;
		u8 bit;

		regval = econet_rreg(irq->status_reg[i]);
		regval &= irq->irqmask[i];

		/* You must write the bits back to the status register
		 * or you will keep receiving the same interrupt. */
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
			dev_dbg_ratelimited(qdma->dev,
					    "%s IRQ from %s[%d]\n",
					    econet_irq_purpose_type_str(p.type),
					    econet_irq_purpose_source_str(p.source),
					    p.chain);
		}

		irq->irqmask[i] &= ~disable_int;
		econet_wreg(irq->irqmask[i], irq->mask_reg[i]);
	}

	return IRQ_HANDLED;
}

#define IRQ_RING_IDX_MASK		GENMASK(20, 16)
#define IRQ_DESC_IDX_MASK		GENMASK(15, 0)

static int econet_poll_tx_complete(struct napi_struct *napi, int budget)
{
	struct qregs_doneq_state state;
	struct econet_tx_doneq *done_q;
	struct econet_qdma *qdma;
	int id, irq_queued;
	u32 done = 0, head;

	done_q = container_of(napi, struct econet_tx_doneq, napi);
	qdma = done_q->qdma;
	id = done_q - &qdma->q_tx_done[0];

	state = econet_rreg(&done_q->regs->state);
	head = get_qregs_doneq_state_head_index(&state);
	head = head % done_q->size;
	irq_queued = get_qregs_doneq_state_length(&state);

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

		qid = FIELD_GET(IRQ_RING_IDX_MASK, val);
		if (WARN_ON_ONCE(qid >= ARRAY_SIZE(qdma->q_tx)))
			continue;

		q = &qdma->q_tx[qid];
		if (WARN_ON_ONCE(!q->ndesc))
			continue;

		index = FIELD_GET(IRQ_DESC_IDX_MASK, val);
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
		union econet_irq_purpose purpose = IRQ_PURPOSE(DONE, TX, id);
		union irq_bit b = en751221_irq_bit(purpose);

		econet_qdma_set_irqmask(qdma, b, true);
	}

	return done;
}

int econet_qdma_xmit(struct econet_qdma *qdma, struct sk_buff *skb,
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
		.pool_size = 256,
		.flags = PP_FLAG_DMA_MAP | PP_FLAG_DMA_SYNC_DEV,
		.dma_dir = DMA_FROM_DEVICE,
		.max_len = PAGE_SIZE,
		.nid = NUMA_NO_NODE,
		.dev = qdma->dev,
		.napi = &q->napi,
	};
	int threshold = clamp(ndesc >> 3, 1, 32);
	struct qregs_rxring_size rrs;
	struct qregs_rxring_low rrl;
	dma_addr_t dma_addr;

	q->buf_size = ECONET_MAX_PACKET_SIZE;
	q->ndesc = ndesc;
	q->qdma = qdma;
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
				      KBUILD_MODNAME "-%d.%d", qdma->id, i);
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
			      struct econet_qdma *qdma, int size)
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
	set_qregs_doneq_cfg_int_threshold(&cfg, 1);
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

	name = devm_kasprintf(qdma->dev, GFP_KERNEL, "qdma%d-buf", qdma->id);
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

	cfg = econet_rreg(&qdma->regs->hwf_cfg);
	set_qregs_hwf_cfg_pkt_sz(&cfg, buf_size_cfg);
	set_qregs_hwf_cfg_low_th(&cfg, qdma->cfg.fwd_low_threshold);
	econet_wreg(cfg, &qdma->regs->hwf_cfg);

	cfg1 = econet_rreg(&qdma->regs->hwf_cfg1);
	set_qregs_hwf_cfg1_fwd_desc_n(&cfg1, num_desc);
	set_qregs_hwf_cfg1_overhead(&cfg1, 0x14);
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

				/* RX and TX done */
				en |= (p.type == IPS_DONE);

				/* Running out of resources */
				en |= (p.type == IPS_NO_DSCP && p.source != IPSC_TX);
				en |= (p.type == IPS_LOW_DSCP && p.source != IPSC_TX);

				/* Error conditions */
				en |= (p.type == IPS_ERR_COHERENT);
				en |= (p.type == IPS_OVERFLOW);

				/* External hardware */
				en |= (p.type == IPS_GPON_INT);
				en |= (p.type == IPS_EPON_INT);
				en |= (p.type == IPS_XPON_INT);

				mask |= en << k;
			}

			qdma->irqs[i].irqmask[j] = mask;
			econet_wreg(mask, qdma->irqs[i].mask_reg[j]);
		}
	}

	qcfg = (struct qregs_qcfg) { 0 };
	set_qregs_qcfg_msg_word_swap(&qcfg, true);
	set_qregs_qcfg_dscp_byte_swap(&qcfg, qdma->soc->dscp_byte_swap);
	set_qregs_qcfg_payload_byte_sw(&qcfg, true);
	set_qregs_qcfg_irq_en(&qcfg, true);
	set_qregs_qcfg_check_done(&qcfg, true);
	set_qregs_qcfg_tx_wb_done(&qcfg, true);
	set_qregs_qcfg_burst_size(&qcfg, QREGS_QCFG_BURST_SIZE_128_BYTES);
	econet_wreg(qcfg, &qdma->regs->qdma_cfg);

	econet_wreg(0U, &qdma->regs->rx_int_delay);

	struct qregs_tx_congest_cfg cngst_cfg = {0};
	set_qregs_tx_congest_cfg_tail_drop_en(&cngst_cfg, true);
	set_qregs_tx_congest_cfg_dei_drop_en(&cngst_cfg, true);
	econet_wreg(cngst_cfg, &qdma->regs->tx_congest_cfg);

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
					 qdma->cfg.done_list_size[i]);
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

static void econet_qdma_destroy_rxq_locked(struct econet_q_rx *q)
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

static int econet_qdma_destroy_locked(struct econet_qdma *qdma)
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

		econet_qdma_destroy_rxq_locked(q);
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

int econet_qdma_destroy(struct econet_qdma *qdma)
{
	guard(mutex)(&qdma->lock);

	return econet_qdma_destroy_locked(qdma);
}

int econet_qdma_use(struct econet_qdma *qdma)
{
	struct qregs_qcfg qcfg;
	int i;

	guard(mutex)(&qdma->lock);

	if (qdma->users++ > 0)
		return 0;

	qcfg = econet_rreg(&qdma->regs->qdma_cfg);
	set_qregs_qcfg_rx_dma_en(&qcfg, true);
	set_qregs_qcfg_tx_dma_en(&qcfg, true);
	econet_wreg(qcfg, &qdma->regs->qdma_cfg);

	for (i = 0; i < ARRAY_SIZE(qdma->q_rx); i++)
		napi_enable(&qdma->q_rx[i].napi);

	for (i = 0; i < ARRAY_SIZE(qdma->q_tx_done); i++)
		napi_enable(&qdma->q_tx_done[i].napi);

	return 0;
}

int econet_qdma_unuse(struct econet_qdma *qdma)
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

struct econet_qdma *econet_qdma_new(struct econet_eth *eth,
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
	qdma->id = id;
	mutex_init(&qdma->lock);
	qdma->regs = qdma_regs;
	memcpy(&qdma->cfg, cfg, sizeof(*cfg));
	qdma->soc = cfg->soc;
	qdma->eth = eth;

	{
		char name[IFNAMSIZ];

		snprintf(name, sizeof(name), "qdma%d_eth", id);
		qdma->napi_dev = airoha_eth_alloc_napi_dev(name);
	}
	if (!qdma->napi_dev)
		return ERR_PTR(-ENOMEM);

	err = econet_init(eth->dev, qdma, irqs, num_irqs);
	if (err) {
		econet_qdma_destroy(qdma);
		return ERR_PTR(err);
	}

	return qdma;
}

/*
 * EcoNet EN751221 has 2 GDM ports, one for LAN and one for WAN.
 * Each port has it's own registers and MAC address, and it's own
 * net_device instance.
 */

struct econet_hw_stats {
	/* protect concurrent hw_stats accesses */
	spinlock_t lock;
	struct u64_stats_sync syncp;

	/* EN751221 gdm1 has only limited stats, gdm1 has all */
	bool g2_stats;

	/* get_stats64 */
	u64 rx_ok_pkts;
	u64 tx_ok_pkts;
	u64 rx_ok_bytes;
	u64 tx_ok_bytes;
	u64 rx_errors;
	u64 rx_drops;
	u64 tx_drops;
	u64 rx_over_errors;

	/* get_stats64 (requires gdm2 extended stats) */
	u64 rx_crc_error;
	u64 rx_multicast;

	/* ethtool stats (all requires gdm2 extended stats) */
	u64 tx_broadcast;
	u64 tx_multicast;
	u64 tx_len[7];
	u64 rx_broadcast;
	u64 rx_fragment;
	u64 rx_jabber;
	u64 rx_len[7];
};

struct econet_gdm_port {
	/* Must stay first for common GDM/phylink and PPE metadata. */
	struct airoha_gdm_common common;

	struct net_device *dev;

	struct gdm __iomem *regs;

	/* Protects accesses to hardware regs */
	spinlock_t reg_lock;

	struct econet_hw_stats stats;

	// DECLARE_BITMAP(qos_sq_bmap, AIROHA_NUM_QOS_CHANNELS);

	/* qos stats counters */
	u64 cpu_tx_packets;
	u64 fwd_tx_packets;

	// struct metadata_dst *dsa_meta[AIROHA_MAX_DSA_PORTS];

	struct econet_qdma *qdma;

	struct econet_eth *eth;

	enum etx_fport fport;

	int qid;
};

static void econet_set_macaddr(struct econet_gdm_port *port, const u8 *addr)
{
	struct gdm_mymac_msb msb;
	struct gdm_mymac_lsb lsb;

	scoped_guard(spinlock, &port->reg_lock) {
		msb = econet_rreg(&port->regs->mymac_msb);
		lsb = econet_rreg(&port->regs->mymac_lsb);
		set_gdm_mymac_msb_a(&msb, addr[0]);
		set_gdm_mymac_msb_b(&msb, addr[1]);
		set_gdm_mymac_lsb_c(&lsb, addr[2]);
		set_gdm_mymac_lsb_d(&lsb, addr[3]);
		set_gdm_mymac_lsb_e(&lsb, addr[4]);
		set_gdm_mymac_lsb_f(&lsb, addr[5]);
		econet_wreg(lsb, &port->regs->mymac_lsb);
		econet_wreg(msb, &port->regs->mymac_msb);
	}

}

static int econet_dev_set_macaddr(struct net_device *dev, void *p)
{
	struct econet_gdm_port *port = netdev_priv(dev);
	int err;

	err = eth_mac_addr(dev, p);
	if (err)
		return err;

	econet_set_macaddr(port, dev->dev_addr);

	return 0;
}

#define EN751221_CDM_STAG_EN		BIT(0)
#define EN751221_GDM_STAG_EN		BIT(24)

static void econet_set_gdm_port_fwd_cfg(struct econet_gdm_port *port,
				      enum etx_fport val)
{
	struct fwd_cfg fc;

	guard(spinlock)(&port->reg_lock);
	fc = econet_rreg(&port->regs->fwd_cfg);
	set_gdm_fwd_cfg_mymac_fport(&fc, val);
	set_gdm_fwd_cfg_mcast_fport(&fc, val);
	set_gdm_fwd_cfg_bcast_fport(&fc, val);
	set_gdm_fwd_cfg_default_fport(&fc, val);
	/*
	 * Bit 25 is DROP_OVERSIZE on newer Airoha GDMs, but GDM_UNTAG_EN
	 * on EN751221. Keep it untouched there so the MTK special tag from
	 * the companion MT7530 reaches the DSA tagger intact.
	 */
	if (!port->qdma->soc->en751221_special_tag)
		set_gdm_fwd_cfg_drop_oversize(&fc, true);
	econet_wreg(fc, &port->regs->fwd_cfg);
}

static int econet_dev_init(struct net_device *dev)
{
	struct econet_gdm_port *port = netdev_priv(dev);

	econet_set_macaddr(port, dev->dev_addr);
	/* Route each GDM port to the CPU side of its matching QDMA. */
	econet_set_gdm_port_fwd_cfg(port,
		port->fport == ETX_FPORT_GDM2 ?
		ETX_FPORT_QDMA1_CPU : ETX_FPORT_QDMA0_CPU);

	return 0;
}

static int econet_dev_open(struct net_device *dev)
{
	struct econet_gdm_port *port = netdev_priv(dev);
	struct gdm_len_th rlt;
	int err;

	err = airoha_gdm_phylink_connect(&port->common, false);
	if (err) {
		netdev_err(dev, "could not attach PHY: %d\n", err);
		return err;
	}

	/* The MT7530 CPU port is represented by ethernet = <&gdm1>. Keep
	 * the MTK special tag on the DSA conduit and disable it on a direct
	 * PHY/WAN GDM. EN751221 additionally needs the legacy CDM/GDM bits
	 * used by the vendor special-tag datapath.
	 */
	scoped_guard(spinlock, &port->reg_lock) {
		bool dsa = netdev_uses_dsa(dev);

		if (port->qdma->soc->en751221_special_tag &&
		    port->fport == ETX_FPORT_GDM1) {
			struct fwd_cfg fc = econet_rreg(&port->regs->fwd_cfg);

			if (dsa)
				fc.word |= EN751221_GDM_STAG_EN;
			else
				fc.word &= ~EN751221_GDM_STAG_EN;
			econet_wreg(fc, &port->regs->fwd_cfg);

			/* CDMA_CSG_CFG is at offset 0 from the GDM1 window. */
			airoha_rmw(port->regs, 0, EN751221_CDM_STAG_EN,
				   dsa ? EN751221_CDM_STAG_EN : 0);
		}

		econet_wreg((u32)dsa, &port->regs->stag_en);
		rlt = econet_rreg(&port->regs->rx_len_threshold);
		set_gdm_len_th_runt_len(&rlt, 60);
		set_gdm_len_th_oversize_len(&rlt,
					 ETH_HLEN + dev->mtu + ETH_FCS_LEN);
		econet_wreg(rlt, &port->regs->rx_len_threshold);
	}

	err = econet_qdma_use(port->qdma);
	if (err) {
		scoped_guard(spinlock, &port->reg_lock)
			econet_wreg(0U, &port->regs->stag_en);
		airoha_gdm_phylink_disconnect(&port->common);
		return err;
	}

	netif_tx_start_all_queues(dev);

	return 0;
}

static int econet_dev_stop(struct net_device *dev)
{
	struct econet_gdm_port *port = netdev_priv(dev);

	netif_tx_disable(dev);
	airoha_gdm_phylink_disconnect(&port->common);

	scoped_guard(spinlock, &port->reg_lock) {
		econet_wreg(0U, &port->regs->stag_en);
		if (port->qdma->soc->en751221_special_tag &&
		    port->fport == ETX_FPORT_GDM1) {
			struct fwd_cfg fc = econet_rreg(&port->regs->fwd_cfg);

			fc.word &= ~EN751221_GDM_STAG_EN;
			econet_wreg(fc, &port->regs->fwd_cfg);
			airoha_rmw(port->regs, 0, EN751221_CDM_STAG_EN, 0);
		}
	}

	return econet_qdma_unuse(port->qdma);
}

static int econet_dev_change_mtu(struct net_device *dev, int mtu)
{
	struct econet_gdm_port *port = netdev_priv(dev);
	struct gdm_len_th rlt;

	guard(spinlock)(&port->reg_lock);
	rlt = econet_rreg(&port->regs->rx_len_threshold);
	set_gdm_len_th_oversize_len(&rlt, ETH_HLEN + mtu + ETH_FCS_LEN);
	econet_wreg(rlt, &port->regs->rx_len_threshold);

	WRITE_ONCE(dev->mtu, mtu);

	return 0;
}

static netdev_tx_t econet_dev_xmit(struct sk_buff *skb, struct net_device *dev)
{
	struct econet_gdm_port *port = netdev_priv(dev);
	int qid, ret = 0, len = skb->len;
	struct netdev_queue *txq;
	union desc_msg msg = {0};

	qid = skb_get_queue_mapping(skb);
	set_etx_channel(&msg.etx, qid / ECONET_NUM_QUEUES);
	set_etx_queue(&msg.etx, qid % ECONET_NUM_QUEUES);
	set_etx_fport(&msg.etx, port->fport);

	txq = netdev_get_tx_queue(dev, qid);

	/* Non-linear skbs are unsupported, we shouldn't get them but
	 * if we do, we'll attempt to linearize. */
	if (skb_linearize(skb))
		goto drop;

	netdev_tx_sent_queue(txq, len);

	ret = econet_qdma_xmit(port->qdma, skb, &msg, 0);
	if (ret == -EBUSY) {
		netdev_tx_completed_queue(txq, 1, len);
		netif_tx_stop_queue(txq);
		return NETDEV_TX_BUSY;
	}
	if (ret < 0) {
		netdev_tx_completed_queue(txq, 1, len);
		goto drop;
	}

	/* Positive EBUSY means this packet was queued and filled the ring. */
	if (ret == EBUSY)
		netif_tx_stop_queue(txq);

	return NETDEV_TX_OK;

drop:
	dev_kfree_skb_any(skb);
	dev->stats.tx_dropped++;
	return NETDEV_TX_OK;
}

static void econet_update_hw_stats(struct econet_gdm_port *port)
{
	struct gdm_counters __iomem *c = &port->regs->counters;
	struct clear_counters cc = {0};
	u32 i = 0;

	guard(spinlock)(&port->stats.lock);
	u64_stats_update_begin(&port->stats.syncp);

	port->stats.tx_ok_pkts += econet_rreg(&c->tx.tx_pkts);
	port->stats.tx_ok_bytes += econet_rreg(&c->tx.bytes);
	port->stats.tx_drops += econet_rreg(&c->tx.drops);
	if (port->stats.g2_stats) {
		port->stats.tx_broadcast += econet_rreg(&c->tx.g2.tx_bcast);
		port->stats.tx_multicast += econet_rreg(&c->tx.g2.tx_mcast);

		port->stats.tx_len[i] += econet_rreg(&c->tx.g2.f_less_64);
		port->stats.tx_len[i++] += econet_rreg(&c->tx.g2.f_64);

		port->stats.tx_len[i++] += econet_rreg(&c->tx.g2.f_65_127);
		port->stats.tx_len[i++] += econet_rreg(&c->tx.g2.f_128_255);
		port->stats.tx_len[i++] += econet_rreg(&c->tx.g2.f_256_511);
		port->stats.tx_len[i++] += econet_rreg(&c->tx.g2.f_512_1023);
		port->stats.tx_len[i++] += econet_rreg(&c->tx.g2.f_1024_1518);
		port->stats.tx_len[i++] += econet_rreg(&c->tx.g2.f_more_1518);
	}

	port->stats.rx_ok_pkts += econet_rreg(&c->rx.pkts);
	port->stats.rx_ok_bytes += econet_rreg(&c->rx.bytes);
	port->stats.rx_errors += econet_rreg(&c->rx.drops_err);
	port->stats.rx_over_errors += econet_rreg(&c->rx.drops_overflow);
	if (port->stats.g2_stats) {
		port->stats.rx_drops += econet_rreg(&c->rx.g2.edrops);
		port->stats.rx_broadcast += econet_rreg(&c->rx.g2.bcast);
		port->stats.rx_multicast += econet_rreg(&c->rx.g2.mcast);
		port->stats.rx_crc_error += econet_rreg(&c->rx.g2.ecrc);
		port->stats.rx_fragment += econet_rreg(&c->rx.g2.efrag);
		port->stats.rx_jabber += econet_rreg(&c->rx.g2.ejabber);

		i = 0;
		port->stats.rx_len[i] += econet_rreg(&c->rx.g2.f_less_64);
		port->stats.rx_len[i++] += econet_rreg(&c->rx.g2.f_64);

		port->stats.rx_len[i++] += econet_rreg(&c->rx.g2.f_65_127);
		port->stats.rx_len[i++] += econet_rreg(&c->rx.g2.f_128_255);
		port->stats.rx_len[i++] += econet_rreg(&c->rx.g2.f_256_511);
		port->stats.rx_len[i++] += econet_rreg(&c->rx.g2.f_512_1023);
		port->stats.rx_len[i++] += econet_rreg(&c->rx.g2.f_1024_1518);
		port->stats.rx_len[i++] += econet_rreg(&c->rx.g2.f_more_1518);
	} else {
		port->stats.rx_drops += econet_rreg(&c->rx.drops_err);
		port->stats.rx_drops += econet_rreg(&c->rx.drops_fc);
		port->stats.rx_drops += econet_rreg(&c->rx.drops_rc);
		port->stats.rx_drops += econet_rreg(&c->rx.drops_overflow);
	}

	set_gdm_cl_cnt_rx(&cc, true);
	set_gdm_cl_cnt_tx(&cc, true);
	econet_wreg(cc, &port->regs->clear_counters);

	u64_stats_update_end(&port->stats.syncp);
}

static void econet_dev_get_stats64(struct net_device *dev,
				 struct rtnl_link_stats64 *storage)
{
	struct econet_gdm_port *port = netdev_priv(dev);
	unsigned int start;

	econet_update_hw_stats(port);
	do {
		start = u64_stats_fetch_begin(&port->stats.syncp);
		storage->rx_packets = port->stats.rx_ok_pkts;
		storage->tx_packets = port->stats.tx_ok_pkts;
		storage->rx_bytes = port->stats.rx_ok_bytes;
		storage->tx_bytes = port->stats.tx_ok_bytes;
		storage->rx_errors = port->stats.rx_errors;
		storage->rx_dropped = port->stats.rx_drops;
		storage->tx_dropped = port->stats.tx_drops;
		storage->rx_over_errors = port->stats.rx_over_errors;
		if (port->stats.g2_stats) {
			storage->multicast = port->stats.rx_multicast;
			storage->rx_crc_errors = port->stats.rx_crc_error;
		}
	} while (u64_stats_fetch_retry(&port->stats.syncp, start));
}

static void econet_ethtool_get_mac_stats(struct net_device *dev,
				       struct ethtool_eth_mac_stats *stats)
{
	struct econet_gdm_port *port = netdev_priv(dev);
	unsigned int start;

	econet_update_hw_stats(port);

	if (!port->stats.g2_stats)
		return;

	do {
		start = u64_stats_fetch_begin(&port->stats.syncp);
		stats->MulticastFramesXmittedOK = port->stats.tx_multicast;
		stats->BroadcastFramesXmittedOK = port->stats.tx_broadcast;
		stats->BroadcastFramesReceivedOK = port->stats.rx_broadcast;
	} while (u64_stats_fetch_retry(&port->stats.syncp, start));
}

static const struct ethtool_rmon_hist_range econet_ethtool_rmon_ranges[] = {
	{    0,    64 },
	{   65,   127 },
	{  128,   255 },
	{  256,   511 },
	{  512,  1023 },
	{ 1024,  1518 },
	{ 1519, 10239 },
	{},
};

static void
econet_ethtool_get_rmon_stats(struct net_device *dev,
			    struct ethtool_rmon_stats *stats,
			    const struct ethtool_rmon_hist_range **ranges)
{
	struct econet_gdm_port *port = netdev_priv(dev);
	struct econet_hw_stats *hw_stats = &port->stats;
	unsigned int start;

	BUILD_BUG_ON(ARRAY_SIZE(econet_ethtool_rmon_ranges) !=
		     ARRAY_SIZE(hw_stats->tx_len) + 1);
	BUILD_BUG_ON(ARRAY_SIZE(econet_ethtool_rmon_ranges) !=
		     ARRAY_SIZE(hw_stats->rx_len) + 1);

	*ranges = econet_ethtool_rmon_ranges;
	econet_update_hw_stats(port);

	if (!port->stats.g2_stats)
		return;

	do {
		int i;

		start = u64_stats_fetch_begin(&port->stats.syncp);
		stats->fragments = hw_stats->rx_fragment;
		stats->jabbers = hw_stats->rx_jabber;
		for (i = 0; i < ARRAY_SIZE(econet_ethtool_rmon_ranges) - 1;
		     i++) {
			stats->hist[i] = hw_stats->rx_len[i];
			stats->hist_tx[i] = hw_stats->tx_len[i];
		}
	} while (u64_stats_fetch_retry(&port->stats.syncp, start));
}

static int econet_dev_setup_tc(struct net_device *dev,
			       enum tc_setup_type type, void *type_data)
{
	struct econet_gdm_port *port = netdev_priv(dev);

	return airoha_ppe_dev_setup_tc(port->common.ppe, dev, type,
				       type_data);
}

static const struct net_device_ops econet_netdev_ops = {
	.ndo_init		= econet_dev_init,
	.ndo_open		= econet_dev_open,
	.ndo_stop		= econet_dev_stop,
	.ndo_change_mtu		= econet_dev_change_mtu,
	.ndo_start_xmit		= econet_dev_xmit,
	.ndo_get_stats64        = econet_dev_get_stats64,
	.ndo_set_mac_address	= econet_dev_set_macaddr,
	.ndo_setup_tc		= econet_dev_setup_tc,
};

static int econet_ethtool_get_link_ksettings(struct net_device *dev,
					     struct ethtool_link_ksettings *cmd)
{
	struct econet_gdm_port *port = netdev_priv(dev);

	return phylink_ethtool_ksettings_get(port->common.phylink, cmd);
}

static int econet_ethtool_set_link_ksettings(struct net_device *dev,
					     const struct ethtool_link_ksettings *cmd)
{
	struct econet_gdm_port *port = netdev_priv(dev);

	return phylink_ethtool_ksettings_set(port->common.phylink, cmd);
}

static int econet_ethtool_nway_reset(struct net_device *dev)
{
	struct econet_gdm_port *port = netdev_priv(dev);

	return phylink_ethtool_nway_reset(port->common.phylink);
}

static const struct ethtool_ops econet_ethtool_ops = {
	.get_drvinfo		= airoha_eth_get_drvinfo,
	.get_link		= ethtool_op_get_link,
	.get_link_ksettings	= econet_ethtool_get_link_ksettings,
	.set_link_ksettings	= econet_ethtool_set_link_ksettings,
	.nway_reset		= econet_ethtool_nway_reset,
	.get_eth_mac_stats      = econet_ethtool_get_mac_stats,
	.get_rmon_stats		= econet_ethtool_get_rmon_stats,
};

static int econet_setup_phylink(struct econet_gdm_port *port,
				struct device_node *np)
{
	struct phylink_config *config = &port->common.phylink_config;
	phy_interface_t phy_mode;
	int err;

	err = of_get_phy_mode(np, &phy_mode);
	if (err)
		return dev_err_probe(port->eth->dev, err,
				     "GDM%u has no valid phy-mode\n",
				     port->common.id);

	config->mac_capabilities = MAC_ASYM_PAUSE | MAC_SYM_PAUSE |
				   MAC_10 | MAC_100 | MAC_1000 |
				   MAC_2500FD | MAC_5000FD | MAC_10000FD;
	__set_bit(PHY_INTERFACE_MODE_INTERNAL, config->supported_interfaces);
	__set_bit(PHY_INTERFACE_MODE_MII, config->supported_interfaces);
	__set_bit(PHY_INTERFACE_MODE_GMII, config->supported_interfaces);
	__set_bit(PHY_INTERFACE_MODE_RGMII, config->supported_interfaces);
	__set_bit(PHY_INTERFACE_MODE_RGMII_ID, config->supported_interfaces);
	__set_bit(PHY_INTERFACE_MODE_RGMII_RXID, config->supported_interfaces);
	__set_bit(PHY_INTERFACE_MODE_RGMII_TXID, config->supported_interfaces);

	return airoha_gdm_phylink_create(&port->common, np, phy_mode);
}

struct net_device *econet_alloc_gdm_port(struct econet_eth *eth,
				       struct device_node *np,
				       struct gdm __iomem *regs,
				       struct econet_qdma *qdma,
				       enum etx_fport fport,
				       bool has_g2_stats)
{
	struct econet_gdm_port *port;
	struct net_device *ndev;
	int err;

	ndev = devm_alloc_etherdev_mqs(eth->dev, sizeof(*port),
				       ECONET_NUM_SOFT_QUEUES,
				       ECONET_NUM_SOFT_QUEUES);
	if (!ndev) {
		dev_err(eth->dev, "alloc_etherdev failed\n");
		return ERR_PTR(-ENOMEM);
	}

	ndev->netdev_ops = &econet_netdev_ops;
	ndev->ethtool_ops = &econet_ethtool_ops;
	ndev->max_mtu = SKB_WITH_OVERHEAD(ECONET_MAX_PACKET_SIZE) -
			ETH_HLEN - ETH_FCS_LEN;
	ndev->watchdog_timeo = 5 * HZ;

	/*
	 * This driver is meant to forward packets, not send/receive them.
	 * Therefore most features are not that useful.
	 *
	 * NETIF_F_TSO | NETIF_F_TSO6:
	 *   - Supported in EN751627, not in EN751221, not used in forwarding.
	 *
	 * NETIF_F_IP_CSUM | NETIF_F_RXCSUM | NETIF_F_IPV6_CSUM:
	 *   - Supported but disabled for simplicity.
	 *
	 * NETIF_F_HW_TC:
	 *   - TODO: When we get the PPE working, we will enable this for
	 *           TC_SETUP_BLOCK / TC_SETUP_FT for NAT flow-table offloading.
	 */
	ndev->hw_features = 0;
	if (eth->ppe && eth->ppe->enabled)
		ndev->hw_features |= NETIF_F_HW_TC;

	ndev->features |= ndev->hw_features;
	ndev->vlan_features = ndev->hw_features;
	ndev->dev.of_node = of_node_get(np);
	SET_NETDEV_DEV(ndev, eth->dev);

	err = airoha_eth_init_mac_address(eth->dev, np, ndev);
	if (err)
		return ERR_PTR(err);

	port = netdev_priv(ndev);
	airoha_gdm_common_init(&port->common, ndev,
			       AIROHA_ETH_FAMILY_ECONET, fport,
			       fport == ETX_FPORT_GDM2 ? DPORT_GDMA2 :
						      DPORT_GDMA1,
			       port, NULL);
	port->common.ppe = eth->ppe;
	u64_stats_init(&port->stats.syncp);
	spin_lock_init(&port->stats.lock);
	spin_lock_init(&port->reg_lock);
	port->stats.g2_stats = has_g2_stats;
	port->dev = ndev;
	port->regs = regs;
	port->fport = fport;
	port->qdma = qdma;
	port->eth = eth;

	err = econet_setup_phylink(port, np);
	if (err)
		goto free_of_node;

	// TODO: This is pretty easy to enable
// 	err = airoha_metadata_dst_alloc(port);
// 	if (err)
// 		return err;

	err = register_netdev(ndev);
	if (err)
		goto free_metadata_dst;

	dev_info(eth->dev, "port %d%s registered with %pM\n",
		fport,
		(fport == ETX_FPORT_GDM1) ? " (LAN)" :
		(fport == ETX_FPORT_GDM2) ? " (WAN)" : "",
		ndev->dev_addr);

	return ndev;

free_metadata_dst:
// 	airoha_metadata_dst_free(port);
	airoha_gdm_phylink_destroy(&port->common);
free_of_node:
	of_node_put(ndev->dev.of_node);
	ndev->dev.of_node = NULL;
	return ERR_PTR(err);
}

/* Frame-engine platform driver. */

struct econet_eth_pvt {
	struct econet_eth			pub;
	const struct econet_soc_data	*soc;
	struct net_device		*ports[ECONET_NUM_GDM_PORTS];
	void __iomem			*fe_base;
	struct gdm __iomem		*gdm[ECONET_NUM_GDM_PORTS];
	void __iomem			*ppe_base;
	struct qregs __iomem		*qdma_regs[ECONET_NUM_QDMA];
	struct reset_control		*reset;
	int				qdma_irq[ECONET_NUM_QDMA * QDMA_NUM_IRQS];
	struct econet_qdma		*qdma[ECONET_NUM_QDMA];
};

#define ECONET_FE_GDM1_OFFSET	0x0400
#define ECONET_FE_PPE_OFFSET	0x0c00
#define ECONET_FE_GDM2_OFFSET	0x1400
#define ECONET_FE_MIN_SIZE	0x2600

static struct net_device *econet_get_sport_dev(struct econet_eth_pvt *eth,
					     enum etx_fport sport)
{
	if (sport == ETX_FPORT_GDM2) {
		return eth->ports[1];
	} else {
		if (sport != ETX_FPORT_GDM1)
			dev_info(eth->pub.dev, "rx: on unexpected fport %d\n", sport);
		return eth->ports[0];
	}
}

int econet_rx_before_recv(struct econet_eth *eth, struct sk_buff *skb,
			enum etx_fport sport)
{
	struct econet_eth_pvt *ep = (struct econet_eth_pvt *) eth;
	struct net_device *port;

	port = econet_get_sport_dev(ep, sport);
	if (!port)
		return -ENODEV;

	skb->dev = port;
	skb->protocol = eth_type_trans(skb, port);

	if (netdev_uses_dsa(port)) {
		/* PPE module requires untagged packets to work
		 * properly and it provides DSA port index via the
		 * DMA descriptor. Report DSA tag to the DSA stack
		 * via skb dst info.
		 */

		// On EN751221 generally, this is not done, but the
		// EN7526C is special cased and it does do this for
		// that one. If we need it, we'll want to do one
		// codepath for everything. For now we'll do nothing
		// and hope that the tag-basd DSA works.
		//port_num = (sp_tag & 0x7); /*switch port id*/
		#if 0
		u32 sptag = get_erx_sp_tag(&desc.t.erx);
		if (sptag < ARRAY_SIZE(port->dsa_meta) &&
			port->dsa_meta[sptag])
			skb_dst_set_noref(q->skb,
						&port->dsa_meta[sptag]->dst);
		#endif
	}

	return 0;
}

static int econet_init_port(struct econet_eth_pvt *eth, struct device_node *np)
{
	struct net_device *dev;
	u32 id;
	int err;

	err = airoha_eth_get_port_id(eth->pub.dev, np, 1,
				     ARRAY_SIZE(eth->ports), &id);
	if (err)
		return err;

	if (eth->ports[id - 1]) {
		dev_err(eth->pub.dev, "duplicate gdm port id: %d\n", id);
		return -EINVAL;
	}

	if (id == 1)
		dev = econet_alloc_gdm_port(&eth->pub, np,
					  eth->gdm[0],
					  eth->qdma[0],
					  ETX_FPORT_GDM1,
					  false);
	else if (id == 2)
		dev = econet_alloc_gdm_port(&eth->pub, np,
					  eth->gdm[1],
					  eth->qdma[1],
					  ETX_FPORT_GDM2,
					  true);
	else
		return -EINVAL;

	if (IS_ERR(dev))
		return PTR_ERR(dev);

	eth->ports[id - 1] = dev;
	return 0;
}

static void econet_prepare_qdma_cfg(struct econet_qdma_cfg *cfg,
				  const struct econet_soc_data *soc)
{
	memset(cfg, 0, sizeof(*cfg));
	for (int i = 0; i < QDMA_NUM_CHAINS; i++)
		cfg->num_rx_descs[i] = 128;
	for (int i = 0; i < QDMA_NUM_CHAINS; i++)
		cfg->num_tx_descs[i] = 128;
	for (int i = 0; i < QDMA_NUM_TX_DONE; i++)
		cfg->done_list_size[i] = 256;
	cfg->fwd_max_packet_size = ECONET_MAX_PACKET_SIZE;
	cfg->fwd_low_threshold = 32;
	cfg->num_fwd_descs = 256;
	cfg->soc = soc;
}

static void econet_eth_remove(struct platform_device *pdev)
{
	struct econet_eth_pvt *eth = platform_get_drvdata(pdev);
	int i;

	if (!eth)
		return;


	for (i = 0; i < ARRAY_SIZE(eth->ports); i++) {
		if (!eth->ports[i])
			continue;

		unregister_netdev(eth->ports[i]);
		airoha_gdm_phylink_destroy(&((struct econet_gdm_port *)
					netdev_priv(eth->ports[i]))->common);
		of_node_put(eth->ports[i]->dev.of_node);
		eth->ports[i]->dev.of_node = NULL;
	}

	airoha_ppe_econet_deinit(eth->pub.ppe);
	eth->pub.ppe = NULL;

	for (i = 0; i < ARRAY_SIZE(eth->qdma); i++)
		if (eth->qdma[i])
			econet_qdma_destroy(eth->qdma[i]);

	platform_set_drvdata(pdev, NULL);
}

static int econet_eth_probe(struct platform_device *pdev)
{
	static const char * const qdma_names[ECONET_NUM_QDMA] = {
		"qdma0", "qdma1",
	};
	struct resource *fe_res;
	struct econet_eth_pvt *eth;
	struct econet_qdma_cfg cfg;
	struct device_node *np;
	void __iomem *fe_base;
	int i, err, irq;

	eth = devm_kzalloc(&pdev->dev, sizeof(*eth), GFP_KERNEL);
	if (!eth)
		return -ENOMEM;

	eth->soc = of_device_get_match_data(&pdev->dev);
	if (!eth->soc)
		return dev_err_probe(&pdev->dev, -EINVAL, "No matching SoC data\n");

	eth->pub.dev = &pdev->dev;
	platform_set_drvdata(pdev, eth);

	err = airoha_eth_set_dma_mask(&pdev->dev);
	if (err)
		return err;

	fe_res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "fe");
	if (!fe_res)
		return dev_err_probe(&pdev->dev, -EINVAL,
				     "missing fe register resource\n");
	if (resource_size(fe_res) < ECONET_FE_MIN_SIZE)
		return dev_err_probe(&pdev->dev, -EINVAL,
				     "fe register resource is too small\n");

	fe_base = devm_ioremap_resource(&pdev->dev, fe_res);
	if (IS_ERR(fe_base))
		return dev_err_probe(&pdev->dev, PTR_ERR(fe_base),
				     "failed to map fe registers\n");

	eth->fe_base = fe_base;
	eth->gdm[0] = fe_base + ECONET_FE_GDM1_OFFSET;
	eth->ppe_base = fe_base + ECONET_FE_PPE_OFFSET;
	eth->gdm[1] = fe_base + ECONET_FE_GDM2_OFFSET;

	for (i = 0; i < ARRAY_SIZE(eth->qdma_regs); i++) {
		eth->qdma_regs[i] =
			devm_platform_ioremap_resource_byname(pdev, qdma_names[i]);
		if (IS_ERR(eth->qdma_regs[i]))
			return dev_err_probe(&pdev->dev,
					     PTR_ERR(eth->qdma_regs[i]),
					     "failed to map %s registers\n",
					     qdma_names[i]);
	}

	eth->reset =
		devm_reset_control_array_get_optional_exclusive(&pdev->dev);
	if (IS_ERR(eth->reset))
		return dev_err_probe(&pdev->dev, PTR_ERR(eth->reset),
				     "failed to get resets\n");

	if (eth->reset) {
		err = reset_control_assert(eth->reset);
		if (err)
			return err;

		msleep(20);
		err = reset_control_deassert(eth->reset);
		if (err)
			return err;
	}

	for (i = 0; i < ARRAY_SIZE(eth->qdma_irq); i++) {
		irq = platform_get_irq(pdev, i);
		if (irq < 0)
			return dev_err_probe(&pdev->dev, irq,
					     "failed to get IRQ %d\n", i);
		eth->qdma_irq[i] = irq;
	}

	BUILD_BUG_ON(ARRAY_SIZE(eth->qdma_irq) !=
		     ECONET_NUM_QDMA * QDMA_NUM_IRQS);
	for (i = 0; i < ARRAY_SIZE(eth->qdma); i++) {
		econet_prepare_qdma_cfg(&cfg, eth->soc);
		eth->qdma[i] = econet_qdma_new(&eth->pub, eth->qdma_regs[i], i,
					       &eth->qdma_irq[i * QDMA_NUM_IRQS],
					     QDMA_NUM_IRQS, &cfg);
		if (IS_ERR(eth->qdma[i])) {
			err = PTR_ERR(eth->qdma[i]);
			eth->qdma[i] = NULL;
			goto error;
		}
	}

	eth->pub.ppe = airoha_ppe_econet_init(&pdev->dev,
					      pdev->dev.of_node,
					      eth->fe_base,
					      eth->ppe_base);
	if (IS_ERR(eth->pub.ppe)) {
		err = PTR_ERR(eth->pub.ppe);
		eth->pub.ppe = NULL;
		goto error;
	}

	for_each_available_child_of_node(pdev->dev.of_node, np) {
		if (!of_device_is_compatible(np, "econet,eth-mac"))
			continue;

		err = econet_init_port(eth, np);
		if (err) {
			of_node_put(np);
			goto error;
		}
	}

	return 0;

error:
	econet_eth_remove(pdev);
	return err;
}

static const struct econet_soc_data en751221_soc_data = {
	.dscp_byte_swap = true,
	.en751221_special_tag = true,
};

static const struct econet_soc_data en7528_soc_data = {
	.dscp_byte_swap = false,
};

static const struct of_device_id of_econet_match[] = {
	{ .compatible = "econet,en751221-eth", .data = &en751221_soc_data },
	{ .compatible = "econet,en7528-eth", .data = &en7528_soc_data },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, of_econet_match);

static struct platform_driver econet_eth_driver = {
	.probe = econet_eth_probe,
	.remove = econet_eth_remove,
	.driver = {
		.name = KBUILD_MODNAME,
		.of_match_table = of_econet_match,
	},
};
module_platform_driver(econet_eth_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Caleb James DeLisle <cjd@cjdns.fr>");
MODULE_AUTHOR("Matheus Sampaio Queiroga <srherobrine20@gmail.com>");
MODULE_DESCRIPTION("Ethernet driver for EcoNet SoC");
