// SPDX-License-Identifier: GPL-2.0-only
/*
 * Airoha EN7523 WED v1 debugfs support (TX offload state)
 *
 * Based on drivers/net/ethernet/mediatek/mtk_wed_debugfs.c
 * Copyright (C) 2021 Felix Fietkau <nbd@nbd.name>
 */

#include <linux/debugfs.h>
#include <linux/io.h>
#include <linux/seq_file.h>

#include "airoha_wed.h"

struct reg_dump {
	const char *name;
	u16 offset;
};

#define DUMP_WED(_reg) { #_reg, MTK_##_reg }
#define DUMP_END() { }

static const struct reg_dump wed_regs[] = {
	DUMP_WED(WED_CTRL),
	DUMP_WED(WED_RESET),
	DUMP_WED(WED_GLO_CFG),
	DUMP_WED(WED_INT_STATUS),
	DUMP_WED(WED_INT_MASK),
	DUMP_WED(WED_EXT_INT_STATUS),
	DUMP_WED(WED_EXT_INT_MASK),
	DUMP_WED(WED_TX_BM_CTRL),
	DUMP_WED(WED_TX_BM_BASE),
	DUMP_WED(WED_TX_BM_BUF_LEN),
	DUMP_WED(WED_TX_BM_DYN_THR),
	DUMP_WED(WED_TX_BM_INTF),
	DUMP_WED(WED_WPDMA_CFG_BASE),
	DUMP_WED(WED_WPDMA_GLO_CFG),
	DUMP_WED(WED_WPDMA_INT_TRIGGER),
	DUMP_WED(WED_WPDMA_INT_CTRL),
	DUMP_WED(WED_WPDMA_INT_MASK),
	DUMP_WED(WED_WDMA_CFG_BASE),
	DUMP_WED(WED_WDMA_OFFSET0),
	DUMP_WED(WED_WDMA_OFFSET1),
	DUMP_WED(WED_WDMA_GLO_CFG),
	DUMP_WED(WED_WDMA_INT_TRIGGER),
	DUMP_WED(WED_WDMA_INT_CTRL),
	DUMP_WED(WED_PCIE_CFG_BASE),
	DUMP_WED(WED_PCIE_INT_TRIGGER),
	DUMP_WED(WED_PCIE_INT_CTRL),
	DUMP_END()
};

static void
dump_wed_regs(struct seq_file *s, struct mtk_wed_device *dev,
	      const struct reg_dump *regs)
{
	const struct reg_dump *cur;

	for (cur = regs; cur->name; cur++) {
		u32 val;

		regmap_read(dev->hw->regs, cur->offset, &val);
		seq_printf(s, "%-32s %08x\n", cur->name, val);
	}
}

static void
dump_wed_tx_rings(struct seq_file *s, struct mtk_wed_device *dev)
{
	int i;

	seq_puts(s, "\nring path        base       count cpu_idx dma_idx\n");
	for (i = 0; i < ARRAY_SIZE(dev->tx_ring); i++) {
		static const char * const path[] = { "WED_TX", "WED_WPDMA" };
		u32 reg[] = { MTK_WED_RING_TX(i), MTK_WED_WPDMA_RING_TX(i) };
		int j;

		for (j = 0; j < ARRAY_SIZE(reg); j++) {
			u32 base = wed_r32(dev, reg[j] + MTK_WED_RING_OFS_BASE);
			u32 cnt = wed_r32(dev, reg[j] + MTK_WED_RING_OFS_COUNT);
			u32 cpu = wed_r32(dev, reg[j] +
					  MTK_WED_RING_OFS_CPU_IDX) &
				  GENMASK(15, 0);
			u32 dma = wed_r32(dev, reg[j] +
					  MTK_WED_RING_OFS_DMA_IDX) &
				  GENMASK(15, 0);

			seq_printf(s, "%4d %-11s 0x%08x %5u %7u %7u\n",
				   i, path[j], base, cnt, cpu, dma);
		}

		if (dev->tx_ring[i].wpdma) {
			void __iomem *w = dev->tx_ring[i].wpdma;
			u32 base = readl(w + MTK_WED_RING_OFS_BASE);
			u32 cnt = readl(w + MTK_WED_RING_OFS_COUNT);
			u32 cpu = readl(w + MTK_WED_RING_OFS_CPU_IDX) &
				  GENMASK(15, 0);
			u32 dma = readl(w + MTK_WED_RING_OFS_DMA_IDX) &
				  GENMASK(15, 0);

			seq_printf(s, "%4d %-11s 0x%08x %5u %7u %7u\n",
				   i, "WLAN_WPDMA", base, cnt, cpu, dma);
		}
	}
}

static int
wed_txinfo_show(struct seq_file *s, void *data)
{
	struct mtk_wed_hw *hw = s->private;
	struct mtk_wed_device *dev = hw->wed_dev;

	if (!dev)
		return 0;

	seq_printf(s, "WED%d version %u, WDMA%d, flow block users %u\n\n",
		   hw->index, dev->version, dev->wdma_idx,
		   READ_ONCE(hw->num_flows));

	dump_wed_regs(s, dev, wed_regs);
	dump_wed_tx_rings(s, dev);

	return 0;
}

DEFINE_SHOW_ATTRIBUTE(wed_txinfo);

static int
wed_regval_get(void *data, u64 *val)
{
	struct mtk_wed_hw *hw = data;
	u32 regval = 0;
	int err;

	err = regmap_read(hw->regs, hw->debugfs_reg, &regval);
	if (err)
		return err;

	*val = regval;

	return 0;
}

static int
wed_regval_set(void *data, u64 val)
{
	struct mtk_wed_hw *hw = data;

	return regmap_write(hw->regs, hw->debugfs_reg, val);
}

DEFINE_DEBUGFS_ATTRIBUTE(wed_regval_fops, wed_regval_get, wed_regval_set,
			 "0x%08llx\n");

void airoha_wed_hw_add_debugfs(struct mtk_wed_hw *hw)
{
	struct dentry *dir;

	dir = debugfs_create_dir(hw->dirname, NULL);
	if (IS_ERR(dir))
		return;

	hw->debugfs_dir = dir;
	debugfs_create_u32("regidx", 0600, dir, &hw->debugfs_reg);
	debugfs_create_file_unsafe("regval", 0600, dir, hw, &wed_regval_fops);
	debugfs_create_file_unsafe("txinfo", 0400, dir, hw, &wed_txinfo_fops);
}
