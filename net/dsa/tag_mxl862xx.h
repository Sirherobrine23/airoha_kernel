/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Helpers shared by the MaxLinear MxL862xx DSA tag drivers
 *
 * Copyright (C) 2025 Daniel Golle <daniel@makrotopia.org>
 */

#ifndef _NET_DSA_TAG_MXL862XX_H
#define _NET_DSA_TAG_MXL862XX_H

#include <linux/icmpv6.h>
#include <linux/if_pppox.h>
#include <linux/if_vlan.h>
#include <linux/ip.h>
#include <linux/ipv6.h>
#include <linux/ppp_defs.h>
#include <linux/skbuff.h>
#include <net/ipv6.h>

/* Must match the IGMP/MLD trap rules the mxl862xx driver installs on
 * all user ports; frames matching them reach the CPU port without
 * being forwarded in hardware. proto is the outermost EtherType of
 * the frame as seen by the hardware parser, start is the offset of
 * its payload relative to skb->data.
 */
static inline bool mxl862_rcv_is_snoop_trapped(struct sk_buff *skb,
					       __be16 proto, int start)
{
	const struct ipv6hdr *ip6h;
	const struct iphdr *iph;
	struct ipv6hdr _ip6h;
	struct iphdr _iph;
	const __be16 *p;
	__be16 frag_off;
	__be16 _proto;
	const u8 *tp;
	u8 nexthdr;
	u8 _tp;
	int i;

	for (i = 0; i < 2 && (proto == htons(ETH_P_8021Q) ||
			      proto == htons(ETH_P_8021AD)); i++) {
		p = skb_header_pointer(skb, start + 2, sizeof(_proto),
				       &_proto);
		if (!p)
			return false;
		proto = *p;
		start += VLAN_HLEN;
	}

	if (proto == htons(ETH_P_PPP_SES)) {
		p = skb_header_pointer(skb, start + sizeof(struct pppoe_hdr),
				       sizeof(_proto), &_proto);
		if (!p)
			return false;
		if (*p == htons(PPP_IP))
			proto = htons(ETH_P_IP);
		else if (*p == htons(PPP_IPV6))
			proto = htons(ETH_P_IPV6);
		else
			return false;
		start += PPPOE_SES_HLEN;
	}

	if (proto == htons(ETH_P_IP)) {
		iph = skb_header_pointer(skb, start, sizeof(_iph), &_iph);
		return iph && iph->version == 4 &&
		       iph->protocol == IPPROTO_IGMP;
	}

	if (proto != htons(ETH_P_IPV6))
		return false;

	ip6h = skb_header_pointer(skb, start, sizeof(_ip6h), &_ip6h);
	if (!ip6h || ip6h->version != 6)
		return false;

	nexthdr = ip6h->nexthdr;
	start = ipv6_skip_exthdr(skb, start + sizeof(*ip6h), &nexthdr,
				 &frag_off);
	if (start < 0 || frag_off != 0 || nexthdr != IPPROTO_ICMPV6)
		return false;

	tp = skb_header_pointer(skb, start, sizeof(_tp), &_tp);
	if (!tp)
		return false;

	return (*tp >= ICMPV6_MGM_QUERY && *tp <= ICMPV6_MGM_REDUCTION) ||
	       *tp == ICMPV6_MLD2_REPORT;
}

#endif /* _NET_DSA_TAG_MXL862XX_H */
