/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _AIROHA_XPON_TOD_H
#define _AIROHA_XPON_TOD_H

#include <linux/errno.h>
#include <linux/kconfig.h>

struct omci_time_of_day;
struct platform_device;
struct xpon_priv;

int airoha_xpon_tod_init(struct platform_device *pdev, struct xpon_priv *priv);
void airoha_xpon_tod_stop(struct xpon_priv *priv);
void airoha_xpon_tod_remove(struct xpon_priv *priv);
int airoha_gpon_omci_hw_set_time_of_day(void *hw_priv,
					const struct omci_time_of_day *time);
#endif /* _AIROHA_XPON_TOD_H */
