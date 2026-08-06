// SPDX-License-Identifier: GPL-2.0-only
/*
 * EcoNet EN751221 Ethernet Driver
 *
 * This Ethernet system consists of 2 QDMA engines and 2 GDM ports.
 *
 * The QDMA engine manages DMA and also QoS prioritization, taking advantage of
 * the fact that the packet is in memory and can therefore be sent at the
 * appropriate time.
 *
 * The GDM port is the outward-facing port of the Ethernet device and it
 * corrisponds to the net_device that are registered with the kernel.
 *
 * The QDMAs are *loosely* coupled with the GDM ports, it is possible to send
 * a packet to GDM2 by registering it with QDMA1 or vice versa. On the receive
 * side, the GDM is configured which QDMA to send various types of packets to.
 * Practically speaking, it is sensible to link GDM1 with QDMA1 and GDM2 with
 * QDMA2.
 *
 * Since QDMA engines are responsible for receiving traffic, the QDMA calls
 * en75_rx_before_recv() before calling napi_gro_receive(), this gives the main
 * driver an opportunity to assign the skb a net_device based on which port is
 * indicated in the DMA packet descriptor. Therefore the QDMA driver
 * (econet_qdma.c) has no awareness of the GDM driver (econet_port.c).
 *
 * When sending, the GDM driver must in fact register the packet with a QDMA
 * engine. The main driver (this file) passes an opaque QDMA driver struct to
 * the GDM driver which then calls en75_qdma_xmit() when it has a packet to
 * send. In order to avoid race conditions during driver teardown, the GDM
 * calls en75_qdma_use() and only calls en75_qdma_unuse() when the GDM driver
 * itself is tearing down. The QDMA will not teardown until it no longer has
 * any users. Any number of GDMs could share a QDMA without problem.
 */
#include <linux/skbuff.h>
#include <linux/etherdevice.h>
#include <net/dsa.h>
#include <linux/platform_device.h>
#include <linux/reset.h>
#include <linux/of_net.h>

#include "econet_gdm_regs.h"
#include "linux/dev_printk.h"
#include "linux/ioport.h"
#include "linux/mdio.h"
#include "linux/netdevice.h"
#include "econet_qdma_desc.h"
#include "econet_qdma_regs.h"
#include "econet_eth.h"

/* MT7530 switch configuration */
#define  PMCR_FORCE_FDX			BIT(1)
#define  PMCR_FORCE_LNK			BIT(0)
#define  PMCR_FORCE_SPEED_1000		BIT(3)
#define  PMCR_BACKOFF_EN		BIT(9)
#define  PMCR_BACKPR_EN			BIT(8)
#define  PMCR_MAC_MODE			BIT(16)
#define  MT7530_FORCE_MODE		BIT(15)
#define  PMCR_MAC_TX_EN			BIT(14)
#define  PMCR_MAC_RX_EN			BIT(13)
#define  PMCR_IFG_XMIT_MASK		GENMASK(19, 18)
#define  PMCR_IFG_XMIT(x)		FIELD_PREP(PMCR_IFG_XMIT_MASK, x)
#define    IFG_64BIT 2
/* Match bootloader value, 0x9e30b */
#define EN75_PMCR_CONFIG ((u32)( \
		PMCR_FORCE_FDX | PMCR_FORCE_LNK | PMCR_FORCE_SPEED_1000 | \
		PMCR_BACKOFF_EN | PMCR_BACKPR_EN | PMCR_MAC_MODE | \
		MT7530_FORCE_MODE | PMCR_MAC_TX_EN | PMCR_MAC_RX_EN | \
		PMCR_IFG_XMIT(IFG_64BIT) \
	))
#define MT753X_PMCR_P(x)		(0x3000 + ((x) * 0x100))

#define MT753X_MFC			0x10
#define  BC_FFP_MASK			GENMASK(31, 24)
#define  BC_FFP(x)			FIELD_PREP(BC_FFP_MASK, x)
#define  UNM_FFP_MASK			GENMASK(23, 16)
#define  UNM_FFP(x)			FIELD_PREP(UNM_FFP_MASK, x)
#define  UNU_FFP_MASK			GENMASK(15, 8)
#define  UNU_FFP(x)			FIELD_PREP(UNU_FFP_MASK, x)
#define  MT7530_CPU_EN			BIT(7)
#define  MT7530_CPU_PORT_MASK		GENMASK(6, 4)
#define  MT7530_CPU_PORT(x)		FIELD_PREP(MT7530_CPU_PORT_MASK, x)

#define EN75_MFC_CONFIG ((u32)( \
		BC_FFP(0xff) | UNM_FFP(0xff) | UNU_FFP(0xff) | \
		MT7530_CPU_EN | MT7530_CPU_PORT(6) \
	))

#define SWITCH_MAC_HI			0x30e8
#define SWITCH_MAC_LO			0x30e4

struct en75_eth_pvt {
	struct en75_eth			pub;
	const struct en75_soc_data	*soc;
	struct net_device		*ports[EN75_NUM_GDM_PORTS];
	struct en751221_regs __iomem	*regs;
	struct reset_control		*reset;
	int				qdma_irq[EN75_NUM_QDMA * QDMA_NUM_IRQS];
	struct en75_qdma		*qdma[EN75_NUM_QDMA];
	struct en75_debug		*debug;
	/* Whether our register window covers the on-die switch, see probe. */
	bool				has_switch_regs;
};

union gdm_regs {
	struct gdm	regs;
	u32		words[0x800 / sizeof(u32)];
};

struct en751221_regs {
	/* BFB50000 - BFB50400 */
	u32		fe[0x400 / sizeof(u32)];

	/* BFB50400 - BFB50C00 */
	union gdm_regs	port0;

	/* BFB50C00 - BFB51000 */
	u32		ppe[0x400 / sizeof(u32)];

	/* BFB51000 - BFB51400 */
	u32		unknown_deadbeef[0x400 / sizeof(u32)];

	/* BFB51400 - BFB51C00 */
	union gdm_regs	port1;

	/* BFB51C00 - BFB52000 */
	u32		unknown_deadbeef2[0x400 / sizeof(u32)];

	/* BFB52000 - BFB52400 */
	u32		ppe_accounting[0x400 / sizeof(u32)];

	/* BFB52400 - BFB54000 */
	u32		ppe_unused[0x1c00 / sizeof(u32)];

	/* BFB54000 - BFB56000 */
	struct qregs	qdma_regs[EN75_NUM_QDMA];

	/* BFB56000 - BFB58000 */
	u32		unknown_zeroed[0x2000 / sizeof(u32)];

	/* BFB58000 - BFB60000 */
	u32		switch_regs[0x8000 / sizeof(u32)];
};
_Static_assert(sizeof(struct en751221_regs) == 0x10000, "en751221_regs size incorrect");

static struct net_device *en75_get_sport_dev(struct en75_eth_pvt *eth,
					     enum etx_fport sport)
{
	if (sport == ETX_FPORT_GDM2) {
		return eth->ports[1];
	} else {
		if (sport != ETX_FPORT_GDM1)
			dev_info(eth->pub.dev, "rx: on unexpected fport %d\n", sport);
		return eth->ports[0];
	}
}

int en75_rx_before_recv(struct en75_eth *eth, struct sk_buff *skb,
			enum etx_fport sport)
{
	struct en75_eth_pvt *ep = (struct en75_eth_pvt *) eth;
	struct net_device *port;

	port = en75_get_sport_dev(ep, sport);
	if (!port)
		return -ENODEV;

	skb->dev = port;
	skb->protocol = eth_type_trans(skb, port);

	if (netdev_uses_dsa(port)) {
		/* PPE module requires untagged packets to work
		 * properly and it provides DSA port index via the
		 * DMA descriptor. Report DSA tag to the DSA stack
		 * via skb dst info.
		 */

		// On EN751221 generally, this is not done, but the
		// EN7526C is special cased and it does do this for
		// that one. If we need it, we'll want to do one
		// codepath for everything. For now we'll do nothing
		// and hope that the tag-basd DSA works.
		//port_num = (sp_tag & 0x7); /*switch port id*/
		#if 0
		u32 sptag = get_erx_sp_tag(&desc.t.erx);
		if (sptag < ARRAY_SIZE(port->dsa_meta) &&
			port->dsa_meta[sptag])
			skb_dst_set_noref(q->skb,
						&port->dsa_meta[sptag]->dst);
		#endif
	}

	return 0;
}

int en75_port_set_macaddr(struct en75_eth *eth, enum etx_fport portn, const u8 *addr)
{
	struct en75_eth_pvt *ep = (struct en75_eth_pvt *) eth;
	u32 __iomem *reg = ep->regs->switch_regs;
	struct gdm_mymac_msb msb = { .word = 0 };
	struct gdm_mymac_lsb lsb = { .word = 0 };

	/*
	 * This programs the switch source MAC address (SMACCR). Those
	 * registers live in the switch register block, so when the switch is
	 * managed by the mt7530 DSA driver they are not ours to write.
	 */
	if (!ep->has_switch_regs)
		return 0;

	if (portn != ETX_FPORT_GDM1)
		return 0;

	set_gdm_mymac_msb_a(&msb, addr[0]);
	set_gdm_mymac_msb_b(&msb, addr[1]);
	set_gdm_mymac_lsb_c(&lsb, addr[2]);
	set_gdm_mymac_lsb_d(&lsb, addr[3]);
	set_gdm_mymac_lsb_e(&lsb, addr[4]);
	set_gdm_mymac_lsb_f(&lsb, addr[5]);

	en75_wreg(lsb, (struct gdm_mymac_lsb *)&reg[SWITCH_MAC_LO / 4]);
	en75_wreg(msb, (struct gdm_mymac_msb *)&reg[SWITCH_MAC_HI / 4]);

	return 0;
}

static int en75_init_port(struct en75_eth_pvt *eth, struct device_node *np)
{
	struct net_device *dev;
	u32 index, id, max_index = ARRAY_SIZE(eth->ports) - 1;
	int err;

	err = airoha_eth_get_port_id(eth->pub.dev, np, 0, max_index, &index);
	if (err)
		return err;

	id = index + 1;

	if (eth->ports[id - 1]) {
		dev_err(eth->pub.dev, "duplicate gdm port id: %d\n", id);
		return -EINVAL;
	}

	if (id == 1)
		dev = en75_alloc_gdm_port(&eth->pub, np,
					  &eth->regs->port0.regs,
					  eth->qdma[0],
					  ETX_FPORT_GDM1,
					  false);
	else if (id == 2)
		dev = en75_alloc_gdm_port(&eth->pub, np,
					  &eth->regs->port1.regs,
					  eth->qdma[1],
					  ETX_FPORT_GDM2,
					  true);
	else
		return -EINVAL;

	if (IS_ERR(dev))
		return PTR_ERR(dev);

	eth->ports[id - 1] = dev;
	return 0;
}

static void en75_prepare_qdma_cfg(struct en75_qdma_cfg *cfg,
				  const struct en75_soc_data *soc)
{
	memset(cfg, 0, sizeof(*cfg));
	for (int i = 0; i < QDMA_NUM_CHAINS; i++)
		cfg->num_rx_descs[i] = 128;
	for (int i = 0; i < QDMA_NUM_CHAINS; i++)
		cfg->num_tx_descs[i] = 128;
	for (int i = 0; i < QDMA_NUM_TX_DONE; i++)
		cfg->done_list_size[i] = 256;
	cfg->fwd_max_packet_size = EN75_MAX_PACKET_SIZE;
	cfg->fwd_low_threshold = 32;
	cfg->num_fwd_descs = 256;
	cfg->soc = soc;
}

static void en75_remove(struct platform_device *pdev)
{
	struct en75_eth_pvt *eth = platform_get_drvdata(pdev);
	int i;

	if (!eth)
		return;

	en75_debugfs_exit(eth->debug);

	for (i = 0; i < ARRAY_SIZE(eth->ports); i++) {
		if (!eth->ports[i])
			continue;

		unregister_netdev(eth->ports[i]);
	}

	for (i = 0; i < ARRAY_SIZE(eth->qdma); i++)
		if (eth->qdma[i])
			en75_qdma_destroy(eth->qdma[i]);

	platform_set_drvdata(pdev, NULL);
}

static int en75_probe(struct platform_device *pdev)
{
	struct en75_debug_conf debug_conf = {0};
	struct en751221_regs __iomem *regs;
	struct resource *regs_res;
	struct en75_eth_pvt *eth;
	struct en75_qdma_cfg cfg;
	struct device_node *np;
	int i, err, irq;

	eth = devm_kzalloc(&pdev->dev, sizeof(*eth), GFP_KERNEL);
	if (!eth)
		return -ENOMEM;

	eth->soc = of_device_get_match_data(&pdev->dev);
	if (!eth->soc)
		return dev_err_probe(&pdev->dev, -EINVAL, "No matching SoC data\n");

	eth->pub.dev = &pdev->dev;
	platform_set_drvdata(pdev, eth);

	err = airoha_eth_set_dma_mask(&pdev->dev);
	if (err)
		return err;

	regs = devm_platform_get_and_ioremap_resource(pdev, 0, &regs_res);
	if (IS_ERR(regs))
		return dev_err_probe(&pdev->dev, PTR_ERR(regs),
				     "failed to map registers\n");

	if (resource_size(regs_res) <
	    offsetof(struct en751221_regs, switch_regs)) {
		return dev_err_probe(&pdev->dev, -EINVAL,
				     "insufficient register space\n");
	}

	/*
	 * The on-die switch sits right above the frame engine in the register
	 * space. When it is managed by the mt7530 DSA driver it is described
	 * by its own device tree node and claims those registers itself, so
	 * the window handed to us stops short of them. In that case we are
	 * purely the DSA conduit and must leave the switch alone.
	 */
	eth->has_switch_regs =
		resource_size(regs_res) >= sizeof(struct en751221_regs);

	eth->regs = regs;

	eth->reset = devm_reset_control_array_get_exclusive(&pdev->dev);
	if (IS_ERR(eth->reset)) {
		err = PTR_ERR(eth->reset);
		if (err == -EPROBE_DEFER)
			return err;

		dev_warn(&pdev->dev, "failed to get resets %pe\n", eth->reset);
		eth->reset = NULL;
	} else {
		err = reset_control_assert(eth->reset);
		if (err)
			return err;

		msleep(20);
		err = reset_control_deassert(eth->reset);
		if (err)
			return err;
	}

	for (i = 0; i < ARRAY_SIZE(eth->qdma_irq); i++) {
		irq = platform_get_irq(pdev, i);
		if (irq < 0)
			return dev_err_probe(&pdev->dev, irq,
					     "failed to get IRQ %d\n", i);
		eth->qdma_irq[i] = irq;
	}

	BUILD_BUG_ON(ARRAY_SIZE(eth->qdma) != ARRAY_SIZE(regs->qdma_regs));
	BUILD_BUG_ON(ARRAY_SIZE(eth->qdma) != ARRAY_SIZE(eth->qdma_irq) * QDMA_NUM_IRQS);
	for (i = 0; i < ARRAY_SIZE(eth->qdma); i++) {
		en75_prepare_qdma_cfg(&cfg, eth->soc);
		eth->qdma[i] = en75_qdma_new(&eth->pub, &regs->qdma_regs[i], i,
					     &eth->qdma_irq[i * QDMA_NUM_IRQS],
					     QDMA_NUM_IRQS, &cfg,
					     &debug_conf.qdma[i]);

		if (IS_ERR(eth->qdma[i])) {
			err = PTR_ERR(eth->qdma[i]);
			eth->qdma[i] = NULL;
			goto error;
		}
	}

	for_each_child_of_node(pdev->dev.of_node, np) {
		if (!of_device_is_compatible(np, "econet,eth-mac"))
			continue;

		if (!of_device_is_available(np))
			continue;

		err = en75_init_port(eth, np);
		if (err) {
			of_node_put(np);
			goto error;
		}
	}

	eth->debug = en75_debugfs_init(&debug_conf);

	/* Configure the MT7530 as a dumb switch, unless it is managed by DSA */
	if (eth->has_switch_regs) {
		en75_wreg(EN75_PMCR_CONFIG, &regs->switch_regs[MT753X_PMCR_P(5) / 4]);
		en75_wreg(EN75_PMCR_CONFIG, &regs->switch_regs[MT753X_PMCR_P(6) / 4]);
		en75_wreg(EN75_MFC_CONFIG, &regs->switch_regs[MT753X_MFC / 4]);
	}

	return 0;

error:
	en75_remove(pdev);
	return err;
}

static const struct en75_soc_data en751221_soc_data = {
	.dscp_byte_swap = true,
};

static const struct en75_soc_data en7528_soc_data = {
	.dscp_byte_swap = false,
};

static const struct of_device_id of_en75_match[] = {
	{ .compatible = "econet,en751221-eth", .data = &en751221_soc_data },
	{ .compatible = "econet,en7528-eth", .data = &en7528_soc_data },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, of_en75_match);

static struct platform_driver en75_driver = {
	.probe = en75_probe,
	.remove = en75_remove,
	.driver = {
		.name = KBUILD_MODNAME,
		.of_match_table = of_en75_match,
	},
};
module_platform_driver(en75_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Caleb James DeLisle <cjd@cjdns.fr>");
MODULE_DESCRIPTION("Ethernet driver for EcoNet SoC");
