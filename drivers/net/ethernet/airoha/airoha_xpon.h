/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _AIROHA_XPON_H
#define _AIROHA_XPON_H

#include <linux/netdevice.h>
#include <linux/skbuff.h>

void gpon_omci_rx_frame(struct net_device *gpon_dev, struct sk_buff *skb);

#endif /* _AIROHA_XPON_H */
