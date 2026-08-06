// SPDX-License-Identifier: GPL-2.0-only
/*
 * EcoNet EN751221 has 2 GDM ports, one for LAN and one for WAN.
 * Each port has it's own registers and MAC address, and it's own
 * net_device instance.
 */

#include <linux/skbuff.h>
#include <linux/etherdevice.h>
#include <linux/netdevice.h>
#include <net/dsa.h>
#include <linux/platform_device.h>
#include <linux/reset.h>
#include <linux/of_net.h>

#include "econet_eth.h"
#include "econet_gdm_regs.h"
#include "econet_qdma_desc.h"

struct en75_hw_stats {
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

struct en75_gdm_port {
	struct net_device *dev;

	struct gdm __iomem *regs;

	/* Protects accesses to hardware regs */
	spinlock_t reg_lock;

	struct en75_hw_stats stats;

	// DECLARE_BITMAP(qos_sq_bmap, AIROHA_NUM_QOS_CHANNELS);

	/* qos stats counters */
	u64 cpu_tx_packets;
	u64 fwd_tx_packets;

	// struct metadata_dst *dsa_meta[AIROHA_MAX_DSA_PORTS];

	struct en75_qdma *qdma;

	struct en75_eth *eth;

	enum etx_fport fport;

	int qid;
};

static void en75_set_macaddr(struct en75_gdm_port *port, const u8 *addr)
{
	struct gdm_mymac_msb msb;
	struct gdm_mymac_lsb lsb;

	scoped_guard(spinlock, &port->reg_lock) {
		msb = en75_rreg(&port->regs->mymac_msb);
		lsb = en75_rreg(&port->regs->mymac_lsb);
		set_gdm_mymac_msb_a(&msb, addr[0]);
		set_gdm_mymac_msb_b(&msb, addr[1]);
		set_gdm_mymac_lsb_c(&lsb, addr[2]);
		set_gdm_mymac_lsb_d(&lsb, addr[3]);
		set_gdm_mymac_lsb_e(&lsb, addr[4]);
		set_gdm_mymac_lsb_f(&lsb, addr[5]);
		en75_wreg(lsb, &port->regs->mymac_lsb);
		en75_wreg(msb, &port->regs->mymac_msb);
	}

	en75_port_set_macaddr(port->eth, port->fport, addr);
}

static int en75_dev_set_macaddr(struct net_device *dev, void *p)
{
	struct en75_gdm_port *port = netdev_priv(dev);
	int err;

	err = eth_mac_addr(dev, p);
	if (err)
		return err;

	en75_set_macaddr(port, dev->dev_addr);

	return 0;
}

static void en75_set_gdm_port_fwd_cfg(struct en75_gdm_port *port,
				      enum etx_fport val)
{
	struct fwd_cfg fc;

	guard(spinlock)(&port->reg_lock);
	fc = en75_rreg(&port->regs->fwd_cfg);
	set_gdm_fwd_cfg_mymac_fport(&fc, val);
	set_gdm_fwd_cfg_mcast_fport(&fc, val);
	set_gdm_fwd_cfg_bcast_fport(&fc, val);
	set_gdm_fwd_cfg_default_fport(&fc, val);
	set_gdm_fwd_cfg_drop_oversize(&fc, true);
	en75_wreg(fc, &port->regs->fwd_cfg);
}

static int en75_dev_init(struct net_device *dev)
{
	struct en75_gdm_port *port = netdev_priv(dev);

	en75_set_macaddr(port, dev->dev_addr);
	/* Route each GDM port to the CPU side of its matching QDMA. */
	en75_set_gdm_port_fwd_cfg(port,
		port->fport == ETX_FPORT_GDM2 ?
		ETX_FPORT_QDMA1_CPU : ETX_FPORT_QDMA0_CPU);

	return 0;
}

static int en75_dev_open(struct net_device *dev)
{
	struct en75_gdm_port *port = netdev_priv(dev);
	struct gdm_len_th rlt;
	int err;

	// TODO DSA
	// if (netdev_uses_dsa(dev))
	// 	airoha_fe_set(qdma->eth, REG_GDM_INGRESS_CFG(port->id),
	// 		      GDM_STAG_EN_MASK);
	// else
	// 	airoha_fe_clear(qdma->eth, REG_GDM_INGRESS_CFG(port->id),
	// 			GDM_STAG_EN_MASK);

	scoped_guard(spinlock, &port->reg_lock) {
		rlt = en75_rreg(&port->regs->rx_len_threshold);
		set_gdm_len_th_runt_len(&rlt, 60);
		set_gdm_len_th_oversize_len(&rlt,
					 ETH_HLEN + dev->mtu + ETH_FCS_LEN);
		en75_wreg(rlt, &port->regs->rx_len_threshold);
	}

	err = en75_qdma_use(port->qdma);
	if (err)
		return err;

	netif_tx_start_all_queues(dev);

	return 0;
}

static int en75_dev_stop(struct net_device *dev)
{
	struct en75_gdm_port *port = netdev_priv(dev);

	netif_tx_disable(dev);

	return en75_qdma_unuse(port->qdma);
}

static int en75_dev_change_mtu(struct net_device *dev, int mtu)
{
	struct en75_gdm_port *port = netdev_priv(dev);
	struct gdm_len_th rlt;

	guard(spinlock)(&port->reg_lock);
	rlt = en75_rreg(&port->regs->rx_len_threshold);
	set_gdm_len_th_oversize_len(&rlt, ETH_HLEN + mtu + ETH_FCS_LEN);
	en75_wreg(rlt, &port->regs->rx_len_threshold);

	WRITE_ONCE(dev->mtu, mtu);

	return 0;
}

static netdev_tx_t en75_dev_xmit(struct sk_buff *skb, struct net_device *dev)
{
	struct en75_gdm_port *port = netdev_priv(dev);
	int qid, ret = 0, len = skb->len;
	struct netdev_queue *txq;
	union desc_msg msg = {0};

	qid = skb_get_queue_mapping(skb);
	set_etx_channel(&msg.etx, qid / EN75_NUM_QUEUES);
	set_etx_queue(&msg.etx, qid % EN75_NUM_QUEUES);
	set_etx_fport(&msg.etx, port->fport);

	txq = netdev_get_tx_queue(dev, qid);

	/* Non-linear skbs are unsupported, we shouldn't get them but
	 * if we do, we'll attempt to linearize. */
	if (skb_linearize(skb))
		goto drop;

	netdev_tx_sent_queue(txq, len);

	ret = en75_qdma_xmit(port->qdma, skb, &msg, 0);
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

static void en75_update_hw_stats(struct en75_gdm_port *port)
{
	struct gdm_counters __iomem *c = &port->regs->counters;
	struct clear_counters cc = {0};
	u32 i = 0;

	guard(spinlock)(&port->stats.lock);
	u64_stats_update_begin(&port->stats.syncp);

	port->stats.tx_ok_pkts += en75_rreg(&c->tx.tx_pkts);
	port->stats.tx_ok_bytes += en75_rreg(&c->tx.bytes);
	port->stats.tx_drops += en75_rreg(&c->tx.drops);
	if (port->stats.g2_stats) {
		port->stats.tx_broadcast += en75_rreg(&c->tx.g2.tx_bcast);
		port->stats.tx_multicast += en75_rreg(&c->tx.g2.tx_mcast);

		port->stats.tx_len[i] += en75_rreg(&c->tx.g2.f_less_64);
		port->stats.tx_len[i++] += en75_rreg(&c->tx.g2.f_64);

		port->stats.tx_len[i++] += en75_rreg(&c->tx.g2.f_65_127);
		port->stats.tx_len[i++] += en75_rreg(&c->tx.g2.f_128_255);
		port->stats.tx_len[i++] += en75_rreg(&c->tx.g2.f_256_511);
		port->stats.tx_len[i++] += en75_rreg(&c->tx.g2.f_512_1023);
		port->stats.tx_len[i++] += en75_rreg(&c->tx.g2.f_1024_1518);
		port->stats.tx_len[i++] += en75_rreg(&c->tx.g2.f_more_1518);
	}

	port->stats.rx_ok_pkts += en75_rreg(&c->rx.pkts);
	port->stats.rx_ok_bytes += en75_rreg(&c->rx.bytes);
	port->stats.rx_errors += en75_rreg(&c->rx.drops_err);
	port->stats.rx_over_errors += en75_rreg(&c->rx.drops_overflow);
	if (port->stats.g2_stats) {
		port->stats.rx_drops += en75_rreg(&c->rx.g2.edrops);
		port->stats.rx_broadcast += en75_rreg(&c->rx.g2.bcast);
		port->stats.rx_multicast += en75_rreg(&c->rx.g2.mcast);
		port->stats.rx_crc_error += en75_rreg(&c->rx.g2.ecrc);
		port->stats.rx_fragment += en75_rreg(&c->rx.g2.efrag);
		port->stats.rx_jabber += en75_rreg(&c->rx.g2.ejabber);

		i = 0;
		port->stats.rx_len[i] += en75_rreg(&c->rx.g2.f_less_64);
		port->stats.rx_len[i++] += en75_rreg(&c->rx.g2.f_64);

		port->stats.rx_len[i++] += en75_rreg(&c->rx.g2.f_65_127);
		port->stats.rx_len[i++] += en75_rreg(&c->rx.g2.f_128_255);
		port->stats.rx_len[i++] += en75_rreg(&c->rx.g2.f_256_511);
		port->stats.rx_len[i++] += en75_rreg(&c->rx.g2.f_512_1023);
		port->stats.rx_len[i++] += en75_rreg(&c->rx.g2.f_1024_1518);
		port->stats.rx_len[i++] += en75_rreg(&c->rx.g2.f_more_1518);
	} else {
		port->stats.rx_drops += en75_rreg(&c->rx.drops_err);
		port->stats.rx_drops += en75_rreg(&c->rx.drops_fc);
		port->stats.rx_drops += en75_rreg(&c->rx.drops_rc);
		port->stats.rx_drops += en75_rreg(&c->rx.drops_overflow);
	}

	set_gdm_cl_cnt_rx(&cc, true);
	set_gdm_cl_cnt_tx(&cc, true);
	en75_wreg(cc, &port->regs->clear_counters);

	u64_stats_update_end(&port->stats.syncp);
}

static void en75_dev_get_stats64(struct net_device *dev,
				 struct rtnl_link_stats64 *storage)
{
	struct en75_gdm_port *port = netdev_priv(dev);
	unsigned int start;

	en75_update_hw_stats(port);
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

static void en75_ethtool_get_mac_stats(struct net_device *dev,
				       struct ethtool_eth_mac_stats *stats)
{
	struct en75_gdm_port *port = netdev_priv(dev);
	unsigned int start;

	en75_update_hw_stats(port);

	if (!port->stats.g2_stats)
		return;

	do {
		start = u64_stats_fetch_begin(&port->stats.syncp);
		stats->MulticastFramesXmittedOK = port->stats.tx_multicast;
		stats->BroadcastFramesXmittedOK = port->stats.tx_broadcast;
		stats->BroadcastFramesReceivedOK = port->stats.rx_broadcast;
	} while (u64_stats_fetch_retry(&port->stats.syncp, start));
}

static const struct ethtool_rmon_hist_range en75_ethtool_rmon_ranges[] = {
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
en75_ethtool_get_rmon_stats(struct net_device *dev,
			    struct ethtool_rmon_stats *stats,
			    const struct ethtool_rmon_hist_range **ranges)
{
	struct en75_gdm_port *port = netdev_priv(dev);
	struct en75_hw_stats *hw_stats = &port->stats;
	unsigned int start;

	BUILD_BUG_ON(ARRAY_SIZE(en75_ethtool_rmon_ranges) !=
		     ARRAY_SIZE(hw_stats->tx_len) + 1);
	BUILD_BUG_ON(ARRAY_SIZE(en75_ethtool_rmon_ranges) !=
		     ARRAY_SIZE(hw_stats->rx_len) + 1);

	*ranges = en75_ethtool_rmon_ranges;
	en75_update_hw_stats(port);

	if (!port->stats.g2_stats)
		return;

	do {
		int i;

		start = u64_stats_fetch_begin(&port->stats.syncp);
		stats->fragments = hw_stats->rx_fragment;
		stats->jabbers = hw_stats->rx_jabber;
		for (i = 0; i < ARRAY_SIZE(en75_ethtool_rmon_ranges) - 1;
		     i++) {
			stats->hist[i] = hw_stats->rx_len[i];
			stats->hist_tx[i] = hw_stats->tx_len[i];
		}
	} while (u64_stats_fetch_retry(&port->stats.syncp, start));
}

static const struct net_device_ops en75_netdev_ops = {
	.ndo_init		= en75_dev_init,
	.ndo_open		= en75_dev_open,
	.ndo_stop		= en75_dev_stop,
	.ndo_change_mtu		= en75_dev_change_mtu,
	.ndo_start_xmit		= en75_dev_xmit,
	.ndo_get_stats64        = en75_dev_get_stats64,
	.ndo_set_mac_address	= en75_dev_set_macaddr,
};

static const struct ethtool_ops en75_ethtool_ops = {
	.get_drvinfo		= airoha_eth_get_drvinfo,
	.get_eth_mac_stats      = en75_ethtool_get_mac_stats,
	.get_rmon_stats		= en75_ethtool_get_rmon_stats,
};

struct net_device *en75_alloc_gdm_port(struct en75_eth *eth,
				       struct device_node *np,
				       struct gdm __iomem *regs,
				       struct en75_qdma *qdma,
				       enum etx_fport fport,
				       bool has_g2_stats)
{
	struct en75_gdm_port *port;
	struct net_device *ndev;
	int err;

	ndev = devm_alloc_etherdev_mqs(eth->dev, sizeof(*port),
				       EN75_NUM_SOFT_QUEUES,
				       EN75_NUM_SOFT_QUEUES);
	if (!ndev) {
		dev_err(eth->dev, "alloc_etherdev failed\n");
		return ERR_PTR(-ENOMEM);
	}

	ndev->netdev_ops = &en75_netdev_ops;
	ndev->ethtool_ops = &en75_ethtool_ops;
	ndev->max_mtu = SKB_WITH_OVERHEAD(EN75_MAX_PACKET_SIZE) -
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

	ndev->features |= ndev->hw_features;
	ndev->vlan_features = ndev->hw_features;
	ndev->dev.of_node = np;
	SET_NETDEV_DEV(ndev, eth->dev);

	err = airoha_eth_init_mac_address(eth->dev, np, ndev);
	if (err)
		return ERR_PTR(err);

	port = netdev_priv(ndev);
	u64_stats_init(&port->stats.syncp);
	spin_lock_init(&port->stats.lock);
	spin_lock_init(&port->reg_lock);
	port->stats.g2_stats = has_g2_stats;
	port->dev = ndev;
	port->regs = regs;
	port->fport = fport;
	port->qdma = qdma;
	port->eth = eth;

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
	return ERR_PTR(err);
}
