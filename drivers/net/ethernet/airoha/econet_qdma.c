// SPDX-License-Identifier: GPL-2.0-only
#include <linux/spinlock.h>
#include <linux/netdevice.h>
#include <net/page_pool/helpers.h>
#include <linux/skbuff.h>
#include <linux/etherdevice.h>
#include <net/dsa.h>
#include <linux/platform_device.h>
#include <linux/of_reserved_mem.h>

#include "asm-generic/int-ll64.h"
#include "linux/bitmap.h"
#include "linux/dev_printk.h"
#include "linux/types.h"
#include "econet_qdma_desc.h"
#include "econet_qdma_regs.h"
#include "econet_eth.h"

/* The non-dma part of RX packet descriptor */
struct en75_q_rx_ent {
	void				*buf;
	dma_addr_t			dma_addr;
	u16				dma_len;
};

struct en75_q_rx {
	/* No lock, access only in NAPI, or else when NAPI is disabled
	 * and qdma->lock is held */
	struct en75_q_rx_ent		*entry;
	struct desc			*desc;
	u16				cpu_i;

	/* Not modified after init */
	struct en75_qdma		*qdma;
	struct qchain_regs __iomem	*qchain_regs;
	int				ndesc;
	int				buf_size;
	struct napi_struct		napi;
	struct page_pool		*page_pool;
};

/* The non-dma part of TX packet descriptor */
struct en75_q_tx_ent {
	struct sk_buff			*skb;
	dma_addr_t			dma_addr;
	u16				dma_len;
	u16				freelist_next;
};

struct en75_q_tx {
	/* protect concurrent queue accesses
	 * use _bh unless in napi poll */
	spinlock_t			lock_bh;
	struct en75_q_tx_ent		*entry;
	struct desc			*desc;
	u32				*debug_tx_cnt;

	/* FIFO of free entries because they complete out of order. */
	u16				freelist_head;
	u16				freelist_tail;

	/* Not modified after init */
	struct en75_qdma		*qdma;
	struct qchain_regs __iomem	*qchain_regs;
	int				ndesc;
	struct napi_struct		napi;
};

struct en75_irq {
	/* protect concurrent irqmask accesses
	 * use _irqsave unless in irq handler */
	spinlock_t 			lock_irq;
	u32 				irqmask[QDMA_REGS_PER_IRQ];
	u32 __iomem 			*mask_reg[QDMA_REGS_PER_IRQ];
	u32 __iomem 			*status_reg[QDMA_REGS_PER_IRQ];
	u32				*debug_int_cnt;

	/* Not modified after init */
	struct en75_qdma 		*qdma;
	int 				irq;
};

struct en75_tx_doneq {
	/* No lock, access only in NAPI */
	u32 				*q;
	struct qregs_doneq __iomem	*regs;

	/* Not modified after init */
	struct en75_qdma 		*qdma;
	int 				size;
	struct napi_struct 		napi;
};

struct en75_qdma {
	/* Protects register programming and users. */
	struct mutex			lock;
	struct qregs __iomem		*regs;
	int				users;

	/* Synchronized inside of the structure */
	struct en75_irq 		irqs[QDMA_NUM_IRQS];
	struct en75_tx_doneq 		q_tx_done[QDMA_NUM_TX_DONE];
	struct en75_q_tx 		q_tx[QDMA_NUM_CHAINS];
	struct en75_q_rx 		q_rx[QDMA_NUM_CHAINS];

	/* Not modified after init */
	struct en75_eth 		*eth;
	struct device 			*dev;
	int 				id;
	struct fwdesc 			*hwf_desc;
	struct net_device 		*napi_dev;
	struct en75_qdma_cfg 		cfg;
	const struct en75_soc_data	*soc;
};

static void en75_fill_rx_queue(struct en75_q_rx *q, u32 end_i)
{
	u32 ndesc = q->ndesc;
	u32 cpu_i = (q->cpu_i + 1) % ndesc;

	for (; cpu_i != end_i; cpu_i = (cpu_i + 1) % ndesc) {
		struct en75_q_rx_ent *e = &q->entry[cpu_i];
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
	en75_wreg(cpu_i, &q->qchain_regs->rx_cpui);
}

static void en75_qdma_rx_process_one(struct en75_q_rx *q, u32 cpu_i,
				     enum dma_data_direction dir)
{
	struct en75_q_rx_ent *e = &q->entry[cpu_i];
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

	/* TODO: When we begin supporting the PPE, we will handle
	 *       PPE_CPU_REASON_HIT_UNBIND_RATE_REACHED here. */

	sport = get_erx_sport(&desc.msg.erx);
	if (en75_rx_before_recv(q->qdma->eth, skb, sport))
		dev_kfree_skb(skb);
	else
		napi_gro_receive(&q->napi, skb);

	return;

return_page:
	page = virt_to_head_page(e->buf);
	page_pool_put_full_page(q->page_pool, page, false);
}

static int en75_qdma_rx_process(struct en75_q_rx *q, int budget)
{
	enum dma_data_direction dir = page_pool_get_dma_dir(q->page_pool);
	u32 hardware_i = en75_rreg(&q->qchain_regs->rx_hwi);
	int ndesc = q->ndesc;
	u32 cpu_i;
	int done;

	/* The stored value of cpu_i is the last entry actually handled.
	 * Whereas hardware_i is the next entry that has not yet been received.
	 */
	cpu_i = (q->cpu_i + 1) % ndesc;

	for (done = 0; done < budget && cpu_i != hardware_i; done++) {
		en75_qdma_rx_process_one(q, cpu_i, dir);
		cpu_i = (cpu_i + 1) % ndesc;
	}

	en75_fill_rx_queue(q, cpu_i);

	return done;
}

#define IRQ_PURPOSE(t, s, c) \
	((union en75_irq_purpose){ .type = IPS_ ## t, .source = IPSC_ ## s, .chain = (c) })

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

static const union en75_irq_purpose EN751221_IRQ_MAP[] = {
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

static union en75_irq_purpose en751221_irq_purpose(union irq_bit bit)
{
	if (WARN_ON_ONCE(!en751221_valid_irq_bit(bit)))
		return (union en75_irq_purpose){0};

	return EN751221_IRQ_MAP[bit.bit_idx];
}

static union irq_bit en751221_irq_bit(union en75_irq_purpose purpose)
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

static void en75_qdma_set_irqmask(struct en75_qdma *qdma, union irq_bit b, bool enable)
{
	struct en75_irq *irq;

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

	en75_wreg(irq->irqmask[b.reg_idx], irq->mask_reg[b.reg_idx]);

	/* Read irq_enable register in order to guarantee the update above
	 * completes in the spinlock critical section.
	 */
	en75_rreg(irq->mask_reg[b.reg_idx]);
}

static int en75_qdma_rx_napi_poll(struct napi_struct *napi, int budget)
{
	struct en75_q_rx *q = container_of(napi, struct en75_q_rx, napi);
	struct en75_qdma *qdma = q->qdma;
	int cur, done = 0;

	do {
		cur = en75_qdma_rx_process(q, budget - done);
		done += cur;
	} while (cur && done < budget);

	if (done < budget && napi_complete(napi)) {
		union en75_irq_purpose purpose;
		union irq_bit b;

		purpose = IRQ_PURPOSE(DONE, RX, q - &qdma->q_rx[0]);
		b = en751221_irq_bit(purpose);
		en75_qdma_set_irqmask(qdma, b, true);
	}

	return done;
}

static irqreturn_t en75_irq_handler(int irq_num, void *dev_instance)
{
	struct en75_irq *irq = dev_instance;
	struct en75_qdma *qdma = irq->qdma;
	int i;

	guard(spinlock)(&irq->lock_irq);

	for (i = 0; i < ARRAY_SIZE(irq->irqmask); i++) {
		unsigned long regval;
		u32 disable_int = 0;
		u8 bit;

		regval = en75_rreg(irq->status_reg[i]);
		regval &= irq->irqmask[i];

		/* You must write the bits back to the status register
		 * or you will keep receiving the same interrupt. */
		en75_wreg((u32)regval, irq->status_reg[i]);

		for_each_set_bit(bit, &regval, 32) {
			union en75_irq_purpose p;

			if (irq->debug_int_cnt)
				irq->debug_int_cnt[i * 32 + bit]++;

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
					    en75_irq_purpose_type_str(p.type),
					    en75_irq_purpose_source_str(p.source),
					    p.chain);
		}

		irq->irqmask[i] &= ~disable_int;
		en75_wreg(irq->irqmask[i], irq->mask_reg[i]);
	}

	return IRQ_HANDLED;
}

#define IRQ_RING_IDX_MASK		GENMASK(20, 16)
#define IRQ_DESC_IDX_MASK		GENMASK(15, 0)

static int en75_poll_tx_complete(struct napi_struct *napi, int budget)
{
	struct qregs_doneq_state state;
	struct en75_tx_doneq *done_q;
	struct en75_qdma *qdma;
	int id, irq_queued;
	u32 done = 0, head;

	done_q = container_of(napi, struct en75_tx_doneq, napi);
	qdma = done_q->qdma;
	id = done_q - &qdma->q_tx_done[0];

	state = en75_rreg(&done_q->regs->state);
	head = get_qregs_doneq_state_head_index(&state);
	head = head % done_q->size;
	irq_queued = get_qregs_doneq_state_length(&state);

	while (irq_queued > 0 && done < budget) {
		u32 index, qid, val = done_q->q[head];
		struct en75_q_tx_ent *e;
		struct en75_q_tx *q;
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
			en75_rreg(&qdma->regs->done_queue.pop_back);
			en75_wreg(0x80U, &qdma->regs->done_queue.pop_back);
		}
		en75_rreg(&qdma->regs->done_queue.pop_back);
		en75_wreg(done & 0x7f, &qdma->regs->done_queue.pop_back);
	}

	if (done < budget && napi_complete(napi)) {
		union en75_irq_purpose purpose = IRQ_PURPOSE(DONE, TX, id);
		union irq_bit b = en751221_irq_bit(purpose);

		en75_qdma_set_irqmask(qdma, b, true);
	}

	return done;
}

int en75_qdma_xmit(struct en75_qdma *qdma, struct sk_buff *skb,
		   union desc_msg *msg, int qid)
{
	struct en75_q_tx *q = &qdma->q_tx[qid];
	int len = skb_headlen(skb);
	struct en75_q_tx_ent *e;
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

	en75_wreg((u32)next_index, &q->qchain_regs->tx_cpui);

	if (q->debug_tx_cnt) {
		int chq;

		chq = get_etx_channel(&msg->etx) % EN75_NUM_CHANNELS;
		chq *= EN75_NUM_QUEUES;
		chq += get_etx_queue(&msg->etx) % EN75_NUM_QUEUES;
		q->debug_tx_cnt[chq]++;
	}

	return q->entry[next_index].freelist_next == 0xffff ?
		EBUSY : 0;
}

/* Init functions, no locks, assumed non-concurrent */

static int en75_init_rx_queue(struct en75_q_rx *q,
			      struct en75_qdma *qdma, int ndesc)
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

	q->buf_size = EN75_MAX_PACKET_SIZE;
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

	netif_napi_add(qdma->napi_dev, &q->napi, en75_qdma_rx_napi_poll);

	en75_wreg(lower_32_bits(dma_addr), &q->qchain_regs->rxbase);

	/* en75_fill_rx_queue fills everything from current cpu index + 1 up to
	 * but excluding end_i, so to fill every entry we run it with 0 to do
	 * 1,2,3,[...],n, then we run it with 1 to do 0. */
	en75_fill_rx_queue(q, 0);
	en75_fill_rx_queue(q, 1);
	for (int i = 0; i < q->ndesc; i++)
		if (!q->entry[i].dma_addr)
			return -ENOMEM;

	/* The RX hardware side considers that hwi == cpui means the queue is
	 * full, not empty. Since cpui starts at 0, we initialize hwi to 1. */
	en75_wreg(1U, &q->qchain_regs->rx_hwi);

	rrs = en75_rreg(&qdma->regs->rxring_size);
	rrl = en75_rreg(&qdma->regs->rxring_low);

	if (q == &qdma->q_rx[0]) {
		set_qregs_rxring_size_ring0(&rrs, ndesc);
		set_qregs_rxring_low_ring0(&rrl, threshold);
	} else {
		set_qregs_rxring_size_ring1(&rrs, ndesc);
		set_qregs_rxring_low_ring1(&rrl, threshold);
	}

	en75_wreg(rrs, &qdma->regs->rxring_size);
	en75_wreg(rrl, &qdma->regs->rxring_low);

	return 0;
}

static int en75_init_irqs(struct device *dev, struct en75_qdma *qdma,
			  int *irqs, int num_irqs)
{
	int i;

	if (num_irqs < ARRAY_SIZE(qdma->irqs))
		return -EINVAL;

	for (i = 0; i < ARRAY_SIZE(qdma->irqs); i++) {
		struct en75_irq *irq = &qdma->irqs[i];
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
				       en75_irq_handler, IRQF_SHARED, name,
				       irq);
		if (err)
			return err;
	}

	return 0;
}

static int en75_init_tx_doneq(struct en75_tx_doneq *done_q,
			      struct en75_qdma *qdma, int size)
{
	dma_addr_t dma_addr;

	netif_napi_add_tx(qdma->napi_dev, &done_q->napi,
			  en75_poll_tx_complete);
	done_q->q = dmam_alloc_coherent(qdma->dev, size * sizeof(u32),
				       &dma_addr, GFP_KERNEL);
	if (!done_q->q)
		return -ENOMEM;

	memset(done_q->q, 0xff, size * sizeof(u32));
	done_q->size = size;
	done_q->qdma = qdma;
	done_q->regs = &qdma->regs->done_queue;

	en75_wreg(lower_32_bits(dma_addr), &qdma->regs->done_queue.address);
	struct qregs_doneq_cfg cfg = en75_rreg(&qdma->regs->done_queue.config);
	set_qregs_doneq_cfg_size(&cfg, size);
	set_qregs_doneq_cfg_int_threshold(&cfg, 1);
	en75_wreg(cfg, &qdma->regs->done_queue.config);

	return 0;
}

static int en75_init_tx_queue(struct en75_q_tx *q,
			      struct en75_qdma *qdma, int size)
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

	en75_wreg(lower_32_bits(dma_addr), &q->qchain_regs->txbase);

	/* On TX, hardware advances hwi until it is equal to cpui. */
	en75_wreg(0U, &q->qchain_regs->tx_cpui);
	en75_wreg(0U, &q->qchain_regs->tx_hwi);

	return 0;
}

static int en75_init_hw_fwd(struct en75_qdma *qdma)
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
		dma_addr = rmem->base;
		/* Compute the number of hw descriptors according to the
		 * reserved memory size and the payload buffer size
		 */
		num_desc = div_u64(rmem->size, buf_size);

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

	en75_wreg(lower_32_bits(dma_addr), &qdma->regs->hwf_data_addr);

	size = num_desc * sizeof(*qdma->hwf_desc);
	qdma->hwf_desc = dmam_alloc_coherent(qdma->dev, size, &dma_addr, GFP_KERNEL);
	if (!qdma->hwf_desc)
		return -ENOMEM;

	en75_wreg(lower_32_bits(dma_addr), &qdma->regs->hwf_desc_addr);

	cfg = en75_rreg(&qdma->regs->hwf_cfg);
	set_qregs_hwf_cfg_pkt_sz(&cfg, buf_size_cfg);
	set_qregs_hwf_cfg_low_th(&cfg, qdma->cfg.fwd_low_threshold);
	en75_wreg(cfg, &qdma->regs->hwf_cfg);

	cfg1 = en75_rreg(&qdma->regs->hwf_cfg1);
	set_qregs_hwf_cfg1_fwd_desc_n(&cfg1, num_desc);
	set_qregs_hwf_cfg1_overhead(&cfg1, 0x14);
	set_qregs_hwf_cfg1_start(&cfg1, true);
	en75_wreg(cfg1, &qdma->regs->hwf_cfg1);

	ret = read_poll_timeout(en75_rreg, cfg1,
				!is_qregs_hwf_cfg1_start(&cfg1), USEC_PER_MSEC,
				30 * USEC_PER_MSEC, true,
				&qdma->regs->hwf_cfg1);
	if (ret)
		dev_err(qdma->dev,
			"Error %pe waiting for HW forwarding engine to start",
			ERR_PTR(ret));
	return ret;
}

static int en75_init_final(struct en75_qdma *qdma)
{
	struct qregs_qcfg qcfg;
	int i;

	for (i = 0; i < ARRAY_SIZE(qdma->irqs); i++) {
		int j;

		/* clear pending irqs */
		for (j = 0; j < ARRAY_SIZE(qdma->irqs[i].status_reg); j++)
			en75_wreg(0xffffffffU, qdma->irqs[i].status_reg[j]);

		/* enable IRQs */
		for (j = 0; j < ARRAY_SIZE(qdma->irqs[i].irqmask); j++) {
			u32 mask = 0;

			for (int k = 0; k < 32; k++) {
				union en75_irq_purpose p;
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
			en75_wreg(mask, qdma->irqs[i].mask_reg[j]);
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
	en75_wreg(qcfg, &qdma->regs->qdma_cfg);

	en75_wreg(0U, &qdma->regs->rx_int_delay);

	struct qregs_tx_congest_cfg cngst_cfg = {0};
	set_qregs_tx_congest_cfg_tail_drop_en(&cngst_cfg, true);
	set_qregs_tx_congest_cfg_dei_drop_en(&cngst_cfg, true);
	en75_wreg(cngst_cfg, &qdma->regs->tx_congest_cfg);

	return 0;
}

static int en75_init(struct device *dev,
		     struct en75_qdma *qdma,
		     int *irqs,
		     int num_irqs)
{
	int err;
	int i;

	/* Make sure RX and TX are shutdown */
	en75_wreg((struct qregs_qcfg) { 0 }, &qdma->regs->qdma_cfg);

	err = en75_init_irqs(dev, qdma, irqs, num_irqs);
	if (err)
		return err;

	for (i = 0; i < ARRAY_SIZE(qdma->q_rx); i++) {
		err = en75_init_rx_queue(&qdma->q_rx[i], qdma,
					 qdma->cfg.num_rx_descs[i]);
		if (err)
			return err;
	}

	for (i = 0; i < ARRAY_SIZE(qdma->q_tx_done); i++) {
		err = en75_init_tx_doneq(&qdma->q_tx_done[i], qdma,
					 qdma->cfg.done_list_size[i]);
		if (err)
			return err;
	}

	for (i = 0; i < ARRAY_SIZE(qdma->q_tx); i++) {
		err = en75_init_tx_queue(&qdma->q_tx[i], qdma,
					 qdma->cfg.num_tx_descs[i]);
		if (err)
			return err;
	}

	err = en75_init_hw_fwd(qdma);
	if (err)
		return err;

	return en75_init_final(qdma);
}

/* End init functions */

static void en75_qdma_destroy_rxq_locked(struct en75_q_rx *q)
{
	int i;

	if (!q->page_pool)
		return;

	for (i = 0; i < q->ndesc; i++) {
		struct en75_q_rx_ent *e = &q->entry[i];
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

static int en75_qdma_destroy_locked(struct en75_qdma *qdma)
{
	struct qregs_qcfg qcfg;
	int i;

	if (WARN_ON_ONCE(qdma->users))
		return -EBUSY;

	qcfg = en75_rreg(&qdma->regs->qdma_cfg);
	set_qregs_qcfg_irq_en(&qcfg, false);
	set_qregs_qcfg_rx_dma_en(&qcfg, false);
	set_qregs_qcfg_tx_dma_en(&qcfg, false);
	en75_wreg(qcfg, &qdma->regs->qdma_cfg);

	for (i = 0; i < ARRAY_SIZE(qdma->irqs); i++) {
		struct en75_irq *irq = &qdma->irqs[i];
		int j;

		for (j = 0; j < ARRAY_SIZE(irq->irqmask); j++) {
			irq->irqmask[j] = 0;
			if (irq->mask_reg[j])
				en75_wreg(0U, irq->mask_reg[j]);
		}

		if (irq->irq > 0)
			synchronize_irq(irq->irq);
	}

	for (i = 0; i < ARRAY_SIZE(qdma->q_rx); i++) {
		struct en75_q_rx *q = &qdma->q_rx[i];

		en75_qdma_destroy_rxq_locked(q);
		if (q->napi.dev)
			netif_napi_del(&q->napi);
	}

	for (i = 0; i < ARRAY_SIZE(qdma->q_tx_done); i++) {
		struct en75_tx_doneq *q = &qdma->q_tx_done[i];

		if (q->napi.dev)
			netif_napi_del(&q->napi);
	}

	if (qdma->napi_dev) {
		free_netdev(qdma->napi_dev);
		qdma->napi_dev = NULL;
	}

	return 0;
}

int en75_qdma_destroy(struct en75_qdma *qdma)
{
	guard(mutex)(&qdma->lock);

	return en75_qdma_destroy_locked(qdma);
}

int en75_qdma_use(struct en75_qdma *qdma)
{
	struct qregs_qcfg qcfg;
	int i;

	guard(mutex)(&qdma->lock);

	if (qdma->users++ > 0)
		return 0;

	qcfg = en75_rreg(&qdma->regs->qdma_cfg);
	set_qregs_qcfg_rx_dma_en(&qcfg, true);
	set_qregs_qcfg_tx_dma_en(&qcfg, true);
	en75_wreg(qcfg, &qdma->regs->qdma_cfg);

	for (i = 0; i < ARRAY_SIZE(qdma->q_rx); i++)
		napi_enable(&qdma->q_rx[i].napi);

	for (i = 0; i < ARRAY_SIZE(qdma->q_tx_done); i++)
		napi_enable(&qdma->q_tx_done[i].napi);

	return 0;
}

int en75_qdma_unuse(struct en75_qdma *qdma)
{
	struct qregs_qcfg qcfg;
	int i, j;

	guard(mutex)(&qdma->lock);

	if (WARN_ON_ONCE(qdma->users <= 0))
		return -EINVAL;
	if (--qdma->users > 0)
		return 0;

	qcfg = en75_rreg(&qdma->regs->qdma_cfg);
	set_qregs_qcfg_rx_dma_en(&qcfg, false);
	set_qregs_qcfg_tx_dma_en(&qcfg, false);
	en75_wreg(qcfg, &qdma->regs->qdma_cfg);

	for (i = 0; i < ARRAY_SIZE(qdma->q_rx); i++)
		napi_disable(&qdma->q_rx[i].napi);

	for (i = 0; i < ARRAY_SIZE(qdma->q_tx_done); i++)
		napi_disable(&qdma->q_tx_done[i].napi);

	for (i = 0; i < ARRAY_SIZE(qdma->q_tx); i++) {
		struct en75_q_tx *q = &qdma->q_tx[i];

		guard(spinlock_bh)(&q->lock_bh);
		for (j = 0; j < q->ndesc; j++) {
			struct en75_q_tx_ent *e = &q->entry[j];

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

struct en75_qdma *en75_qdma_new(struct en75_eth *eth,
				void __iomem *qdma_regs,
				int id,
				int *irqs,
				int num_irqs,
				struct en75_qdma_cfg *cfg,
				struct en75_debug_qdma_conf *debug)
{
	struct en75_qdma *qdma;
	int err;
	int i;

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

	err = en75_init(eth->dev, qdma, irqs, num_irqs);
	if (err) {
		en75_qdma_destroy(qdma);
		return ERR_PTR(err);
	}

	if (!debug)
		return qdma;

	debug->regs = qdma_regs;
	debug->hwf_desc = qdma->hwf_desc;
	BUILD_BUG_ON(ARRAY_SIZE(qdma->q_rx) != ARRAY_SIZE(debug->chains));
	BUILD_BUG_ON(ARRAY_SIZE(qdma->q_tx) != ARRAY_SIZE(debug->chains));
	for (i = 0; i < ARRAY_SIZE(debug->chains); i++) {
		u32 *cntr;

		cntr = devm_kzalloc(eth->dev, sizeof(*cntr) *
					EN75_NUM_CHANNELS * EN75_NUM_QUEUES,
					GFP_KERNEL);
		debug->chains[i].tx_per_ch_q = cntr;
		qdma->q_tx[i].debug_tx_cnt = cntr;

		debug->chains[i].rx_descs = qdma->q_rx[i].desc;
		debug->chains[i].rx_count = qdma->q_rx[i].ndesc;
		debug->chains[i].tx_descs = qdma->q_tx[i].desc;
		debug->chains[i].tx_count = qdma->q_tx[i].ndesc;
	}

	for (i = 0; i < ARRAY_SIZE(debug->irqs); i++) {
		struct en75_debug_qdma_irq_conf *cnf;
		u32 *cntr;
		int j;

		cnf = &debug->irqs[i];
		cntr = devm_kzalloc(eth->dev, sizeof(*cntr) *
				    ARRAY_SIZE(cnf->desc), GFP_KERNEL);
		cnf->counters = cntr;
		qdma->irqs[i].debug_int_cnt = cntr;

		for (j = 0; j < ARRAY_SIZE(cnf->desc); j++) {
			union irq_bit bit = {
				.irq_idx = i,
				.reg_idx = j / 32,
				.bit_idx = j % 32,
			};

			if (en751221_valid_irq_bit(bit))
				cnf->desc[j] = en751221_irq_purpose(bit);
		}
	}

	return qdma;
}
