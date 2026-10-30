/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _AIROHA_XPON_TOD_H
#define _AIROHA_XPON_TOD_H

struct platform_device;
struct xpon_priv;

int airoha_xpon_tod_init(struct platform_device *pdev, struct xpon_priv *priv);
void airoha_xpon_tod_stop(struct xpon_priv *priv);
void airoha_xpon_tod_remove(struct xpon_priv *priv);

#endif /* _AIROHA_XPON_TOD_H */
