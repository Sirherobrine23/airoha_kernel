// SPDX-License-Identifier: GPL-2.0-only
/* IEEE 802.3ah OAMPDU wire handling. */

#include <linux/etherdevice.h>
#include <linux/skbuff.h>
#include <linux/unaligned.h>

#include "internal.h"

#define XPON_OAM_INFO_TLV	0x01
#define XPON_OAM_INFO_TLV_LEN	16
#define XPON_OAM_END_TLV	0x00

static int xpon_oam_parse_information(struct xpon_oam *oam,
				      struct sk_buff *skb, u8 llid_index)
{
	struct xpon_oam_info remote = {};
	const u8 *p;

	if (skb->len < sizeof(struct xpon_oam_hdr) + 2)
		return -EINVAL;

	p = skb->data + sizeof(struct xpon_oam_hdr);
	while (p + 2 <= skb_tail_pointer(skb)) {
		u8 type = p[0];
		u8 len = p[1];

		if (type == XPON_OAM_END_TLV)
			break;
		if (len < 2 || p + len > skb_tail_pointer(skb))
			return -EINVAL;
		if (type == XPON_OAM_INFO_TLV && len >= XPON_OAM_INFO_TLV_LEN) {
			remote.version = p[2];
			remote.revision = get_unaligned_be16(p + 3);
			remote.state = p[5];
			remote.config = p[6];
			remote.pdu_config = get_unaligned_be16(p + 7);
			memcpy(remote.oui, p + 9, sizeof(remote.oui));
			remote.vendor_info = get_unaligned_be32(p + 12);
			remote.valid = true;
		}
		p += len;
	}

	if (remote.valid) {
		mutex_lock(&oam->lock);
		oam->llids[llid_index].remote = remote;
		mutex_unlock(&oam->lock);
	}
	return 0;
}

int xpon_oam_wire_receive(struct xpon_oam *oam, struct sk_buff *skb,
			  u8 llid_index)
{
	struct xpon_oam_hdr *hdr;

	if (!pskb_may_pull(skb, sizeof(*hdr)))
		return -EINVAL;
	hdr = (struct xpon_oam_hdr *)skb->data;
	if (hdr->subtype != XPON_OAM_SUBTYPE)
		return -EPROTO;

	switch (hdr->code) {
	case XPON_OAM_CODE_INFO:
		return xpon_oam_parse_information(oam, skb, llid_index);
	case XPON_OAM_CODE_EVENT:
	case XPON_OAM_CODE_VAR_REQ:
	case XPON_OAM_CODE_VAR_RESP:
	case XPON_OAM_CODE_LOOPBACK:
	case XPON_OAM_CODE_ORG_SPECIFIC:
		return -EOPNOTSUPP;
	default:
		return -EPROTO;
	}
}
