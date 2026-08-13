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
#include <linux/ethtool.h>
#include <linux/ioport.h>
#include <linux/if_vlan.h>
#include <linux/mdio.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/netdevice.h>
#include <linux/of_net.h>
#include <linux/platform_device.h>
#include <linux/reset.h>
#include <linux/rcupdate.h>
#include <linux/skbuff.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <net/dsa.h>
#include <net/page_pool/helpers.h>

#include "airoha_eth.h"
#include "airoha_eth_gen1.h"
#include "airoha_regs.h"

static bool xpon_tx_debug = true;

/* Generation-1 GDM/netdev backend. */
struct econet_hw_stats {
	/* protect concurrent hw_stats accesses */
	spinlock_t lock;
	struct u64_stats_sync syncp;

	/* EN751221 gdm1 has only limited stats, gdm2 has all */
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

	/* ethtool stats (all require gdm2 extended stats) */
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

	/* EN751221 GDM2 xPON host state.  The MAC protocol remains in
	 * airoha_xpon.c; this block only owns FE/QDMA transport state.
	 */
	struct mutex xpon_lock;
	const struct airoha_xpon_link_ops *xpon_ops;
	void *xpon_priv;
	enum airoha_xpon_mode xpon_mode;
	bool xpon_managed;
	bool xpon_started;
	bool xpon_control_started;
	/* Protects xpon_link. */
	spinlock_t xpon_state_lock;
	struct airoha_xpon_link_state xpon_link;
	struct airoha_xpon_oam_handler __rcu *xpon_oam;
	atomic64_t xpon_oam_rx_packets;
	atomic64_t xpon_oam_rx_bytes;
	atomic64_t xpon_oam_rx_delivered;
	atomic64_t xpon_oam_rx_dropped;
	atomic64_t xpon_oam_rx_no_handler;
	/* Protects xpon_services. */
	spinlock_t xpon_service_lock;
	struct airoha_xpon_service_cfg
		xpon_services[AIROHA_XPON_MAX_SERVICES];

	struct econet_qdma *qdma;

	struct airoha_eth *eth;

	enum etx_fport fport;

	int qid;
};

static int econet_xpon_start(struct econet_gdm_port *port);
static void econet_xpon_stop(struct econet_gdm_port *port);

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


static u16 econet_gdm_oversize_len(struct econet_gdm_port *port, int mtu)
{
	u16 len = ETH_HLEN + mtu + ETH_FCS_LEN;

	/*
	 * The EN7512/EN7521 SDK programs GDM1_LONG_LEN_VALUE to 1700, not to
	 * the bare 1518-byte Ethernet size. GDM1 sees the in-band MT7530
	 * special tag and can also see customer/service VLAN tags, so using the
	 * bare MTU wire length causes otherwise valid full-sized frames to be
	 * classified as long packets and dropped before they reach QDMA/PPE.
	 */
	if (airoha_is(port->eth, econet_en751221) &&
	    port->fport == ETX_FPORT_GDM1)
		len = max_t(u16, len, EN751221_GDM1_LONG_LEN);

	return len;
}

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
	 * on EN751221. The vendor EN7512 datapath explicitly leaves UNTAG
	 * disabled when special-tag mode is enabled. Do not preserve the
	 * reset value here: some bootloaders leave bit 25 set.
	 */
	if (airoha_is(port->eth, econet_en751221))
		fc.word &= ~EN751221_GDM_UNTAG_EN;
	else
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

	if (airoha_is(port->eth, econet_en751221) &&
	    port->fport == ETX_FPORT_GDM1) {
		struct g1_cport_cfg cport_cfg;
		struct gdm_vlan vlan;

		/*
		 * Match macSetMACCR() in the EN7512 vendor driver. CDMA1
		 * needs the 0x8100 insertion TPID and the GDM1 CPORT padding
		 * path is always enabled before LAN traffic is started. Do not
		 * rely on the bootloader leaving either register initialized.
		 */
		scoped_guard(spinlock, &port->reg_lock) {
			vlan = econet_rreg(&port->regs->vlan);
			vlan.tpid = ETH_P_8021Q;
			econet_wreg(vlan, &port->regs->vlan);

			cport_cfg = econet_rreg(&port->regs->g1_cport_cfg);
			set_gdm_g1_cport_cfg_pad(&cport_cfg, true);
			econet_wreg(cport_cfg, &port->regs->g1_cport_cfg);
		}
	}

	return 0;
}

static int econet_dev_open(struct net_device *dev)
{
	struct econet_gdm_port *port = netdev_priv(dev);
	bool dsa = netdev_uses_dsa(dev);
	struct gdm_len_th rlt;
	int err;

	err = airoha_gdm_phylink_connect(&port->common,
					 port->xpon_managed);
	if (err) {
		netdev_err(dev, "could not attach PHY: %d\n", err);
		return err;
	}
	if (port->xpon_managed)
		netif_carrier_off(dev);

	/* The MT7530 CPU port is represented by ethernet = <&gdm1>. Keep
	 * the MTK special tag on the DSA conduit and disable it on a direct
	 * PHY/WAN GDM. EN751221 additionally needs the legacy CDM/GDM bits
	 * used by the vendor special-tag datapath.
	 */
	scoped_guard(spinlock, &port->reg_lock) {
		if (airoha_is(port->eth, econet_en751221) &&
		    port->fport == ETX_FPORT_GDM1) {
			struct fwd_cfg fc = econet_rreg(&port->regs->fwd_cfg);

			if (dsa) {
				fc.word &= ~EN751221_GDM_UNTAG_EN;
				fc.word |= EN751221_GDM_STAG_EN;
			} else {
				fc.word &= ~EN751221_GDM_STAG_EN;
			}
			econet_wreg(fc, &port->regs->fwd_cfg);

			/* CDMA_CSG_CFG is at offset 0 from the GDM1 window. */
			airoha_rmw(port->regs, 0, EN751221_CDM_STAG_EN,
				   dsa ? EN751221_CDM_STAG_EN : 0);
		}

		econet_wreg((u32)dsa, &port->regs->stag_en);
		rlt = econet_rreg(&port->regs->rx_len_threshold);
		set_gdm_len_th_runt_len(&rlt, 60);
		set_gdm_len_th_oversize_len(&rlt,
					 econet_gdm_oversize_len(port, dev->mtu));
		econet_wreg(rlt, &port->regs->rx_len_threshold);
	}

	err = airoha_qdma_gen1_use(port->qdma);
	if (err) {
		scoped_guard(spinlock, &port->reg_lock)
			econet_wreg(0U, &port->regs->stag_en);
		airoha_gdm_phylink_disconnect(&port->common);
		return err;
	}

	netif_tx_start_all_queues(dev);

	err = econet_xpon_start(port);
	if (err) {
		netif_tx_disable(dev);
		airoha_qdma_gen1_unuse(port->qdma);
		airoha_gdm_phylink_disconnect(&port->common);
		return err;
	}

	return 0;
}

static int econet_dev_stop(struct net_device *dev)
{
	struct econet_gdm_port *port = netdev_priv(dev);

	netif_tx_disable(dev);
	econet_xpon_stop(port);
	airoha_gdm_phylink_disconnect(&port->common);

	scoped_guard(spinlock, &port->reg_lock) {
		econet_wreg(0U, &port->regs->stag_en);
		if (airoha_is(port->eth, econet_en751221) &&
		    port->fport == ETX_FPORT_GDM1) {
			struct fwd_cfg fc = econet_rreg(&port->regs->fwd_cfg);

			fc.word &= ~EN751221_GDM_STAG_EN;
			econet_wreg(fc, &port->regs->fwd_cfg);
			airoha_rmw(port->regs, 0, EN751221_CDM_STAG_EN, 0);
		}
	}

	return airoha_qdma_gen1_unuse(port->qdma);
}

static int econet_dev_change_mtu(struct net_device *dev, int mtu)
{
	struct econet_gdm_port *port = netdev_priv(dev);
	struct gdm_len_th rlt;

	guard(spinlock)(&port->reg_lock);
	rlt = econet_rreg(&port->regs->rx_len_threshold);
	set_gdm_len_th_oversize_len(&rlt, econet_gdm_oversize_len(port, mtu));
	econet_wreg(rlt, &port->regs->rx_len_threshold);

	WRITE_ONCE(dev->mtu, mtu);

	return 0;
}

#define EN751221_GPON_RX_CHN_MASK	GENMASK(1, 0)
#define EN751221_EPON_RX_CHN_MASK	GENMASK(7, 0)
#define EN751221_EPON_TX_CHN_MASK	(GENMASK(7, 0) | GENMASK(23, 16))
#define EN751221_EPON_HWF_CHN_MASK	GENMASK(7, 0)

static int econet_validate_xpon_gdm2(struct net_device *netdev,
				     struct econet_gdm_port **gdm)
{
	struct econet_gdm_port *port;

	if (!netdev)
		return -EINVAL;

	port = netdev_priv(netdev);
	if (!port->eth || !airoha_is(port->eth, econet_en751221) ||
	    port->fport != ETX_FPORT_GDM2)
		return -EOPNOTSUPP;

	*gdm = port;
	return 0;
}

static int econet_xpon_lookup_service(struct econet_gdm_port *port,
				      bool vlan_valid, u16 vlan_id,
				      bool pcp_valid, u8 pcp,
				      struct airoha_xpon_tx_info *info)
{
	const struct airoha_xpon_service_cfg *fallback = NULL;
	int i;

	spin_lock_bh(&port->xpon_service_lock);
	for (i = 0; i < AIROHA_XPON_MAX_SERVICES; i++) {
		const struct airoha_xpon_service_cfg *service;

		service = &port->xpon_services[i];
		if (!service->valid)
			continue;
		if (service->default_service)
			fallback = service;
		if (!service->vlan_valid && !service->pcp_valid)
			continue;
		if (service->vlan_valid &&
		    (!vlan_valid || service->vlan_id != vlan_id))
			continue;
		if (service->pcp_valid &&
		    (!pcp_valid || service->pcp != pcp))
			continue;
		fallback = service;
		break;
	}

	if (fallback) {
		info->gem_port_id = fallback->gem_port_id;
		info->tcont = fallback->tcont;
		info->queue = fallback->queue;
		info->oam = false;
	}
	spin_unlock_bh(&port->xpon_service_lock);

	return fallback ? 0 : -ENOENT;
}

static void econet_xpon_get_vlan_meta(struct sk_buff *skb, bool *vlan_valid, u16 *vlan_id, u8 *pcp)
{
	struct vlan_ethhdr vlan_hdr_buf;
	const struct vlan_ethhdr *vlan_hdr;

	*vlan_valid = skb_vlan_tag_present(skb);
	*vlan_id = 0;
	*pcp = 0;

	if (*vlan_valid) {
		u16 tci = skb_vlan_tag_get(skb);

		*vlan_id = tci & VLAN_VID_MASK;
		*pcp = (tci & VLAN_PRIO_MASK) >> VLAN_PRIO_SHIFT;
		return;
	}

	vlan_hdr = skb_header_pointer(skb, 0, sizeof(vlan_hdr_buf),
				      &vlan_hdr_buf);
	if (vlan_hdr && eth_type_vlan(vlan_hdr->h_vlan_proto)) {
		u16 tci = ntohs(vlan_hdr->h_vlan_TCI);

		*vlan_id = tci & VLAN_VID_MASK;
		*pcp = (tci & VLAN_PRIO_MASK) >> VLAN_PRIO_SHIFT;
		*vlan_valid = true;
	}
}

static int econet_xpon_classify(struct econet_gdm_port *port,
				struct sk_buff *skb,
				struct airoha_xpon_tx_info *info)
{
	u16 vlan_id;
	u8 pcp;
	bool vlan_valid;

	econet_xpon_get_vlan_meta(skb, &vlan_valid, &vlan_id, &pcp);

	return econet_xpon_lookup_service(port, vlan_valid, vlan_id,
					  vlan_valid, pcp, info);
}

static u32 econet_xpon_en7523_route_msg0(const struct airoha_xpon_tx_info *info)
{
	u32 msg0;

	msg0 = FIELD_PREP(QDMA_ETH_TXMSG_QUEUE_MASK, info->queue) |
	       FIELD_PREP(QDMA_ETH_TXMSG_CHAN_MASK, info->tcont) |
	       FIELD_PREP(QDMA_ETH_TXMSG_SP_TAG_MASK, info->gem_port_id);
	if (info->oam)
		msg0 |= QDMA_ETH_TXMSG_OAM_MASK;

	return msg0;
}

static netdev_tx_t econet_dev_xmit(struct sk_buff *skb, struct net_device *dev)
{
	struct econet_gdm_port *port = netdev_priv(dev);
	struct airoha_qdma_skb_meta skb_meta;
	int qid, ret = 0, len = skb->len;
	struct netdev_queue *txq;
	struct airoha_xpon_tx_info xpon_info;
	union desc_msg msg = {0};
	bool xpon = false;
	u8 channel;

	if (READ_ONCE(port->xpon_managed)) {
		switch (READ_ONCE(port->xpon_mode)) {
		case AIROHA_XPON_MODE_GPON:
			ret = econet_xpon_classify(port, skb, &xpon_info);
			if (ret) {
				if (READ_ONCE(xpon_tx_debug)) {
					u16 vlan_id;
					u8 pcp;
					bool vlan_valid;

					econet_xpon_get_vlan_meta(skb, &vlan_valid, &vlan_id, &pcp);
					netdev_info(dev,
						    "EN751221 xPON TX no service: len=%u proto=%#06x vlan=%s%u pcp=%u ret=%d\n",
						    skb->len, ntohs(skb->protocol),
						    vlan_valid ? "" : "none/", vlan_id,
						    pcp, ret);
				}
				goto drop;
			}
			xpon = true;
			break;
		case AIROHA_XPON_MODE_EPON:
			/* Initial EPON support uses LLID/channel 0. Multi-LLID
			 * classification can be layered on the service API later; the
			 * important part here is to emit a PWAN descriptor instead of
			 * an Ethernet/MT7530 special-tag descriptor.
			 */
			memset(&xpon_info, 0, sizeof(xpon_info));
			xpon_info.queue = skb_get_queue_mapping(skb) %
					  ECONET_NUM_QUEUES;
			xpon = true;
			break;
		default:
			goto drop;
		}
	}

	qid = skb_get_queue_mapping(skb);
	if (xpon) {
		/* PWAN_FETxMsg_T on EN751221: queue[2:0], channel[10:3],
		 * OAM[11] and GEM[23:12].  The GEM field overlays the Ethernet
		 * MediaTek special-tag field and must therefore be programmed only
		 * for the managed PON datapath.
		 */
		channel = xpon_info.tcont;
		set_etx_queue(&msg.etx, xpon_info.queue);
		set_etx_xpon_gem(&msg.etx, xpon_info.gem_port_id);
	} else if (airoha_is(port->eth, econet_en751221)) {
		/*
		 * The EN751221 vendor LAN path does not map Linux flow/hash
		 * queues onto QDMA's eight hardware QoS queues.  Unless QoS
		 * explicitly marks txq_is_valid, qdma_transmit_packet() forces
		 * queue 0 and selects the channel from the destination switch
		 * port.  Keep that policy here; Linux qid is still used for BQL
		 * accounting and queue stop/wake.
		 */
		channel = 0;
		set_etx_queue(&msg.etx, 0);
	} else {
		channel = qid / ECONET_NUM_QUEUES;
		set_etx_queue(&msg.etx, qid % ECONET_NUM_QUEUES);
	}
	set_etx_fport(&msg.etx, port->fport);

	txq = netdev_get_tx_queue(dev, qid);

	/* Non-linear skbs are unsupported, we shouldn't get them but
	 * if we do, we'll attempt to linearize. */
	if (skb_linearize(skb))
		goto drop;

	/*
	 * EN751221 keeps the MediaTek DSA special tag in-band, unlike newer
	 * Airoha QDMA which can move it to descriptor metadata. The logical
	 * port-mask/channel classification is nevertheless shared.
	 */
	if (!xpon && airoha_is(port->eth, econet_en751221)) {
		airoha_qdma_skb_get_mtk_meta(skb, dev, AIROHA_MTK_TAG_IN_SKB,
					     &skb_meta);
		if (skb_meta.has_mtk_tag &&
		    skb_meta.channel != AIROHA_MTK_INVALID_CHANNEL)
			channel = skb_meta.channel;
	}

	set_etx_channel(&msg.etx, channel);

	/*
	 * EN751221 QDMA can generate IPv4/IPv6 TCP and UDP checksums.
	 * The vendor driver programs all parser checksum-offload bits for a
	 * CHECKSUM_PARTIAL skb and lets the hardware select the applicable
	 * protocol.
	 */
	if (skb->ip_summed == CHECKSUM_PARTIAL) {
		set_etx_ico(&msg.etx, true);
		set_etx_uco(&msg.etx, true);
		set_etx_tco(&msg.etx, true);
	}

	netdev_tx_sent_queue(txq, len);

	if (xpon && READ_ONCE(xpon_tx_debug)) {
		struct ethhdr eth_buf;
		const struct ethhdr *eth;
		u32 en7523_msg0;
		u32 txchn, hwf;
		u16 vlan_id;
		u8 pcp;
		bool vlan_valid;

		eth = skb_header_pointer(skb, 0, sizeof(eth_buf), &eth_buf);
		econet_xpon_get_vlan_meta(skb, &vlan_valid, &vlan_id, &pcp);
		en7523_msg0 = econet_xpon_en7523_route_msg0(&xpon_info);
		txchn = airoha_fe_rr(port->eth, REG_GDM_TXCHN_EN(AIROHA_GDM2_IDX));
		hwf = airoha_fe_rr(port->eth, REG_CDM_HWF_CHN_EN(2));

		if (eth)
			netdev_info(dev,
				    "EN751221 xPON TX: len=%u dst=%pM src=%pM proto=%#06x vlan=%s%u pcp=%u qdma=%u qid=%d queue=%u channel=%u gem=%u oam=%u fport=%u msg=%#010x/%#010x en7523-route=%#010x txchn=%#010x hwf=%#010x\n",
				    skb->len, eth->h_dest, eth->h_source,
				    ntohs(skb->protocol), vlan_valid ? "" : "none/",
				    vlan_id, pcp, airoha_qdma_gen1_common(port->qdma)->id, qid,
				    get_etx_queue(&msg.etx), get_etx_channel(&msg.etx),
				    get_etx_xpon_gem(&msg.etx), is_etx_oam(&msg.etx),
				    get_etx_fport(&msg.etx), msg.raw[0], msg.raw[1],
				    en7523_msg0, txchn, hwf);
		else
			netdev_info(dev,
				    "EN751221 xPON TX: len=%u proto=%#06x vlan=%s%u pcp=%u qdma=%u qid=%d queue=%u channel=%u gem=%u oam=%u fport=%u msg=%#010x/%#010x en7523-route=%#010x txchn=%#010x hwf=%#010x\n",
				    skb->len, ntohs(skb->protocol),
				    vlan_valid ? "" : "none/", vlan_id, pcp,
				    airoha_qdma_gen1_common(port->qdma)->id, qid,
				    get_etx_queue(&msg.etx),
				    get_etx_channel(&msg.etx), get_etx_xpon_gem(&msg.etx),
				    is_etx_oam(&msg.etx),
				    get_etx_fport(&msg.etx), msg.raw[0], msg.raw[1],
				    en7523_msg0, txchn, hwf);
	}

	ret = airoha_qdma_gen1_xmit(port->qdma, skb, &msg, 0);
	if (xpon && READ_ONCE(xpon_tx_debug))
		netdev_info(dev, "EN751221 xPON TX submit ret=%d\n", ret);
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
	struct airoha_xpon_link_state state;
	unsigned long flags;

	if (!READ_ONCE(port->xpon_managed))
		return phylink_ethtool_ksettings_get(port->common.phylink, cmd);

	spin_lock_irqsave(&port->xpon_state_lock, flags);
	state = port->xpon_link;
	spin_unlock_irqrestore(&port->xpon_state_lock, flags);

	if (!state.valid && port->common.phylink_started)
		return phylink_ethtool_ksettings_get(port->common.phylink, cmd);

	ethtool_link_ksettings_zero_link_mode(cmd, supported);
	ethtool_link_ksettings_zero_link_mode(cmd, advertising);
	ethtool_link_ksettings_zero_link_mode(cmd, lp_advertising);
	linkmode_set_bit(ETHTOOL_LINK_MODE_FIBRE_BIT,
			 cmd->link_modes.supported);
	linkmode_set_bit(ETHTOOL_LINK_MODE_FIBRE_BIT,
			 cmd->link_modes.advertising);

	cmd->base.speed = state.valid ? state.speed : SPEED_UNKNOWN;
	cmd->base.duplex = state.valid ? state.duplex : DUPLEX_UNKNOWN;
	cmd->base.autoneg = AUTONEG_DISABLE;
	cmd->base.port = state.valid ? state.port : PORT_FIBRE;
	cmd->base.phy_address = 0xff;

	return 0;
}

static int econet_ethtool_set_link_ksettings(struct net_device *dev,
					     const struct ethtool_link_ksettings *cmd)
{
	struct econet_gdm_port *port = netdev_priv(dev);

	if (READ_ONCE(port->xpon_managed))
		return -EOPNOTSUPP;

	return phylink_ethtool_ksettings_set(port->common.phylink, cmd);
}

static int econet_ethtool_nway_reset(struct net_device *dev)
{
	struct econet_gdm_port *port = netdev_priv(dev);

	if (READ_ONCE(port->xpon_managed))
		return -EOPNOTSUPP;

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

struct net_device *econet_alloc_gdm_port(struct airoha_eth *eth,
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
				       airoha_qdma_gen1_common(qdma)->num_channels *
				       ECONET_NUM_QUEUES,
				       airoha_qdma_gen1_common(qdma)->num_channels *
				       ECONET_NUM_QUEUES);
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
	 * EN751221 supports IPv4/IPv6 TCP/UDP checksum offload, but unlike
	 * EN751627 it does not provide TSO/TSO6. Scatter-gather also remains
	 * disabled until the QDMA TX path can submit fragmented skbs directly.
	 */
	ndev->hw_features = NETIF_F_IP_CSUM | NETIF_F_RXCSUM |
			      NETIF_F_IPV6_CSUM;
	if (eth->ppe_dev && eth->ppe_dev->enabled)
		ndev->hw_features |= NETIF_F_HW_TC;

	ndev->features |= ndev->hw_features;
	ndev->vlan_features = ndev->hw_features;
	ndev->dev.of_node = of_node_get(np);
	SET_NETDEV_DEV(ndev, eth->dev);

	err = airoha_eth_init_mac_address(eth->dev, np, ndev);
	if (err)
		return ERR_PTR(err);

	port = netdev_priv(ndev);
	airoha_gdm_common_init(&port->common, eth, ndev,
			       AIROHA_ETH_FAMILY_ECONET, fport,
			       fport == ETX_FPORT_GDM2 ? DPORT_GDMA2 :
						      DPORT_GDMA1,
			       port, NULL);
	port->common.ppe = eth->ppe_dev;
	u64_stats_init(&port->stats.syncp);
	spin_lock_init(&port->stats.lock);
	spin_lock_init(&port->reg_lock);
	mutex_init(&port->xpon_lock);
	spin_lock_init(&port->xpon_state_lock);
	spin_lock_init(&port->xpon_service_lock);
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

struct airoha_eth_gen1 {
	struct net_device		*ports[ECONET_NUM_GDM_PORTS];
	struct gdm __iomem		*gdm[ECONET_NUM_GDM_PORTS];
	struct qregs __iomem		*qdma_regs[ECONET_NUM_QDMA];
	struct reset_control		*reset;
	int				qdma_irq[ECONET_NUM_QDMA * QDMA_NUM_IRQS];
	struct econet_qdma		*qdma[ECONET_NUM_QDMA];
};

static inline struct airoha_eth_gen1 *airoha_eth_gen1_priv(struct airoha_eth *eth)
{
	return eth->priv;
}


#define EN751221_DSA_SPORT_BASE		8
#define EN751221_DSA_NUM_PORTS		5

static bool econet_en751221_dsa_sport(u8 sport)
{
	return sport >= EN751221_DSA_SPORT_BASE &&
	       sport < EN751221_DSA_SPORT_BASE + EN751221_DSA_NUM_PORTS;
}

static struct net_device *econet_get_sport_dev(struct airoha_eth *eth,
					     u8 sport)
{
	struct airoha_eth_gen1 *priv = airoha_eth_gen1_priv(eth);

	/*
	 * EN7512/EN7521 vendor RX metadata uses SPORT_QDMA_LAN (0) for packets
	 * delivered to the CPU by QDMA0 and SPORT_QDMA_WAN (5) for QDMA1.
	 * These are valid source-port values, not malformed descriptors.
	 */
	if (sport == ETX_FPORT_GDM2 || sport == ETX_FPORT_QDMA1_CPU)
		return priv->ports[1];

	if (sport == ETX_FPORT_GDM1 || sport == ETX_FPORT_QDMA0_CPU ||
	    (airoha_is(eth, econet_en751221) &&
	     econet_en751221_dsa_sport(sport)))
		return priv->ports[0];

	dev_info_ratelimited(eth->dev, "rx: on unexpected sport %u\n", sport);
	return priv->ports[0];
}

int econet_rx_before_recv(struct airoha_eth *eth, struct sk_buff *skb,
			  u8 sport)
{
	struct net_device *port;

	port = econet_get_sport_dev(eth, sport);
	if (!port)
		return -ENODEV;

	/*
	 * EN7512/EN7521 keeps the MT7530 special tag in-band. The vendor
	 * receive path removes that tag from skb data in software; rxMsgW3 is
	 * only used for descriptor-based tag recovery by the EN7526C special
	 * case. Leave the frame untouched here so the DSA MTK tagger consumes
	 * the wire tag directly.
	 */
	skb->dev = port;
	skb->protocol = eth_type_trans(skb, port);

	return 0;
}

static int econet_xpon_start(struct econet_gdm_port *port)
{
	int ret = 0;

	mutex_lock(&port->xpon_lock);
	if (port->xpon_ops && !port->xpon_started) {
		ret = port->xpon_ops->start(port->xpon_priv);
		if (!ret)
			port->xpon_started = true;
	}
	mutex_unlock(&port->xpon_lock);

	return ret;
}

static void econet_xpon_stop(struct econet_gdm_port *port)
{
	mutex_lock(&port->xpon_lock);
	if (port->xpon_ops && port->xpon_started) {
		port->xpon_started = false;
		port->xpon_ops->stop(port->xpon_priv);
	}
	mutex_unlock(&port->xpon_lock);
}

static int econet_set_xpon_mode(struct net_device *netdev,
				enum airoha_xpon_mode mode)
{
	struct econet_gdm_port *port;
	int ret;

	ret = econet_validate_xpon_gdm2(netdev, &port);
	if (ret)
		return ret;
	if (mode != AIROHA_XPON_MODE_GPON && mode != AIROHA_XPON_MODE_EPON)
		return -EINVAL;

	/* feDevGdm2Cdm2Stop(XPON_ENABLE) starts both protocols from a fully
	 * quiescent GDM2/CDM2 channel map.  The EN751221 FE register layout is
	 * the same 0x1500/0x1400 layout described by the shared register file.
	 */
	airoha_fe_wr(port->eth, REG_GDM_TXCHN_EN(AIROHA_GDM2_IDX), 0);
	airoha_fe_wr(port->eth, REG_GDM_RXCHN_EN(AIROHA_GDM2_IDX), 0);
	airoha_fe_wr(port->eth, REG_CDM_HWF_CHN_EN(2), 0);
	WRITE_ONCE(port->xpon_mode, mode);

	return 0;
}

static int econet_set_xpon_datapath(struct net_device *netdev,
				    enum airoha_xpon_mode mode, bool enable)
{
	struct econet_gdm_port *port;
	int ret;

	ret = econet_validate_xpon_gdm2(netdev, &port);
	if (ret)
		return ret;

	switch (mode) {
	case AIROHA_XPON_MODE_GPON:
		/* EN7521 feDevGdm2Cdm2Stop(XPON_DISABLE): only downstream
		 * receive channels 0 and 1 are released here. T-CONT TX/HWF
		 * channels are enabled when their Alloc-ID is provisioned.
		 */
		airoha_fe_rmw(port->eth, REG_GDM_RXCHN_EN(AIROHA_GDM2_IDX),
			      EN751221_GPON_RX_CHN_MASK,
			      enable ? EN751221_GPON_RX_CHN_MASK : 0);
		break;
	case AIROHA_XPON_MODE_EPON:
		/* eponFeChannelEnable(): LLID 0..7 plus TX 16..23 used by
		 * the vendor OAM-favour path.
		 */
		airoha_fe_rmw(port->eth, REG_GDM_TXCHN_EN(AIROHA_GDM2_IDX),
			      EN751221_EPON_TX_CHN_MASK,
			      enable ? EN751221_EPON_TX_CHN_MASK : 0);
		airoha_fe_rmw(port->eth, REG_GDM_RXCHN_EN(AIROHA_GDM2_IDX),
			      EN751221_EPON_RX_CHN_MASK,
			      enable ? EN751221_EPON_RX_CHN_MASK : 0);
		airoha_fe_rmw(port->eth, REG_CDM_HWF_CHN_EN(2),
			      EN751221_EPON_HWF_CHN_MASK,
			      enable ? EN751221_EPON_HWF_CHN_MASK : 0);
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int econet_set_xpon_tcont_channel(struct net_device *netdev,
					 unsigned int channel, bool enable)
{
	struct econet_gdm_port *port;
	u32 mask;
	int ret;

	ret = econet_validate_xpon_gdm2(netdev, &port);
	if (ret)
		return ret;
	if (channel >= 32)
		return -EINVAL;

	mask = BIT(channel);
	airoha_fe_rmw(port->eth, REG_GDM_TXCHN_EN(AIROHA_GDM2_IDX), mask,
		      enable ? mask : 0);
	airoha_fe_rmw(port->eth, REG_CDM_HWF_CHN_EN(2), mask,
		      enable ? mask : 0);

	return 0;
}

static int econet_register_xpon(struct net_device *netdev,
				enum airoha_xpon_mode mode,
				const struct airoha_xpon_link_ops *ops,
				void *priv)
{
	struct econet_gdm_port *port;
	unsigned long flags;
	int ret;

	if (!ops || !ops->start || !ops->stop || !ops->mac_irq)
		return -EINVAL;

	ret = econet_validate_xpon_gdm2(netdev, &port);
	if (ret)
		return ret;

	mutex_lock(&port->xpon_lock);
	if (port->xpon_ops) {
		ret = -EBUSY;
		goto out_unlock;
	}

	port->xpon_ops = ops;
	port->xpon_priv = priv;
	port->xpon_mode = mode;
	port->xpon_managed = true;
	spin_lock_irqsave(&port->xpon_state_lock, flags);
	memset(&port->xpon_link, 0, sizeof(port->xpon_link));
	port->xpon_link.mode = mode;
	spin_unlock_irqrestore(&port->xpon_state_lock, flags);
	netif_carrier_off(netdev);

	/* EN751221 has no standalone xPON platform IRQ. The MAC interrupt is
	 * aggregated into QDMA_WAN bits 16/17 and is enabled only after the
	 * provider callback is fully published.
	 */
	ret = airoha_qdma_gen1_set_xpon_irq(port->qdma, mode, true);
	if (ret) {
		port->xpon_managed = false;
		port->xpon_ops = NULL;
		port->xpon_priv = NULL;
		goto out_unlock;
	}

out_unlock:
	mutex_unlock(&port->xpon_lock);
	if (ret)
		return ret;

	if (netif_running(netdev))
		return econet_xpon_start(port);

	return 0;
}

static void econet_unregister_xpon(struct net_device *netdev,
				   const struct airoha_xpon_link_ops *ops,
				   void *priv)
{
	struct econet_gdm_port *port;
	unsigned long flags;

	if (econet_validate_xpon_gdm2(netdev, &port))
		return;
	if (READ_ONCE(port->xpon_ops) != ops || READ_ONCE(port->xpon_priv) != priv)
		return;

	/* Mask the QDMA aggregator first and wait out an in-flight hard IRQ
	 * before the provider private pointer can disappear.
	 */
	airoha_qdma_gen1_set_xpon_irq(port->qdma, port->xpon_mode, false);
	econet_xpon_stop(port);

	mutex_lock(&port->xpon_lock);
	if (port->xpon_ops == ops && port->xpon_priv == priv) {
		port->xpon_ops = NULL;
		port->xpon_priv = NULL;
		port->xpon_managed = false;
	}
	mutex_unlock(&port->xpon_lock);

	spin_lock_irqsave(&port->xpon_state_lock, flags);
	memset(&port->xpon_link, 0, sizeof(port->xpon_link));
	spin_unlock_irqrestore(&port->xpon_state_lock, flags);
	netif_carrier_off(netdev);
}

static void econet_xpon_update_link(struct net_device *netdev,
				    const struct airoha_xpon_link_state *state)
{
	struct airoha_xpon_link_state new_state;
	struct econet_gdm_port *port;
	unsigned long flags;

	if (!state || econet_validate_xpon_gdm2(netdev, &port))
		return;
	if (!READ_ONCE(port->xpon_managed) ||
	    state->mode != READ_ONCE(port->xpon_mode))
		return;

	new_state = *state;
	new_state.valid = true;
	spin_lock_irqsave(&port->xpon_state_lock, flags);
	port->xpon_link = new_state;
	spin_unlock_irqrestore(&port->xpon_state_lock, flags);

	if (new_state.link && netif_running(netdev))
		netif_carrier_on(netdev);
	else
		netif_carrier_off(netdev);
}

static int econet_xpon_control_start(struct net_device *netdev)
{
	struct econet_gdm_port *port;
	int ret;

	ret = econet_validate_xpon_gdm2(netdev, &port);
	if (ret)
		return ret;

	mutex_lock(&port->xpon_lock);
	if (port->xpon_control_started) {
		ret = 0;
		goto out;
	}

	/* OMCI activation is allowed while pon0 is administratively down. Keep
	 * QDMA1 alive independently from ndo_open(), mirroring the vendor WAN
	 * QDMA control-plane ownership.
	 */
	ret = airoha_qdma_gen1_use(port->qdma);
	if (!ret)
		port->xpon_control_started = true;
out:
	mutex_unlock(&port->xpon_lock);
	return ret;
}

static void econet_xpon_control_stop(struct net_device *netdev)
{
	struct econet_gdm_port *port;

	if (econet_validate_xpon_gdm2(netdev, &port))
		return;

	mutex_lock(&port->xpon_lock);
	if (port->xpon_control_started) {
		port->xpon_control_started = false;
		airoha_qdma_gen1_unuse(port->qdma);
	}
	mutex_unlock(&port->xpon_lock);
}

static void econet_xpon_dump_oam_rx_state(struct net_device *netdev)
{
	struct econet_gdm_port *port;

	if (econet_validate_xpon_gdm2(netdev, &port))
		return;

	netdev_info(netdev,
		    "xPON OAM RX: packets=%lld bytes=%lld delivered=%lld dropped=%lld no-handler=%lld txchn=%#010x rxchn=%#010x hwf=%#010x\n",
		    (long long)atomic64_read(&port->xpon_oam_rx_packets),
		    (long long)atomic64_read(&port->xpon_oam_rx_bytes),
		    (long long)atomic64_read(&port->xpon_oam_rx_delivered),
		    (long long)atomic64_read(&port->xpon_oam_rx_dropped),
		    (long long)atomic64_read(&port->xpon_oam_rx_no_handler),
		    airoha_fe_rr(port->eth, REG_GDM_TXCHN_EN(AIROHA_GDM2_IDX)),
		    airoha_fe_rr(port->eth, REG_GDM_RXCHN_EN(AIROHA_GDM2_IDX)),
		    airoha_fe_rr(port->eth, REG_CDM_HWF_CHN_EN(2)));
}

static int econet_register_xpon_oam(struct net_device *netdev,
				    struct airoha_xpon_oam_handler *handler)
{
	struct econet_gdm_port *port;
	int ret;

	if (!handler || !handler->rx)
		return -EINVAL;
	ret = econet_validate_xpon_gdm2(netdev, &port);
	if (ret)
		return ret;
	if (rcu_access_pointer(port->xpon_oam))
		return -EBUSY;

	rcu_assign_pointer(port->xpon_oam, handler);
	return 0;
}

static void econet_unregister_xpon_oam(struct net_device *netdev,
				       struct airoha_xpon_oam_handler *handler)
{
	struct econet_gdm_port *port;

	if (econet_validate_xpon_gdm2(netdev, &port))
		return;
	if (rcu_access_pointer(port->xpon_oam) != handler)
		return;

	RCU_INIT_POINTER(port->xpon_oam, NULL);
	synchronize_rcu();
}

static int econet_xmit_xpon_oam(struct net_device *netdev, struct sk_buff *skb,
				u16 gem_port_id)
{
	struct econet_gdm_port *port;
	struct netdev_queue *txq;
	union desc_msg msg = {};
	int len, ret;

	ret = econet_validate_xpon_gdm2(netdev, &port);
	if (ret)
		return ret;
	if (!skb || gem_port_id > FIELD_MAX(ETX_XPON_GEM_MASK) ||
	    !READ_ONCE(port->xpon_control_started))
		return -EINVAL;
	if (skb_linearize(skb))
		return -ENOMEM;

	set_etx_queue(&msg.etx, 0);
	set_etx_channel(&msg.etx, 0);
	set_etx_oam(&msg.etx, true);
	set_etx_xpon_gem(&msg.etx, gem_port_id);
	set_etx_fport(&msg.etx, ETX_FPORT_GDM2);

	skb->dev = netdev;
	skb_set_queue_mapping(skb, 0);
	len = skb->len;
	txq = netdev_get_tx_queue(netdev, 0);
	netdev_tx_sent_queue(txq, len);

	if (READ_ONCE(xpon_tx_debug)) {
		struct airoha_xpon_tx_info info = {
			.gem_port_id = gem_port_id,
			.tcont = 0,
			.queue = 0,
			.oam = true,
		};

		netdev_info(netdev,
			    "EN751221 xPON OAM TX: len=%u qdma=%u queue=%u channel=%u gem=%u oam=%u fport=%u msg=%#010x/%#010x en7523-route=%#010x txchn=%#010x hwf=%#010x\n",
			    skb->len, airoha_qdma_gen1_common(port->qdma)->id,
			    get_etx_queue(&msg.etx), get_etx_channel(&msg.etx),
			    get_etx_xpon_gem(&msg.etx), is_etx_oam(&msg.etx),
			    get_etx_fport(&msg.etx), msg.raw[0], msg.raw[1],
			    econet_xpon_en7523_route_msg0(&info),
			    airoha_fe_rr(port->eth, REG_GDM_TXCHN_EN(AIROHA_GDM2_IDX)),
			    airoha_fe_rr(port->eth, REG_CDM_HWF_CHN_EN(2)));
	}

	ret = airoha_qdma_gen1_xmit(port->qdma, skb, &msg, 0);
	if (READ_ONCE(xpon_tx_debug))
		netdev_info(netdev, "EN751221 xPON OAM TX submit ret=%d\n", ret);
	if (ret < 0) {
		netdev_tx_completed_queue(txq, 1, len);
		return ret;
	}

	return 0;
}

static int econet_xpon_get_tx_info(struct net_device *netdev, bool vlan_valid,
				   u16 vlan_id, bool pcp_valid, u8 pcp,
				   struct airoha_xpon_tx_info *info)
{
	struct econet_gdm_port *port;
	int ret;

	if (!info)
		return -EINVAL;
	ret = econet_validate_xpon_gdm2(netdev, &port);
	if (ret)
		return ret;
	if (!READ_ONCE(port->xpon_managed) ||
	    READ_ONCE(port->xpon_mode) != AIROHA_XPON_MODE_GPON)
		return -EOPNOTSUPP;

	return econet_xpon_lookup_service(port, vlan_valid, vlan_id,
					  pcp_valid, pcp, info);
}

static int econet_xpon_add_service(struct net_device *netdev,
				   const struct airoha_xpon_service_cfg *cfg)
{
	struct econet_gdm_port *port;
	int empty = -1, i, ret;

	if (!cfg || cfg->gem_port_id > FIELD_MAX(ETX_XPON_GEM_MASK) ||
	    cfg->tcont >= 32 || cfg->queue >= ECONET_NUM_QUEUES)
		return -EINVAL;
	ret = econet_validate_xpon_gdm2(netdev, &port);
	if (ret)
		return ret;

	spin_lock_bh(&port->xpon_service_lock);
	for (i = 0; i < AIROHA_XPON_MAX_SERVICES; i++) {
		if (!port->xpon_services[i].valid && empty < 0)
			empty = i;
		if (port->xpon_services[i].valid &&
		    port->xpon_services[i].cookie == cfg->cookie) {
			empty = i;
			break;
		}
	}
	if (empty < 0) {
		spin_unlock_bh(&port->xpon_service_lock);
		return -ENOSPC;
	}
	if (cfg->default_service)
		for (i = 0; i < AIROHA_XPON_MAX_SERVICES; i++)
			port->xpon_services[i].default_service = false;
	port->xpon_services[empty] = *cfg;
	port->xpon_services[empty].valid = true;
	spin_unlock_bh(&port->xpon_service_lock);

	netdev_info(netdev,
		    "EN751221 xPON service[%d]: cookie=%#x gem=%u channel=%u queue=%u vlan=%s%u pcp=%s%u default=%u\n",
		    empty, cfg->cookie, cfg->gem_port_id, cfg->tcont, cfg->queue,
		    cfg->vlan_valid ? "" : "any/", cfg->vlan_id,
		    cfg->pcp_valid ? "" : "any/", cfg->pcp,
		    cfg->default_service);

	return 0;
}

static bool econet_xpon_del_service(struct net_device *netdev, u32 cookie,
				    u16 *gem_port_id)
{
	struct econet_gdm_port *port;
	bool found = false;
	int i;

	if (econet_validate_xpon_gdm2(netdev, &port))
		return false;

	spin_lock_bh(&port->xpon_service_lock);
	for (i = 0; i < AIROHA_XPON_MAX_SERVICES; i++) {
		if (!port->xpon_services[i].valid ||
		    port->xpon_services[i].cookie != cookie)
			continue;
		if (gem_port_id)
			*gem_port_id = port->xpon_services[i].gem_port_id;
		memset(&port->xpon_services[i], 0, sizeof(port->xpon_services[i]));
		found = true;
		break;
	}
	spin_unlock_bh(&port->xpon_service_lock);

	return found;
}

static bool econet_xpon_has_gem_service(struct net_device *netdev,
					u16 gem_port_id)
{
	struct econet_gdm_port *port;
	bool found = false;
	int i;

	if (econet_validate_xpon_gdm2(netdev, &port))
		return false;

	spin_lock_bh(&port->xpon_service_lock);
	for (i = 0; i < AIROHA_XPON_MAX_SERVICES; i++)
		if (port->xpon_services[i].valid &&
		    port->xpon_services[i].gem_port_id == gem_port_id) {
			found = true;
			break;
		}
	spin_unlock_bh(&port->xpon_service_lock);

	return found;
}

static void econet_xpon_flush_services(struct net_device *netdev)
{
	struct econet_gdm_port *port;

	if (econet_validate_xpon_gdm2(netdev, &port))
		return;

	spin_lock_bh(&port->xpon_service_lock);
	memset(port->xpon_services, 0, sizeof(port->xpon_services));
	spin_unlock_bh(&port->xpon_service_lock);
}

bool airoha_eth_gen1_rx_xpon_oam(struct airoha_eth *eth, u8 qdma_id,
				 struct sk_buff *skb, union desc_msg *msg)
{
	struct airoha_eth_gen1 *priv = airoha_eth_gen1_priv(eth);
	struct airoha_xpon_oam_handler *handler;
	struct econet_gdm_port *port;
	u32 raw0, flags = 0;
	u16 gem_port_id;
	u32 skb_len;
	bool consumed = false;

	if (!airoha_is(eth, econet_en751221) || qdma_id != 1 || !priv->ports[1])
		return false;

	raw0 = READ_ONCE(msg->raw[0]);
	if (!(raw0 & ERX_XPON_OAM))
		return false;

	port = netdev_priv(priv->ports[1]);
	if (!READ_ONCE(port->xpon_managed))
		return false;

	gem_port_id = FIELD_GET(ERX_XPON_GEM_MASK, raw0);
	if (raw0 & ERX_XPON_CRC_ERROR)
		flags |= AIROHA_XPON_OAM_RX_F_CRC_ERROR;

	skb->dev = port->dev;
	skb_len = skb->len;
	atomic64_inc(&port->xpon_oam_rx_packets);
	atomic64_add(skb_len, &port->xpon_oam_rx_bytes);

	rcu_read_lock();
	handler = rcu_dereference(port->xpon_oam);
	if (handler && handler->rx)
		consumed = handler->rx(handler->priv, skb, gem_port_id, flags);
	else
		atomic64_inc(&port->xpon_oam_rx_no_handler);
	rcu_read_unlock();

	if (consumed) {
		atomic64_inc(&port->xpon_oam_rx_delivered);
	} else {
		atomic64_inc(&port->xpon_oam_rx_dropped);
		dev_kfree_skb_any(skb);
	}

	dev_dbg_ratelimited(eth->dev,
			    "EN751221 xPON OAM RX: len=%u msg0=%#010x channel=%u gem=%u crc=%u runt=%u long=%u consumed=%u\n",
			    skb_len, raw0,
			    (unsigned int)FIELD_GET(ERX_XPON_CHANNEL_MASK, raw0),
			    gem_port_id, !!(raw0 & ERX_XPON_CRC_ERROR),
			    !!(raw0 & ERX_XPON_RUNT), !!(raw0 & ERX_XPON_LONG),
			    consumed);

	return true;
}

void airoha_eth_gen1_xpon_irq(struct airoha_eth *eth, u8 qdma_id,
			      enum airoha_xpon_mode mode)
{
	struct airoha_eth_gen1 *priv = airoha_eth_gen1_priv(eth);
	const struct airoha_xpon_link_ops *ops;
	struct econet_gdm_port *port;
	void *xpon_priv;

	if (!airoha_is(eth, econet_en751221) || qdma_id != 1 || !priv->ports[1])
		return;

	port = netdev_priv(priv->ports[1]);
	if (!READ_ONCE(port->xpon_managed) || READ_ONCE(port->xpon_mode) != mode)
		return;

	ops = READ_ONCE(port->xpon_ops);
	xpon_priv = READ_ONCE(port->xpon_priv);
	if (ops && ops->mac_irq)
		ops->mac_irq(xpon_priv);
}

static const struct airoha_eth_xpon_ops econet_xpon_ops = {
	.set_mode = econet_set_xpon_mode,
	.set_datapath = econet_set_xpon_datapath,
	.set_tcont_channel = econet_set_xpon_tcont_channel,
	.register_link = econet_register_xpon,
	.unregister_link = econet_unregister_xpon,
	.update_link = econet_xpon_update_link,
	.control_start = econet_xpon_control_start,
	.control_stop = econet_xpon_control_stop,
	.dump_oam_rx_state = econet_xpon_dump_oam_rx_state,
	.register_oam = econet_register_xpon_oam,
	.unregister_oam = econet_unregister_xpon_oam,
	.xmit_oam = econet_xmit_xpon_oam,
	.add_service = econet_xpon_add_service,
	.get_tx_info = econet_xpon_get_tx_info,
	.del_service = econet_xpon_del_service,
	.has_gem_service = econet_xpon_has_gem_service,
	.flush_services = econet_xpon_flush_services,
};

static int econet_init_port(struct airoha_eth *eth, struct device_node *np)
{
	struct airoha_eth_gen1 *priv = airoha_eth_gen1_priv(eth);
	struct net_device *dev;
	u32 id;
	int err;

	err = airoha_eth_get_port_id(eth->dev, np, 1,
				     ARRAY_SIZE(priv->ports), &id);
	if (err)
		return err;

	if (priv->ports[id - 1]) {
		dev_err(eth->dev, "duplicate gdm port id: %d\n", id);
		return -EINVAL;
	}

	if (id == 1)
		dev = econet_alloc_gdm_port(eth, np,
					    priv->gdm[0],
					    priv->qdma[0],
					    ETX_FPORT_GDM1,
					    false);
	else if (id == 2)
		dev = econet_alloc_gdm_port(eth, np,
					    priv->gdm[1],
					    priv->qdma[1],
					    ETX_FPORT_GDM2,
					    true);
	else
		return -EINVAL;

	if (IS_ERR(dev))
		return PTR_ERR(dev);

	priv->ports[id - 1] = dev;
	return 0;
}

static void econet_prepare_qdma_cfg(struct econet_qdma_cfg *cfg,
				    const struct airoha_eth_soc_data *soc,
				    int id)
{
	int i;

	memset(cfg, 0, sizeof(*cfg));
	for (i = 0; i < QDMA_NUM_CHAINS; i++)
		cfg->num_rx_descs[i] = 128;
	for (i = 0; i < QDMA_NUM_CHAINS; i++)
		cfg->num_tx_descs[i] = 128;
	for (i = 0; i < QDMA_NUM_TX_DONE; i++) {
		cfg->done_list_size[i] = 256;
		cfg->done_list_irq_threshold[i] = 1;
	}
	cfg->fwd_max_packet_size = ECONET_MAX_PACKET_SIZE;
	cfg->fwd_low_threshold = 32;
	cfg->num_fwd_descs = 256;
	cfg->num_channels = ECONET_NUM_CHANNELS;
	/*
	 * Do not enable the legacy RX_2B_OFFSET mode while RX buffers are backed
	 * by recyclable page-pool fragments. The vendor driver owns a complete
	 * skb for each descriptor and places the DMA address two bytes before
	 * skb->data; that ownership/layout is not equivalent to the current
	 * page-pool zero-copy path on non-coherent MIPS. Keep the stable layout
	 * until RX_2B_OFFSET is implemented with dedicated EN751221 buffers.
	 */
	cfg->rx_2b_offset = false;
	cfg->soc = soc;

	/*
	 * QDMA0 is QDMA_LAN on EN7512/EN7521. Match the descriptor profile
	 * used by the vendor Ethernet/QDMA modules. In particular RX1 and
	 * the hardware-forwarding descriptor pool are deliberately larger
	 * than the generic EcoNet defaults because PPE traffic consumes the
	 * same LMGR resource pool.
	 */
	if (soc->version == econet_en751221 && id == 0) {
		cfg->num_rx_descs[0] = 128;
		cfg->num_rx_descs[1] = 512;
		cfg->num_tx_descs[0] = 128;
		cfg->num_tx_descs[1] = 128;
		cfg->done_list_size[0] = 2048;
		cfg->done_list_irq_threshold[0] = 16;
		cfg->num_fwd_descs = 1024;
		cfg->fwd_low_threshold = 128;
		cfg->num_channels = 8;
	} else if (soc->version == econet_en751221 && id == 1) {
		/* Vendor QDMA_WAN profile. */
		cfg->num_rx_descs[0] = 512;
		cfg->num_rx_descs[1] = 256;
		cfg->num_tx_descs[0] = 1024;
		cfg->num_tx_descs[1] = 128;
		cfg->done_list_size[0] = 2048;
		cfg->done_list_irq_threshold[0] = 16;
		cfg->num_fwd_descs = 4096;
		cfg->fwd_low_threshold = 256;
		cfg->num_channels = 32;
	}
}

static void airoha_eth_gen1_remove(struct platform_device *pdev,
			    struct airoha_eth *eth)
{
	struct airoha_eth_gen1 *priv = airoha_eth_gen1_priv(eth);
	int i;

	if (!priv)
		return;

	for (i = 0; i < ARRAY_SIZE(priv->ports); i++) {
		if (!priv->ports[i])
			continue;

		unregister_netdev(priv->ports[i]);
		airoha_gdm_phylink_destroy(&((struct econet_gdm_port *)
					netdev_priv(priv->ports[i]))->common);
		of_node_put(priv->ports[i]->dev.of_node);
		priv->ports[i]->dev.of_node = NULL;
	}

	airoha_ppe_deinit(eth);

	for (i = 0; i < ARRAY_SIZE(priv->qdma); i++) {
		eth->qdma_common[i] = NULL;
		if (priv->qdma[i])
			airoha_qdma_gen1_destroy(priv->qdma[i]);
	}

	eth->priv = NULL;
}

static int airoha_eth_gen1_probe(struct platform_device *pdev,
				 struct airoha_eth *eth)
{
	static const char * const qdma_names[ECONET_NUM_QDMA] = {
		"qdma0", "qdma1",
	};
	struct airoha_eth_gen1 *priv;
	struct resource *fe_res;
	struct econet_qdma_cfg cfg;
	struct device_node *np;
	void __iomem *fe_base;
	int i, err, irq;

	priv = devm_kzalloc(eth->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;
	eth->priv = priv;

	err = airoha_eth_set_dma_mask(eth->dev);
	if (err)
		return err;

	fe_res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "fe");
	if (!fe_res)
		return dev_err_probe(eth->dev, -EINVAL,
				     "missing fe register resource\n");
	if (resource_size(fe_res) < EN751221_FE_MIN_SIZE)
		return dev_err_probe(eth->dev, -EINVAL,
				     "fe register resource is too small\n");

	fe_base = devm_ioremap_resource(eth->dev, fe_res);
	if (IS_ERR(fe_base))
		return dev_err_probe(eth->dev, PTR_ERR(fe_base),
				     "failed to map fe registers\n");

	eth->fe_regs = fe_base;
	priv->gdm[0] = fe_base + EN751221_FE_GDM1_OFFSET;
	priv->gdm[1] = fe_base + EN751221_FE_GDM2_OFFSET;

	eth->gdmp_regs = devm_platform_ioremap_resource_byname(pdev, "gdmp");
	if (IS_ERR(eth->gdmp_regs))
		return dev_err_probe(eth->dev, PTR_ERR(eth->gdmp_regs),
				     "failed to map gdmp registers\n");

	for (i = 0; i < ARRAY_SIZE(priv->qdma_regs); i++) {
		priv->qdma_regs[i] =
			devm_platform_ioremap_resource_byname(pdev, qdma_names[i]);
		if (IS_ERR(priv->qdma_regs[i]))
			return dev_err_probe(eth->dev,
					     PTR_ERR(priv->qdma_regs[i]),
					     "failed to map %s registers\n",
					     qdma_names[i]);
	}

	priv->reset = devm_reset_control_array_get_optional_exclusive(eth->dev);
	if (IS_ERR(priv->reset))
		return dev_err_probe(eth->dev, PTR_ERR(priv->reset),
				     "failed to get resets\n");

	if (priv->reset) {
		err = reset_control_assert(priv->reset);
		if (err)
			return err;

		msleep(20);
		err = reset_control_deassert(priv->reset);
		if (err)
			return err;
	}

	for (i = 0; i < ARRAY_SIZE(priv->qdma_irq); i++) {
		irq = platform_get_irq(pdev, i);
		if (irq < 0)
			return dev_err_probe(eth->dev, irq,
					     "failed to get IRQ %d\n", i);
		priv->qdma_irq[i] = irq;
	}

	BUILD_BUG_ON(ARRAY_SIZE(priv->qdma_irq) !=
		     ECONET_NUM_QDMA * QDMA_NUM_IRQS);
	for (i = 0; i < ARRAY_SIZE(priv->qdma); i++) {
		econet_prepare_qdma_cfg(&cfg, eth->soc, i);
		priv->qdma[i] = airoha_qdma_gen1_new(eth, priv->qdma_regs[i], i,
					       &priv->qdma_irq[i * QDMA_NUM_IRQS],
					       QDMA_NUM_IRQS, &cfg);
		if (IS_ERR(priv->qdma[i])) {
			err = PTR_ERR(priv->qdma[i]);
			priv->qdma[i] = NULL;
			goto error;
		}
		eth->qdma_common[i] = airoha_qdma_gen1_common(priv->qdma[i]);
	}

	err = airoha_ppe_init(eth);
	if (err)
		goto error;

	for_each_available_child_of_node(eth->dev->of_node, np) {
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
	airoha_eth_gen1_remove(pdev, eth);
	return err;
}

static const struct airoha_eth_ops airoha_eth_gen1_ops = {
	.probe = airoha_eth_gen1_probe,
	.remove = airoha_eth_gen1_remove,
};

const struct airoha_eth_soc_data econet_en751221_soc_data = {
	.version = econet_en751221,
	.eth_ops = &airoha_eth_gen1_ops,
	.xpon_ops = &econet_xpon_ops,
	.num_ppe = 1,
	.ppe_dram_entries = 16 * 1024,
};

const struct airoha_eth_soc_data econet_en7528_soc_data = {
	.version = econet_en7528,
	.eth_ops = &airoha_eth_gen1_ops,
	.num_ppe = 1,
	.ppe_dram_entries = 16 * 1024,
};
