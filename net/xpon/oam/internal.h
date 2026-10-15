/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _NET_XPON_OAM_INTERNAL_H
#define _NET_XPON_OAM_INTERNAL_H

#include <net/xpon/oam.h>

#define XPON_OAM_SUBTYPE 0x03

struct xpon_oam {
	struct xpon_device *xpon;
	const struct xpon_oam_ops *ops;
	void *priv;
};

int xpon_oam_sysfs_register(struct xpon_oam *oam);
void xpon_oam_sysfs_unregister(struct xpon_oam *oam);

#endif /* _NET_XPON_OAM_INTERNAL_H */
