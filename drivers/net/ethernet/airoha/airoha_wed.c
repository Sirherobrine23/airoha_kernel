// SPDX-License-Identifier: GPL-2.0-only
/*
 * Airoha EN7523 WED v1 (Wireless Ethernet Dispatch) driver
 *
 * Implements the mtk_wed_ops interface so that standard mt76 WiFi drivers
 * can offload TX to WED v1 hardware on Airoha EN7523 SoCs.
 *
 * Based on drivers/net/ethernet/mediatek/mtk_wed.c
 * Copyright (C) 2021 Felix Fietkau <nbd@nbd.name>
 */

#include <linux/bitfield.h>
#include <linux/debugfs.h>
#include <linux/dma-mapping.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/skbuff.h>
#include <linux/slab.h>
#include <linux/soc/mediatek/mtk_wed.h>

#include "airoha_eth.h"
#include "airoha_wed.h"

#define MTK_WED_PKT_SIZE		1900
#define MTK_WED_BUF_SIZE		2048
#define MTK_WED_BUF_PER_PAGE		(PAGE_SIZE / MTK_WED_BUF_SIZE)
#define MTK_WED_TX_RING_SIZE		2048
#define MTK_WED_WDMA_RING_SIZE		1024
#define MTK_WED_DLY_INT_CFG		0x210
#define MTK_WED_DLY_INT_VALUE		0xc014c014
#define AIROHA_WED_EXT_INT_MASK_VENDOR	0x2c008003
#define AIROHA_WED_PCIE_OFST		0x564
#define AIROHA_WED_WPDMA_OFST0		0x584
#define AIROHA_WED_WPDMA_OFST1		0x588
#define AIROHA_WED_PCIE_OFST_VENDOR	0x04200424
#define AIROHA_PCIE_INT_MASK		0x420
#define AIROHA_PCIE_MSI_MASK		BIT(23)

struct airoha_wed_soc_data {
	phys_addr_t pcie_base[2];
};

static struct mtk_wed_hw *hw_list[2];
static DEFINE_MUTEX(hw_lock);

static struct {
	u32 msi_mask;
	bool saved;
} pcie_irq[ARRAY_SIZE(hw_list)];

static void airoha_wed_mask_pcie_msi(struct mtk_wed_hw *hw)
{
	u32 mask = readl(hw->pcie + AIROHA_PCIE_INT_MASK);

	/* Repeated starts must retain the mask from before WED took ownership. */
	if (!pcie_irq[hw->index].saved) {
		pcie_irq[hw->index].msi_mask = mask & AIROHA_PCIE_MSI_MASK;
		pcie_irq[hw->index].saved = true;
	}
	writel(mask | AIROHA_PCIE_MSI_MASK, hw->pcie + AIROHA_PCIE_INT_MASK);
}

static void airoha_wed_restore_pcie_msi(struct mtk_wed_hw *hw)
{
	u32 mask;

	if (!pcie_irq[hw->index].saved)
		return;

	mask = readl(hw->pcie + AIROHA_PCIE_INT_MASK);
	mask &= ~AIROHA_PCIE_MSI_MASK;
	mask |= pcie_irq[hw->index].msi_mask;
	writel(mask, hw->pcie + AIROHA_PCIE_INT_MASK);
	pcie_irq[hw->index].saved = false;
}

static const struct mtk_wed_soc_data airoha_wed_v1_data = {
	.regmap = {
		.tx_bm_tkid		= 0x088,
		.wpdma_rx_ring		= { 0x770, },
		.reset_idx_tx_mask	= GENMASK(3, 0),
		.reset_idx_rx_mask	= GENMASK(17, 16),
	},
	.tx_ring_desc_size = sizeof(struct mtk_wdma_desc),
	.wdma_desc_size = sizeof(struct mtk_wdma_desc),
};

static const struct airoha_wed_soc_data en7523_wed_data = {
	.pcie_base = { 0x1fa91000, 0x1fa92000 },
};

static const struct of_device_id airoha_wed_of_match[] = {
	{ .compatible = "airoha,en7523-wed", .data = &en7523_wed_data },
	{ /* sentinel */ }
};

static void
wed_m32(struct mtk_wed_device *dev, u32 reg, u32 mask, u32 val)
{
	regmap_update_bits(dev->hw->regs, reg, mask | val, val);
}

static void
wed_set(struct mtk_wed_device *dev, u32 reg, u32 mask)
{
	wed_m32(dev, reg, 0, mask);
}

static void
wed_clr(struct mtk_wed_device *dev, u32 reg, u32 mask)
{
	wed_m32(dev, reg, mask, 0);
}

static void
wdma_m32(struct mtk_wed_device *dev, u32 reg, u32 mask, u32 val)
{
	wdma_w32(dev, reg, (wdma_r32(dev, reg) & ~mask) | val);
}

static void
wdma_set(struct mtk_wed_device *dev, u32 reg, u32 mask)
{
	wdma_m32(dev, reg, 0, mask);
}

static void
wdma_clr(struct mtk_wed_device *dev, u32 reg, u32 mask)
{
	wdma_m32(dev, reg, mask, 0);
}

static void
wlan_w32(struct mtk_wed_device *dev, u32 phys, u32 val)
{
	if (!dev->wlan.base || phys < dev->wlan.phy_base)
		return;

	writel(val, dev->wlan.base + phys - dev->wlan.phy_base);
}

static u32
mtk_wed_wlan_irq_mask(struct mtk_wed_device *dev, u32 mask)
{
	return mask | BIT(dev->wlan.rx_tbit[0]);
}

static u32
mtk_wed_core_irq_mask(struct mtk_wed_device *dev, u32 mask)
{
	return mask;
}

static u32
mtk_wed_ext_irq_mask(struct mtk_wed_device *dev, u32 mask)
{
	return mask ? AIROHA_WED_EXT_INT_MASK_VENDOR : 0;
}

static u32
mtk_wed_wpdma_irq_mask(struct mtk_wed_device *dev, u32 mask)
{
	return mtk_wed_wlan_irq_mask(dev, mask) |
	       BIT(dev->wlan.tx_tbit[0]) | BIT(dev->wlan.tx_tbit[1]);
}

static u32
mtk_wed_read_reset(struct mtk_wed_device *dev)
{
	return wed_r32(dev, MTK_WED_RESET);
}

static u32
mtk_wdma_read_reset(struct mtk_wed_device *dev)
{
	return wdma_r32(dev, MTK_WDMA_GLO_CFG);
}

static int
mtk_wdma_rx_reset(struct mtk_wed_device *dev)
{
	u32 status, mask = MTK_WDMA_GLO_CFG_RX_DMA_BUSY;
	int i, ret;

	wdma_clr(dev, MTK_WDMA_GLO_CFG, MTK_WDMA_GLO_CFG_RX_DMA_EN);
	ret = readx_poll_timeout(mtk_wdma_read_reset, dev, status,
				 !(status & mask), 0, 10000);
	if (ret)
		dev_err(dev->hw->dev, "rx reset failed\n");

	wdma_w32(dev, MTK_WDMA_RESET_IDX, MTK_WDMA_RESET_IDX_RX);
	wdma_w32(dev, MTK_WDMA_RESET_IDX, 0);

	for (i = 0; i < ARRAY_SIZE(dev->rx_wdma); i++) {
		if (dev->rx_wdma[i].desc)
			continue;

		wdma_w32(dev,
			 MTK_WDMA_RING_RX(i) + MTK_WED_RING_OFS_CPU_IDX, 0);
	}

	return ret;
}

static u32
mtk_wed_check_busy(struct mtk_wed_device *dev, u32 reg, u32 mask)
{
	return !!(wed_r32(dev, reg) & mask);
}

static int
mtk_wed_poll_busy(struct mtk_wed_device *dev, u32 reg, u32 mask)
{
	int sleep = 15000;
	int timeout = 100 * sleep;
	u32 val;

	return read_poll_timeout(mtk_wed_check_busy, val, !val, sleep,
				 timeout, false, dev, reg, mask);
}

static int
mtk_wed_reset(struct mtk_wed_device *dev, u32 mask)
{
	u32 status;
	int ret;

	wed_w32(dev, MTK_WED_RESET, mask);
	ret = readx_poll_timeout(mtk_wed_read_reset, dev, status,
				 !(status & mask), 0, 1000);
	if (ret) {
		dev_err(dev->hw->dev,
			"wed%d: reset timeout mask=%08x status=%08x\n",
			dev->hw->index, mask, status);
		return ret;
	}

	return 0;
}

void airoha_wed_fe_reset(void)
{
	int i;

	mutex_lock(&hw_lock);

	for (i = 0; i < ARRAY_SIZE(hw_list); i++) {
		struct mtk_wed_hw *hw = hw_list[i];
		struct mtk_wed_device *dev;
		int err;

		if (!hw)
			continue;

		dev = hw->wed_dev;
		if (!dev || !dev->wlan.reset)
			continue;

		err = dev->wlan.reset(dev);
		if (err)
			dev_err(dev->dev, "wlan reset failed: %d\n", err);
	}

	mutex_unlock(&hw_lock);
}

void airoha_wed_fe_reset_complete(void)
{
	int i;

	mutex_lock(&hw_lock);

	for (i = 0; i < ARRAY_SIZE(hw_list); i++) {
		struct mtk_wed_hw *hw = hw_list[i];
		struct mtk_wed_device *dev;

		if (!hw)
			continue;

		dev = hw->wed_dev;
		if (!dev || !dev->wlan.reset_complete)
			continue;

		dev->wlan.reset_complete(dev);
	}

	mutex_unlock(&hw_lock);
}

static struct mtk_wed_hw *
mtk_wed_assign(struct mtk_wed_device *dev)
{
	struct mtk_wed_hw *hw;

	/* Match the WED instance to the PCI root port/domain. */
	if (dev->wlan.bus_type == MTK_WED_BUS_PCIE) {
		struct pci_dev *pdev = dev->wlan.pci_dev;
		struct pci_host_bridge *bridge = pci_find_host_bridge(pdev->bus);
		struct device_node *np = dev_of_node(bridge->dev.parent);
		int index = pci_domain_nr(pdev->bus);
		struct resource res;
		u32 revision, reset;
		int i, err;

		/* Dedicated host bridges both expose root-port slot 0. Match
		 * their controller address before considering legacy slot IDs.
		 */
		if (!of_address_to_resource(np, 0, &res)) {
			for (i = 0; i < ARRAY_SIZE(hw_list); i++) {
				if (hw_list[i] &&
				    hw_list[i]->pcie_base == res.start) {
					index = i;
					goto found;
				}
			}
		}
		if (pdev->bus->self)
			index = PCI_SLOT(pdev->bus->self->devfn);
found:
		if (index < 0 || index >= ARRAY_SIZE(hw_list))
			return NULL;

		hw = hw_list[index];
		if (!hw)
			return NULL;

		/* Reject an inaccessible block before changing IRQ routing. */
		err = regmap_read(hw->regs, 0, &revision);
		if (!err)
			err = regmap_read(hw->regs, MTK_WED_RESET, &reset);
		if (err) {
			dev_err(hw->dev, "WED%d register read failed: %d\n",
				hw->index, err);
			return NULL;
		}
		if (revision == 0xdeadbeef || reset == 0xdeadbeef) {
			dev_err(hw->dev,
				"WED%d MMIO unavailable: revision=%08x reset=%08x\n",
				hw->index, revision, reset);
			return NULL;
		}

		if (hw->wed_dev)
			return NULL;

		hw->wed_dev = dev;
		dev_dbg(hw->dev, "wed%d: assigned to WLAN device %s\n",
			hw->index, dev_name(&dev->wlan.pci_dev->dev));
		return hw;
	}

	return NULL;
}

static int
mtk_wed_tx_buffer_alloc(struct mtk_wed_device *dev)
{
	u32 desc_size = dev->hw->soc->tx_ring_desc_size;
	int i, page_idx = 0, n_pages, ring_size;
	int token = dev->wlan.token_start;
	struct mtk_wed_buf *page_list;
	dma_addr_t desc_phys;
	void *desc_ptr;

	ring_size = dev->wlan.nbuf & ~(MTK_WED_BUF_PER_PAGE - 1);
	dev->tx_buf_ring.size = ring_size;
	n_pages = dev->tx_buf_ring.size / MTK_WED_BUF_PER_PAGE;

	page_list = kcalloc(n_pages, sizeof(*page_list), GFP_KERNEL);
	if (!page_list)
		return -ENOMEM;

	dev->tx_buf_ring.pages = page_list;

	desc_ptr = dma_alloc_coherent(dev->hw->dev,
				      dev->tx_buf_ring.size * desc_size,
				      &desc_phys, GFP_KERNEL);
	if (!desc_ptr)
		return -ENOMEM;

	dev->tx_buf_ring.desc = desc_ptr;
	dev->tx_buf_ring.desc_phys = desc_phys;

	for (i = 0; i < ring_size; i += MTK_WED_BUF_PER_PAGE) {
		dma_addr_t page_phys, buf_phys;
		struct page *page;
		void *buf;
		int s;

		page = __dev_alloc_page(GFP_KERNEL | GFP_DMA32);
		if (!page)
			return -ENOMEM;

		page_phys = dma_map_page(dev->hw->dev, page, 0, PAGE_SIZE,
					 DMA_BIDIRECTIONAL);
		if (dma_mapping_error(dev->hw->dev, page_phys)) {
			__free_page(page);
			return -ENOMEM;
		}

		page_list[page_idx].p = page;
		page_list[page_idx++].phy_addr = page_phys;
		dma_sync_single_for_cpu(dev->hw->dev, page_phys, PAGE_SIZE,
					DMA_BIDIRECTIONAL);

		buf = page_to_virt(page);
		buf_phys = page_phys;

		for (s = 0; s < MTK_WED_BUF_PER_PAGE; s++) {
			struct mtk_wdma_desc *desc = desc_ptr;
			u32 txd_size, ctrl;

			desc->buf0 = cpu_to_le32(buf_phys);
			txd_size = dev->wlan.init_buf(buf, buf_phys, token++);
			if (WARN_ON_ONCE(!txd_size ||
					 txd_size + MTK_WED_PKT_SIZE >
					 MTK_WED_BUF_SIZE))
				return -EINVAL;

			desc->buf1 = cpu_to_le32(buf_phys + txd_size);
			ctrl = FIELD_PREP(MTK_WDMA_DESC_CTRL_LEN0, txd_size) |
			       MTK_WDMA_DESC_CTRL_LAST_SEG1 |
			       FIELD_PREP(MTK_WDMA_DESC_CTRL_LEN1,
					  MTK_WED_BUF_SIZE - txd_size);
			desc->ctrl = cpu_to_le32(ctrl);
			desc->info = 0;

			desc_ptr += desc_size;
			buf += MTK_WED_BUF_SIZE;
			buf_phys += MTK_WED_BUF_SIZE;
		}

		dma_sync_single_for_device(dev->hw->dev, page_phys, PAGE_SIZE,
					   DMA_BIDIRECTIONAL);
	}

	return 0;
}

static void
mtk_wed_free_tx_buffer(struct mtk_wed_device *dev)
{
	struct mtk_wed_buf *page_list = dev->tx_buf_ring.pages;
	struct mtk_wed_hw *hw = dev->hw;
	int i, page_idx = 0;

	if (!page_list)
		return;

	if (!dev->tx_buf_ring.desc)
		goto free_pagelist;

	for (i = 0; i < dev->tx_buf_ring.size; i += MTK_WED_BUF_PER_PAGE) {
		dma_addr_t page_phy = page_list[page_idx].phy_addr;
		void *page = page_list[page_idx++].p;

		if (!page)
			break;

		dma_unmap_page(dev->hw->dev, page_phy, PAGE_SIZE,
			       DMA_BIDIRECTIONAL);
		__free_page(page);
	}

	dma_free_coherent(dev->hw->dev,
			  dev->tx_buf_ring.size * hw->soc->tx_ring_desc_size,
			  dev->tx_buf_ring.desc, dev->tx_buf_ring.desc_phys);

free_pagelist:
	kfree(page_list);
}

static void
mtk_wed_free_ring(struct mtk_wed_device *dev, struct mtk_wed_ring *ring)
{
	if (!ring->desc)
		return;

	dma_free_coherent(dev->hw->dev, ring->size * ring->desc_size,
			  ring->desc, ring->desc_phys);
}

static void
mtk_wed_free_tx_rings(struct mtk_wed_device *dev)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(dev->tx_ring); i++)
		mtk_wed_free_ring(dev, &dev->tx_ring[i]);
	for (i = 0; i < ARRAY_SIZE(dev->rx_wdma); i++)
		mtk_wed_free_ring(dev, &dev->rx_wdma[i]);
}

static void
mtk_wed_set_ext_int(struct mtk_wed_device *dev, bool en, u32 irq_mask)
{
	u32 mask = irq_mask ?: wed_r32(dev, MTK_WED_INT_MASK);

	wed_w32(dev, MTK_WED_EXT_INT_MASK,
		en ? mtk_wed_ext_irq_mask(dev, mask) : 0);
	wed_r32(dev, MTK_WED_EXT_INT_MASK);
}

static void
mtk_wed_dma_disable(struct mtk_wed_device *dev)
{
	wed_clr(dev, MTK_WED_WPDMA_GLO_CFG,
		MTK_WED_WPDMA_GLO_CFG_TX_DRV_EN |
		MTK_WED_WPDMA_GLO_CFG_RX_DRV_EN);

	wed_clr(dev, MTK_WED_WDMA_GLO_CFG, MTK_WED_WDMA_GLO_CFG_RX_DRV_EN);

	wed_clr(dev, MTK_WED_GLO_CFG,
		MTK_WED_GLO_CFG_TX_DMA_EN |
		MTK_WED_GLO_CFG_RX_DMA_EN);

	wdma_clr(dev, MTK_WDMA_GLO_CFG,
		 MTK_WDMA_GLO_CFG_RX_DMA_EN |
		 MTK_WDMA_GLO_CFG_RX_INFO1_PRERES |
		 MTK_WDMA_GLO_CFG_RX_INFO2_PRERES |
		 MTK_WDMA_GLO_CFG_RX_INFO3_PRERES);
}

static void
mtk_wed_stop(struct mtk_wed_device *dev)
{
	wlan_w32(dev, dev->wlan.wpdma_mask, 0);
	wed_w32(dev, MTK_WED_INT_MASK, 0);
	wed_w32(dev, MTK_WED_WPDMA_INT_MASK, 0);
	mtk_wed_dma_disable(dev);
	mtk_wed_set_ext_int(dev, false, 0);

	wed_w32(dev, MTK_WED_WPDMA_INT_TRIGGER, 0);
	wed_w32(dev, MTK_WED_WDMA_INT_TRIGGER, 0);
	wdma_w32(dev, MTK_WDMA_INT_MASK, 0);
	wdma_w32(dev, MTK_WDMA_INT_GRP2, 0);
	dev->running = false;
}

static void
mtk_wed_deinit(struct mtk_wed_device *dev)
{
	mtk_wed_stop(dev);

	wed_clr(dev, MTK_WED_CTRL,
		MTK_WED_CTRL_WDMA_INT_AGENT_EN |
		MTK_WED_CTRL_WPDMA_INT_AGENT_EN |
		MTK_WED_CTRL_WED_TX_BM_EN |
		MTK_WED_CTRL_WED_TX_FREE_AGENT_EN);
}

static void
__mtk_wed_detach(struct mtk_wed_device *dev)
{
	struct mtk_wed_hw *hw = dev->hw;

	mtk_wed_deinit(dev);

	mtk_wdma_rx_reset(dev);
	mtk_wed_reset(dev, MTK_WED_RESET_WED);
	mtk_wed_free_tx_buffer(dev);
	mtk_wed_free_tx_rings(dev);
	airoha_wed_restore_pcie_msi(hw);

	memset(dev, 0, sizeof(*dev));
	module_put(THIS_MODULE);

	hw->wed_dev = NULL;
	hw->num_flows = 0;
}

static void
mtk_wed_detach(struct mtk_wed_device *dev)
{
	mutex_lock(&hw_lock);
	__mtk_wed_detach(dev);
	mutex_unlock(&hw_lock);
}

static void
mtk_wed_set_wpdma(struct mtk_wed_device *dev)
{
	/* Preserve the PCI BAR assigned by Linux, including on port 1. */
	wed_w32(dev, MTK_WED_WPDMA_CFG_BASE, dev->wlan.wpdma_phys);
	/* The WiFi driver supplies BAR-relative addresses for each chip. */
	wed_w32(dev, AIROHA_WED_WPDMA_OFST0,
		((dev->wlan.wpdma_mask - dev->wlan.wpdma_phys) << 16) |
		(dev->wlan.wpdma_int - dev->wlan.wpdma_phys));
	wed_w32(dev, AIROHA_WED_WPDMA_OFST1,
		((dev->wlan.wpdma_txfree - dev->wlan.wpdma_phys -
		  0x10) << 16) |
		(dev->wlan.wpdma_tx - dev->wlan.wpdma_phys));
}

static int
mtk_wed_hw_init_early(struct mtk_wed_device *dev)
{
	u32 mask = MTK_WED_WDMA_GLO_CFG_BT_SIZE |
		   MTK_WED_WDMA_GLO_CFG_DYNAMIC_DMAD_RECYCLE |
		   MTK_WED_WDMA_GLO_CFG_DYNAMIC_SKIP_DMAD_PREP |
		   MTK_WED_WDMA_GLO_CFG_IDLE_DMAD_SUPPLY |
		   MTK_WED_WDMA_GLO_CFG_RX_DIS_FSM_AUTO_IDLE |
		   MTK_WED_WDMA_GLO_CFG_AXI_W_AFTER_AW |
		   MTK_WED_WDMA_GLO_CFG_WCOMPLETE_SEL;
	u32 set = FIELD_PREP(MTK_WED_WDMA_GLO_CFG_BT_SIZE, 2) |
		  MTK_WED_WDMA_GLO_CFG_DYNAMIC_SKIP_DMAD_PREP |
		  MTK_WED_WDMA_GLO_CFG_IDLE_DMAD_SUPPLY |
		  MTK_WED_WDMA_GLO_CFG_AXI_W_AFTER_AW |
		  MTK_WED_WDMA_GLO_CFG_WCOMPLETE_SEL;
	int ret;

	mtk_wed_deinit(dev);
	ret = mtk_wed_reset(dev, MTK_WED_RESET_WED);
	if (ret)
		return ret;

	mtk_wed_set_wpdma(dev);
	wed_m32(dev, MTK_WED_WDMA_GLO_CFG, mask, set);

	wed_w32(dev, MTK_WED_WDMA_CFG_BASE, 0x1fa00000);
	wed_w32(dev, MTK_WED_WDMA_OFFSET0,
		dev->hw->index ? 0x66046620 : 0x62046220);
	wed_w32(dev, MTK_WED_WDMA_OFFSET1,
		dev->hw->index ? 0x65006400 : 0x61006000);

	wed_w32(dev, MTK_WED_PCIE_CFG_BASE, dev->hw->pcie_base);
	wed_w32(dev, AIROHA_WED_PCIE_OFST, AIROHA_WED_PCIE_OFST_VENDOR);
	wed_w32(dev, MTK_WED_PCIE_INT_CTRL,
		FIELD_PREP(MTK_WED_PCIE_INT_CTRL_POLL_EN, 1));

	/* WDMA GLO_CFG: mark pre-reserved RX info words (v1 path) */
	wdma_set(dev, MTK_WDMA_GLO_CFG,
		 MTK_WDMA_GLO_CFG_RX_INFO1_PRERES |
		 MTK_WDMA_GLO_CFG_RX_INFO2_PRERES |
		 MTK_WDMA_GLO_CFG_RX_INFO3_PRERES);

	return 0;
}

static void
mtk_wed_hw_init(struct mtk_wed_device *dev)
{
	if (dev->init_done)
		return;

	dev->init_done = true;
	mtk_wed_set_ext_int(dev, false, 0);

	wed_w32(dev, MTK_WED_TX_BM_BASE, dev->tx_buf_ring.desc_phys);
	wed_w32(dev, MTK_WED_TX_BM_BUF_LEN, MTK_WED_PKT_SIZE);

	wed_w32(dev, MTK_WED_TX_BM_CTRL,
		MTK_WED_TX_BM_CTRL_PAUSE |
		FIELD_PREP(MTK_WED_TX_BM_CTRL_VLD_GRP_NUM,
			   dev->tx_buf_ring.size / 128) |
		FIELD_PREP(MTK_WED_TX_BM_CTRL_RSV_GRP_NUM,
			   MTK_WED_TX_RING_SIZE / 256));
	wed_w32(dev, MTK_WED_TX_BM_DYN_THR,
		FIELD_PREP(MTK_WED_TX_BM_DYN_THR_LO, 1) |
		MTK_WED_TX_BM_DYN_THR_HI);

	wed_w32(dev, dev->hw->soc->regmap.tx_bm_tkid,
		FIELD_PREP(MTK_WED_TX_BM_TKID_START, dev->wlan.token_start) |
		FIELD_PREP(MTK_WED_TX_BM_TKID_END,
			   dev->wlan.token_start + dev->wlan.nbuf - 1));

	mtk_wed_reset(dev, MTK_WED_RESET_TX_BM);

	wed_set(dev, MTK_WED_CTRL,
		MTK_WED_CTRL_WED_TX_BM_EN |
		MTK_WED_CTRL_WED_TX_FREE_AGENT_EN);

	wed_clr(dev, MTK_WED_TX_BM_CTRL, MTK_WED_TX_BM_CTRL_PAUSE);
}

static void
mtk_wed_ring_reset(struct mtk_wed_ring *ring, int size, bool tx)
{
	void *head = ring->desc;
	int i;

	for (i = 0; i < size; i++) {
		struct mtk_wdma_desc *desc = head + i * ring->desc_size;

		desc->buf0 = 0;
		desc->ctrl = tx ? cpu_to_le32(MTK_WDMA_DESC_CTRL_DMA_DONE)
				: cpu_to_le32(MTK_WFDMA_DESC_CTRL_TO_HOST);
		desc->buf1 = 0;
		desc->info = 0;
	}
}

static int
mtk_wed_ring_alloc(struct mtk_wed_device *dev, struct mtk_wed_ring *ring,
		   int size, u32 desc_size, bool tx)
{
	ring->desc = dma_alloc_coherent(dev->hw->dev, size * desc_size,
					&ring->desc_phys, GFP_KERNEL);
	if (!ring->desc)
		return -ENOMEM;

	ring->desc_size = desc_size;
	ring->size = size;
	mtk_wed_ring_reset(ring, size, tx);

	return 0;
}

static void
mtk_wed_reset_dma(struct mtk_wed_device *dev)
{
	bool busy = false;
	u32 val;
	int i;

	wlan_w32(dev, dev->wlan.wpdma_mask, 0);
	wed_w32(dev, MTK_WED_INT_MASK, 0);
	wed_w32(dev, MTK_WED_WPDMA_INT_MASK, 0);

	/* 1. Reset WED TX DMA */
	wed_clr(dev, MTK_WED_GLO_CFG, MTK_WED_GLO_CFG_TX_DMA_EN);
	busy = mtk_wed_poll_busy(dev, MTK_WED_GLO_CFG,
				 MTK_WED_GLO_CFG_TX_DMA_BUSY);
	if (busy) {
		mtk_wed_reset(dev, MTK_WED_RESET_WED_TX_DMA);
	} else {
		wed_w32(dev, MTK_WED_RESET_IDX,
			dev->hw->soc->regmap.reset_idx_tx_mask);
		wed_w32(dev, MTK_WED_RESET_IDX, 0);
	}

	/* 2. Reset WDMA RX DMA */
	busy = !!mtk_wdma_rx_reset(dev);
	wed_clr(dev, MTK_WED_WDMA_GLO_CFG, MTK_WED_WDMA_GLO_CFG_RX_DRV_EN);
	if (!busy)
		busy = mtk_wed_poll_busy(dev, MTK_WED_WDMA_GLO_CFG,
					 MTK_WED_WDMA_GLO_CFG_RX_DRV_BUSY);
	if (busy) {
		mtk_wed_reset(dev, MTK_WED_RESET_WDMA_INT_AGENT);
		mtk_wed_reset(dev, MTK_WED_RESET_WDMA_RX_DRV);
	} else {
		wed_w32(dev, MTK_WED_WDMA_RESET_IDX,
			MTK_WED_WDMA_RESET_IDX_RX |
			MTK_WED_WDMA_RESET_IDX_DRV);
		wed_w32(dev, MTK_WED_WDMA_RESET_IDX, 0);

		wed_set(dev, MTK_WED_WDMA_GLO_CFG,
			MTK_WED_WDMA_GLO_CFG_RST_INIT_COMPLETE);
		wed_clr(dev, MTK_WED_WDMA_GLO_CFG,
			MTK_WED_WDMA_GLO_CFG_RST_INIT_COMPLETE);
	}

	/* 3. Reset WED TX-free agent */
	wed_clr(dev, MTK_WED_CTRL, MTK_WED_CTRL_WED_TX_FREE_AGENT_EN);

	for (i = 0; i < 100; i++) {
		val = FIELD_GET(MTK_WED_TX_BM_INTF_TKFIFO_FDEP,
				wed_r32(dev, MTK_WED_TX_BM_INTF));
		if (val == 0x40)
			break;
	}

	mtk_wed_reset(dev, MTK_WED_RESET_TX_FREE_AGENT);
	wed_clr(dev, MTK_WED_CTRL, MTK_WED_CTRL_WED_TX_BM_EN);
	mtk_wed_reset(dev, MTK_WED_RESET_TX_BM);

	/* 4. Reset WED WPDMA TX/RX drivers */
	busy = mtk_wed_poll_busy(dev, MTK_WED_WPDMA_GLO_CFG,
				 MTK_WED_WPDMA_GLO_CFG_TX_DRV_BUSY);
	wed_clr(dev, MTK_WED_WPDMA_GLO_CFG,
		MTK_WED_WPDMA_GLO_CFG_TX_DRV_EN |
		MTK_WED_WPDMA_GLO_CFG_RX_DRV_EN);
	if (!busy)
		busy = mtk_wed_poll_busy(dev, MTK_WED_WPDMA_GLO_CFG,
					 MTK_WED_WPDMA_GLO_CFG_RX_DRV_BUSY);

	if (busy) {
		mtk_wed_reset(dev, MTK_WED_RESET_WPDMA_INT_AGENT);
		mtk_wed_reset(dev, MTK_WED_RESET_WPDMA_TX_DRV);
		mtk_wed_reset(dev, MTK_WED_RESET_WPDMA_RX_DRV);
	} else {
		wed_w32(dev, MTK_WED_WPDMA_RESET_IDX,
			MTK_WED_WPDMA_RESET_IDX_TX |
			MTK_WED_WPDMA_RESET_IDX_RX);
		wed_w32(dev, MTK_WED_WPDMA_RESET_IDX, 0);
	}

	for (i = 0; i < ARRAY_SIZE(dev->tx_ring); i++) {
		if (!dev->tx_ring[i].desc)
			continue;

		mtk_wed_ring_reset(&dev->tx_ring[i], MTK_WED_TX_RING_SIZE,
				   true);
	}

	dev->init_done = false;
}

static int
mtk_wed_wdma_rx_ring_setup(struct mtk_wed_device *dev, int idx, int size,
			   bool reset)
{
	struct mtk_wed_ring *wdma;

	if (idx >= ARRAY_SIZE(dev->rx_wdma))
		return -EINVAL;

	wdma = &dev->rx_wdma[idx];
	if (!reset && !wdma->desc &&
	    mtk_wed_ring_alloc(dev, wdma, size,
			       dev->hw->soc->wdma_desc_size, true))
		return -ENOMEM;

	wdma_w32(dev, MTK_WDMA_RING_RX(idx) + MTK_WED_RING_OFS_BASE,
		 wdma->desc_phys);
	wdma_w32(dev, MTK_WDMA_RING_RX(idx) + MTK_WED_RING_OFS_COUNT, size);
	wdma_w32(dev, MTK_WDMA_RING_RX(idx) + MTK_WED_RING_OFS_CPU_IDX, 0);
	if (reset)
		wdma_w32(dev, MTK_WDMA_RING_RX(idx) + MTK_WED_RING_OFS_DMA_IDX,
			 0);

	wed_w32(dev, MTK_WED_WDMA_RING_RX(idx) + MTK_WED_RING_OFS_BASE,
		wdma->desc_phys);
	wed_w32(dev, MTK_WED_WDMA_RING_RX(idx) + MTK_WED_RING_OFS_COUNT, size);
	if (reset) {
		wed_w32(dev, MTK_WED_WDMA_RING_RX(idx) +
			MTK_WED_RING_OFS_CPU_IDX, 0);
		wed_w32(dev, MTK_WED_WDMA_RING_RX(idx) +
			MTK_WED_RING_OFS_DMA_IDX, 0);
	}

	return 0;
}

static void
mtk_wed_configure_irq(struct mtk_wed_device *dev, u32 irq_mask)
{
	u32 wdma_mask = FIELD_PREP(MTK_WDMA_INT_MASK_RX_DONE, GENMASK(1, 0));
	u32 core_mask = mtk_wed_core_irq_mask(dev, irq_mask);
	u32 wlan_mask = mtk_wed_wlan_irq_mask(dev, irq_mask);
	u32 wpdma_mask = mtk_wed_wpdma_irq_mask(dev, irq_mask);

	dev->hw->irq_mask = irq_mask;

	/* Enable WED control agents */
	wed_set(dev, MTK_WED_CTRL,
		MTK_WED_CTRL_WDMA_INT_AGENT_EN |
		MTK_WED_CTRL_WPDMA_INT_AGENT_EN |
		MTK_WED_CTRL_WED_TX_BM_EN |
		MTK_WED_CTRL_WED_TX_FREE_AGENT_EN);

	/* v1: use PCIe interrupt trigger for WPDMA */
	wed_w32(dev, MTK_WED_PCIE_INT_TRIGGER, MTK_WED_PCIE_INT_TRIGGER_STATUS);

	wed_w32(dev, MTK_WED_WPDMA_INT_TRIGGER,
		MTK_WED_WPDMA_INT_TRIGGER_RX_DONE |
		MTK_WED_WPDMA_INT_TRIGGER_TX_DONE);

	wed_clr(dev, MTK_WED_WDMA_INT_CTRL, wdma_mask);
	wed_w32(dev, MTK_WED_WDMA_INT_TRIGGER, wdma_mask);

	wdma_w32(dev, MTK_WDMA_INT_MASK, wdma_mask);
	wdma_w32(dev, MTK_WDMA_INT_GRP2, wdma_mask);
	wed_w32(dev, MTK_WED_WPDMA_INT_MASK, wpdma_mask);
	wed_w32(dev, MTK_WED_INT_MASK, core_mask);
	/* Keep external errors masked until all DMA engines are running. */
	wed_w32(dev, MTK_WED_EXT_INT_MASK, 0);
	wed_w32(dev, MTK_WED_DLY_INT_CFG, MTK_WED_DLY_INT_VALUE);
	wlan_w32(dev, dev->wlan.wpdma_mask, wlan_mask);
}

static int
mtk_wed_dma_enable(struct mtk_wed_device *dev)
{
	u32 status;
	int ret;

	/* The WED RX driver must leave reset while the WDMA RX DMA is idle. */
	wdma_clr(dev, MTK_WDMA_GLO_CFG, MTK_WDMA_GLO_CFG_RX_DMA_EN);
	ret = readx_poll_timeout(mtk_wdma_read_reset, dev, status,
				 !(status & MTK_WDMA_GLO_CFG_RX_DMA_BUSY),
				 0, 10000);
	if (ret) {
		dev_err(dev->hw->dev,
			"failed to stop WDMA RX before WED start\n");
		return ret;
	}

	/* Discard errors latched while the WED block was being reset. */
	status = wed_r32(dev, MTK_WED_EXT_INT_STATUS);
	wed_w32(dev, MTK_WED_EXT_INT_STATUS, status);

	wed_set(dev, MTK_WED_WPDMA_INT_CTRL, MTK_WED_WPDMA_INT_CTRL_SUBRT_ADV);
	wed_set(dev, MTK_WED_WPDMA_GLO_CFG,
		MTK_WED_WPDMA_GLO_CFG_TX_DRV_EN |
		MTK_WED_WPDMA_GLO_CFG_RX_DRV_EN);
	wed_set(dev, MTK_WED_WPDMA_CTRL, MTK_WED_WPDMA_CTRL_SDL1_FIXED);
	wed_set(dev, MTK_WED_WDMA_GLO_CFG, MTK_WED_WDMA_GLO_CFG_RX_DRV_EN);
	wdma_set(dev, MTK_WDMA_GLO_CFG, MTK_WDMA_GLO_CFG_RX_DMA_EN);
	wed_set(dev, MTK_WED_GLO_CFG,
		MTK_WED_GLO_CFG_TX_DMA_EN |
		MTK_WED_GLO_CFG_RX_DMA_EN);

	return 0;
}

static void
mtk_wed_start(struct mtk_wed_device *dev, u32 irq_mask)
{
	int err;

	airoha_wed_mask_pcie_msi(dev->hw);

	mtk_wed_hw_init(dev);
	mtk_wed_configure_irq(dev, irq_mask);

	err = mtk_wed_dma_enable(dev);
	if (err) {
		mtk_wed_stop(dev);
		return;
	}

	mtk_wed_set_ext_int(dev, true, irq_mask);
	dev->running = true;
	wed_w32(dev, MTK_WED_INT_MASK, mtk_wed_core_irq_mask(dev, irq_mask));
}

static int
mtk_wed_attach(struct mtk_wed_device *dev)
	__releases(RCU)
{
	struct mtk_wed_hw *hw;
	int i, ret = 0;

	RCU_LOCKDEP_WARN(!rcu_read_lock_held(),
			 "mtk_wed_attach without holding the RCU read lock");

	if (dev->wlan.bus_type != MTK_WED_BUS_PCIE ||
	    !try_module_get(THIS_MODULE))
		ret = -ENODEV;

	rcu_read_unlock();

	if (ret)
		return ret;

	mutex_lock(&hw_lock);

	hw = mtk_wed_assign(dev);
	if (!hw) {
		module_put(THIS_MODULE);
		ret = -ENODEV;
		goto unlock;
	}

	dev->hw = hw;
	dev->dev = hw->dev;
	dev->irq = hw->irq;
	dev->wdma_idx = hw->index;
	dev->version = hw->version;

	ret = dma_set_mask_and_coherent(hw->dev, DMA_BIT_MASK(32));
	if (ret)
		goto out;

	ret = mtk_wed_tx_buffer_alloc(dev);
	if (ret)
		goto out;

	ret = mtk_wed_hw_init_early(dev);
	if (ret)
		goto out;

	for (i = 0; i < ARRAY_SIZE(dev->rx_wdma); i++) {
		ret = mtk_wed_wdma_rx_ring_setup(dev, i,
						 MTK_WED_WDMA_RING_SIZE,
						 false);
		if (ret)
			goto out;
	}

out:
	if (ret) {
		dev_err(dev->hw->dev, "failed to attach wed device\n");
		__mtk_wed_detach(dev);
	}
unlock:
	mutex_unlock(&hw_lock);

	return ret;
}

static void
airoha_wed_clear_unused_tx_ring(struct mtk_wed_device *dev)
{
	u32 wpdma_phys, offset;

	if (dev->tx_ring[1].desc || !dev->wlan.base ||
	    dev->wlan.wpdma_tx < dev->wlan.phy_base)
		return;

	wpdma_phys = dev->wlan.wpdma_tx +
		     MTK_WED_RING_TX(1) - MTK_WED_RING_TX(0);

	for (offset = 0; offset <= MTK_WED_RING_OFS_DMA_IDX; offset += 4) {
		wed_w32(dev, MTK_WED_RING_TX(1) + offset, 0);
		wlan_w32(dev, wpdma_phys + offset, 0);
		wed_w32(dev, MTK_WED_WPDMA_RING_TX(1) + offset, 0);
	}
}

static int
mtk_wed_tx_ring_setup(struct mtk_wed_device *dev, int idx,
		      void __iomem *regs, bool reset)
{
	struct mtk_wed_ring *ring;

	if (WARN_ON(idx < 0 || idx >= ARRAY_SIZE(dev->tx_ring)))
		return -EINVAL;

	ring = &dev->tx_ring[idx];

	if (!reset && mtk_wed_ring_alloc(dev, ring, MTK_WED_TX_RING_SIZE,
					 sizeof(*ring->desc), true))
		return -ENOMEM;

	if (mtk_wed_wdma_rx_ring_setup(dev, idx, MTK_WED_WDMA_RING_SIZE, reset))
		return -ENOMEM;

	ring->reg_base = MTK_WED_RING_TX(idx);
	ring->wpdma = regs;

	wed_w32(dev, MTK_WED_RING_TX(idx) + MTK_WED_RING_OFS_BASE,
		ring->desc_phys);
	wed_w32(dev, MTK_WED_RING_TX(idx) + MTK_WED_RING_OFS_COUNT,
		MTK_WED_TX_RING_SIZE);
	wed_w32(dev, MTK_WED_RING_TX(idx) + MTK_WED_RING_OFS_CPU_IDX, 0);
	wed_w32(dev, MTK_WED_RING_TX(idx) + MTK_WED_RING_OFS_DMA_IDX, 0);

	/* WED -> WPDMA */
	wpdma_tx_w32(dev, idx, MTK_WED_RING_OFS_BASE, ring->desc_phys);
	wpdma_tx_w32(dev, idx, MTK_WED_RING_OFS_COUNT, MTK_WED_TX_RING_SIZE);
	wpdma_tx_w32(dev, idx, MTK_WED_RING_OFS_CPU_IDX, 0);
	wpdma_tx_w32(dev, idx, MTK_WED_RING_OFS_DMA_IDX, 0);

	wed_w32(dev, MTK_WED_WPDMA_RING_TX(idx) + MTK_WED_RING_OFS_BASE,
		ring->desc_phys);
	wed_w32(dev, MTK_WED_WPDMA_RING_TX(idx) + MTK_WED_RING_OFS_COUNT,
		MTK_WED_TX_RING_SIZE);
	wed_w32(dev, MTK_WED_WPDMA_RING_TX(idx) + MTK_WED_RING_OFS_CPU_IDX, 0);
	wed_w32(dev, MTK_WED_WPDMA_RING_TX(idx) + MTK_WED_RING_OFS_DMA_IDX, 0);

	if (!idx)
		airoha_wed_clear_unused_tx_ring(dev);

	return 0;
}

static int
mtk_wed_txfree_ring_setup(struct mtk_wed_device *dev, void __iomem *regs)
{
	struct mtk_wed_ring *ring = &dev->txfree_ring;
	/* For v1: txfree ring is at RX ring index 1 */
	int index = 1;
	int i;

	ring->reg_base = MTK_WED_RING_RX(index);
	ring->wpdma = regs;

	for (i = 0; i < 12; i += 4) {
		u32 val = readl(regs + i);

		wed_w32(dev, MTK_WED_RING_RX(index) + i, val);
		wed_w32(dev, MTK_WED_WPDMA_RING_RX(index) + i, val);
	}

	return 0;
}

static u32
mtk_wed_irq_get(struct mtk_wed_device *dev, u32 mask)
{
	u32 val, ext_mask = MTK_WED_EXT_INT_STATUS_ERROR_MASK;
	u32 wpdma_mask = mask | BIT(dev->wlan.tx_tbit[0]) |
			 BIT(dev->wlan.tx_tbit[1]);

	val = wed_r32(dev, MTK_WED_EXT_INT_STATUS);
	wed_w32(dev, MTK_WED_EXT_INT_STATUS, val);
	val &= ext_mask;
	if (!dev->hw->num_flows)
		val &= ~MTK_WED_EXT_INT_STATUS_TKID_WO_PYLD;
	if (val && net_ratelimit())
		dev_dbg(dev->hw->dev, "wed%d: error status=%08x\n",
			dev->hw->index, val);

	val = wed_r32(dev, MTK_WED_INT_STATUS);
	val &= wpdma_mask;
	wed_w32(dev, MTK_WED_INT_STATUS, val); /* ACK */
	dev_dbg_ratelimited(dev->hw->dev, "wed%d: irq_get: ack=%08x\n",
			    dev->hw->index, val);

	return val & mask;
}

static void
mtk_wed_irq_set_mask(struct mtk_wed_device *dev, u32 mask)
{
	u32 core_mask = mask ? mtk_wed_core_irq_mask(dev, mask) : 0;

	wed_w32(dev, MTK_WED_INT_MASK, core_mask);
	wed_w32(dev, MTK_WED_EXT_INT_MASK, mtk_wed_ext_irq_mask(dev, mask));
}

int airoha_wed_flow_add(int index)
{
	struct mtk_wed_hw *hw;
	int ret = 0;

	mutex_lock(&hw_lock);

	if (index < 0 || index >= ARRAY_SIZE(hw_list)) {
		ret = -ENODEV;
		goto out;
	}

	hw = hw_list[index];
	if (!hw || !hw->wed_dev) {
		ret = -ENODEV;
		goto out;
	}

	if (!hw->wed_dev->wlan.offload_enable)
		goto out;

	if (hw->num_flows) {
		hw->num_flows++;
		goto out;
	}

	ret = hw->wed_dev->wlan.offload_enable(hw->wed_dev);
	if (!ret)
		hw->num_flows++;

	mtk_wed_set_ext_int(hw->wed_dev, true, 0);

out:
	mutex_unlock(&hw_lock);
	return ret;
}

void airoha_wed_flow_remove(int index)
{
	struct mtk_wed_hw *hw;

	mutex_lock(&hw_lock);

	if (index < 0 || index >= ARRAY_SIZE(hw_list))
		goto out;

	hw = hw_list[index];
	if (!hw || !hw->wed_dev)
		goto out;

	if (!hw->wed_dev->wlan.offload_disable)
		goto out;

	if (!hw->num_flows)
		goto out;

	if (--hw->num_flows)
		goto out;

	hw->wed_dev->wlan.offload_disable(hw->wed_dev);
	mtk_wed_set_ext_int(hw->wed_dev, true, 0);

out:
	mutex_unlock(&hw_lock);
}

/* WED v1 leaves wireless receive processing on the CPU. */
static void
mtk_wed_ppe_check(struct mtk_wed_device *dev, struct sk_buff *skb,
		  u32 reason, u32 hash)
{
}

static int
mtk_wed_setup_tc(struct mtk_wed_device *wed, struct net_device *dev,
		 enum tc_setup_type type, void *type_data)
{
	struct flow_block_offload *offload = type_data;
	int err;

	if (type != TC_SETUP_BLOCK && type != TC_SETUP_FT)
		return -EOPNOTSUPP;

	if (offload->command == FLOW_BLOCK_BIND) {
		err = airoha_wed_flow_add(wed->wdma_idx);
		if (err)
			return err;
	}

	err = airoha_ppe_setup_tc_wed(wed->hw->airoha, dev, type, type_data);
	if ((err && offload->command == FLOW_BLOCK_BIND) ||
	    (!err && offload->command == FLOW_BLOCK_UNBIND))
		airoha_wed_flow_remove(wed->wdma_idx);

	return err;
}

static int
mtk_wed_rx_ring_setup(struct mtk_wed_device *dev, int idx, void __iomem *regs,
		      bool reset)
{
	return 0;
}

static int
mtk_wed_mcu_msg_update(struct mtk_wed_device *dev, int id, void *data, int len)
{
	return 0;
}

static void
mtk_wed_start_hw_rro(struct mtk_wed_device *dev, u32 irq_mask, bool reset)
{
}

static void
mtk_wed_rro_rx_ring_setup(struct mtk_wed_device *dev, int idx,
			  void __iomem *regs)
{
}

static void
mtk_wed_msdu_pg_rx_ring_setup(struct mtk_wed_device *dev, int idx,
			      void __iomem *regs)
{
}

static int
mtk_wed_ind_rx_ring_setup(struct mtk_wed_device *dev, void __iomem *regs)
{
	return 0;
}

static const struct mtk_wed_ops airoha_wed_ops = {
	.attach			= mtk_wed_attach,
	.tx_ring_setup		= mtk_wed_tx_ring_setup,
	.txfree_ring_setup	= mtk_wed_txfree_ring_setup,
	.start			= mtk_wed_start,
	.stop			= mtk_wed_stop,
	.reset_dma		= mtk_wed_reset_dma,
	.reg_read		= wed_r32,
	.reg_write		= wed_w32,
	.irq_get		= mtk_wed_irq_get,
	.irq_set_mask		= mtk_wed_irq_set_mask,
	.detach			= mtk_wed_detach,
	.ppe_check		= mtk_wed_ppe_check,
	.setup_tc		= mtk_wed_setup_tc,
	.rx_ring_setup		= mtk_wed_rx_ring_setup,
	.msg_update		= mtk_wed_mcu_msg_update,
	.start_hw_rro		= mtk_wed_start_hw_rro,
	.rro_rx_ring_setup	= mtk_wed_rro_rx_ring_setup,
	.msdu_pg_rx_ring_setup	= mtk_wed_msdu_pg_rx_ring_setup,
	.ind_rx_ring_setup	= mtk_wed_ind_rx_ring_setup,
};

int airoha_wed_add_hw(struct airoha_eth *eth, struct device_node *np, int index)
{
	const struct airoha_wed_soc_data *soc;
	const struct of_device_id *match;
	struct platform_device *pdev;
	struct resource res;
	struct mtk_wed_hw *hw;
	struct regmap *regs;
	void __iomem *pcie;
	void __iomem *wdma;
	phys_addr_t wdma_phy;
	int irq, err;

	if (!np)
		return -EINVAL;

	if (!airoha_has_wed(eth)) {
		err = -EOPNOTSUPP;
		goto err_node_put;
	}

	if (index < 0 || index >= ARRAY_SIZE(hw_list)) {
		err = -EINVAL;
		goto err_node_put;
	}

	match = of_match_node(airoha_wed_of_match, np);
	if (!match) {
		err = -EINVAL;
		goto err_node_put;
	}
	soc = match->data;

	pdev = of_find_device_by_node(np);
	if (!pdev) {
		err = -EPROBE_DEFER;
		goto err_node_put;
	}

	irq = platform_get_irq(pdev, 0);
	if (irq < 0) {
		err = irq;
		goto err_device_put;
	}

	regs = syscon_regmap_lookup_by_phandle(np, NULL);
	if (IS_ERR(regs)) {
		err = PTR_ERR(regs);
		goto err_device_put;
	}

	wdma = of_iomap(np, 1);
	if (!wdma) {
		err = -ENOMEM;
		goto err_device_put;
	}

	err = of_address_to_resource(np, 1, &res);
	if (err)
		goto err_iounmap;

	wdma_phy = res.start;
	pcie = ioremap(soc->pcie_base[index], SZ_4K);
	if (!pcie) {
		err = -ENOMEM;
		goto err_iounmap;
	}

	hw = kzalloc_obj(*hw);
	if (!hw) {
		err = -ENOMEM;
		goto err_pcie_iounmap;
	}

	hw->airoha = eth;
	hw->node = np;
	hw->regs = regs;
	hw->dev = &pdev->dev;
	hw->pcie = pcie;
	hw->pcie_base = soc->pcie_base[index];
	hw->wdma_phy = wdma_phy;
	hw->wdma = wdma;
	hw->index = index;
	hw->irq = irq;
	hw->version = 1;
	hw->soc = &airoha_wed_v1_data;
	snprintf(hw->dirname, sizeof(hw->dirname), "wed%d", index);

	mutex_lock(&hw_lock);
	if (hw_list[index]) {
		err = -EEXIST;
		goto err_unlock;
	}

	hw_list[index] = hw;
	airoha_wed_hw_add_debugfs(hw);
	err = mtk_wed_ops_register(&airoha_wed_ops);
	if (err) {
		hw_list[index] = NULL;
		debugfs_remove(hw->debugfs_dir);
		goto err_unlock;
	}
	mutex_unlock(&hw_lock);

	dev_info(hw->dev, "registered WED%d, irq %d, WDMA %pa\n",
		 hw->index, hw->irq, &hw->wdma_phy);

	return 0;

err_unlock:
	mutex_unlock(&hw_lock);
	kfree(hw);
err_pcie_iounmap:
	iounmap(pcie);
err_iounmap:
	iounmap(wdma);
err_device_put:
	put_device(&pdev->dev);
err_node_put:
	of_node_put(np);

	return err;
}

void airoha_wed_exit(void)
{
	int i;

	mtk_wed_ops_unregister(&airoha_wed_ops);

	mutex_lock(&hw_lock);
	for (i = 0; i < ARRAY_SIZE(hw_list); i++) {
		struct mtk_wed_hw *hw = hw_list[i];

		if (!hw)
			continue;

		WARN_ON_ONCE(hw->wed_dev);
		airoha_wed_restore_pcie_msi(hw);
		hw_list[i] = NULL;
		debugfs_remove(hw->debugfs_dir);
		iounmap(hw->pcie);
		iounmap(hw->wdma);
		put_device(hw->dev);
		of_node_put(hw->node);
		kfree(hw);
	}
	mutex_unlock(&hw_lock);
}
