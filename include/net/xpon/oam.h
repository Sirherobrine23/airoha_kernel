/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _NET_XPON_OAM_H
#define _NET_XPON_OAM_H

#include <linux/types.h>

struct sk_buff;
struct xpon_device;
struct xpon_oam;

#define XPON_OAM_MAX_LLID	8

struct xpon_oam_ops {
	int (*xmit)(struct xpon_oam *oam, struct sk_buff *skb, u8 llid_index);
};

struct xpon_oam *xpon_oam_register(struct xpon_device *xpon,
				   const struct xpon_oam_ops *ops,
				   void *priv);
void xpon_oam_unregister(struct xpon_oam *oam);
void *xpon_oam_priv(struct xpon_oam *oam);

void xpon_oam_llid_registered(struct xpon_oam *oam, u8 index, u16 llid);
void xpon_oam_llid_unregistered(struct xpon_oam *oam, u8 index);
void xpon_oam_receive(struct xpon_oam *oam, struct sk_buff *skb,
		      u8 llid_index);

#endif /* _NET_XPON_OAM_H */
