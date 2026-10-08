// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2026 AIROHA Inc
 * Author: Matheus Sampaio Queiroga <srherobrine20@gmail.com>
 */

#include <linux/bitfield.h>
#include <linux/dma-mapping.h>
#include <linux/hrtimer.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/of_dma.h>
#include <linux/platform_device.h>
#include <linux/reset.h>
#include <linux/slab.h>

#include "virt-dma.h"

#define GDMA_CHANNELS		8
#define GDMA_SRC(id)		((id) * 0x10)
#define GDMA_DST(id)		(GDMA_SRC(id) + 0x04)
#define GDMA_CTRL0(id)		(GDMA_SRC(id) + 0x08)
#define GDMA_CTRL1(id)		(GDMA_SRC(id) + 0x0c)
#define GDMA_DONE		0x204
#define GDMA_CTRL0_LEN		GENMASK(31, 16)
#define GDMA_CTRL0_BURST	GENMASK(5, 3)
#define GDMA_CTRL0_DONE_IRQ	BIT(2)
#define GDMA_CTRL0_ENABLE	BIT(1)
#define GDMA_CTRL0_SW_MODE	BIT(0)
#define GDMA_CTRL1_COHERENT	BIT(2)
#define GDMA_CTRL1_MASK		BIT(0)
#define GDMA_MAX_LEN		0xffff
#define GDMA_POLL_NS		100000

struct airoha_gdma_soc {
	u8 address_bits;
};

struct airoha_gdma_desc {
	struct virt_dma_desc vd;
	dma_addr_t src;
	dma_addr_t dst;
	size_t residue;
};

struct airoha_gdma;

struct airoha_gdma_chan {
	struct virt_dma_chan vc;
	struct airoha_gdma *gdma;
	struct airoha_gdma_desc *active;
	struct hrtimer timer;
	u32 count;
	u8 id;
};

struct airoha_gdma {
	struct dma_device dma;
	void __iomem *base;
	const struct airoha_gdma_soc *soc;
	struct reset_control *reset;
	struct airoha_gdma_chan chan[GDMA_CHANNELS];
	int irq;
};

static struct airoha_gdma_chan *to_gdma_chan(struct dma_chan *chan)
{
	return container_of(chan, struct airoha_gdma_chan, vc.chan);
}

static struct airoha_gdma_desc *to_gdma_desc(struct virt_dma_desc *vd)
{
	return container_of(vd, struct airoha_gdma_desc, vd);
}

static u32 airoha_gdma_read(struct airoha_gdma *gdma, u32 reg)
{
	/* EcoNet MIPS uses native MMIO; its readl() does not swap I/O space. */
	return readl(gdma->base + reg);
}

static void airoha_gdma_write(struct airoha_gdma *gdma, u32 reg, u32 val)
{
	writel(val, gdma->base + reg);
}

static void airoha_gdma_desc_free(struct virt_dma_desc *vd)
{
	dma_descriptor_unmap(&vd->tx);
	kfree(to_gdma_desc(vd));
}

static void airoha_gdma_start(struct airoha_gdma_chan *chan)
{
	struct airoha_gdma *gdma = chan->gdma;
	struct airoha_gdma_desc *desc = chan->active;
	struct virt_dma_desc *vd;
	u32 ctrl;

	lockdep_assert_held(&chan->vc.lock);

	if (!desc) {
		vd = vchan_next_desc(&chan->vc);
		if (!vd)
			return;
		list_del(&vd->node);
		desc = to_gdma_desc(vd);
		chan->active = desc;
	}

	chan->count = min_t(size_t, desc->residue, GDMA_MAX_LEN);

	airoha_gdma_write(gdma, GDMA_DONE, BIT(chan->id));
	airoha_gdma_write(gdma, GDMA_SRC(chan->id), desc->src);
	airoha_gdma_write(gdma, GDMA_DST(chan->id), desc->dst);
	/* Keep payload byte swapping disabled, including on big-endian CPUs. */
	airoha_gdma_write(gdma, GDMA_CTRL1(chan->id), GDMA_CTRL1_COHERENT);

	ctrl = FIELD_PREP(GDMA_CTRL0_LEN, chan->count) |
	       FIELD_PREP(GDMA_CTRL0_BURST, 4) |
	       GDMA_CTRL0_SW_MODE | GDMA_CTRL0_ENABLE;
	if (gdma->irq >= 0)
		ctrl |= GDMA_CTRL0_DONE_IRQ;

	airoha_gdma_write(gdma, GDMA_CTRL0(chan->id), ctrl);
}

static void airoha_gdma_complete(struct airoha_gdma_chan *chan)
{
	struct airoha_gdma_desc *desc = chan->active;

	if (!desc)
		return;

	desc->src += chan->count;
	desc->dst += chan->count;
	desc->residue -= chan->count;
	if (!desc->residue) {
		chan->active = NULL;
		dma_descriptor_unmap(&desc->vd.tx);
		vchan_cookie_complete(&desc->vd);
	}

	airoha_gdma_start(chan);
}

static enum hrtimer_restart airoha_gdma_poll(struct hrtimer *timer)
{
	struct airoha_gdma_chan *chan = container_of(timer, typeof(*chan), timer);
	struct airoha_gdma *gdma = chan->gdma;
	unsigned long flags;
	bool pending;
	spin_lock_irqsave(&chan->vc.lock, flags);

	if (chan->active &&
	    !(airoha_gdma_read(gdma, GDMA_CTRL0(chan->id)) & GDMA_CTRL0_ENABLE)) {
		airoha_gdma_write(gdma, GDMA_DONE, BIT(chan->id));
		airoha_gdma_complete(chan);
	}

	pending = !!chan->active;
	if (pending)
		hrtimer_forward_now(timer, ns_to_ktime(GDMA_POLL_NS));

	spin_unlock_irqrestore(&chan->vc.lock, flags);

	return pending ? HRTIMER_RESTART : HRTIMER_NORESTART;
}

static irqreturn_t airoha_gdma_irq(int irq, void *data)
{
	struct airoha_gdma *gdma = data;
	irqreturn_t ret = IRQ_NONE;
	unsigned int i;

	for (i = 0; i < GDMA_CHANNELS; i++) {
		struct airoha_gdma_chan *chan = &gdma->chan[i];
		spin_lock(&chan->vc.lock);

		/* Read and acknowledge under the lock also used for cancellation. */
		if (airoha_gdma_read(gdma, GDMA_DONE) & BIT(i)) {
			airoha_gdma_write(gdma, GDMA_DONE, BIT(i));
			airoha_gdma_complete(chan);
			ret = IRQ_HANDLED;
		}

		spin_unlock(&chan->vc.lock);
	}

	return ret;
}

static struct dma_async_tx_descriptor *
airoha_gdma_prep_memcpy(struct dma_chan *c, dma_addr_t dst, dma_addr_t src,
			size_t len, unsigned long flags)
{
	struct airoha_gdma_chan *chan = to_gdma_chan(c);
	u64 mask = DMA_BIT_MASK(chan->gdma->soc->address_bits);
	struct airoha_gdma_desc *desc;

	if (!len || len > U32_MAX || src > mask || dst > mask ||
	    len - 1 > mask - src || len - 1 > mask - dst)
		return NULL;

	desc = kzalloc(sizeof(*desc), GFP_NOWAIT);
	if (!desc)
		return NULL;

	desc->src = src;
	desc->dst = dst;
	desc->residue = len;

	return vchan_tx_prep(&chan->vc, &desc->vd, flags);
}

static void airoha_gdma_issue_pending(struct dma_chan *c)
{
	struct airoha_gdma_chan *chan = to_gdma_chan(c);
	unsigned long flags;
	spin_lock_irqsave(&chan->vc.lock, flags);

	if (vchan_issue_pending(&chan->vc) && !chan->active) {
		airoha_gdma_start(chan);
		if (chan->gdma->irq < 0)
			hrtimer_start(&chan->timer, ns_to_ktime(GDMA_POLL_NS),
				      HRTIMER_MODE_REL);
	}

	spin_unlock_irqrestore(&chan->vc.lock, flags);
}

static enum dma_status airoha_gdma_tx_status(struct dma_chan *c,
					     dma_cookie_t cookie,
					     struct dma_tx_state *state)
{
	struct airoha_gdma_chan *chan = to_gdma_chan(c);
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
				residue = to_gdma_desc(vd)->residue;
			list_for_each_entry(vd, &chan->vc.desc_submitted, node)
				if (vd->tx.cookie == cookie)
					residue = to_gdma_desc(vd)->residue;
		}

		dma_set_residue(state, residue);
	}
	spin_unlock_irqrestore(&chan->vc.lock, flags);

	return status;
}

static int airoha_gdma_terminate_all(struct dma_chan *c)
{
	struct airoha_gdma_chan *chan = to_gdma_chan(c);
	struct airoha_gdma *gdma = chan->gdma;
	unsigned long flags;
	u32 val;
	int ret;
	LIST_HEAD(head);

	spin_lock_irqsave(&chan->vc.lock, flags);
	airoha_gdma_write(gdma, GDMA_CTRL1(chan->id), GDMA_CTRL1_MASK);
	airoha_gdma_write(gdma, GDMA_CTRL0(chan->id), 0);

	ret = read_poll_timeout_atomic(airoha_gdma_read, val,
				       !(val & GDMA_CTRL0_ENABLE), 1, 1000,
				       false, gdma, GDMA_CTRL0(chan->id));
	if (ret) {
		spin_unlock_irqrestore(&chan->vc.lock, flags);
		return ret;
	}

	airoha_gdma_write(gdma, GDMA_DONE, BIT(chan->id));
	if (chan->active) {
		vchan_terminate_vdesc(&chan->active->vd);
		chan->active = NULL;
	}

	vchan_get_all_descriptors(&chan->vc, &head);
	spin_unlock_irqrestore(&chan->vc.lock, flags);
	vchan_dma_desc_free_list(&chan->vc, &head);

	return 0;
}

static void airoha_gdma_synchronize(struct dma_chan *c)
{
	struct airoha_gdma_chan *chan = to_gdma_chan(c);

	hrtimer_cancel(&chan->timer);
	vchan_synchronize(&chan->vc);
}

static void airoha_gdma_free_chan_resources(struct dma_chan *c)
{
	if (airoha_gdma_terminate_all(c))
		return;
	airoha_gdma_synchronize(c);
	vchan_free_chan_resources(&to_gdma_chan(c)->vc);
}

static void airoha_gdma_cleanup_channels(struct airoha_gdma *gdma)
{
	unsigned int i;

	for (i = 0; i < GDMA_CHANNELS; i++) {
		airoha_gdma_free_chan_resources(&gdma->chan[i].vc.chan);
		tasklet_kill(&gdma->chan[i].vc.task);
		list_del(&gdma->chan[i].vc.chan.device_node);
	}
}

static int airoha_gdma_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct airoha_gdma *gdma;
	struct dma_device *dma;
	unsigned int i;
	int ret;

	gdma = devm_kzalloc(dev, sizeof(*gdma), GFP_KERNEL);
	if (!gdma)
		return -ENOMEM;

	gdma->soc = device_get_match_data(dev);
	if (!gdma->soc)
		return -ENODEV;

	gdma->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(gdma->base))
		return PTR_ERR(gdma->base);

	gdma->irq = platform_get_irq_optional(pdev, 0);
	if (gdma->irq < 0 && gdma->irq != -ENXIO && gdma->irq != -ENODEV)
		return dev_err_probe(dev, gdma->irq, "error on get gdma irq");

	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(gdma->soc->address_bits));
	if (ret)
		return ret;

	gdma->reset = devm_reset_control_get_optional_exclusive(dev, NULL);
	if (IS_ERR(gdma->reset))
		return dev_err_probe(dev, PTR_ERR(gdma->reset), 
				     "error on get reset controller");
	ret = reset_control_reset(gdma->reset);
	if (ret)
		return ret;

	dma = &gdma->dma;
	dma->dev = dev;
	dma_cap_set(DMA_MEMCPY, dma->cap_mask);
	dma->copy_align = DMAENGINE_ALIGN_1_BYTE;
	dma->device_prep_dma_memcpy = airoha_gdma_prep_memcpy;
	dma->device_issue_pending = airoha_gdma_issue_pending;
	dma->device_tx_status = airoha_gdma_tx_status;
	dma->device_terminate_all = airoha_gdma_terminate_all;
	dma->device_synchronize = airoha_gdma_synchronize;
	dma->device_free_chan_resources = airoha_gdma_free_chan_resources;
	dma->residue_granularity = DMA_RESIDUE_GRANULARITY_SEGMENT;
	INIT_LIST_HEAD(&dma->channels);

	for (i = 0; i < GDMA_CHANNELS; i++) {
		struct airoha_gdma_chan *chan = &gdma->chan[i];

		chan->gdma = gdma;
		chan->id = i;
		chan->vc.desc_free = airoha_gdma_desc_free;
		vchan_init(&chan->vc, dma);
		hrtimer_setup(&chan->timer, airoha_gdma_poll, CLOCK_MONOTONIC,
			      HRTIMER_MODE_REL);
		airoha_gdma_write(gdma, GDMA_CTRL0(i), 0);
		airoha_gdma_write(gdma, GDMA_CTRL1(i), GDMA_CTRL1_MASK);
	}

	airoha_gdma_write(gdma, GDMA_DONE, GENMASK(GDMA_CHANNELS - 1, 0));
	if (gdma->irq >= 0) {
		ret = devm_request_irq(dev, gdma->irq, airoha_gdma_irq, 0,
				       dev_name(dev), gdma);
		if (ret)
			goto err_channels;
	}

	ret = dma_async_device_register(dma);
	if (ret)
		goto err_irq;

	ret = of_dma_controller_register(dev->of_node, of_dma_xlate_by_chan_id, dma);
	if (ret)
		goto err_unregister;

	platform_set_drvdata(pdev, gdma);
	dev_info(dev, "8 GDMA memcpy channels (%s completion)\n",
		 gdma->irq >= 0 ? "IRQ" : "polled");

	return 0;

err_unregister:
	dma_async_device_unregister(dma);
err_irq:
	if (gdma->irq >= 0)
		devm_free_irq(dev, gdma->irq, gdma);
err_channels:
	airoha_gdma_cleanup_channels(gdma);
	return ret;
}

static void airoha_gdma_remove(struct platform_device *pdev)
{
	struct airoha_gdma *gdma = platform_get_drvdata(pdev);

	of_dma_controller_free(pdev->dev.of_node);
	dma_async_device_unregister(&gdma->dma);

	if (gdma->irq >= 0)
		devm_free_irq(&pdev->dev, gdma->irq, gdma);

	airoha_gdma_cleanup_channels(gdma);
	reset_control_assert(gdma->reset);
}

static const struct airoha_gdma_soc en751221_gdma = {
	.address_bits = 29,
};

static const struct airoha_gdma_soc en7523_gdma = {
	.address_bits = 32,
};

static const struct of_device_id airoha_gdma_match[] = {
	{ .compatible = "econet,en751221-gdma", .data = &en751221_gdma },
	{ .compatible = "econet,en751627-gdma", .data = &en751221_gdma },
	{ .compatible = "airoha,en7523-gdma", .data = &en7523_gdma },
	{ }
};
MODULE_DEVICE_TABLE(of, airoha_gdma_match);

static struct platform_driver airoha_gdma_driver = {
	.probe = airoha_gdma_probe,
	.remove = airoha_gdma_remove,
	.driver = {
		.name = "airoha-gdma",
		.of_match_table = airoha_gdma_match,
		.suppress_bind_attrs = true,
	},
};
module_platform_driver(airoha_gdma_driver);

MODULE_DESCRIPTION("Airoha General-purpose DMA controller");
MODULE_LICENSE("GPL");
