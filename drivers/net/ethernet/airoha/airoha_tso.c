// SPDX-License-Identifier: GPL-2.0-only
/* EcoNet QDMA TSO v1 and EN7523 PDMA TSO v2. */
#include <linux/delay.h>
#include <linux/ethtool.h>
#include <linux/hrtimer.h>
#include <linux/interrupt.h>
#include <linux/ip.h>
#include <linux/ipv6.h>
#include <linux/moduleparam.h>
#include <linux/reset.h>
#include <linux/tcp.h>
#include <net/ip.h>
#include <net/ipv6.h>

#include "airoha_tso.h"
#include "airoha_regs.h"

#define TSO_RINGS		2
#define TSO_DESCS		1024
#define TSO_RING_BASE(n)	(0x800 + (n) * 0x10)
#define TSO_RING_SIZE(n)	(0x804 + (n) * 0x10)
#define TSO_RING_CPU(n)		(0x808 + (n) * 0x10)
#define TSO_RING_DMA(n)		(0x80c + (n) * 0x10)
#define TSO_GLO_CFG		0xa04
#define TSO_TX_ENABLE		BIT(0)
#define TSO_DELAY_INT		0xa0c
#define TSO_INT_STATUS		0xa20
#define TSO_INT_MASK		0xa28
#define TSO_TX_DONE		GENMASK(1, 0)
#define TSO_POLL_NS		(100 * NSEC_PER_USEC)
#define TSO_IRQ_GUARD_NS		NSEC_PER_MSEC

enum airoha_tso_stat {
	TSO_STAT_IRQ_MODE,
	TSO_STAT_IRQ_EVENTS,
	TSO_STAT_IRQ_TX0,
	TSO_STAT_IRQ_TX1,
	TSO_STAT_TIMER_FIRES,
	TSO_STAT_IRQ_RECLAIMED,
	TSO_STAT_POLL_RECLAIMED,
	TSO_STAT_TX_PACKETS,
	TSO_STAT_GSO_PACKETS,
	TSO_STAT_COUNT,
};

static const char airoha_tso_stat_names[TSO_STAT_COUNT][ETH_GSTRING_LEN] = {
	"pdma_irq_mode",
	"pdma_irq_events",
	"pdma_irq_tx0",
	"pdma_irq_tx1",
	"pdma_timer_fires",
	"pdma_irq_reclaimed",
	"pdma_poll_reclaimed",
	"pdma_tx_packets",
	"pdma_gso_packets",
};

struct airoha_tso_entry {
	dma_addr_t dma;
	u32 len;
	bool page;
	struct sk_buff *skb;
};

struct airoha_tso_ring {
	/* Protect descriptor ownership and producer/reclaim indices. */
	spinlock_t lock;
	struct airoha_qdma_desc *desc;
	dma_addr_t dma;
	struct airoha_tso_entry *entry;
	u16 head, tail, queued;
};

struct airoha_tso {
	struct airoha_eth *eth;
	struct reset_control *rst;
	struct airoha_tso_ring ring[TSO_RINGS];
	struct napi_struct napi;
	struct hrtimer timer;
	/* Serialize flush and reset of the shared PDMA engine. */
	struct mutex mutex;
	/* Serialize IRQ masking, NAPI scheduling and stop/restart. */
	spinlock_t irq_lock;
	atomic64_t stats[TSO_STAT_COUNT];
	int irq;
	bool poll_from_irq;
	bool running;
	bool napi_enabled;
	bool programmed;
	u8 next_ring;
};

int airoha_tso_get_sset_count(struct airoha_eth *eth)
{
	return eth->tso ? TSO_STAT_COUNT : 0;
}

void airoha_tso_get_strings(struct airoha_eth *eth, u8 *data)
{
	if (eth->tso)
		memcpy(data, airoha_tso_stat_names, sizeof(airoha_tso_stat_names));
}

void airoha_tso_get_stats(struct airoha_eth *eth, u64 *data)
{
	int i;

	if (!eth->tso)
		return;
	for (i = 0; i < TSO_STAT_COUNT; i++)
		data[i] = atomic64_read(&eth->tso->stats[i]);
}

enum airoha_tso_version airoha_tso_version(struct airoha_eth *eth)
{
	if (!IS_ENABLED(CONFIG_NET_AIROHA_TSO))
		return AIROHA_TSO_NONE;

	switch (get_airoha_target(eth)) {
	case econet_en751627:
	case econet_en7528:
	case econet_en7580:
		return AIROHA_TSO_V1;
	case airoha_en7523:
	case airoha_en7581:
	case airoha_an7583:
	case airoha_an7552:
		return AIROHA_TSO_V2;
	default:
		break;
	}

	return AIROHA_TSO_NONE;
}

/* Restrict the hardware parser to plain TCP/IP. The core segments other
 * GSO types, tunnels, IP options and extension headers in software.
 */
static bool airoha_tso_valid(struct sk_buff *skb)
{
	unsigned int type = skb_shinfo(skb)->gso_type;
	unsigned int off = skb_network_offset(skb);
	unsigned int transport = skb_transport_offset(skb);
	struct ipv6hdr ip6h, *ip6;
	struct tcphdr th, *tcp;
	struct iphdr iph, *ip;

	if (skb->encapsulation || skb_has_frag_list(skb) ||
	    skb->len > U16_MAX || !skb_shinfo(skb)->gso_size ||
	    (type & ~(SKB_GSO_TCPV4 | SKB_GSO_TCPV6 | SKB_GSO_DODGY)) ||
	    (type & (SKB_GSO_TCPV4 | SKB_GSO_TCPV6)) ==
	    (SKB_GSO_TCPV4 | SKB_GSO_TCPV6))
		return false;
	tcp = skb_header_pointer(skb, transport, sizeof(th), &th);
	if (!tcp || tcp->doff < 5 || transport + tcp->doff * 4 > skb->len)
		return false;
	if (type & SKB_GSO_TCPV4) {
		ip = skb_header_pointer(skb, off, sizeof(iph), &iph);
		return ip && ip->version == 4 && ip->ihl == 5 &&
		       ip->protocol == IPPROTO_TCP && !ip_is_fragment(ip) &&
		       skb_transport_offset(skb) == off + sizeof(iph);
	}
	if (type & SKB_GSO_TCPV6) {
		ip6 = skb_header_pointer(skb, off, sizeof(ip6h), &ip6h);
		return ip6 && ip6->version == 6 && ip6->nexthdr == IPPROTO_TCP &&
		       skb_transport_offset(skb) == off + sizeof(ip6h);
	}
	return false;
}

netdev_features_t airoha_tso_features_check(struct sk_buff *skb,
					    struct net_device *dev,
					    netdev_features_t features)
{
	struct airoha_gdm_dev *gdm = netdev_priv(dev);

	if (airoha_tso_version(gdm->eth) != AIROHA_TSO_NONE &&
	    skb_is_gso(skb) && !airoha_tso_valid(skb))
		features &= ~NETIF_F_GSO_MASK;
	return features;
}

int airoha_tso_prepare(struct sk_buff *skb)
{
	unsigned int len;

	if (!skb_is_gso(skb))
		return 0;
	if (!airoha_tso_valid(skb))
		return -EOPNOTSUPP;
	len = skb_transport_offset(skb) + sizeof(struct tcphdr);
	if (skb_ensure_writable(skb, len))
		return -ENOMEM;
	len = skb_transport_offset(skb) + tcp_hdrlen(skb);
	if (!pskb_may_pull(skb, len))
		return -ENOMEM;
	/* The SDK passes the MSS in the TCP checksum field. */
	tcp_hdr(skb)->check = htons(skb_shinfo(skb)->gso_size);
	return 0;
}

void airoha_tx_complete(struct sk_buff *skb)
{
	struct airoha_gdm_dev *dev = netdev_priv(skb->dev);

	spin_lock_bh(&dev->tx_complete_lock);
	netdev_tx_completed_queue(skb_get_tx_queue(skb->dev, skb), 1, skb->len);
	spin_unlock_bh(&dev->tx_complete_lock);
}

static void airoha_tso_unmap(struct airoha_tso *tso,
			     struct airoha_tso_entry *e)
{
	if (e->page)
		dma_unmap_page(tso->eth->dev, e->dma, e->len, DMA_TO_DEVICE);
	else
		dma_unmap_single(tso->eth->dev, e->dma, e->len, DMA_TO_DEVICE);
	if (e->skb) {
		struct net_device *dev = e->skb->dev;

		airoha_tx_complete(e->skb);
		dev_kfree_skb_any(e->skb);
		dev_put(dev);
	}
	memset(e, 0, sizeof(*e));
}

/* Recycle in order only after both DMA_IDX and DONE have passed the entry.
 * Never discard a completion just because descriptor writeback is late.
 */
static int airoha_tso_reap(struct airoha_tso *tso, int id, int budget)
{
	struct airoha_tso_ring *q = &tso->ring[id];
	u32 dma = airoha_fe_rr(tso->eth, TSO_RING_DMA(id));
	int done = 0;

	if (unlikely(dma >= TSO_DESCS))
		return 0;
	spin_lock_bh(&q->lock);
	while (q->queued && q->tail != dma && done < budget) {
		struct airoha_qdma_desc *d = &q->desc[q->tail];

		if (!(le32_to_cpu(READ_ONCE(d->ctrl)) & QDMA_DESC_DONE_MASK))
			break;
		dma_rmb();
		airoha_tso_unmap(tso, &q->entry[q->tail]);
		q->tail = (q->tail + 1) % TSO_DESCS;
		q->queued--;
		done++;
	}
	spin_unlock_bh(&q->lock);
	return done;
}

static bool airoha_tso_pending(struct airoha_tso *tso)
{
	return READ_ONCE(tso->ring[0].queued) ||
	       READ_ONCE(tso->ring[1].queued);
}

static void airoha_tso_wake(struct airoha_tso *tso,
			    struct net_device *stopping)
{
	int i, j;

	for (i = 0; i < tso->eth->soc->max_gdm_ports; i++) {
		struct airoha_gdm_port *port = tso->eth->ports[i];

		if (!port)
			continue;
		for (j = 0; j < ARRAY_SIZE(port->devs); j++) {
			struct airoha_gdm_dev *dev = port->devs[j];

			if (dev && dev->common.netdev != stopping &&
			    netif_running(dev->common.netdev) &&
			    !READ_ONCE(dev->xpon_tx_stopped))
				netif_tx_wake_all_queues(dev->common.netdev);
		}
	}
}

static void airoha_tso_mask_irq(struct airoha_tso *tso)
{
	airoha_fe_wr(tso->eth, TSO_INT_MASK, 0);
	airoha_fe_rr(tso->eth, TSO_INT_MASK);
}

static void airoha_tso_ack_irq(struct airoha_tso *tso, u32 status)
{
	airoha_fe_wr(tso->eth, TSO_INT_STATUS, status);
	airoha_fe_rr(tso->eth, TSO_INT_STATUS);
}

/* Called with irq_lock held and local IRQs disabled. */
static void airoha_tso_schedule(struct airoha_tso *tso, bool from_irq)
{
	if (!tso->running)
		return;
	if (tso->irq)
		airoha_tso_mask_irq(tso);
	if (napi_schedule_prep(&tso->napi)) {
		tso->poll_from_irq = from_irq;
		__napi_schedule(&tso->napi);
	}
}

static void airoha_tso_arm_timer(struct airoha_tso *tso)
{
	if (!hrtimer_active(&tso->timer))
		hrtimer_start(&tso->timer,
			      ns_to_ktime(tso->irq ? TSO_IRQ_GUARD_NS : TSO_POLL_NS),
			      HRTIMER_MODE_REL);
}

static irqreturn_t airoha_tso_irq(int irq, void *data)
{
	struct airoha_tso *tso = data;
	unsigned long flags;
	u32 status;

	spin_lock_irqsave(&tso->irq_lock, flags);
	status = airoha_fe_rr(tso->eth, TSO_INT_STATUS) & TSO_TX_DONE;
	status &= airoha_fe_rr(tso->eth, TSO_INT_MASK);
	if (!status) {
		spin_unlock_irqrestore(&tso->irq_lock, flags);
		return IRQ_NONE;
	}
	airoha_tso_mask_irq(tso);
	airoha_tso_ack_irq(tso, status);
	atomic64_inc(&tso->stats[TSO_STAT_IRQ_EVENTS]);
	if (status & BIT(0))
		atomic64_inc(&tso->stats[TSO_STAT_IRQ_TX0]);
	if (status & BIT(1))
		atomic64_inc(&tso->stats[TSO_STAT_IRQ_TX1]);
	airoha_tso_schedule(tso, true);
	spin_unlock_irqrestore(&tso->irq_lock, flags);
	return IRQ_HANDLED;
}

static enum hrtimer_restart airoha_tso_timer(struct hrtimer *timer)
{
	struct airoha_tso *tso = container_of(timer, struct airoha_tso, timer);
	unsigned long flags;

	spin_lock_irqsave(&tso->irq_lock, flags);
	if (tso->running && airoha_tso_pending(tso)) {
		atomic64_inc(&tso->stats[TSO_STAT_TIMER_FIRES]);
		if (tso->irq) {
			u32 status;

			airoha_tso_mask_irq(tso);
			status = airoha_fe_rr(tso->eth, TSO_INT_STATUS) & TSO_TX_DONE;
			airoha_tso_ack_irq(tso, status);
		}
		airoha_tso_schedule(tso, false);
	}
	spin_unlock_irqrestore(&tso->irq_lock, flags);
	return HRTIMER_NORESTART;
}

static int airoha_tso_poll(struct napi_struct *napi, int budget)
{
	struct airoha_tso *tso = container_of(napi, struct airoha_tso, napi);
	bool from_irq = tso->poll_from_irq;
	unsigned long flags;
	int done = 0, i;

	if (!budget)
		return 0;
	for (i = 0; i < TSO_RINGS && done < budget; i++)
		done += airoha_tso_reap(tso, (tso->next_ring + i) % TSO_RINGS,
				       budget - done);
	tso->next_ring ^= 1;
	if (done) {
		atomic64_add(done, &tso->stats[from_irq ? TSO_STAT_IRQ_RECLAIMED :
						       TSO_STAT_POLL_RECLAIMED]);
		airoha_tso_wake(tso, NULL);
	}
	spin_lock_irqsave(&tso->irq_lock, flags);
	if (done < budget && napi_complete_done(napi, done) && tso->running) {
		if (tso->irq) {
			u32 status;

			airoha_fe_wr(tso->eth, TSO_INT_MASK, TSO_TX_DONE);
			airoha_fe_rr(tso->eth, TSO_INT_MASK);
			/* Catch events latched while NAPI owned the masked rings. */
			status = airoha_fe_rr(tso->eth, TSO_INT_STATUS) & TSO_TX_DONE;
			if (status) {
				airoha_tso_mask_irq(tso);
				airoha_tso_ack_irq(tso, status);
				airoha_tso_schedule(tso, false);
			}
		}
		if (airoha_tso_pending(tso))
			airoha_tso_arm_timer(tso);
	}
	spin_unlock_irqrestore(&tso->irq_lock, flags);
	return done;
}

netdev_tx_t airoha_tso_xmit(struct sk_buff *skb, struct net_device *dev,
			    unsigned int ring, u32 msg0, u32 msg1)
{
	struct airoha_gdm_dev *gdm = netdev_priv(dev);
	struct airoha_tso *tso = gdm->eth->tso;
	struct netdev_queue *txq = skb_get_tx_queue(dev, skb);
	struct airoha_tso_ring *q;
	unsigned int n = 1 + skb_shinfo(skb)->nr_frags, i;
	unsigned long flags;
	u16 index;

	if (unlikely(!tso || ring >= TSO_RINGS || !skb_headlen(skb) ||
		     skb_has_frag_list(skb) || skb->len > U16_MAX))
		goto drop;
	q = &tso->ring[ring];
	spin_lock_bh(&q->lock);
	if (!READ_ONCE(tso->running) || q->queued + n >= TSO_DESCS) {
		netif_tx_stop_queue(txq);
		spin_unlock_bh(&q->lock);
		return NETDEV_TX_BUSY;
	}
	index = q->head;
	for (i = 0; i < n; i++) {
		struct airoha_tso_entry *e = &q->entry[index];
		struct airoha_qdma_desc *d = &q->desc[index];
		u32 ctrl;

		e->page = i != 0;
		if (i) {
			skb_frag_t *frag = &skb_shinfo(skb)->frags[i - 1];

			e->len = skb_frag_size(frag);
			e->dma = skb_frag_dma_map(tso->eth->dev, frag, 0,
						  e->len, DMA_TO_DEVICE);
		} else {
			e->len = skb_headlen(skb);
			e->dma = dma_map_single(tso->eth->dev, skb->data,
						e->len, DMA_TO_DEVICE);
		}
		if (dma_mapping_error(tso->eth->dev, e->dma)) {
			memset(e, 0, sizeof(*e));
			goto unmap;
		}
		index = (index + 1) % TSO_DESCS;
		ctrl = e->len | (i + 1 < n ? QDMA_DESC_MORE_MASK : 0);
		WRITE_ONCE(d->addr, cpu_to_le32(lower_32_bits(e->dma)));
		WRITE_ONCE(d->data, cpu_to_le32(index));
		WRITE_ONCE(d->msg0, cpu_to_le32(msg0));
		/* SDK tso_transmit_packet(): preserve the nine-bit destination
		 * (GDM + NBQ), and route the assembled packet via HWF.
		 */
		WRITE_ONCE(d->msg1, cpu_to_le32(msg1 | QDMA_ETH_TXMSG_HWF_MASK));
		WRITE_ONCE(d->msg2, 0);
		WRITE_ONCE(d->msg3, 0);
		WRITE_ONCE(d->ctrl, cpu_to_le32(ctrl));
	}
	q->entry[(index + TSO_DESCS - 1) % TSO_DESCS].skb = skb;
	dev_hold(dev);
	q->head = index;
	q->queued += n;
	atomic64_inc(&tso->stats[TSO_STAT_TX_PACKETS]);
	if (skb_is_gso(skb))
		atomic64_inc(&tso->stats[TSO_STAT_GSO_PACKETS]);
	skb_tx_timestamp(skb);
	netdev_tx_sent_queue(txq, skb->len);
	if (TSO_DESCS - q->queued <= MAX_SKB_FRAGS + 1)
		netif_tx_stop_queue(txq);
	dma_wmb();
	airoha_fe_wr(tso->eth, TSO_RING_CPU(ring), q->head);
	spin_unlock_bh(&q->lock);
	spin_lock_irqsave(&tso->irq_lock, flags);
	if (tso->running) {
		if (tso->irq)
			airoha_tso_arm_timer(tso);
		else
			airoha_tso_schedule(tso, false);
	}
	spin_unlock_irqrestore(&tso->irq_lock, flags);
	return NETDEV_TX_OK;

unmap:
	/* The CPU index and BQL have not been published. Roll back only the
	 * successful maps; ownership of the skb remains with this function.
	 */
	while (i--) {
		index = (index + TSO_DESCS - 1) % TSO_DESCS;
		airoha_tso_unmap(tso, &q->entry[index]);
	}
	spin_unlock_bh(&q->lock);
drop:
	dev->stats.tx_dropped++;
	dev_kfree_skb_any(skb);
	return NETDEV_TX_OK;
}

static void airoha_tso_program(struct airoha_tso *tso)
{
	int i, j;

	airoha_fe_wr(tso->eth, TSO_INT_MASK, 0);
	airoha_fe_wr(tso->eth, TSO_DELAY_INT, 0);
	airoha_fe_wr(tso->eth, TSO_INT_STATUS, GENMASK(1, 0));
	for (i = 0; i < TSO_RINGS; i++) {
		struct airoha_tso_ring *q = &tso->ring[i];

		q->head = 0;
		q->tail = 0;
		q->queued = 0;
		memset(q->desc, 0, sizeof(*q->desc) * TSO_DESCS);
		for (j = 0; j < TSO_DESCS; j++)
			q->desc[j].ctrl = cpu_to_le32(QDMA_DESC_DONE_MASK);
		airoha_fe_wr(tso->eth, TSO_RING_BASE(i), lower_32_bits(q->dma));
		airoha_fe_wr(tso->eth, TSO_RING_SIZE(i), TSO_DESCS);
		airoha_fe_wr(tso->eth, TSO_RING_CPU(i), 0);
		airoha_fe_wr(tso->eth, TSO_RING_DMA(i), 0);
	}
	dma_wmb();
	airoha_fe_set(tso->eth, TSO_GLO_CFG, TSO_TX_ENABLE);
	tso->programmed = true;
}

static int airoha_tso_request_irq(struct airoha_tso *tso)
{
	struct device *dev = tso->eth->dev;
	int irq, err;

	irq = platform_get_irq_byname_optional(to_platform_device(dev), "pdma");
	if (irq == -EPROBE_DEFER)
		return irq;
	if (irq < 0) {
		dev_warn(dev, "PDMA IRQ unavailable (%d); using TX polling\n", irq);
		return 0;
	}
	err = request_irq(irq, airoha_tso_irq, 0, "airoha-pdma", tso);
	if (err) {
		dev_warn(dev, "PDMA IRQ request failed (%d); using TX polling\n", err);
		return 0;
	}
	tso->irq = irq;
	atomic64_set(&tso->stats[TSO_STAT_IRQ_MODE], 1);
	dev_info(dev, "PDMA TX IRQ enabled, IRQ %d, 1 ms polling guard\n", irq);
	return 0;
}

static void airoha_tso_start(struct airoha_tso *tso)
{
	unsigned long flags;

	napi_enable(&tso->napi);
	tso->napi_enabled = true;
	spin_lock_irqsave(&tso->irq_lock, flags);
	WRITE_ONCE(tso->running, true);
	if (tso->irq) {
		airoha_fe_wr(tso->eth, TSO_INT_MASK, TSO_TX_DONE);
		airoha_fe_rr(tso->eth, TSO_INT_MASK);
	}
	spin_unlock_irqrestore(&tso->irq_lock, flags);
}

/* Stop producers before draining, including during ndo_stop/BQL reset and
 * GPON retirement. A timed-out engine is reset before any map is released.
 */
static int airoha_tso_quiesce(struct airoha_tso *tso)
{
	unsigned long timeout = jiffies + msecs_to_jiffies(100);
	unsigned long flags;
	int i, err;

	spin_lock_irqsave(&tso->irq_lock, flags);
	WRITE_ONCE(tso->running, false);
	airoha_tso_mask_irq(tso);
	spin_unlock_irqrestore(&tso->irq_lock, flags);
	if (tso->irq)
		synchronize_irq(tso->irq);
	for (i = 0; i < TSO_RINGS; i++) {
		spin_lock_bh(&tso->ring[i].lock);
		spin_unlock_bh(&tso->ring[i].lock);
	}
	hrtimer_cancel(&tso->timer);
	if (tso->napi_enabled)
		napi_disable(&tso->napi);
	tso->napi_enabled = false;
	/* NAPI may have rearmed the timer while it was being disabled. */
	hrtimer_cancel(&tso->timer);
	do {
		for (i = 0; i < TSO_RINGS; i++)
			airoha_tso_reap(tso, i, TSO_DESCS);
		if (!airoha_tso_pending(tso))
			break;
		usleep_range(100, 200);
	} while (time_before(jiffies, timeout));
	airoha_fe_clear(tso->eth, TSO_GLO_CFG, TSO_TX_ENABLE);
	airoha_fe_rr(tso->eth, TSO_GLO_CFG);
	/* The EN7523 reset named "pdma" is QDMA0/LAN, not this engine.
	 * Only the dedicated TDMA reset can stop TSO without losing the
	 * QDMA queue configuration while the Ethernet interfaces are live.
	 */
	err = reset_control_assert(tso->rst);
	if (err)
		return err;
	usleep_range(1000, 2000);
	tso->programmed = false;
	for (i = 0; i < TSO_RINGS; i++) {
		struct airoha_tso_ring *q = &tso->ring[i];

		while (q->queued) {
			airoha_tso_unmap(tso, &q->entry[q->tail]);
			q->tail = (q->tail + 1) % TSO_DESCS;
			q->queued--;
		}
	}
	return 0;
}

void airoha_tso_flush(struct airoha_eth *eth, struct net_device *stopping)
{
	struct airoha_tso *tso = eth->tso;
	int err;

	if (!tso)
		return;
	mutex_lock(&tso->mutex);
	if (!tso->running)
		goto out;
	err = airoha_tso_quiesce(tso);
	if (!err)
		err = reset_control_deassert(tso->rst);
	if (err) {
		dev_err(eth->dev, "TSO v2 reset failed: %d; TX remains disabled\n", err);
		goto out;
	}
	airoha_tso_program(tso);
	airoha_tso_start(tso);
	airoha_tso_wake(tso, stopping);
out:
	mutex_unlock(&tso->mutex);
}

void airoha_tso_cleanup(struct airoha_eth *eth)
{
	struct airoha_tso *tso = eth->tso;
	int i, err = 0;

	if (!tso)
		return;
	if (tso->programmed)
		err = airoha_tso_quiesce(tso);
	if (tso->irq)
		free_irq(tso->irq, tso);
	if (err) {
		/* Retain DMA buffers rather than free memory still owned by a
		 * controller which could not be stopped/reset.
		 */
		dev_err(eth->dev, "TSO v2 failed to stop; retaining DMA buffers\n");
		netif_napi_del(&tso->napi);
		eth->tso = NULL;
		return;
	}
	netif_napi_del(&tso->napi);
	for (i = 0; i < TSO_RINGS; i++) {
		struct airoha_tso_ring *q = &tso->ring[i];

		if (q->desc)
			dma_free_coherent(eth->dev, sizeof(*q->desc) * TSO_DESCS,
					  q->desc, q->dma);
		kfree(q->entry);
	}
	eth->tso = NULL;
	kfree(tso);
}

int airoha_tso_init(struct airoha_eth *eth)
{
	struct reset_control *rst;
	struct airoha_tso *tso;
	int i, err = -ENOMEM;

	if (airoha_tso_version(eth) != AIROHA_TSO_V2)
		return 0;
	rst = devm_reset_control_get_exclusive(eth->dev, "tdma");
	if (IS_ERR(rst))
		return dev_err_probe(eth->dev, PTR_ERR(rst),
				     "TSO v2 requires the dedicated tdma reset\n");
	tso = kzalloc(sizeof(*tso), GFP_KERNEL);
	if (!tso)
		return -ENOMEM;
	tso->eth = eth;
	tso->rst = rst;
	eth->tso = tso;
	mutex_init(&tso->mutex);
	spin_lock_init(&tso->irq_lock);
	for (i = 0; i < TSO_STAT_COUNT; i++)
		atomic64_set(&tso->stats[i], 0);
	hrtimer_setup(&tso->timer, airoha_tso_timer, CLOCK_MONOTONIC,
		      HRTIMER_MODE_REL);
	netif_napi_add_tx(eth->napi_dev, &tso->napi, airoha_tso_poll);
	for (i = 0; i < TSO_RINGS; i++) {
		struct airoha_tso_ring *q = &tso->ring[i];

		spin_lock_init(&q->lock);
		q->entry = kcalloc(TSO_DESCS, sizeof(*q->entry), GFP_KERNEL);
		q->desc = dma_alloc_coherent(eth->dev,
					     sizeof(*q->desc) * TSO_DESCS,
					     &q->dma, GFP_KERNEL);
		if (!q->entry || !q->desc)
			goto error;
	}
	err = reset_control_assert(tso->rst);
	if (err)
		goto error;
	usleep_range(1000, 2000);
	err = reset_control_deassert(tso->rst);
	if (err)
		goto error;
	airoha_tso_program(tso);
	err = airoha_tso_request_irq(tso);
	if (err)
		goto error;
	airoha_tso_start(tso);
	return 0;
error:
	airoha_tso_cleanup(eth);
	return err;
}
