// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2026 AIROHA Inc
 * Author: Matheus Sampaio Queiroga <srherobrine20@gmail.com>
 */

#include <linux/bitfield.h>
#include <linux/dma-mapping.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/of_dma.h>
#include <linux/platform_device.h>
#include <linux/reset.h>
#include <linux/slab.h>

#include "virt-dma.h"

#define HSDMA_CHANNELS		2
#define HSDMA_RING_SIZE		256
#define HSDMA_RING_MASK		(HSDMA_RING_SIZE - 1)
#define HSDMA_RING_BYTES	(2 * HSDMA_RING_SIZE * sizeof(struct airoha_hsdma_hwdesc))
#define HSDMA_MAX_LEN		0xffff
#define HSDMA_TX_BASE(id)	((id) * 0x10)
#define HSDMA_TX_COUNT(id)	(HSDMA_TX_BASE(id) + 0x04)
#define HSDMA_TX_CPU(id)	(HSDMA_TX_BASE(id) + 0x08)
#define HSDMA_RX_BASE(id)	(0x100 + (id) * 0x10)
#define HSDMA_RX_COUNT(id)	(HSDMA_RX_BASE(id) + 0x04)
#define HSDMA_RX_CPU(id)	(HSDMA_RX_BASE(id) + 0x08)
#define HSDMA_GLO		0x204
#define HSDMA_RESET		0x208
#define HSDMA_DELAY_IRQ		0x20c
#define HSDMA_FREE_THRESHOLD	0x210
#define HSDMA_IRQ_STATUS	0x220
#define HSDMA_IRQ_ENABLE	0x228
#define HSDMA_GLO_READBACK	BIT(11)
#define HSDMA_GLO_TX_WRITEBACK	BIT(6)
#define HSDMA_GLO_BURST_128	GENMASK(5, 4)
#define HSDMA_GLO_BUSY		(BIT(3) | BIT(1))
#define HSDMA_GLO_DMA		(BIT(2) | BIT(0))
#define HSDMA_GLO_CONFIG	(HSDMA_GLO_READBACK | HSDMA_GLO_TX_WRITEBACK | \
				 HSDMA_GLO_BURST_128)
#define HSDMA_RESET_CHAN(id)	((BIT(0) | BIT(16)) << (id))
#define HSDMA_IRQ_RX(id)	BIT(16 + (id))
#define HSDMA_IRQ_RX_ALL	GENMASK(17, 16)
#define HSDMA_DESC_DONE		BIT(31)
#define HSDMA_DESC_NLS		BIT(29)
#define HSDMA_DESC_LEN		GENMASK(15, 0)

#define HSDMA_READ(dev, reg)		readl((dev)->base + reg)
#define HSDMA_WRITE(dev, reg, data)	writel(data, (dev)->base + reg)

struct airoha_hsdma_hwdesc {
	__le32 reserved0;
	__le32 control;
	__le32 address;
	__le32 reserved1[5];
} __aligned(32);

struct airoha_hsdma_desc {
	struct virt_dma_desc vd;
	dma_addr_t src;
	dma_addr_t dst;
	size_t remaining;
	size_t residue;
};

struct airoha_hsdma;

struct airoha_hsdma_chan {
	struct virt_dma_chan vc;
	struct airoha_hsdma *hsdma;
	struct airoha_hsdma_desc *active;
	struct airoha_hsdma_hwdesc *tx;
	struct airoha_hsdma_hwdesc *rx;
	dma_addr_t ring_dma;
	u16 head;
	u16 tail;
	u16 used;
	u8 id;
};

struct airoha_hsdma {
	struct dma_device dma;
	void __iomem *base;
	struct reset_control *reset;
	struct airoha_hsdma_chan chan[HSDMA_CHANNELS];
	/* Protect global registers and serialize hardware access across channels. */
	spinlock_t lock;
	int irq;
};

static struct airoha_hsdma_chan *to_hsdma_chan(struct dma_chan *c)
{
	return container_of(c, struct airoha_hsdma_chan, vc.chan);
}

static struct airoha_hsdma_desc *to_hsdma_desc(struct virt_dma_desc *vd)
{
	return container_of(vd, struct airoha_hsdma_desc, vd);
}

static void airoha_hsdma_desc_free(struct virt_dma_desc *vd)
{
	dma_descriptor_unmap(&vd->tx);
	kfree(to_hsdma_desc(vd));
}

static int airoha_hsdma_stop(struct airoha_hsdma *hsdma)
{
	u32 val;

	HSDMA_WRITE(hsdma, HSDMA_GLO, HSDMA_GLO_CONFIG);
	return readl_poll_timeout_atomic(hsdma->base + HSDMA_GLO, val,
					 !(val & HSDMA_GLO_BUSY), 1, 1000);
}

/* The engine must be stopped before resetting a ring. */
static void airoha_hsdma_reset_ring(struct airoha_hsdma_chan *chan)
{
	struct airoha_hsdma *hsdma = chan->hsdma;
	unsigned int i;

	memset(chan->tx, 0, HSDMA_RING_BYTES);
	for (i = 0; i < HSDMA_RING_SIZE; i++)
		chan->tx[i].control = cpu_to_le32(HSDMA_DESC_DONE);

	chan->head = 0;
	chan->tail = 0;
	chan->used = 0;
	dma_wmb();
	HSDMA_WRITE(hsdma, HSDMA_RESET, HSDMA_RESET_CHAN(chan->id));
	HSDMA_WRITE(hsdma, HSDMA_TX_BASE(chan->id), chan->ring_dma);
	HSDMA_WRITE(hsdma, HSDMA_TX_COUNT(chan->id), HSDMA_RING_SIZE);
	HSDMA_WRITE(hsdma, HSDMA_TX_CPU(chan->id), 0);
	HSDMA_WRITE(hsdma, HSDMA_RX_BASE(chan->id),
		    chan->ring_dma + HSDMA_RING_BYTES / 2);
	HSDMA_WRITE(hsdma, HSDMA_RX_COUNT(chan->id), HSDMA_RING_SIZE);
	HSDMA_WRITE(hsdma, HSDMA_RX_CPU(chan->id), HSDMA_RING_MASK);
}

static void airoha_hsdma_fill_ring(struct airoha_hsdma_chan *chan)
{
	struct airoha_hsdma_desc *desc = chan->active;
	struct virt_dma_desc *vd;
	bool queued = false;

	lockdep_assert_held(&chan->hsdma->lock);
	lockdep_assert_held(&chan->vc.lock);

	if (!desc) {
		vd = vchan_next_desc(&chan->vc);
		if (!vd)
			return;

		list_del(&vd->node);
		desc = to_hsdma_desc(vd);
		chan->active = desc;
	}

	while (desc->remaining && chan->used < HSDMA_RING_SIZE - 1) {
		struct airoha_hsdma_hwdesc *tx = &chan->tx[chan->head];
		struct airoha_hsdma_hwdesc *rx = &chan->rx[chan->head];
		u32 len = min_t(size_t, desc->remaining, HSDMA_MAX_LEN);
		u32 ctrl = FIELD_PREP(HSDMA_DESC_LEN, len);

		if (desc->remaining > len)
			ctrl |= HSDMA_DESC_NLS;

		WRITE_ONCE(rx->address, cpu_to_le32(desc->dst));
		WRITE_ONCE(tx->address, cpu_to_le32(desc->src));
		WRITE_ONCE(rx->control, cpu_to_le32(ctrl));
		WRITE_ONCE(tx->control, cpu_to_le32(ctrl));

		desc->src += len;
		desc->dst += len;
		desc->remaining -= len;
		chan->head = (chan->head + 1) & HSDMA_RING_MASK;
		chan->used++;
		queued = true;
	}

	if (queued) {
		/* Publish both RX and TX descriptors before the doorbell. */
		dma_wmb();
		HSDMA_WRITE(chan->hsdma, HSDMA_TX_CPU(chan->id), chan->head);
	}
}

static void airoha_hsdma_reclaim(struct airoha_hsdma_chan *chan)
{
	struct airoha_hsdma_desc *desc = chan->active;
	bool reclaimed = false;

	while (chan->used) {
		struct airoha_hsdma_hwdesc *rx = &chan->rx[chan->tail];
		u32 ctrl = le32_to_cpu(READ_ONCE(rx->control));

		if (!(ctrl & HSDMA_DESC_DONE))
			break;

		dma_rmb();
		desc->residue -= FIELD_GET(HSDMA_DESC_LEN, ctrl);
		WRITE_ONCE(rx->address, 0);
		WRITE_ONCE(rx->control, 0);
		chan->tail = (chan->tail + 1) & HSDMA_RING_MASK;
		chan->used--;
		reclaimed = true;
	}

	if (reclaimed) {
		dma_wmb();
		HSDMA_WRITE(chan->hsdma, HSDMA_RX_CPU(chan->id),
			    (chan->tail - 1) & HSDMA_RING_MASK);
	}

	if (desc && !desc->residue) {
		chan->active = NULL;
		dma_descriptor_unmap(&desc->vd.tx);
		vchan_cookie_complete(&desc->vd);
	}

	airoha_hsdma_fill_ring(chan);
}

static irqreturn_t airoha_hsdma_irq(int irq, void *data)
{
	struct airoha_hsdma *hsdma = data;
	u32 status;
	unsigned int i;

	spin_lock(&hsdma->lock);
	status = HSDMA_READ(hsdma, HSDMA_IRQ_STATUS) & HSDMA_IRQ_RX_ALL;
	if (!status) {
		spin_unlock(&hsdma->lock);
		return IRQ_NONE;
	}

	/* A completion arriving after this ACK remains pending for the next IRQ. */
	HSDMA_WRITE(hsdma, HSDMA_IRQ_STATUS, status);

	for (i = 0; i < HSDMA_CHANNELS; i++) {
		struct airoha_hsdma_chan *chan = &hsdma->chan[i];
		if (!(status & HSDMA_IRQ_RX(i)))
			continue;

		spin_lock(&chan->vc.lock);
		airoha_hsdma_reclaim(chan);
		spin_unlock(&chan->vc.lock);
	}

	spin_unlock(&hsdma->lock);
	return IRQ_HANDLED;
}

static struct dma_async_tx_descriptor *
airoha_hsdma_prep_memcpy(struct dma_chan *c, dma_addr_t dst, dma_addr_t src,
			 size_t len, unsigned long flags)
{
	struct airoha_hsdma_desc *desc;

	if (!len || len > U32_MAX || src > U32_MAX || dst > U32_MAX ||
	    len - 1 > U32_MAX - src || len - 1 > U32_MAX - dst)
		return NULL;

	desc = kzalloc(sizeof(*desc), GFP_NOWAIT);
	if (!desc)
		return NULL;

	desc->src = src;
	desc->dst = dst;
	desc->remaining = len;
	desc->residue = len;

	return vchan_tx_prep(&to_hsdma_chan(c)->vc, &desc->vd, flags);
}

static void airoha_hsdma_issue_pending(struct dma_chan *c)
{
	struct airoha_hsdma_chan *chan = to_hsdma_chan(c);
	unsigned long flags;

	spin_lock_irqsave(&chan->hsdma->lock, flags);
	spin_lock(&chan->vc.lock);

	if (vchan_issue_pending(&chan->vc))
		airoha_hsdma_fill_ring(chan);

	spin_unlock(&chan->vc.lock);
	spin_unlock_irqrestore(&chan->hsdma->lock, flags);
}

static enum dma_status airoha_hsdma_tx_status(struct dma_chan *c,
					      dma_cookie_t cookie,
					      struct dma_tx_state *state)
{
	struct airoha_hsdma_chan *chan = to_hsdma_chan(c);
	struct virt_dma_desc *vd;
	unsigned long flags;
	enum dma_status status;
	size_t residue = 0;
	spin_lock_irqsave(&chan->vc.lock, flags);

	status = dma_cookie_status(c, cookie, state);
	if (status != DMA_COMPLETE && state) {
		if (chan->active && chan->active->vd.tx.cookie == cookie)
			residue = chan->active->residue;
		else {
			vd = vchan_find_desc(&chan->vc, cookie);
			if (vd)
				residue = to_hsdma_desc(vd)->residue;

			list_for_each_entry(vd, &chan->vc.desc_submitted, node)
				if (vd->tx.cookie == cookie)
					residue = to_hsdma_desc(vd)->residue;
		}

		dma_set_residue(state, residue);
	}

	spin_unlock_irqrestore(&chan->vc.lock, flags);
	return status;
}

static int airoha_hsdma_terminate_all(struct dma_chan *c)
{
	struct airoha_hsdma_chan *chan = to_hsdma_chan(c);
	struct airoha_hsdma *hsdma = chan->hsdma;
	unsigned long flags;
	int ret;
	LIST_HEAD(head);

	spin_lock_irqsave(&hsdma->lock, flags);
	spin_lock(&chan->vc.lock);

	/* Global enable bits briefly pause both channels; reset only this ring. */
	ret = airoha_hsdma_stop(hsdma);
	if (ret)
		goto out_restart;

	if (chan->active) {
		vchan_terminate_vdesc(&chan->active->vd);
		chan->active = NULL;
	}

	vchan_get_all_descriptors(&chan->vc, &head);
	airoha_hsdma_reset_ring(chan);

	HSDMA_WRITE(hsdma, HSDMA_IRQ_STATUS, HSDMA_IRQ_RX(chan->id));
out_restart:
	HSDMA_WRITE(hsdma, HSDMA_GLO, HSDMA_GLO_CONFIG | HSDMA_GLO_DMA);
	spin_unlock(&chan->vc.lock);
	spin_unlock_irqrestore(&hsdma->lock, flags);
	vchan_dma_desc_free_list(&chan->vc, &head);

	return ret;
}

static void airoha_hsdma_synchronize(struct dma_chan *c)
{
	vchan_synchronize(&to_hsdma_chan(c)->vc);
}

static void airoha_hsdma_free_chan_resources(struct dma_chan *c)
{
	if (airoha_hsdma_terminate_all(c))
		return;

	airoha_hsdma_synchronize(c);
	vchan_free_chan_resources(&to_hsdma_chan(c)->vc);
}

static void airoha_hsdma_cleanup_channels(struct airoha_hsdma *hsdma)
{
	unsigned int i;

	for (i = 0; i < HSDMA_CHANNELS; i++) {
		struct airoha_hsdma_chan *chan = &hsdma->chan[i];

		tasklet_kill(&chan->vc.task);
		vchan_free_chan_resources(&chan->vc);

		if (chan->active) {
			airoha_hsdma_desc_free(&chan->active->vd);
			chan->active = NULL;
		}

		list_del(&chan->vc.chan.device_node);
		if (chan->tx)
			dma_free_coherent(hsdma->dma.dev, HSDMA_RING_BYTES,
					  chan->tx, chan->ring_dma);
	}
}

static int airoha_hsdma_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct airoha_hsdma *hsdma;
	struct dma_device *dma;
	unsigned int i;
	int ret;

	hsdma = devm_kzalloc(dev, sizeof(*hsdma), GFP_KERNEL);
	if (!hsdma)
		return -ENOMEM;

	hsdma->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(hsdma->base))
		return PTR_ERR(hsdma->base);

	hsdma->irq = platform_get_irq(pdev, 0);
	if (hsdma->irq < 0)
		return hsdma->irq;

	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		return ret;

	hsdma->reset = devm_reset_control_get_optional_exclusive(dev, NULL);
	if (IS_ERR(hsdma->reset))
		return PTR_ERR(hsdma->reset);
	ret = reset_control_reset(hsdma->reset);
	if (ret)
		return ret;

	spin_lock_init(&hsdma->lock);

	HSDMA_WRITE(hsdma, HSDMA_IRQ_ENABLE, 0);
	ret = airoha_hsdma_stop(hsdma);
	if (ret)
		return dev_err_probe(dev, ret, "HSDMA did not become idle\n");

	HSDMA_WRITE(hsdma, HSDMA_DELAY_IRQ, 0);
	HSDMA_WRITE(hsdma, HSDMA_FREE_THRESHOLD, 0);
	HSDMA_WRITE(hsdma, HSDMA_IRQ_STATUS, U32_MAX);

	dma = &hsdma->dma;
	dma->dev = dev;
	dma_cap_set(DMA_MEMCPY, dma->cap_mask);
	dma->copy_align = DMAENGINE_ALIGN_1_BYTE;
	dma->device_prep_dma_memcpy = airoha_hsdma_prep_memcpy;
	dma->device_issue_pending = airoha_hsdma_issue_pending;
	dma->device_tx_status = airoha_hsdma_tx_status;
	dma->device_terminate_all = airoha_hsdma_terminate_all;
	dma->device_synchronize = airoha_hsdma_synchronize;
	dma->device_free_chan_resources = airoha_hsdma_free_chan_resources;
	dma->residue_granularity = DMA_RESIDUE_GRANULARITY_SEGMENT;
	INIT_LIST_HEAD(&dma->channels);

	for (i = 0; i < HSDMA_CHANNELS; i++) {
		struct airoha_hsdma_chan *chan = &hsdma->chan[i];

		chan->id = i;
		chan->hsdma = hsdma;
		chan->vc.desc_free = airoha_hsdma_desc_free;
		vchan_init(&chan->vc, dma);
	}

	for (i = 0; i < HSDMA_CHANNELS; i++) {
		struct airoha_hsdma_chan *chan = &hsdma->chan[i];

		chan->tx = dma_alloc_coherent(dev, HSDMA_RING_BYTES,
					      &chan->ring_dma, GFP_KERNEL);
		if (!chan->tx) {
			ret = -ENOMEM;
			goto err_channels;
		}

		chan->rx = chan->tx + HSDMA_RING_SIZE;
		airoha_hsdma_reset_ring(chan);
	}

	ret = devm_request_irq(dev, hsdma->irq, airoha_hsdma_irq, 0,
			       dev_name(dev), hsdma);
	if (ret)
		goto err_channels;

	HSDMA_WRITE(hsdma, HSDMA_IRQ_ENABLE, HSDMA_IRQ_RX_ALL);
	HSDMA_WRITE(hsdma, HSDMA_GLO, HSDMA_GLO_CONFIG | HSDMA_GLO_DMA);

	ret = dma_async_device_register(dma);
	if (ret)
		goto err_stop;

	ret = of_dma_controller_register(dev->of_node, of_dma_xlate_by_chan_id, dma);
	if (ret)
		goto err_unregister;

	platform_set_drvdata(pdev, hsdma);
	dev_info(dev, "2 HSDMA memcpy channels\n");

	return 0;

err_unregister:
	dma_async_device_unregister(dma);

err_stop:
	HSDMA_WRITE(hsdma, HSDMA_IRQ_ENABLE, 0);
	devm_free_irq(dev, hsdma->irq, hsdma);
	if (airoha_hsdma_stop(hsdma) &&
	    (!hsdma->reset || reset_control_assert(hsdma->reset))) {
		dev_err(dev, "HSDMA stuck busy; retaining DMA rings\n");
		for (i = 0; i < HSDMA_CHANNELS; i++)
			hsdma->chan[i].tx = NULL;
	}

err_channels:
	airoha_hsdma_cleanup_channels(hsdma);
	return ret;
}

static void airoha_hsdma_remove(struct platform_device *pdev)
{
	struct airoha_hsdma *hsdma = platform_get_drvdata(pdev);
	unsigned int i;

	of_dma_controller_free(pdev->dev.of_node);
	dma_async_device_unregister(&hsdma->dma);
	HSDMA_WRITE(hsdma, HSDMA_IRQ_ENABLE, 0);
	devm_free_irq(&pdev->dev, hsdma->irq, hsdma);

	/* An asserted block reset also fences any outstanding bus accesses. */
	if (airoha_hsdma_stop(hsdma) &&
	    (!hsdma->reset || reset_control_assert(hsdma->reset))) {
		dev_err(&pdev->dev, "HSDMA stuck busy; retaining DMA rings\n");
		for (i = 0; i < HSDMA_CHANNELS; i++)
			hsdma->chan[i].tx = NULL;
	}

	airoha_hsdma_cleanup_channels(hsdma);
	reset_control_assert(hsdma->reset);
}

static const struct of_device_id airoha_hsdma_match[] = {
	{ .compatible = "airoha,en7523-hsdma" },
	{ }
};
MODULE_DEVICE_TABLE(of, airoha_hsdma_match);

static struct platform_driver airoha_hsdma_driver = {
	.probe = airoha_hsdma_probe,
	.remove = airoha_hsdma_remove,
	.driver = {
		.name = "airoha-hsdma",
		.of_match_table = airoha_hsdma_match,
		.suppress_bind_attrs = true,
	},
};
module_platform_driver(airoha_hsdma_driver);

MODULE_DESCRIPTION("Airoha High Speed DMA controller");
MODULE_LICENSE("GPL");
