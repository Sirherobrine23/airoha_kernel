/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * EcoNet EN7521/EN7526 GPON MAC driver — public interface
 */

#ifndef _ECONET_GPON_H
#define _ECONET_GPON_H

#include <linux/netdevice.h>
#include <linux/skbuff.h>

/**
 * gpon_omci_rx_frame() - deliver an OMCI frame received from the PON
 * @gpon_dev: the GPON PON WAN netdev (as returned at probe time)
 * @skb: received raw OMCI PDU; ownership transfers to the network stack
 *
 * Intended to be called from the vendor QDMA RX callback when a packet
 * is received on the OMCI GEM port.  The frame is handed to the OMCI
 * management netdev for delivery to a userspace OMCI daemon.
 */
void gpon_omci_rx_frame(struct net_device *gpon_dev, struct sk_buff *skb);

#endif /* _ECONET_GPON_H */
