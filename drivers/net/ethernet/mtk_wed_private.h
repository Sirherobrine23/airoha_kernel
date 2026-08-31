/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2021 Felix Fietkau <nbd@nbd.name> */

#ifndef __MTK_WED_COMMON_PRIV_H
#define __MTK_WED_COMMON_PRIV_H

#include <linux/debugfs.h>
#include <linux/netdevice.h>
#include <linux/of.h>
#include <linux/regmap.h>
#include <linux/soc/mediatek/mtk_wed.h>

#include "mediatek/mtk_wed_regs.h"

struct mtk_eth;
struct mtk_wed_wo;

struct mtk_wed_soc_data {
	struct {
		u32 tx_bm_tkid;
		u32 wpdma_rx_ring[MTK_WED_RX_QUEUES];
		u32 reset_idx_tx_mask;
		u32 reset_idx_rx_mask;
	} regmap;
	u32 tx_ring_desc_size;
	u32 wdma_desc_size;
};

struct mtk_wed_amsdu {
	void *txd;
	dma_addr_t txd_phy;
};

struct mtk_wed_hw {
	const struct mtk_wed_soc_data *soc;
	struct device_node *node;
	struct mtk_eth *eth;
	struct regmap *regs;
	struct regmap *hifsys;
	struct device *dev;
	void __iomem *pcie;
	void __iomem *wdma;
	phys_addr_t wdma_phy;
	struct regmap *mirror;
	struct dentry *debugfs_dir;
	struct mtk_wed_device *wed_dev;
	struct mtk_wed_wo *wed_wo;
	struct mtk_wed_amsdu *wed_amsdu;
	u32 pcie_base;
	u32 wpdma_base;
	u32 debugfs_reg;
	u32 irq_mask;
	u32 num_flows;
	u8 version;
	char dirname[5];
	int irq;
	int index;
};

static inline bool mtk_wed_is_v1(struct mtk_wed_hw *hw)
{
	return hw->version == 1;
}

static inline bool mtk_wed_is_v2(struct mtk_wed_hw *hw)
{
	return hw->version == 2;
}

static inline bool mtk_wed_is_v3(struct mtk_wed_hw *hw)
{
	return hw->version == 3;
}

static inline bool mtk_wed_is_v3_or_greater(struct mtk_wed_hw *hw)
{
	return hw->version > 2;
}

static inline void
wed_w32(struct mtk_wed_device *dev, u32 reg, u32 val)
{
	regmap_write(dev->hw->regs, reg, val);
}

static inline u32
wed_r32(struct mtk_wed_device *dev, u32 reg)
{
	unsigned int val;

	regmap_read(dev->hw->regs, reg, &val);

	return val;
}

static inline void
wdma_w32(struct mtk_wed_device *dev, u32 reg, u32 val)
{
	writel(val, dev->hw->wdma + reg);
}

static inline u32
wdma_r32(struct mtk_wed_device *dev, u32 reg)
{
	return readl(dev->hw->wdma + reg);
}

static inline u32
wpdma_tx_r32(struct mtk_wed_device *dev, int ring, u32 reg)
{
	if (!dev->tx_ring[ring].wpdma)
		return 0;

	return readl(dev->tx_ring[ring].wpdma + reg);
}

static inline void
wpdma_tx_w32(struct mtk_wed_device *dev, int ring, u32 reg, u32 val)
{
	if (!dev->tx_ring[ring].wpdma)
		return;

	writel(val, dev->tx_ring[ring].wpdma + reg);
}

static inline u32
wpdma_rx_r32(struct mtk_wed_device *dev, int ring, u32 reg)
{
	if (!dev->rx_ring[ring].wpdma)
		return 0;

	return readl(dev->rx_ring[ring].wpdma + reg);
}

static inline void
wpdma_rx_w32(struct mtk_wed_device *dev, int ring, u32 reg, u32 val)
{
	if (!dev->rx_ring[ring].wpdma)
		return;

	writel(val, dev->rx_ring[ring].wpdma + reg);
}

static inline u32
wpdma_txfree_r32(struct mtk_wed_device *dev, u32 reg)
{
	if (!dev->txfree_ring.wpdma)
		return 0;

	return readl(dev->txfree_ring.wpdma + reg);
}

static inline void
wpdma_txfree_w32(struct mtk_wed_device *dev, u32 reg, u32 val)
{
	if (!dev->txfree_ring.wpdma)
		return;

	writel(val, dev->txfree_ring.wpdma + reg);
}

#endif
