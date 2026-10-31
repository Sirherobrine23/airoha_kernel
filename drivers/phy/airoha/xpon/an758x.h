/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __AIROHA_AN758X_XPON_H
#define __AIROHA_AN758X_XPON_H

#include <linux/types.h>

struct airoha_xpon_phy;

enum airoha_an758x_variant {
	AIROHA_AN7581,
	AIROHA_AN7583,
};

int airoha_an758x_xgspon_probe(struct airoha_xpon_phy *priv,
			       enum airoha_an758x_variant variant,
			       int (*calibrate_cdr)(struct airoha_xpon_phy *priv));
int airoha_an758x_xgspon_power_on(struct airoha_xpon_phy *priv);
int airoha_an758x_xgspon_power_off(struct airoha_xpon_phy *priv);
int airoha_an758x_xgspon_get_link_state(struct airoha_xpon_phy *priv,
					bool *ready, bool *los);
int airoha_an758x_xgspon_set_tx_enable(struct airoha_xpon_phy *priv, bool enable);

#endif
