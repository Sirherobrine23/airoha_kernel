// SPDX-License-Identifier: GPL-2.0
/*
 * Airoha EN7523 SGMII/HSGMII PCS driver
 *
 * This driver uses the EN7523 DTS layout used by OpenWrt:
 *  - airoha,en7523-pcs-pon:  one PON PCS provider, #pcs-cells = <0>
 *  - airoha,en7523-pcs-pcie: one provider for PCIe0/PCIe1, #pcs-cells = <1>
 *  - airoha,en7523-pcs-usb:  one USB3 PCS provider, #pcs-cells = <0>
 */

#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/property.h>
#include <linux/pcs/pcs-provider.h>
#include <linux/phylink.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/reset.h>
#include <linux/rtnetlink.h>
#include <linux/phy/phy.h>
#include <linux/of.h>

#include "pcs-airoha.h"

#define EN7523_SCU_SSR3				0x94
#define EN7523_SCU_WAN_CONF			0x70

#define EN7523_SCU_SSR3_PCIE0_HSGMII_DIS	BIT(31)
#define EN7523_SCU_SSR3_PCIE1_HSGMII_DIS	BIT(30)
#define EN7523_SCU_SSR3_USB0_HSGMII_DIS	BIT(29)
#define EN7523_SCU_SSR3_PON_MODE		GENMASK(14, 13)
#define EN7523_SCU_SSR3_PON_SGMII		FIELD_PREP_CONST(EN7523_SCU_SSR3_PON_MODE, 0x2)

#define EN7523_WAN_CONF_SGMII			0xe0000010

/* PON serdes analog PMA (the PON PCS has phya=INVALID, so the analog block
 * needs explicit init that the vendor performs in sgmii_api_pon0_force_sgmii).
 */
#define EN7523_SCU_RST_CFG			0x830
#define EN7523_PON_PMA_BASE			0x1faf0000
#define EN7523_PON_PMA_SIZE			0x5000
#define EN7523_XSI_MAC_GLB_CFG			0x77fe2830
#define PMA_SS_LCPLL_TDC_FLT_2			0x4130
#define PMA_SS_LCPLL_TDC_FLT_3			0x4134
#define PMA_SS_LCPLL_TDC_FLT_5			0x413c
#define PMA_PON_SERDES_CTRL_0			0x4200
#define PMA_PON_SERDES_CTRL_10			0x4228
#define PMA_PON_SYS_CTRL_0			0x4300
#define PMA_HG_RST_CTRL_0			0x4310
#define PMA_HG_MODE_CTRL_0			0x4314
#define PMA_HG_RG_CTRL_0			0x4318
#define PMA_FRQ_CTRL_2				0x4364
#define PMA_FRQ_CTRL_4				0x436c
#define PMA_PON_RXFEDIG_CTRL_12			0x4530
#define PMA_PON_DA_CTRL_2			0x4608
#define PMA_PON_OSR_SEL_CTRL			0x4320
#define PMA_RG_SSUSB_CDR_BYPASS			0x3028

#define EN7523_WAN_CONF_HSGMII			0xe0000011

enum en7523_pcs_type {
	EN7523_PCS_PON,
	EN7523_PCS_PCIE,
	EN7523_PCS_USB,
};

struct en7523_pcs_res {
	const char *mac;
	const char *pcs1;
	const char *pcs2;
	const char *an;
	const char *ra;
	const char *phya;
	u32 ssr3_clear;
	bool setup_pon;
};

struct en7523_pcs_data {
	enum en7523_pcs_type type;
	unsigned int num_ports;
	const struct en7523_pcs_res *res;
};

struct en7523_pcs;

struct en7523_pcs_port {
	struct phylink_pcs pcs;
	struct en7523_pcs *priv;
	struct regmap *mac;
	struct regmap *pcs1;
	struct regmap *pcs2;
	struct regmap *an;
	struct regmap *ra;
	struct regmap *phya;
	phy_interface_t interface;
	unsigned int index;
};

struct en7523_pcs {
	struct device *dev;
	const struct en7523_pcs_data *data;
	struct en7523_pcs_port *ports;
	struct regmap *scu;
	void __iomem *pma;
	struct reset_control_bulk_data rsts[2];
};

static struct en7523_pcs_port *to_en7523_pcs_port(struct phylink_pcs *pcs)
{
	return container_of(pcs, struct en7523_pcs_port, pcs);
}

static int en7523_pcs_reset(struct en7523_pcs *priv)
{
	int ret;

	ret = reset_control_bulk_assert(ARRAY_SIZE(priv->rsts), priv->rsts);
	if (ret)
		return ret;

	usleep_range(1000, 2000);

	ret = reset_control_bulk_deassert(ARRAY_SIZE(priv->rsts), priv->rsts);
	if (ret)
		return ret;

	usleep_range(1000, 2000);

	return 0;
}

static int en7523_pcs_init_regmap(struct platform_device *pdev,
					  const char *regname, struct regmap **map)
{
	struct regmap_config config = {
		.name = regname,
		.reg_bits = 32,
		.val_bits = 32,
		.reg_stride = 4,
	};
	struct resource *res;
	void __iomem *base;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, regname);
	if (!res)
		return dev_err_probe(&pdev->dev, -EINVAL,
				     "missing reg resource %s\n", regname);

	base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(base))
		return PTR_ERR(base);

	*map = devm_regmap_init_mmio(&pdev->dev, base, &config);
	return PTR_ERR_OR_ZERO(*map);
}

/*
 * PON serdes SGMII (1.25G) analog init, ported from the vendor
 * sgmii_api_pon0_force_sgmii(). Required because the PON PCS has
 * phya=INVALID so mainline never initialises the analog PMA, and for a 1G
 * copper link the RTL8221B uses an SGMII serdes (not 2500Base-X), which the
 * uninitialised PMA cannot drive. SSR3/WAN_CONF are set by setup_scu().
 */
static void en7523_pcs_pon_serdes_sgmii_init(struct en7523_pcs_port *port)
{
	struct en7523_pcs *priv = port->priv;
	void __iomem *pma = priv->pma;
	u32 glb = EN7523_XSI_MAC_GLB_CFG;

	if (!pma)
		return;

	/* The serdes + XSI-MAC clock-domain reset clears the MAC global
	 * config; save and restore it like the vendor does.
	 */
	if (port->mac)
		regmap_read(port->mac, 0x0, &glb);

	regmap_write(priv->scu, EN7523_SCU_RST_CFG, 0x1);
	regmap_write(priv->scu, EN7523_SCU_RST_CFG, 0x0);
	usleep_range(10000, 11000);
	regmap_write(priv->scu, EN7523_SCU_RST_CFG, 0x180);
	regmap_write(priv->scu, EN7523_SCU_RST_CFG, 0x0);

	regmap_update_bits(priv->scu, EN7523_SCU_SSR3,
			   EN7523_SCU_SSR3_PON_MODE, EN7523_SCU_SSR3_PON_SGMII);
	regmap_write(priv->scu, EN7523_SCU_WAN_CONF, EN7523_WAN_CONF_SGMII);

	writel(0x64000000, pma + PMA_SS_LCPLL_TDC_FLT_2);
	writel(0x00000220, pma + PMA_HG_RG_CTRL_0);
	writel(0x00000103, pma + PMA_PON_SERDES_CTRL_10);
	writel(0x00003003, pma + PMA_FRQ_CTRL_2);
	writel(0x02710271, pma + PMA_FRQ_CTRL_4);
	writel(0x18001722, pma + PMA_RG_SSUSB_CDR_BYPASS);
	writel(0x0000003f, pma + PMA_HG_RST_CTRL_0);
	writel(0x54101801, pma + PMA_PON_DA_CTRL_2);
	writel(0x00380013, pma + PMA_PON_RXFEDIG_CTRL_12);
	writel(0x00000001, pma + PMA_HG_MODE_CTRL_0);
	writel(0x20000100, pma + PMA_SS_LCPLL_TDC_FLT_3);
	writel(0x01010100, pma + PMA_SS_LCPLL_TDC_FLT_5);
	writel(0x00100381, pma + PMA_PON_SERDES_CTRL_0);
	writel(0x3f0003c0, pma + PMA_PON_SYS_CTRL_0);

	if (port->mac)
		regmap_write(port->mac, 0x0, glb);
}

/*
 * PON serdes 2500Base-X (3.125G) analog init, ported from the vendor
 * sgmii_api_pon0_force_hsgmii(). Used when the RTL8221B copper link is 2.5G,
 * where its serdes runs 2500Base-X. Differs from the SGMII init in the
 * frequency divider, system-control and OSR-select values.
 */
static void en7523_pcs_pon_serdes_2500basex_init(struct en7523_pcs_port *port)
{
	struct en7523_pcs *priv = port->priv;
	void __iomem *pma = priv->pma;
	u32 glb = EN7523_XSI_MAC_GLB_CFG;

	if (!pma)
		return;

	if (port->mac)
		regmap_read(port->mac, 0x0, &glb);

	regmap_write(priv->scu, EN7523_SCU_RST_CFG, 0x1);
	regmap_write(priv->scu, EN7523_SCU_RST_CFG, 0x0);
	usleep_range(10000, 11000);
	regmap_write(priv->scu, EN7523_SCU_RST_CFG, 0x180);
	regmap_write(priv->scu, EN7523_SCU_RST_CFG, 0x0);

	regmap_update_bits(priv->scu, EN7523_SCU_SSR3,
			   EN7523_SCU_SSR3_PON_MODE, EN7523_SCU_SSR3_PON_SGMII);
	regmap_write(priv->scu, EN7523_SCU_WAN_CONF, EN7523_WAN_CONF_HSGMII);

	writel(0x64000000, pma + PMA_SS_LCPLL_TDC_FLT_2);
	if (port->an)
		regmap_write(port->an, AIROHA_PCS_HSGMII_AN_SGMII_REG_AN_0,
			     0x00000140);

	writel(0x00000103, pma + PMA_PON_SERDES_CTRL_10);
	writel(0x00003003, pma + PMA_FRQ_CTRL_2);
	writel(0x15601560, pma + PMA_FRQ_CTRL_4);
	writel(0x18001722, pma + PMA_RG_SSUSB_CDR_BYPASS);
	writel(0x250003c0, pma + PMA_PON_SYS_CTRL_0);
	writel(0x0000003f, pma + PMA_HG_RST_CTRL_0);
	writel(0x54101801, pma + PMA_PON_DA_CTRL_2);
	writel(0x00380013, pma + PMA_PON_RXFEDIG_CTRL_12);
	writel(0x00000011, pma + PMA_PON_OSR_SEL_CTRL);
	writel(0x00000001, pma + PMA_HG_MODE_CTRL_0);
	writel(0x240003c0, pma + PMA_PON_SYS_CTRL_0);
	writel(0x00010100, pma + PMA_SS_LCPLL_TDC_FLT_5);
	writel(0x20000100, pma + PMA_SS_LCPLL_TDC_FLT_3);
	writel(0x01010100, pma + PMA_SS_LCPLL_TDC_FLT_5);
	writel(0x00100381, pma + PMA_PON_SERDES_CTRL_0);

	if (port->mac)
		regmap_write(port->mac, 0x0, glb);
}

/*
 * XSI serdes (USB / PCIe0 / PCIe1) init, ported from the vendor
 * sgmii_api_{usb0,pcie0,pcie1}_force_hsgmii(). These serdes have a real phya
 * (handled by set_phya_mode()), so unlike the PON serdes no analog PMA poke is
 * needed -- only the rate-adapt/AN/PCS-control values and a per-port
 * clock-domain reset (RST_CFG bits, saving/restoring the MAC global config),
 * which mainline otherwise omits, leaving the serdes link down. The reset bits
 * differ per port: PCIe0=0x12000, PCIe1=0x24000, USB=0x48000.
 */
static void en7523_pcs_xsi_serdes_init(struct en7523_pcs_port *port,
				       u32 rst_bits)
{
	struct en7523_pcs *priv = port->priv;
	u32 glb = EN7523_XSI_MAC_GLB_CFG, rst;

	regmap_write(port->ra, AIROHA_PCS_HSGMII_RATE_ADAPT_CTRL_0, 0x0c000c11);
	regmap_write(port->an, AIROHA_PCS_HSGMII_AN_SGMII_REG_AN_0, 0x00000140);
	regmap_write(port->pcs2, AIROHA_PCS_HSGMII_PCS_CTROL_6, 0x3);

	if (port->mac)
		regmap_read(port->mac, 0x0, &glb);
	regmap_read(priv->scu, EN7523_SCU_RST_CFG, &rst);
	regmap_write(priv->scu, EN7523_SCU_RST_CFG, rst | rst_bits);
	regmap_write(priv->scu, EN7523_SCU_RST_CFG, rst & ~rst_bits);
	if (port->mac)
		regmap_write(port->mac, 0x0, glb);
}

/*
 * The USB (SSUSB) serdes that GDM3 rides needs its XSI-MAC/serdes
 * clock-domain reset pulsed before the RX datapath comes up. The vendor
 * sgmii_api_usb0_force_{sgmii,hsgmii}() do this (SCU_RST bits 15+18) around
 * a save/restore of the PCS MAC global config; without it the USB PCS
 * trains link/AN but receives no frames. The PON path resets inside its
 * PMA init; the non-PON (USB) path otherwise never resets.
 */
static void en7523_pcs_usb_serdes_reset(struct en7523_pcs_port *port)
{
	struct en7523_pcs *priv = port->priv;
	u32 glb = 0, rst;

	if (port->mac)
		regmap_read(port->mac, 0x0, &glb);
	regmap_read(priv->scu, EN7523_SCU_RST_CFG, &rst);
	regmap_write(priv->scu, EN7523_SCU_RST_CFG, rst | 0x48000);
	regmap_write(priv->scu, EN7523_SCU_RST_CFG, rst & ~0x48000);
	if (port->mac)
		regmap_write(port->mac, 0x0, glb);
}

static void en7523_pcs_setup_scu(struct en7523_pcs_port *port,
					 phy_interface_t interface)
{
	struct en7523_pcs *priv = port->priv;
	const struct en7523_pcs_res *res = &priv->data->res[port->index];

	if (res->setup_pon) {
		/* SSR3/WAN_CONF for the PON serdes are programmed inside the
		 * PMA init, after the SCU clock-domain reset, since that reset
		 * clears them.
		 */
		return;
	}

	if (res->ssr3_clear)
		regmap_clear_bits(priv->scu, EN7523_SCU_SSR3, res->ssr3_clear);
}

static int en7523_pcs_set_phya_mode(struct en7523_pcs_port *port,
					    phy_interface_t interface)
{
	u32 val;

	if (!port->phya)
		return 0;

	val = (interface == PHY_INTERFACE_MODE_2500BASEX) ? 0x14817 : 0x14813;

	return regmap_write(port->phya, AIROHA_PCS_HSGMII_ANA_SGMII_PHYA_11,
				 val);
}

static unsigned int en7523_pcs_inband_caps(struct phylink_pcs *pcs,
						  phy_interface_t interface)
{
	switch (interface) {
	case PHY_INTERFACE_MODE_SGMII:
	case PHY_INTERFACE_MODE_1000BASEX:
	case PHY_INTERFACE_MODE_2500BASEX:
		return LINK_INBAND_ENABLE | LINK_INBAND_DISABLE;
	default:
		return 0;
	}
}

static void en7523_pcs_get_state_sgmii(struct en7523_pcs_port *port,
				       unsigned int neg_mode,
				       struct phylink_link_state *state)
{
	u32 raw_bmsr, raw_lpa, bmsr, lpa;

	regmap_read(port->an, AIROHA_PCS_HSGMII_AN_SGMII_REG_AN_1,
		    &raw_bmsr);
	regmap_read(port->an, AIROHA_PCS_HSGMII_AN_SGMII_REG_AN_5,
		    &raw_lpa);

	bmsr = raw_bmsr & (AIROHA_PCS_HSGMII_AN_SGMII_AN_COMPLETE |
			    AIROHA_PCS_HSGMII_AN_SGMII_REMOTE_FAULT |
			    AIROHA_PCS_HSGMII_AN_SGMII_AN_ABILITY |
			    AIROHA_PCS_HSGMII_AN_SGMII_LINK_STATUS);
	lpa = raw_lpa & AIROHA_PCS_HSGMII_AN_SGMII_PARTNER_ABILITY;

	phylink_mii_c22_pcs_decode_state(state, neg_mode, bmsr, lpa);
}

static void en7523_pcs_get_state_hsgmii(struct en7523_pcs_port *port,
					   struct phylink_link_state *state)
{
	u32 pcs_sts = 0, bmsr = 0;

	regmap_read(port->pcs2, ARIOHA_PCS_HSGMII_PCS_STATE_2, &pcs_sts);
	regmap_read(port->an, AIROHA_PCS_HSGMII_AN_SGMII_REG_AN_1, &bmsr);

	state->link = !!(pcs_sts & AIROHA_PCS_HSGMII_PCS_RX_SYNC) ||
		      !!(bmsr & AIROHA_PCS_HSGMII_AN_SGMII_LINK_STATUS);
	state->an_complete = !!(pcs_sts & AIROHA_PCS_HSGMII_PCS_AN_DONE) ||
			     !!(bmsr & AIROHA_PCS_HSGMII_AN_SGMII_AN_COMPLETE);
	if (state->link) {
		state->speed = SPEED_2500;
		state->duplex = DUPLEX_FULL;
	}
}

static void en7523_pcs_get_state(struct phylink_pcs *pcs,
				 unsigned int neg_mode,
				 struct phylink_link_state *state)
{
	struct en7523_pcs_port *port = to_en7523_pcs_port(pcs);

	switch (state->interface) {
	case PHY_INTERFACE_MODE_SGMII:
	case PHY_INTERFACE_MODE_1000BASEX:
		en7523_pcs_get_state_sgmii(port, neg_mode, state);
		break;
	case PHY_INTERFACE_MODE_2500BASEX:
		en7523_pcs_get_state_hsgmii(port, state);
		break;
	default:
		state->link = false;
		break;
	}

	dev_dbg(port->priv->dev,
		"get_state: type=%u port=%u interface=%s link=%d an_complete=%d speed=%d duplex=%d\n",
		port->priv->data->type, port->index, phy_modes(state->interface),
		state->link, state->an_complete, state->speed, state->duplex);
}

static int en7523_pcs_config_an(struct en7523_pcs_port *port,
				phy_interface_t interface,
				unsigned int neg_mode,
				const unsigned long *advertising)
{
	u32 if_mode = AIROHA_PCS_HSGMII_AN_SIDEBAND_EN;
	int advertise, link_timer;

	if (interface == PHY_INTERFACE_MODE_SGMII)
		if_mode |= AIROHA_PCS_HSGMII_AN_SGMII_EN;

	if (neg_mode != PHYLINK_PCS_NEG_INBAND_ENABLED)
		if_mode |= AIROHA_PCS_HSGMII_AN_SGMII_COMPAT_EN;

	if (neg_mode & PHYLINK_PCS_NEG_INBAND)
		regmap_set_bits(port->an, AIROHA_PCS_HSGMII_AN_SGMII_REG_AN_13,
				AIROHA_PCS_HSGMII_AN_SGMII_REMOTE_FAULT_DIS);
	else
		regmap_clear_bits(port->an, AIROHA_PCS_HSGMII_AN_SGMII_REG_AN_13,
				  AIROHA_PCS_HSGMII_AN_SGMII_REMOTE_FAULT_DIS);

	regmap_update_bits(port->an, AIROHA_PCS_HSGMII_AN_SGMII_REG_AN_13,
			   AIROHA_PCS_HSGMII_AN_SGMII_IF_MODE_5_0, if_mode);

	link_timer = phylink_get_link_timer_ns(interface);
	if (link_timer < 0)
		return link_timer;

	regmap_update_bits(port->an, AIROHA_PCS_HSGMII_AN_SGMII_REG_AN_11,
			   AIROHA_PCS_HSGMII_AN_SGMII_LINK_TIMER,
			   FIELD_PREP(AIROHA_PCS_HSGMII_AN_SGMII_LINK_TIMER,
				      link_timer >> 4));
	regmap_update_bits(port->pcs2, AIROHA_PCS_HSGMII_PCS_CTROL_3,
			   AIROHA_PCS_HSGMII_PCS_LINK_STSTIME,
			   FIELD_PREP(AIROHA_PCS_HSGMII_PCS_LINK_STSTIME,
				      link_timer >> 4));

	if (neg_mode != PHYLINK_PCS_NEG_INBAND_ENABLED) {
		regmap_clear_bits(port->an, AIROHA_PCS_HSGMII_AN_SGMII_REG_AN_0,
				  AIROHA_PCS_HSGMII_AN_SGMII_RA_ENABLE);
		return 0;
	}

	advertise = phylink_mii_c22_pcs_encode_advertisement(interface,
							     advertising);
	if (advertise < 0)
		return advertise;

	regmap_update_bits(port->an, AIROHA_PCS_HSGMII_AN_SGMII_REG_AN_4,
			   AIROHA_PCS_HSGMII_AN_SGMII_DEV_ABILITY,
			   FIELD_PREP(AIROHA_PCS_HSGMII_AN_SGMII_DEV_ABILITY,
				      advertise));
	regmap_set_bits(port->an, AIROHA_PCS_HSGMII_AN_SGMII_REG_AN_0,
			AIROHA_PCS_HSGMII_AN_SGMII_RA_ENABLE);

	return 0;
}

static void en7523_pcs_config_rate_adapt(struct en7523_pcs_port *port,
					 phy_interface_t interface)
{
	u32 val;

	val = AIROHA_PCS_HSGMII_RATE_ADAPT_RX_EN |
	      AIROHA_PCS_HSGMII_RATE_ADAPT_TX_EN |
	      AIROHA_PCS_HSGMII_RATE_ADAPT_RX_BYPASS |
	      AIROHA_PCS_HSGMII_RATE_ADAPT_TX_BYPASS;

	regmap_update_bits(port->ra, AIROHA_PCS_HSGMII_RATE_ADAPT_CTRL_0,
			   AIROHA_PCS_HSGMII_RATE_ADAPT_RX_EN |
			   AIROHA_PCS_HSGMII_RATE_ADAPT_TX_EN |
			   AIROHA_PCS_HSGMII_RATE_ADAPT_RX_BYPASS |
			   AIROHA_PCS_HSGMII_RATE_ADAPT_TX_BYPASS,
			   val);
}

static int en7523_pcs_config(struct phylink_pcs *pcs, unsigned int neg_mode,
			     phy_interface_t interface,
			     const unsigned long *advertising,
			     bool permit_pause_to_mac)
{
	struct en7523_pcs_port *port = to_en7523_pcs_port(pcs);
	int ret;

	switch (interface) {
	case PHY_INTERFACE_MODE_SGMII:
	case PHY_INTERFACE_MODE_1000BASEX:
	case PHY_INTERFACE_MODE_2500BASEX:
		break;
	default:
		return -EOPNOTSUPP;
	}

	en7523_pcs_setup_scu(port, interface);

	if (port->priv->data->res[port->index].setup_pon) {
		/* The RTL8221B drives an SGMII serdes for a <=1G copper link
		 * and a 2500Base-X serdes at 2.5G; bring up the matching PON
		 * analog PMA so the serdes link establishes for either speed.
		 */
		if (interface == PHY_INTERFACE_MODE_2500BASEX)
			en7523_pcs_pon_serdes_2500basex_init(port);
		else if (interface == PHY_INTERFACE_MODE_SGMII ||
			 interface == PHY_INTERFACE_MODE_1000BASEX)
			en7523_pcs_pon_serdes_sgmii_init(port);
	}

	ret = en7523_pcs_set_phya_mode(port, interface);
	if (ret)
		return ret;

	en7523_pcs_config_rate_adapt(port, interface);

	regmap_set_bits(port->pcs2, AIROHA_PCS_HSGMII_PCS_CTROL_6,
			AIROHA_PCS_HSGMII_PCS_TX_ENABLE |
			AIROHA_PCS_HSGMII_PCS_MODE2_EN);

	regmap_set_bits(port->pcs2, AIROHA_PCS_HSGMII_PCS_CTROL_1,
			AIROHA_PCS_TBI_10B_MODE |
			AIROHA_PCS_RX_CLK_ENA |
			AIROHA_PCS_GMII_TXCLK_ENA);

	switch (port->priv->data->type) {
	case EN7523_PCS_PCIE:
		/*
		 * The vendor SGMII and HSGMII sequences both pulse the XSI
		 * clock-domain reset after programming PHYA, rate-adapt, AN
		 * and PCS control. Without this reset, PCIe-backed SerDes lanes
		 * can remain out of sync when no PCIe root port initialized them.
		 */
		en7523_pcs_xsi_serdes_init(port,
					   port->index ? 0x24000 : 0x12000);
		break;
	case EN7523_PCS_USB:
		en7523_pcs_usb_serdes_reset(port);
		break;
	default:
		break;
	}

	if (interface == PHY_INTERFACE_MODE_2500BASEX) {
		regmap_clear_bits(port->an, AIROHA_PCS_HSGMII_AN_SGMII_REG_AN_0,
				  AIROHA_PCS_HSGMII_AN_SGMII_RA_ENABLE);
		regmap_update_bits(port->pcs2, AIROHA_PCS_HSGMII_PCS_CTROL_6,
				   AIROHA_PCS_HSGMII_PCS_SGMII_SPD_FORCE_10 |
				   AIROHA_PCS_HSGMII_PCS_SGMII_SPD_FORCE_100 |
				   AIROHA_PCS_HSGMII_PCS_SGMII_SPD_FORCE_1000 |
				   AIROHA_PCS_HSGMII_PCS_FORCE_RATEADAPT |
				   AIROHA_PCS_HSGMII_PCS_FORCE_RATEADAPT_VAL,
				   0);

		if (port->priv->data->type == EN7523_PCS_USB)
			en7523_pcs_xsi_serdes_init(port, 0x48000);

		port->interface = interface;
		return 0;
	}

	ret = en7523_pcs_config_an(port, interface, neg_mode, advertising);
	if (ret)
		return ret;

	if (neg_mode == PHYLINK_PCS_NEG_INBAND_ENABLED)
		regmap_clear_bits(port->pcs2, AIROHA_PCS_HSGMII_PCS_CTROL_6,
				  AIROHA_PCS_HSGMII_PCS_SGMII_SPD_FORCE_10 |
				  AIROHA_PCS_HSGMII_PCS_SGMII_SPD_FORCE_100 |
				  AIROHA_PCS_HSGMII_PCS_SGMII_SPD_FORCE_1000 |
				  AIROHA_PCS_HSGMII_PCS_MAC_MODE |
				  AIROHA_PCS_HSGMII_PCS_FORCE_RATEADAPT |
				  AIROHA_PCS_HSGMII_PCS_FORCE_RATEADAPT_VAL);
	else
		regmap_set_bits(port->pcs2, AIROHA_PCS_HSGMII_PCS_CTROL_6,
				AIROHA_PCS_HSGMII_PCS_MAC_MODE |
				AIROHA_PCS_HSGMII_PCS_FORCE_RATEADAPT);

	port->interface = interface;
	return 0;
}

static void en7523_pcs_an_restart(struct phylink_pcs *pcs)
{
	struct en7523_pcs_port *port = to_en7523_pcs_port(pcs);

	regmap_set_bits(port->an, AIROHA_PCS_HSGMII_AN_SGMII_REG_AN_0,
			AIROHA_PCS_HSGMII_AN_SGMII_AN_RESTART);
	udelay(3);
	regmap_clear_bits(port->an, AIROHA_PCS_HSGMII_AN_SGMII_REG_AN_0,
			  AIROHA_PCS_HSGMII_AN_SGMII_AN_RESTART);
}

static void en7523_pcs_link_up(struct phylink_pcs *pcs, unsigned int neg_mode,
			      phy_interface_t interface, int speed, int duplex)
{
	struct en7523_pcs_port *port = to_en7523_pcs_port(pcs);
	u32 force_speed = 0, rate_adapt = 0, speed_mode = 0, clk_mode = 0;
	u32 ra_ctrl;

	if (neg_mode == PHYLINK_PCS_NEG_INBAND_ENABLED &&
	    port->priv->data->type != EN7523_PCS_PON)
		return;

	if (interface == PHY_INTERFACE_MODE_2500BASEX) {
		en7523_pcs_set_phya_mode(port, interface);
		return;
	}

	switch (speed) {
	case SPEED_1000:
		force_speed = AIROHA_PCS_HSGMII_PCS_SGMII_SPD_FORCE_1000;
		rate_adapt = AIROHA_PCS_HSGMII_PCS_FORCE_RATEADAPT_VAL_1000;
		speed_mode = AIROHA_PCS_HSGMII_AN_SPEED_FORCE_MODE_1000;
		clk_mode = AIROHA_PCS_HSGMII_PCS_FORCE_CUR_SGMII_MODE_1000;
		ra_ctrl = AIROHA_PCS_HSGMII_RATE_ADAPT_RX_EN |
			  AIROHA_PCS_HSGMII_RATE_ADAPT_TX_EN |
			  AIROHA_PCS_HSGMII_RATE_ADAPT_RX_BYPASS |
			  AIROHA_PCS_HSGMII_RATE_ADAPT_TX_BYPASS;
		break;
	case SPEED_100:
		force_speed = AIROHA_PCS_HSGMII_PCS_SGMII_SPD_FORCE_100;
		rate_adapt = AIROHA_PCS_HSGMII_PCS_FORCE_RATEADAPT_VAL_100;
		speed_mode = AIROHA_PCS_HSGMII_AN_SPEED_FORCE_MODE_100;
		clk_mode = AIROHA_PCS_HSGMII_PCS_FORCE_CUR_SGMII_MODE_100;
		ra_ctrl = AIROHA_PCS_HSGMII_RATE_ADAPT_RX_EN |
			  AIROHA_PCS_HSGMII_RATE_ADAPT_TX_EN;
		break;
	case SPEED_10:
		force_speed = AIROHA_PCS_HSGMII_PCS_SGMII_SPD_FORCE_10;
		rate_adapt = AIROHA_PCS_HSGMII_PCS_FORCE_RATEADAPT_VAL_10;
		speed_mode = AIROHA_PCS_HSGMII_AN_SPEED_FORCE_MODE_10;
		clk_mode = AIROHA_PCS_HSGMII_PCS_FORCE_CUR_SGMII_MODE_10;
		ra_ctrl = AIROHA_PCS_HSGMII_RATE_ADAPT_RX_EN |
			  AIROHA_PCS_HSGMII_RATE_ADAPT_TX_EN;
		break;
	default:
		return;
	}

	regmap_update_bits(port->pcs2, AIROHA_PCS_HSGMII_PCS_CTROL_6,
			   AIROHA_PCS_HSGMII_PCS_SGMII_SPD_FORCE_10 |
			   AIROHA_PCS_HSGMII_PCS_SGMII_SPD_FORCE_100 |
			   AIROHA_PCS_HSGMII_PCS_SGMII_SPD_FORCE_1000 |
			   AIROHA_PCS_HSGMII_PCS_FORCE_RATEADAPT_VAL |
			   AIROHA_PCS_HSGMII_PCS_FORCE_RATEADAPT,
			   force_speed | rate_adapt |
			   AIROHA_PCS_HSGMII_PCS_FORCE_RATEADAPT);

	regmap_update_bits(port->an, AIROHA_PCS_HSGMII_AN_SGMII_REG_AN_13,
			   AIROHA_PCS_HSGMII_AN_SPEED_FORCE_MODE, speed_mode);
	regmap_update_bits(port->pcs2, AIROHA_PCS_HSGMII_PCS_AN_SGMII_MODE_FORCE,
			   AIROHA_PCS_HSGMII_PCS_FORCE_CUR_SGMII_MODE |
			   AIROHA_PCS_HSGMII_PCS_FORCE_CUR_SGMII_MODE_SEL,
			   clk_mode |
			   AIROHA_PCS_HSGMII_PCS_FORCE_CUR_SGMII_MODE_SEL);

	regmap_update_bits(port->ra, AIROHA_PCS_HSGMII_RATE_ADAPT_CTRL_0,
			   AIROHA_PCS_HSGMII_RATE_ADAPT_RX_EN |
			   AIROHA_PCS_HSGMII_RATE_ADAPT_TX_EN |
			   AIROHA_PCS_HSGMII_RATE_ADAPT_RX_BYPASS |
			   AIROHA_PCS_HSGMII_RATE_ADAPT_TX_BYPASS,
			   ra_ctrl);
}

static const struct phylink_pcs_ops en7523_pcs_ops = {
	.pcs_inband_caps = en7523_pcs_inband_caps,
	.pcs_get_state = en7523_pcs_get_state,
	.pcs_config = en7523_pcs_config,
	.pcs_an_restart = en7523_pcs_an_restart,
	.pcs_link_up = en7523_pcs_link_up,
};

static struct phylink_pcs *en7523_pcs_get(struct fwnode_reference_args *args,
						       void *data)
{
	struct en7523_pcs *priv = data;
	unsigned int index = 0;

	if (priv->data->num_ports == 1) {
		if (args->nargs)
			return ERR_PTR(-EINVAL);
	} else {
		if (args->nargs != 1)
			return ERR_PTR(-EINVAL);

		index = args->args[0];
		if (index >= priv->data->num_ports)
			return ERR_PTR(-EINVAL);
	}

	return &priv->ports[index].pcs;
}

static int en7523_pcs_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	const struct en7523_pcs_data *data;
	struct en7523_pcs *priv;
	unsigned int i;
	int ret;

	data = of_device_get_match_data(dev);
	if (!data)
		return -EINVAL;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->ports = devm_kcalloc(dev, data->num_ports, sizeof(*priv->ports),
					GFP_KERNEL);
	if (!priv->ports)
		return -ENOMEM;

	priv->dev = dev;
	priv->data = data;
	priv->scu = syscon_regmap_lookup_by_phandle(dev->of_node, "airoha,scu");
	if (IS_ERR(priv->scu))
		return dev_err_probe(dev, PTR_ERR(priv->scu),
				     "failed to get SCU regmap\n");

	if (data->type == EN7523_PCS_PON) {
		priv->pma = devm_ioremap(dev, EN7523_PON_PMA_BASE,
					 EN7523_PON_PMA_SIZE);
		if (!priv->pma)
			return dev_err_probe(dev, -ENOMEM,
					     "failed to map PON PMA\n");
	}

	priv->rsts[0].id = "mac";
	priv->rsts[1].id = "phy";
	ret = devm_reset_control_bulk_get_optional_exclusive(dev,
						       ARRAY_SIZE(priv->rsts),
						       priv->rsts);
	if (ret)
		return dev_err_probe(dev, ret, "failed to get resets\n");

	ret = en7523_pcs_reset(priv);
	if (ret)
		return dev_err_probe(dev, ret, "failed to reset PCS\n");

	for (i = 0; i < data->num_ports; i++) {
		const struct en7523_pcs_res *res = &data->res[i];
		struct en7523_pcs_port *port = &priv->ports[i];

		port->priv = priv;
		port->index = i;
		port->interface = PHY_INTERFACE_MODE_NA;
		port->pcs.ops = &en7523_pcs_ops;
		port->pcs.poll = true;

		__set_bit(PHY_INTERFACE_MODE_SGMII,
			  port->pcs.supported_interfaces);
		__set_bit(PHY_INTERFACE_MODE_1000BASEX,
			  port->pcs.supported_interfaces);
		__set_bit(PHY_INTERFACE_MODE_2500BASEX,
			  port->pcs.supported_interfaces);

		ret = en7523_pcs_init_regmap(pdev, res->mac, &port->mac);
		if (ret)
			return ret;

		ret = en7523_pcs_init_regmap(pdev, res->pcs1, &port->pcs1);
		if (ret)
			return ret;

		ret = en7523_pcs_init_regmap(pdev, res->pcs2, &port->pcs2);
		if (ret)
			return ret;

		ret = en7523_pcs_init_regmap(pdev, res->an, &port->an);
		if (ret)
			return ret;

		ret = en7523_pcs_init_regmap(pdev, res->ra, &port->ra);
		if (res->phya && ret == 0)
			ret = en7523_pcs_init_regmap(pdev, res->phya, &port->phya);
		if (ret)
			return ret;
	}

	platform_set_drvdata(pdev, priv);

	return fwnode_pcs_add_provider(dev_fwnode(dev), en7523_pcs_get, priv);
}

static void en7523_pcs_remove(struct platform_device *pdev)
{
	struct en7523_pcs *priv = platform_get_drvdata(pdev);
	unsigned int i;

	fwnode_pcs_del_provider(dev_fwnode(&pdev->dev));

	rtnl_lock();
	for (i = 0; i < priv->data->num_ports; i++)
		phylink_release_pcs(&priv->ports[i].pcs);
	rtnl_unlock();
}

static const struct en7523_pcs_res en7523_pon_res[] = {
	{
		.mac = "pcs-mac",
		.pcs1 = "pcs1",
		.pcs2 = "pcs2",
		.an = "an",
		.ra = "rate-adapt",
		.setup_pon = true,
	},
};

static const struct en7523_pcs_res en7523_pcie_res[] = {
	{
		.mac = "pcs-mac0",
		.pcs1 = "pcie0-pcs1",
		.pcs2 = "pcie0-pcs2",
		.an = "pcie0-an",
		.ra = "pcie0-rate-adapt",
		.phya = "pcie0-phya",
		.ssr3_clear = EN7523_SCU_SSR3_PCIE0_HSGMII_DIS,
	},
	{
		.mac = "pcs-mac1",
		.pcs1 = "pcie1-pcs1",
		.pcs2 = "pcie1-pcs2",
		.an = "pcie1-an",
		.ra = "pcie1-rate-adapt",
		.phya = "pcie1-phya",
		.ssr3_clear = EN7523_SCU_SSR3_PCIE1_HSGMII_DIS,
	},
};

static const struct en7523_pcs_res en7523_usb_res[] = {
	{
		.mac = "pcs-mac",
		.pcs1 = "pcs1",
		.pcs2 = "pcs2",
		.an = "an",
		.ra = "rate-adapt",
		.phya = "phya",
		.ssr3_clear = EN7523_SCU_SSR3_USB0_HSGMII_DIS,
	},
};

static const struct en7523_pcs_data en7523_pon_data = {
	.type = EN7523_PCS_PON,
	.num_ports = ARRAY_SIZE(en7523_pon_res),
	.res = en7523_pon_res,
};

static const struct en7523_pcs_data en7523_pcie_data = {
	.type = EN7523_PCS_PCIE,
	.num_ports = ARRAY_SIZE(en7523_pcie_res),
	.res = en7523_pcie_res,
};

static const struct en7523_pcs_data en7523_usb_data = {
	.type = EN7523_PCS_USB,
	.num_ports = ARRAY_SIZE(en7523_usb_res),
	.res = en7523_usb_res,
};

static const struct of_device_id en7523_pcs_of_match[] = {
	{ .compatible = "airoha,en7523-pcs-pon", .data = &en7523_pon_data },
	{ .compatible = "airoha,en7523-pcs-pcie", .data = &en7523_pcie_data },
	{ .compatible = "airoha,en7523-pcs-usb", .data = &en7523_usb_data },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, en7523_pcs_of_match);

static struct platform_driver en7523_pcs_driver = {
	.probe = en7523_pcs_probe,
	.remove = en7523_pcs_remove,
	.driver = {
		.name = "airoha-en7523-pcs",
		.of_match_table = en7523_pcs_of_match,
	},
};
module_platform_driver(en7523_pcs_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Airoha EN7523 PCS driver");
MODULE_AUTHOR("Matheus Sampaio Queiroga <srherobrine200@gmail.com>");
MODULE_AUTHOR("Benjamin Larsson <benjamin.larsson@genexis.eu>");
