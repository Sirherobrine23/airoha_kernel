/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * EN7523 WED (Wireless Ethernet Dispatch) private interface.
 */

#ifndef __AIROHA_WED_PRIV_H
#define __AIROHA_WED_PRIV_H

#include "../mtk_wed_private.h"

/* EN7523 PCIe controller bases (PCIe0 = domain 0, PCIe1 = domain 1). */
#define EN7523_PCIE_BASE(n)	(0x1fa91000 + (n) * 0x1000)

#if IS_ENABLED(CONFIG_NET_AIROHA_SOC_WED)
int airoha_wed_add_hw(struct device_node *np, int index);
void airoha_wed_exit(void);
int airoha_wed_flow_add(int index);
void airoha_wed_flow_remove(int index);
void airoha_wed_fe_reset(void);
void airoha_wed_fe_reset_complete(void);
#else
static inline int airoha_wed_add_hw(struct device_node *np, int index)
{
	of_node_put(np);
	return 0;
}

static inline void airoha_wed_exit(void)
{
}

static inline int airoha_wed_flow_add(int index)
{
	return -EINVAL;
}

static inline void airoha_wed_flow_remove(int index)
{
}

static inline void airoha_wed_fe_reset(void)
{
}

static inline void airoha_wed_fe_reset_complete(void)
{
}
#endif

#ifdef CONFIG_DEBUG_FS
void airoha_wed_hw_add_debugfs(struct mtk_wed_hw *hw);
#else
static inline void airoha_wed_hw_add_debugfs(struct mtk_wed_hw *hw)
{
}
#endif

#endif
