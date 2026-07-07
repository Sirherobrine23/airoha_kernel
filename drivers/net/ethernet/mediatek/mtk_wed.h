// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (C) 2021 Felix Fietkau <nbd@nbd.name> */

#ifndef __MTK_WED_PRIV_H
#define __MTK_WED_PRIV_H

#include "../mtk_wed_private.h"

struct mtk_wdma_info {
	u8 wdma_idx;
	u8 queue;
	u16 wcid;
	u8 bss;
	u8 amsdu;
};

static inline u32 mtk_wed_get_pcie_base(struct mtk_wed_device *dev)
{
	if (!mtk_wed_is_v3_or_greater(dev->hw))
		return MTK_WED_PCIE_BASE;

	switch (dev->hw->index) {
	case 1:
		return MTK_WED_PCIE_BASE1;
	case 2:
		return MTK_WED_PCIE_BASE2;
	default:
		return MTK_WED_PCIE_BASE0;
	}
}

#if IS_ENABLED(CONFIG_NET_MEDIATEK_SOC_WED)
void mtk_wed_add_hw(struct device_node *np, struct mtk_eth *eth,
		    void __iomem *wdma, phys_addr_t wdma_phy,
		    int index);
void mtk_wed_exit(void);
int mtk_wed_flow_add(int index);
void mtk_wed_flow_remove(int index);
void mtk_wed_fe_reset(void);
void mtk_wed_fe_reset_complete(void);
#else
static inline void
mtk_wed_add_hw(struct device_node *np, struct mtk_eth *eth,
	       void __iomem *wdma, phys_addr_t wdma_phy,
	       int index)
{
}

static inline void mtk_wed_exit(void)
{
}

static inline int mtk_wed_flow_add(int index)
{
	return -EINVAL;
}

static inline void mtk_wed_flow_remove(int index)
{
}

static inline void mtk_wed_fe_reset(void)
{
}

static inline void mtk_wed_fe_reset_complete(void)
{
}
#endif

#ifdef CONFIG_DEBUG_FS
void mtk_wed_hw_add_debugfs(struct mtk_wed_hw *hw);
#else
static inline void mtk_wed_hw_add_debugfs(struct mtk_wed_hw *hw)
{
}
#endif

#endif
