/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __LINUX_PHY_AIROHA_XPON_H
#define __LINUX_PHY_AIROHA_XPON_H

#include <linux/errno.h>
#include <linux/kconfig.h>
#include <linux/types.h>

struct phy;

/* Driver-specific submodes used with PHY_MODE_ETHERNET. */
enum airoha_xpon_phy_submode {
	AIROHA_XPON_PHY_SUBMODE_GPON = 0,
	AIROHA_XPON_PHY_SUBMODE_EPON = 1,
};

/*
 * Values programmed into the GPON PHY extended-preamble operational-state
 * field.  These are PHY states, not the complete G.984 ONU activation state.
 */
enum airoha_xpon_phy_gpon_oper_state {
	AIROHA_XPON_PHY_GPON_OPER_DISABLED = 0,
	AIROHA_XPON_PHY_GPON_OPER_RANGING = 2,
	AIROHA_XPON_PHY_GPON_OPER_OPERATION = 3,
};

int airoha_xpon_phy_get_link_state(struct phy *phy, bool *ready, bool *los);
int airoha_xpon_phy_get_gpon_tx_counters(struct phy *phy,
					 u32 *frame_count,
					 u32 *burst_count);
int airoha_xpon_phy_get_gpon_fec_status(struct phy *phy,
					bool *downstream, bool *upstream);
int airoha_xpon_phy_set_gpon_overhead(struct phy *phy, u8 guard_bits,
				      u8 t1_pbits, u8 t2_pbits,
				      u8 t3_pattern,
				      const u8 delimiter[3]);
int airoha_xpon_phy_set_gpon_extended_preamble(struct phy *phy,
					       u8 o3_o4_preamble,
					       u8 o5_preamble);
int airoha_xpon_phy_set_gpon_oper_state(
	struct phy *phy, enum airoha_xpon_phy_gpon_oper_state state);

#endif /* __LINUX_PHY_AIROHA_XPON_H */
