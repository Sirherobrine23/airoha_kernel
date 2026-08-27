/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _NET_XPON_OAM_INTERNAL_H
#define _NET_XPON_OAM_INTERNAL_H

#include <linux/mutex.h>
#include <linux/types.h>
#include <net/xpon/oam.h>

#define XPON_OAM_SUBTYPE	0x03

enum xpon_oam_code {
	XPON_OAM_CODE_INFO		= 0x00,
	XPON_OAM_CODE_EVENT		= 0x01,
	XPON_OAM_CODE_VAR_REQ		= 0x02,
	XPON_OAM_CODE_VAR_RESP		= 0x03,
	XPON_OAM_CODE_LOOPBACK		= 0x04,
	XPON_OAM_CODE_ORG_SPECIFIC	= 0xfe,
};

struct xpon_oam_hdr {
	u8 subtype;
	__be16 flags;
	u8 code;
} __packed;

struct xpon_oam_info {
	u8 version;
	u16 revision;
	u8 state;
	u8 config;
	u16 pdu_config;
	u8 oui[3];
	u32 vendor_info;
	bool valid;
};

struct xpon_oam_llid {
	u16 llid;
	bool registered;
	struct xpon_oam_info remote;
	u64 rx_oampdu;
	u64 tx_oampdu;
};

struct xpon_oam {
	struct xpon_device *xpon;
	const struct xpon_oam_ops *ops;
	void *priv;
	struct mutex lock;
	struct xpon_oam_llid llids[XPON_OAM_MAX_LLID];
};

int xpon_oam_wire_receive(struct xpon_oam *oam, struct sk_buff *skb,
			  u8 llid_index);
int xpon_oam_sysfs_register(struct xpon_oam *oam);
void xpon_oam_sysfs_unregister(struct xpon_oam *oam);

#endif /* _NET_XPON_OAM_INTERNAL_H */
