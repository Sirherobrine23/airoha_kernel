// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2024 AIROHA Inc
 * Author: Christian Marangi <ansuelsmth@gmail.com>
 */

#include <linux/device.h>
#include <linux/mfd/syscon.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/pcs/pcs-airoha.h>
#include <linux/phylink.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/reset.h>

#include "pcs-airoha.h"

static void airoha_pcs_setup_scu_eth(struct airoha_pcs_priv *priv,
				     phy_interface_t interface)
{
	struct device *dev = priv->dev;
	u32 xsi_sel;

	switch (interface) {
	case PHY_INTERFACE_MODE_SGMII:
	case PHY_INTERFACE_MODE_1000BASEX:
	case PHY_INTERFACE_MODE_2500BASEX:
		xsi_sel = AIROHA_SCU_ETH_XSI_HSGMII;
		break;
	case PHY_INTERFACE_MODE_USXGMII:
	case PHY_INTERFACE_MODE_10GBASER:
	default:
		xsi_sel = AIROHA_SCU_ETH_XSI_USXGMII;
	}

	regmap_update_bits(priv->scu, AIROHA_SCU_SSR3,
			   AIROHA_SCU_ETH_XSI_SEL,
			   xsi_sel);

	/* AN7583 require additional setting */
	if (device_is_compatible(dev, "airoha,an7583-pcs-eth"))
		regmap_update_bits(priv->scu, AIROHA_SCU_WAN_CONF,
				   AIROHA_SCU_ETH_MAC_SEL,
				   AIROHA_SCU_ETH_MAC_SEL_XFI);
}

static void airoha_pcs_setup_scu_pon(struct airoha_pcs_priv *priv,
				     phy_interface_t interface)
{
	u32 xsi_sel, wan_sel;

	switch (interface) {
	case PHY_INTERFACE_MODE_SGMII:
	case PHY_INTERFACE_MODE_1000BASEX:
		wan_sel = AIROHA_SCU_WAN_SEL_SGMII;
		xsi_sel = AIROHA_SCU_PON_XSI_HSGMII;
		break;
	case PHY_INTERFACE_MODE_2500BASEX:
		wan_sel = AIROHA_SCU_WAN_SEL_HSGMII;
		xsi_sel = AIROHA_SCU_PON_XSI_HSGMII;
		break;
	case PHY_INTERFACE_MODE_USXGMII:
	case PHY_INTERFACE_MODE_10GBASER:
	default:
		wan_sel = AIROHA_SCU_WAN_SEL_USXGMII;
		xsi_sel = AIROHA_SCU_PON_XSI_USXGMII;
	}

	regmap_update_bits(priv->scu, AIROHA_SCU_SSTR,
			   AIROHA_SCU_PON_XSI_SEL,
			   xsi_sel);

	regmap_update_bits(priv->scu, AIROHA_SCU_WAN_CONF,
			   AIROHA_SCU_WAN_SEL,
			   wan_sel);
}

static int airoha_pcs_setup_scu(struct airoha_pcs_priv *priv,
				phy_interface_t interface)
{
	const struct airoha_pcs_match_data *data = priv->data;
	int ret;

	ret = reset_control_assert(priv->xfi_rst);
	if (ret)
		return ret;

	switch (data->port_type) {
	case AIROHA_PCS_ETH:
		airoha_pcs_setup_scu_eth(priv, interface);
		break;
	case AIROHA_PCS_PON:
		airoha_pcs_setup_scu_pon(priv, interface);
		break;
	}

	ret = reset_control_deassert(priv->xfi_rst);
	if (ret)
		return ret;

	/* TODO better handle reset from MAC */
	ret = reset_control_bulk_assert(ARRAY_SIZE(priv->rsts),
					priv->rsts);
	if (ret)
		return ret;

	ret = reset_control_bulk_deassert(ARRAY_SIZE(priv->rsts),
					  priv->rsts);
	if (ret)
		return ret;

	return 0;
}

static void airoha_pcs_init_usxgmii(struct airoha_pcs_priv *priv)
{
	const struct airoha_pcs_match_data *data = priv->data;

	regmap_set_bits(priv->multi_sgmii, AIROHA_PCS_MULTI_SGMII_MSG_RX_CTRL_0,
			AIROHA_PCS_HSGMII_XFI_SEL);

	/* Disable Hibernation */
	if (data->hibernation_workaround)
		regmap_clear_bits(priv->usxgmii_pcs, AIROHA_PCS_USXGMII_PCS_CTROL_1,
				AIROHA_PCS_USXGMII_SPEED_SEL_H);

	/* FIXME: wait Airoha */
	/* Avoid PCS sending garbage to MAC in some HW revision (E0) */
	if (data->usxgmii_ber_time_fixup)
		regmap_write(priv->usxgmii_pcs, AIROHA_PCS_USGMII_VENDOR_DEFINE_116, 0);

	if (data->usxgmii_rx_gb_out_vld_tweak)
		regmap_clear_bits(priv->usxgmii_pcs, AN7583_PCS_USXGMII_RTL_MODIFIED,
				  AIROHA_PCS_USXGMII_MODIFIED_RX_GB_OUT_VLD);
}

static void airoha_pcs_init_hsgmii(struct airoha_pcs_priv *priv)
{
	regmap_clear_bits(priv->multi_sgmii, AIROHA_PCS_MULTI_SGMII_MSG_RX_CTRL_0,
			  AIROHA_PCS_HSGMII_XFI_SEL);

	regmap_set_bits(priv->hsgmii_pcs, AIROHA_PCS_HSGMII_PCS_CTROL_1,
			AIROHA_PCS_TBI_10B_MODE);
}

static void airoha_pcs_init_sgmii(struct airoha_pcs_priv *priv)
{
	regmap_clear_bits(priv->multi_sgmii, AIROHA_PCS_MULTI_SGMII_MSG_RX_CTRL_0,
			  AIROHA_PCS_HSGMII_XFI_SEL);

	regmap_set_bits(priv->hsgmii_pcs, AIROHA_PCS_HSGMII_PCS_CTROL_1,
			AIROHA_PCS_TBI_10B_MODE);

	regmap_update_bits(priv->hsgmii_rate_adp, AIROHA_PCS_HSGMII_RATE_ADAPT_CTRL_6,
			   AIROHA_PCS_HSGMII_RATE_ADAPT_RX_AFIFO_DOUT_L,
			   FIELD_PREP(AIROHA_PCS_HSGMII_RATE_ADAPT_RX_AFIFO_DOUT_L, 0x07070707));

	regmap_update_bits(priv->hsgmii_rate_adp, AIROHA_PCS_HSGMII_RATE_ADAPT_CTRL_8,
			   AIROHA_PCS_HSGMII_RATE_ADAPT_RX_AFIFO_DOUT_C,
			   FIELD_PREP(AIROHA_PCS_HSGMII_RATE_ADAPT_RX_AFIFO_DOUT_C, 0xff));
}

static void airoha_pcs_init(struct airoha_pcs_priv *priv,
			    phy_interface_t interface)
{
	switch (interface) {
	case PHY_INTERFACE_MODE_SGMII:
	case PHY_INTERFACE_MODE_1000BASEX:
		airoha_pcs_init_sgmii(priv);
		break;
	case PHY_INTERFACE_MODE_2500BASEX:
		airoha_pcs_init_hsgmii(priv);
		break;
	case PHY_INTERFACE_MODE_USXGMII:
	case PHY_INTERFACE_MODE_10GBASER:
		airoha_pcs_init_usxgmii(priv);
		break;
	default:
		return;
	}
}

static void airoha_pcs_interrupt_init_sgmii(struct airoha_pcs_priv *priv)
{
	/* Disable every interrupt */
	regmap_clear_bits(priv->usxgmii_pcs, AIROHA_PCS_HSGMII_PCS_HSGMII_MODE_INTERRUPT,
			  AIROHA_PCS_HSGMII_MODE2_REMOVE_FAULT_OCCUR_INT |
			  AIROHA_PCS_HSGMII_MODE2_AN_CL37_TIMERDONE_INT |
			  AIROHA_PCS_HSGMII_MODE2_AN_MIS_INT |
			  AIROHA_PCS_HSGMII_MODE2_RX_SYN_DONE_INT |
			  AIROHA_PCS_HSGMII_MODE2_AN_DONE_INT);

	/* Clear interrupt */
	regmap_set_bits(priv->usxgmii_pcs, AIROHA_PCS_HSGMII_PCS_HSGMII_MODE_INTERRUPT,
			AIROHA_PCS_HSGMII_MODE2_REMOVE_FAULT_OCCUR_INT_CLEAR |
			AIROHA_PCS_HSGMII_MODE2_AN_CL37_TIMERDONE_INT_CLEAR |
			AIROHA_PCS_HSGMII_MODE2_AN_MIS_INT_CLEAR |
			AIROHA_PCS_HSGMII_MODE2_RX_SYN_DONE_INT_CLEAR |
			AIROHA_PCS_HSGMII_MODE2_AN_DONE_INT_CLEAR);

	regmap_clear_bits(priv->usxgmii_pcs, AIROHA_PCS_HSGMII_PCS_HSGMII_MODE_INTERRUPT,
			  AIROHA_PCS_HSGMII_MODE2_REMOVE_FAULT_OCCUR_INT_CLEAR |
			  AIROHA_PCS_HSGMII_MODE2_AN_CL37_TIMERDONE_INT_CLEAR |
			  AIROHA_PCS_HSGMII_MODE2_AN_MIS_INT_CLEAR |
			  AIROHA_PCS_HSGMII_MODE2_RX_SYN_DONE_INT_CLEAR |
			  AIROHA_PCS_HSGMII_MODE2_AN_DONE_INT_CLEAR);
}

static void airoha_pcs_interrupt_init_usxgmii(struct airoha_pcs_priv *priv)
{
	/* Disable every Interrupt */
	regmap_clear_bits(priv->usxgmii_pcs, AIROHA_PCS_USXGMII_PCS_CTRL_0,
			  AIROHA_PCS_USXGMII_T_TYPE_T_INT_EN |
			  AIROHA_PCS_USXGMII_T_TYPE_D_INT_EN |
			  AIROHA_PCS_USXGMII_T_TYPE_C_INT_EN |
			  AIROHA_PCS_USXGMII_T_TYPE_S_INT_EN);

	regmap_clear_bits(priv->usxgmii_pcs, AIROHA_PCS_USXGMII_PCS_CTRL_1,
			  AIROHA_PCS_USXGMII_R_TYPE_C_INT_EN |
			  AIROHA_PCS_USXGMII_R_TYPE_S_INT_EN |
			  AIROHA_PCS_USXGMII_TXPCS_FSM_ENC_ERR_INT_EN |
			  AIROHA_PCS_USXGMII_T_TYPE_E_INT_EN);

	regmap_clear_bits(priv->usxgmii_pcs, AIROHA_PCS_USXGMII_PCS_CTRL_2,
			  AIROHA_PCS_USXGMII_RPCS_FSM_DEC_ERR_INT_EN |
			  AIROHA_PCS_USXGMII_R_TYPE_E_INT_EN |
			  AIROHA_PCS_USXGMII_R_TYPE_T_INT_EN |
			  AIROHA_PCS_USXGMII_R_TYPE_D_INT_EN);

	regmap_clear_bits(priv->usxgmii_pcs, AIROHA_PCS_USXGMII_PCS_CTRL_3,
			  AIROHA_PCS_USXGMII_FAIL_SYNC_XOR_ST_INT_EN |
			  AIROHA_PCS_USXGMII_RX_BLOCK_LOCK_ST_INT_EN |
			  AIROHA_PCS_USXGMII_LINK_UP_ST_INT_EN |
			  AIROHA_PCS_USXGMII_HI_BER_ST_INT_EN);

	regmap_clear_bits(priv->usxgmii_pcs, AIROHA_PCS_USXGMII_PCS_CTRL_4,
			  AIROHA_PCS_USXGMII_LINK_DOWN_ST_INT_EN);

	/* Clear any pending interrupt */
	regmap_set_bits(priv->usxgmii_pcs, AIROHA_PCS_USXGMII_PCS_INT_STA_2,
			AIROHA_PCS_USXGMII_RPCS_FSM_DEC_ERR_INT |
			AIROHA_PCS_USXGMII_R_TYPE_E_INT |
			AIROHA_PCS_USXGMII_R_TYPE_T_INT |
			AIROHA_PCS_USXGMII_R_TYPE_D_INT);

	regmap_set_bits(priv->usxgmii_pcs, AIROHA_PCS_USXGMII_PCS_INT_STA_3,
			AIROHA_PCS_USXGMII_FAIL_SYNC_XOR_ST_INT |
			AIROHA_PCS_USXGMII_RX_BLOCK_LOCK_ST_INT |
			AIROHA_PCS_USXGMII_LINK_UP_ST_INT |
			AIROHA_PCS_USXGMII_HI_BER_ST_INT);

	regmap_set_bits(priv->usxgmii_pcs, AIROHA_PCS_USXGMII_PCS_INT_STA_4,
			AIROHA_PCS_USXGMII_LINK_DOWN_ST_INT);

	/* Interrupt saddly seems to be not weel supported for Link Down.
	 * PCS Poll is a must to correctly read and react on Cable Deatch
	 * as only cable attach interrupt are fired and Link Down interrupt
	 * are fired only in special case like AN restart.
	 */
}

static void airoha_pcs_interrupt_init(struct airoha_pcs_priv *priv,
				      phy_interface_t interface)
{
	switch (interface) {
	case PHY_INTERFACE_MODE_SGMII:
	case PHY_INTERFACE_MODE_1000BASEX:
	case PHY_INTERFACE_MODE_2500BASEX:
		return airoha_pcs_interrupt_init_sgmii(priv);
	case PHY_INTERFACE_MODE_USXGMII:
	case PHY_INTERFACE_MODE_10GBASER:
		return airoha_pcs_interrupt_init_usxgmii(priv);
	default:
		return;
	}
}

static void airoha_pcs_get_state_sgmii(struct airoha_pcs_priv *priv,
				       struct phylink_link_state *state)
{
	u32 bmsr, lpa;

	regmap_read(priv->hsgmii_an, AIROHA_PCS_HSGMII_AN_SGMII_REG_AN_1,
		    &bmsr);
	regmap_read(priv->hsgmii_an, AIROHA_PCS_HSGMII_AN_SGMII_REG_AN_5,
		    &lpa);

	bmsr = (AIROHA_PCS_HSGMII_AN_SGMII_AN_COMPLETE |
		AIROHA_PCS_HSGMII_AN_SGMII_REMOTE_FAULT |
		AIROHA_PCS_HSGMII_AN_SGMII_AN_ABILITY |
		AIROHA_PCS_HSGMII_AN_SGMII_LINK_STATUS) & bmsr;
	lpa = AIROHA_PCS_HSGMII_AN_SGMII_PARTNER_ABILITY & lpa;

	phylink_mii_c22_pcs_decode_state(state, bmsr, lpa);
}

static void airoha_pcs_get_state_hsgmii(struct airoha_pcs_priv *priv,
					struct phylink_link_state *state)
{
	u32 bmsr;

	regmap_read(priv->hsgmii_an, AIROHA_PCS_HSGMII_AN_SGMII_REG_AN_1,
		    &bmsr);

	bmsr = (AIROHA_PCS_HSGMII_AN_SGMII_AN_COMPLETE |
		AIROHA_PCS_HSGMII_AN_SGMII_REMOTE_FAULT |
		AIROHA_PCS_HSGMII_AN_SGMII_AN_ABILITY |
		AIROHA_PCS_HSGMII_AN_SGMII_LINK_STATUS) & bmsr;

	state->link = !!(bmsr & BMSR_LSTATUS);
	state->an_complete = !!(bmsr & BMSR_ANEGCOMPLETE);
	state->speed = SPEED_2500;
	state->duplex = DUPLEX_FULL;
}

static void airoha_pcs_get_state_usxgmii(struct airoha_pcs_priv *priv,
					 struct phylink_link_state *state)
{
	const struct airoha_pcs_match_data *data = priv->data;
	u32 an_done, lpa;

	/* Trigger HW workaround if needed. If an error is reported,
	 * consider link down and test again later.
	 */
	if (data->rxlock_workaround && data->rxlock_workaround(priv)) {
		state->link = false;
		return;
	}

	/* Toggle AN Status */
	regmap_set_bits(priv->usxgmii_pcs, AIROHA_PCS_USXGMII_PCS_AN_CONTROL_6,
			AIROHA_PCS_USXGMII_TOG_PCS_AUTONEG_STS);
	regmap_clear_bits(priv->usxgmii_pcs, AIROHA_PCS_USXGMII_PCS_AN_CONTROL_6,
			  AIROHA_PCS_USXGMII_TOG_PCS_AUTONEG_STS);

	regmap_read(priv->usxgmii_pcs, AIROHA_PCS_USXGMII_PCS_AN_STATS_0, &lpa);
	regmap_read(priv->usxgmii_pcs, AIROHA_PCS_USXGMII_PCS_AN_STATS_2, &an_done);

	state->link = !!(lpa & MDIO_USXGMII_LINK);
	state->an_complete = !!(an_done & AIROHA_PCS_USXGMII_PCS_AN_COMPLETE);

	phylink_decode_usxgmii_word(state, lpa);
}

static void airoha_pcs_get_state_10gbaser(struct airoha_pcs_priv *priv,
					  struct phylink_link_state *state)
{
	u32 status, curr_mode;

	/* Toggle AN Status */
	regmap_set_bits(priv->usxgmii_pcs, AIROHA_PCS_USXGMII_PCS_AN_CONTROL_6,
			AIROHA_PCS_USXGMII_TOG_PCS_AUTONEG_STS);
	regmap_clear_bits(priv->usxgmii_pcs, AIROHA_PCS_USXGMII_PCS_AN_CONTROL_6,
			  AIROHA_PCS_USXGMII_TOG_PCS_AUTONEG_STS);

	regmap_read(priv->usxgmii_pcs, AIROHA_PCS_USXGMII_BASE_R_10GB_T_PCS_STUS_1,
		    &status);
	regmap_read(priv->usxgmii_pcs, AIROHA_PCS_USXGMII_PCS_AN_STATS_0, &curr_mode);

	state->link = !!(status & AIROHA_PCS_USXGMII_RX_LINK_STUS);

	switch (curr_mode & AIROHA_PCS_USXGMII_CUR_USXGMII_MODE) {
	case AIROHA_PCS_USXGMII_CUR_USXGMII_MODE_10G:
		state->speed = SPEED_10000;
		break;
	case AIROHA_PCS_USXGMII_CUR_USXGMII_MODE_5G:
		state->speed = SPEED_5000;
		break;
	case AIROHA_PCS_USXGMII_CUR_USXGMII_MODE_2_5G:
		state->speed = SPEED_2500;
		break;
	default:
		state->speed = SPEED_UNKNOWN;
		return;
	}

	state->duplex = DUPLEX_FULL;
}

static void airoha_pcs_get_state(struct phylink_pcs *pcs,
				 struct phylink_link_state *state)
{
	struct airoha_pcs_port *port = to_airoha_pcs_port(pcs);
	struct airoha_pcs_priv *priv = port->priv;

	switch (state->interface) {
	case PHY_INTERFACE_MODE_SGMII:
	case PHY_INTERFACE_MODE_1000BASEX:
		airoha_pcs_get_state_sgmii(priv, state);
		break;
	case PHY_INTERFACE_MODE_2500BASEX:
		airoha_pcs_get_state_hsgmii(priv, state);
		break;
	case PHY_INTERFACE_MODE_USXGMII:
		airoha_pcs_get_state_usxgmii(priv, state);
		break;
	case PHY_INTERFACE_MODE_10GBASER:
		airoha_pcs_get_state_10gbaser(priv, state);
		break;
	default:
		return;
	}
}

static int airoha_pcs_config(struct phylink_pcs *pcs, unsigned int neg_mode,
			     phy_interface_t interface,
			     const unsigned long *advertising,
			     bool permit_pause_to_mac)
{
	struct airoha_pcs_port *port = to_airoha_pcs_port(pcs);
	struct airoha_pcs_priv *priv = port->priv;
	const struct airoha_pcs_match_data *data;
	u32 rate_adapt;
	int ret;

	priv->interface = interface;
	data = priv->data;

	/* Apply Analog and Digital configuration for PCS */
	if (data->bringup) {
		ret = data->bringup(priv, interface);
		if (ret)
			return ret;
	}

	/* Set final configuration for various modes */
	airoha_pcs_init(priv, interface);

	/* Configure Interrupt for various modes */
	airoha_pcs_interrupt_init(priv, interface);

	rate_adapt = AIROHA_PCS_HSGMII_RATE_ADAPT_RX_EN |
		     AIROHA_PCS_HSGMII_RATE_ADAPT_TX_EN;

	if (interface == PHY_INTERFACE_MODE_SGMII)
		rate_adapt |= AIROHA_PCS_HSGMII_RATE_ADAPT_RX_BYPASS |
			      AIROHA_PCS_HSGMII_RATE_ADAPT_TX_BYPASS;

	/* AN Auto Settings (Rate Adaptation) */
	regmap_update_bits(priv->hsgmii_rate_adp, AIROHA_PCS_HSGMII_RATE_ADAPT_CTRL_0,
			   AIROHA_PCS_HSGMII_RATE_ADAPT_RX_BYPASS |
			   AIROHA_PCS_HSGMII_RATE_ADAPT_TX_BYPASS |
			   AIROHA_PCS_HSGMII_RATE_ADAPT_RX_EN |
			   AIROHA_PCS_HSGMII_RATE_ADAPT_TX_EN, rate_adapt);

	/* FIXME: With an attached Aeonsemi PHY, AN is needed
	 * even with no inband.
	 */
	if (interface == PHY_INTERFACE_MODE_USXGMII ||
	    interface == PHY_INTERFACE_MODE_10GBASER) {
		if (interface == PHY_INTERFACE_MODE_USXGMII)
			regmap_set_bits(priv->usxgmii_pcs,
					AIROHA_PCS_USXGMII_PCS_AN_CONTROL_0,
					AIROHA_PCS_USXGMII_AN_ENABLE);
		else
			regmap_clear_bits(priv->usxgmii_pcs,
					  AIROHA_PCS_USXGMII_PCS_AN_CONTROL_0,
					  AIROHA_PCS_USXGMII_AN_ENABLE);

		if (data->usxgmii_xfi_mode_sel &&
		    neg_mode == PHYLINK_PCS_NEG_INBAND_ENABLED)
			regmap_set_bits(priv->usxgmii_pcs,
					AIROHA_PCS_USXGMII_PCS_AN_CONTROL_7,
					AIROHA_PCS_USXGMII_XFI_MODE_TX_SEL |
					AIROHA_PCS_USXGMII_XFI_MODE_RX_SEL);
	}

	/* Clear any force bit that my be set by bootloader */
	if (interface == PHY_INTERFACE_MODE_SGMII ||
	    interface == PHY_INTERFACE_MODE_1000BASEX ||
	    interface == PHY_INTERFACE_MODE_2500BASEX) {
		regmap_clear_bits(priv->multi_sgmii, AIROHA_PCS_MULTI_SGMII_SGMII_STS_CTRL_0,
				  AIROHA_PCS_LINK_MODE_P0 |
				  AIROHA_PCS_FORCE_SPD_MODE_P0 |
				  AIROHA_PCS_FORCE_LINKDOWN_P0 |
				  AIROHA_PCS_FORCE_LINKUP_P0);
	}

	/* Toggle Rate Adaption for SGMII/HSGMII mode */
	if (interface == PHY_INTERFACE_MODE_SGMII ||
	    interface == PHY_INTERFACE_MODE_1000BASEX ||
	    interface == PHY_INTERFACE_MODE_2500BASEX) {
		if (neg_mode == PHYLINK_PCS_NEG_INBAND_ENABLED)
			regmap_clear_bits(priv->hsgmii_rate_adp,
					  AIROHA_PCS_HSGMII_RATE_ADP_P0_CTRL_0,
					  AIROHA_PCS_HSGMII_P0_DIS_MII_MODE);
		else
			regmap_set_bits(priv->hsgmii_rate_adp,
					AIROHA_PCS_HSGMII_RATE_ADP_P0_CTRL_0,
					AIROHA_PCS_HSGMII_P0_DIS_MII_MODE);
	}

	/* Setup AN Link Timer */
	if (interface == PHY_INTERFACE_MODE_SGMII ||
	    interface == PHY_INTERFACE_MODE_1000BASEX) {
		u32 an_timer;

		an_timer = phylink_get_link_timer_ns(interface);

		/* Value needs to be shifted by 4, seems value is internally * 16 */
		regmap_update_bits(priv->hsgmii_an, AIROHA_PCS_HSGMII_AN_SGMII_REG_AN_11,
				   AIROHA_PCS_HSGMII_AN_SGMII_LINK_TIMER,
				   FIELD_PREP(AIROHA_PCS_HSGMII_AN_SGMII_LINK_TIMER,
					      an_timer >> 4));

		regmap_update_bits(priv->hsgmii_pcs, AIROHA_PCS_HSGMII_PCS_CTROL_3,
				   AIROHA_PCS_HSGMII_PCS_LINK_STSTIME,
				   FIELD_PREP(AIROHA_PCS_HSGMII_PCS_LINK_STSTIME,
					      an_timer >> 4));
	}

	/* Setup SGMII AN and advertisement in DEV_ABILITY */
	if (interface == PHY_INTERFACE_MODE_SGMII) {
		if (neg_mode == PHYLINK_PCS_NEG_INBAND_ENABLED) {
			int advertise = phylink_mii_c22_pcs_encode_advertisement(interface,
										 advertising);
			if (advertise < 0)
				return advertise;

			regmap_update_bits(priv->hsgmii_an, AIROHA_PCS_HSGMII_AN_SGMII_REG_AN_4,
					   AIROHA_PCS_HSGMII_AN_SGMII_DEV_ABILITY,
					   FIELD_PREP(AIROHA_PCS_HSGMII_AN_SGMII_DEV_ABILITY,
						      advertise));

			regmap_set_bits(priv->hsgmii_an, AIROHA_PCS_HSGMII_AN_SGMII_REG_AN_0,
					AIROHA_PCS_HSGMII_AN_SGMII_RA_ENABLE);
		} else {
			regmap_clear_bits(priv->hsgmii_an, AIROHA_PCS_HSGMII_AN_SGMII_REG_AN_0,
					  AIROHA_PCS_HSGMII_AN_SGMII_RA_ENABLE);
		}
	}

	if (interface == PHY_INTERFACE_MODE_2500BASEX) {
		regmap_clear_bits(priv->hsgmii_an, AIROHA_PCS_HSGMII_AN_SGMII_REG_AN_0,
				  AIROHA_PCS_HSGMII_AN_SGMII_RA_ENABLE);

		regmap_set_bits(priv->hsgmii_pcs, AIROHA_PCS_HSGMII_PCS_CTROL_6,
				AIROHA_PCS_HSGMII_PCS_TX_ENABLE);
	}

	if (interface == PHY_INTERFACE_MODE_SGMII ||
	    interface == PHY_INTERFACE_MODE_1000BASEX) {
		u32 if_mode = AIROHA_PCS_HSGMII_AN_SIDEBAND_EN;

		/* Toggle SGMII or 1000base-x mode */
		if (interface == PHY_INTERFACE_MODE_SGMII)
			if_mode |= AIROHA_PCS_HSGMII_AN_SGMII_EN;

		if (neg_mode & PHYLINK_PCS_NEG_INBAND)
			regmap_set_bits(priv->hsgmii_an, AIROHA_PCS_HSGMII_AN_SGMII_REG_AN_13,
					AIROHA_PCS_HSGMII_AN_SGMII_REMOTE_FAULT_DIS);
		else
			regmap_clear_bits(priv->hsgmii_an, AIROHA_PCS_HSGMII_AN_SGMII_REG_AN_13,
					  AIROHA_PCS_HSGMII_AN_SGMII_REMOTE_FAULT_DIS);

		if (neg_mode == PHYLINK_PCS_NEG_INBAND_ENABLED) {
			/* Clear force speed bits and MAC mode */
			regmap_clear_bits(priv->hsgmii_pcs, AIROHA_PCS_HSGMII_PCS_CTROL_6,
					  AIROHA_PCS_HSGMII_PCS_SGMII_SPD_FORCE_10 |
					  AIROHA_PCS_HSGMII_PCS_SGMII_SPD_FORCE_100 |
					  AIROHA_PCS_HSGMII_PCS_SGMII_SPD_FORCE_1000 |
					  AIROHA_PCS_HSGMII_PCS_MAC_MODE |
					  AIROHA_PCS_HSGMII_PCS_FORCE_RATEADAPT_VAL |
					  AIROHA_PCS_HSGMII_PCS_FORCE_RATEADAPT);
		} else {
			/* Enable compatibility with MAC PCS Layer */
			if_mode |= AIROHA_PCS_HSGMII_AN_SGMII_COMPAT_EN;

			/* AN off force rate adaption, speed is set later in Link Up */
			regmap_set_bits(priv->hsgmii_pcs, AIROHA_PCS_HSGMII_PCS_CTROL_6,
					AIROHA_PCS_HSGMII_PCS_MAC_MODE |
					AIROHA_PCS_HSGMII_PCS_FORCE_RATEADAPT);
		}

		regmap_update_bits(priv->hsgmii_an, AIROHA_PCS_HSGMII_AN_SGMII_REG_AN_13,
				   AIROHA_PCS_HSGMII_AN_SGMII_IF_MODE_5_0, if_mode);

		regmap_set_bits(priv->hsgmii_pcs, AIROHA_PCS_HSGMII_PCS_CTROL_6,
				AIROHA_PCS_HSGMII_PCS_TX_ENABLE |
				AIROHA_PCS_HSGMII_PCS_MODE2_EN);
	}

	if (interface == PHY_INTERFACE_MODE_1000BASEX &&
	    neg_mode != PHYLINK_PCS_NEG_INBAND_ENABLED) {
		regmap_set_bits(priv->hsgmii_pcs, AIROHA_PCS_HSGMII_PCS_CTROL_1,
				AIROHA_PCS_SGMII_SEND_AN_ERR_EN);

		regmap_set_bits(priv->hsgmii_pcs, AIROHA_PCS_HSGMII_AN_SGMII_REG_AN_FORCE_CL37,
				AIROHA_PCS_HSGMII_AN_FORCE_AN_DONE);
	}

	/* Configure Flow Control on XFI */
	regmap_update_bits(priv->xfi_mac, AIROHA_PCS_XFI_MAC_XFI_GIB_CFG,
			   AIROHA_PCS_XFI_TX_FC_EN | AIROHA_PCS_XFI_RX_FC_EN,
			   permit_pause_to_mac ?
				AIROHA_PCS_XFI_TX_FC_EN | AIROHA_PCS_XFI_RX_FC_EN :
				0);

	return 0;
}

static void airoha_pcs_an_restart(struct phylink_pcs *pcs)
{
	struct airoha_pcs_port *port = to_airoha_pcs_port(pcs);
	struct airoha_pcs_priv *priv = port->priv;

	switch (priv->interface) {
	case PHY_INTERFACE_MODE_SGMII:
	case PHY_INTERFACE_MODE_1000BASEX:
	case PHY_INTERFACE_MODE_2500BASEX:
		regmap_set_bits(priv->hsgmii_an, AIROHA_PCS_HSGMII_AN_SGMII_REG_AN_0,
				AIROHA_PCS_HSGMII_AN_SGMII_AN_RESTART);
		udelay(3);
		regmap_clear_bits(priv->hsgmii_an, AIROHA_PCS_HSGMII_AN_SGMII_REG_AN_0,
				  AIROHA_PCS_HSGMII_AN_SGMII_AN_RESTART);
		break;
	case PHY_INTERFACE_MODE_USXGMII:
		regmap_set_bits(priv->usxgmii_pcs, AIROHA_PCS_USXGMII_PCS_AN_CONTROL_0,
				AIROHA_PCS_USXGMII_AN_RESTART);
		udelay(3);
		regmap_clear_bits(priv->usxgmii_pcs, AIROHA_PCS_USXGMII_PCS_AN_CONTROL_0,
				  AIROHA_PCS_USXGMII_AN_RESTART);
	default:
		return;
	}
}

static void airoha_pcs_link_up(struct phylink_pcs *pcs, unsigned int neg_mode,
			       phy_interface_t interface, int speed, int duplex)
{
	struct airoha_pcs_port *port = to_airoha_pcs_port(pcs);
	struct airoha_pcs_priv *priv = port->priv;
	const struct airoha_pcs_match_data *data;

	data = priv->data;

	if (neg_mode == PHYLINK_PCS_NEG_INBAND_ENABLED) {
		if (interface == PHY_INTERFACE_MODE_SGMII) {
			regmap_update_bits(priv->hsgmii_rate_adp,
					   AIROHA_PCS_HSGMII_RATE_ADAPT_CTRL_1,
					   AIROHA_PCS_HSGMII_RATE_ADAPT_RX_AFIFO_WR_THR |
					   AIROHA_PCS_HSGMII_RATE_ADAPT_RX_AFIFO_RD_THR,
					   FIELD_PREP(AIROHA_PCS_HSGMII_RATE_ADAPT_RX_AFIFO_WR_THR, 0x0) |
					   FIELD_PREP(AIROHA_PCS_HSGMII_RATE_ADAPT_RX_AFIFO_RD_THR, 0x0));
			udelay(1);
			regmap_update_bits(priv->hsgmii_rate_adp,
					   AIROHA_PCS_HSGMII_RATE_ADAPT_CTRL_1,
					   AIROHA_PCS_HSGMII_RATE_ADAPT_RX_AFIFO_WR_THR |
					   AIROHA_PCS_HSGMII_RATE_ADAPT_RX_AFIFO_RD_THR,
					   FIELD_PREP(AIROHA_PCS_HSGMII_RATE_ADAPT_RX_AFIFO_WR_THR, 0xf) |
					   FIELD_PREP(AIROHA_PCS_HSGMII_RATE_ADAPT_RX_AFIFO_RD_THR, 0x5));
		}
	} else {
		if (interface == PHY_INTERFACE_MODE_USXGMII ||
		    interface == PHY_INTERFACE_MODE_10GBASER) {
			u32 mode;
			u32 rate_adapt;

			switch (speed) {
			case SPEED_10000:
				rate_adapt = AIROHA_PCS_HSGMII_RATE_ADPT_FORCE_RATE_ADAPT_MODE_10000;
				mode = AIROHA_PCS_USXGMII_MODE_10000;
				break;
			case SPEED_5000:
				rate_adapt = AIROHA_PCS_HSGMII_RATE_ADPT_FORCE_RATE_ADAPT_MODE_5000;
				mode = AIROHA_PCS_USXGMII_MODE_5000;
				break;
			case SPEED_2500:
				rate_adapt = AIROHA_PCS_HSGMII_RATE_ADPT_FORCE_RATE_ADAPT_MODE_2500;
				mode = AIROHA_PCS_USXGMII_MODE_2500;
				break;
			case SPEED_1000:
				rate_adapt = AIROHA_PCS_HSGMII_RATE_ADPT_FORCE_RATE_ADAPT_MODE_1000;
				mode = AIROHA_PCS_USXGMII_MODE_1000;
				break;
			case SPEED_100:
				rate_adapt = AIROHA_PCS_HSGMII_RATE_ADPT_FORCE_RATE_ADAPT_MODE_100;
				mode = AIROHA_PCS_USXGMII_MODE_100;
				break;
			}

			/* Trigger USXGMII change mode and force selected speed */
			regmap_update_bits(priv->usxgmii_pcs, AIROHA_PCS_USXGMII_PCS_AN_CONTROL_7,
					   AIROHA_PCS_USXGMII_RATE_UPDATE_MODE |
					   AIROHA_PCS_USXGMII_MODE,
					   AIROHA_PCS_USXGMII_RATE_UPDATE_MODE | mode);

			regmap_update_bits(priv->hsgmii_rate_adp, AIROHA_PCS_HSGMII_RATE_ADAPT_CTRL_11,
					   AIROHA_PCS_HSGMII_RATE_ADPT_FORCE_RATE_ADAPT_MODE_EN |
					   AIROHA_PCS_HSGMII_RATE_ADPT_FORCE_RATE_ADAPT_MODE,
					   AIROHA_PCS_HSGMII_RATE_ADPT_FORCE_RATE_ADAPT_MODE_EN |
					   rate_adapt);
		}

		if (interface == PHY_INTERFACE_MODE_SGMII ||
		    interface == PHY_INTERFACE_MODE_1000BASEX) {
			u32 force_speed;
			u32 rate_adapt;

			switch (speed) {
			case SPEED_1000:
				force_speed = AIROHA_PCS_HSGMII_PCS_SGMII_SPD_FORCE_1000;
				rate_adapt = AIROHA_PCS_HSGMII_PCS_FORCE_RATEADAPT_VAL_1000;
				break;
			case SPEED_100:
				force_speed = AIROHA_PCS_HSGMII_PCS_SGMII_SPD_FORCE_100;
				rate_adapt = AIROHA_PCS_HSGMII_PCS_FORCE_RATEADAPT_VAL_100;
				break;
			case SPEED_10:
				force_speed = AIROHA_PCS_HSGMII_PCS_SGMII_SPD_FORCE_10;
				rate_adapt = AIROHA_PCS_HSGMII_PCS_FORCE_RATEADAPT_VAL_10;
				break;
			}

			regmap_update_bits(priv->hsgmii_pcs, AIROHA_PCS_HSGMII_PCS_CTROL_6,
					   AIROHA_PCS_HSGMII_PCS_SGMII_SPD_FORCE_10 |
					   AIROHA_PCS_HSGMII_PCS_SGMII_SPD_FORCE_100 |
					   AIROHA_PCS_HSGMII_PCS_SGMII_SPD_FORCE_1000 |
					   AIROHA_PCS_HSGMII_PCS_FORCE_RATEADAPT_VAL,
					   force_speed | rate_adapt);
		}

		if (interface == PHY_INTERFACE_MODE_SGMII ||
		    interface == PHY_INTERFACE_MODE_2500BASEX) {
			u32 ck_gen_mode;
			u32 speed_reg;
			u32 if_mode;

			switch (speed) {
			case SPEED_2500:
				speed_reg = AIROHA_PCS_LINK_MODE_P0_2_5G;
				break;
			case SPEED_1000:
				speed_reg = AIROHA_PCS_LINK_MODE_P0_1G;
				if_mode = AIROHA_PCS_HSGMII_AN_SPEED_FORCE_MODE_1000;
				ck_gen_mode = AIROHA_PCS_HSGMII_PCS_FORCE_CUR_SGMII_MODE_1000;
				break;
			case SPEED_100:
				speed_reg = AIROHA_PCS_LINK_MODE_P0_100M;
				if_mode = AIROHA_PCS_HSGMII_AN_SPEED_FORCE_MODE_100;
				ck_gen_mode = AIROHA_PCS_HSGMII_PCS_FORCE_CUR_SGMII_MODE_100;
				break;
			case SPEED_10:
				speed_reg = AIROHA_PCS_LINK_MODE_P0_100M;
				if_mode = AIROHA_PCS_HSGMII_AN_SPEED_FORCE_MODE_10;
				ck_gen_mode = AIROHA_PCS_HSGMII_PCS_FORCE_CUR_SGMII_MODE_10;
				break;
			}

			if (interface == PHY_INTERFACE_MODE_SGMII) {
				regmap_update_bits(priv->hsgmii_an, AIROHA_PCS_HSGMII_AN_SGMII_REG_AN_13,
						   AIROHA_PCS_HSGMII_AN_SPEED_FORCE_MODE,
						   if_mode);

				regmap_update_bits(priv->hsgmii_pcs, AIROHA_PCS_HSGMII_PCS_AN_SGMII_MODE_FORCE,
						   AIROHA_PCS_HSGMII_PCS_FORCE_CUR_SGMII_MODE |
						   AIROHA_PCS_HSGMII_PCS_FORCE_CUR_SGMII_MODE_SEL,
						   ck_gen_mode |
						   AIROHA_PCS_HSGMII_PCS_FORCE_CUR_SGMII_MODE_SEL);
			}

			regmap_update_bits(priv->multi_sgmii, AIROHA_PCS_MULTI_SGMII_SGMII_STS_CTRL_0,
					   AIROHA_PCS_LINK_MODE_P0 |
					   AIROHA_PCS_FORCE_SPD_MODE_P0,
					   speed_reg |
					   AIROHA_PCS_FORCE_SPD_MODE_P0);
		}
	}

	if (data->link_up)
		data->link_up(priv);

	/* BPI BMI enable */
	regmap_clear_bits(priv->xfi_mac, AIROHA_PCS_XFI_MAC_XFI_GIB_CFG,
			  AIROHA_PCS_XFI_RXMPI_STOP |
			  AIROHA_PCS_XFI_RXMBI_STOP |
			  AIROHA_PCS_XFI_TXMPI_STOP |
			  AIROHA_PCS_XFI_TXMBI_STOP);
}

static void airoha_pcs_link_down(struct phylink_pcs *pcs)
{
	struct airoha_pcs_port *port = to_airoha_pcs_port(pcs);
	struct airoha_pcs_priv *priv = port->priv;

	/* MPI MBI disable */
	regmap_set_bits(priv->xfi_mac, AIROHA_PCS_XFI_MAC_XFI_GIB_CFG,
			AIROHA_PCS_XFI_RXMPI_STOP |
			AIROHA_PCS_XFI_RXMBI_STOP |
			AIROHA_PCS_XFI_TXMPI_STOP |
			AIROHA_PCS_XFI_TXMBI_STOP);
}

static void airoha_pcs_pre_config(struct phylink_pcs *pcs,
				  phy_interface_t interface)
{
	struct airoha_pcs_port *port = to_airoha_pcs_port(pcs);
	struct airoha_pcs_priv *priv = port->priv;

	/* Select HSGMII or USXGMII in SCU regs */
	airoha_pcs_setup_scu(priv, interface);

	/* MPI MBI disable */
	regmap_set_bits(priv->xfi_mac, AIROHA_PCS_XFI_MAC_XFI_GIB_CFG,
			AIROHA_PCS_XFI_RXMPI_STOP |
			AIROHA_PCS_XFI_RXMBI_STOP |
			AIROHA_PCS_XFI_TXMPI_STOP |
			AIROHA_PCS_XFI_TXMBI_STOP);

	/* Write 1 to trigger reset and clear */
	regmap_clear_bits(priv->xfi_mac, AIROHA_PCS_XFI_MAC_XFI_LOGIC_RST,
			  AIROHA_PCS_XFI_MAC_LOGIC_RST);
	regmap_set_bits(priv->xfi_mac, AIROHA_PCS_XFI_MAC_XFI_LOGIC_RST,
			AIROHA_PCS_XFI_MAC_LOGIC_RST);

	usleep_range(1000, 2000);

	/* Clear XFI MAC counter */
	regmap_set_bits(priv->xfi_mac, AIROHA_PCS_XFI_MAC_XFI_CNT_CLR,
			AIROHA_PCS_XFI_GLB_CNT_CLR);
}

static int airoha_pcs_post_config(struct phylink_pcs *pcs,
				  phy_interface_t interface)
{
	struct airoha_pcs_port *port = to_airoha_pcs_port(pcs);
	struct airoha_pcs_priv *priv = port->priv;

	/* Frag disable */
	regmap_update_bits(priv->xfi_mac, AIROHA_PCS_XFI_MAC_XFI_GIB_CFG,
			   AIROHA_PCS_XFI_RX_FRAG_LEN,
			   FIELD_PREP(AIROHA_PCS_XFI_RX_FRAG_LEN, 31));
	regmap_update_bits(priv->xfi_mac, AIROHA_PCS_XFI_MAC_XFI_GIB_CFG,
			   AIROHA_PCS_XFI_TX_FRAG_LEN,
			   FIELD_PREP(AIROHA_PCS_XFI_TX_FRAG_LEN, 31));

	/* IPG NUM */
	regmap_update_bits(priv->xfi_mac, AIROHA_PCS_XFI_MAC_XFI_GIB_CFG,
			   AIROHA_PCS_XFI_IPG_NUM,
			   FIELD_PREP(AIROHA_PCS_XFI_IPG_NUM, 10));

	/* Enable TX/RX flow control */
	regmap_set_bits(priv->xfi_mac, AIROHA_PCS_XFI_MAC_XFI_GIB_CFG,
			AIROHA_PCS_XFI_TX_FC_EN);
	regmap_set_bits(priv->xfi_mac, AIROHA_PCS_XFI_MAC_XFI_GIB_CFG,
			AIROHA_PCS_XFI_RX_FC_EN);

	return 0;
}

static const struct phylink_pcs_ops airoha_pcs_ops = {
	.pcs_pre_config = airoha_pcs_pre_config,
	.pcs_post_config = airoha_pcs_post_config,
	.pcs_get_state = airoha_pcs_get_state,
	.pcs_config = airoha_pcs_config,
	.pcs_an_restart = airoha_pcs_an_restart,
	.pcs_link_up = airoha_pcs_link_up,
	.pcs_link_down = airoha_pcs_link_down,
};

struct phylink_pcs *airoha_pcs_create(struct device *dev)
{
	struct platform_device *pdev;
	struct airoha_pcs_port *port;
	struct device_node *np;

	np = of_parse_phandle(dev->of_node, "pcs", 0);
	if (!np)
		return ERR_PTR(-ENODEV);

	if (!of_device_is_available(np)) {
		of_node_put(np);
		return ERR_PTR(-ENODEV);
	}

	pdev = of_find_device_by_node(np);
	of_node_put(np);
	if (!pdev || !platform_get_drvdata(pdev)) {
		if (pdev)
			put_device(&pdev->dev);
		return ERR_PTR(-EPROBE_DEFER);
	}

	port = kzalloc(sizeof(*port), GFP_KERNEL);
	if (!port)
		return ERR_PTR(-ENOMEM);

	port->priv = platform_get_drvdata(pdev);
	port->pcs.ops = &airoha_pcs_ops;
	port->pcs.neg_mode = true;
	port->pcs.poll = true;

	return &port->pcs;
}
EXPORT_SYMBOL(airoha_pcs_create);

void airoha_pcs_destroy(struct phylink_pcs *pcs)
{
	struct airoha_pcs_port *port = to_airoha_pcs_port(pcs);

	kfree(port);
}
EXPORT_SYMBOL(airoha_pcs_destroy);

static const struct regmap_config airoha_pcs_regmap_config = {
	.reg_bits = 32,
	.val_bits = 32,
	.reg_stride = 4,
};

static int airoha_pcs_probe(struct platform_device *pdev)
{
	struct regmap_config syscon_config = airoha_pcs_regmap_config;
	const struct airoha_pcs_match_data *data;
	struct device *dev = &pdev->dev;
	struct airoha_pcs_priv *priv;
	void *base;
	int ret;

	data = of_device_get_match_data(dev);

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->dev = dev;
	priv->data = data;

	base = devm_platform_ioremap_resource_byname(pdev, "xfi_mac");
	if (IS_ERR(base))
		return PTR_ERR(base);

	syscon_config.name = "xfi_mac";
	priv->xfi_mac = devm_regmap_init_mmio(dev, base, &syscon_config);
	if (IS_ERR(priv->xfi_mac))
		return PTR_ERR(priv->xfi_mac);

	base = devm_platform_ioremap_resource_byname(pdev, "hsgmii_an");
	if (IS_ERR(base))
		return PTR_ERR(base);

	syscon_config.name = "hsgmii_an";
	priv->hsgmii_an = devm_regmap_init_mmio(dev, base, &syscon_config);
	if (IS_ERR(priv->hsgmii_an))
		return PTR_ERR(priv->hsgmii_an);

	base = devm_platform_ioremap_resource_byname(pdev, "hsgmii_pcs");
	if (IS_ERR(base))
		return PTR_ERR(base);

	syscon_config.name = "hsgmii_pcs";
	priv->hsgmii_pcs = devm_regmap_init_mmio(dev, base, &syscon_config);
	if (IS_ERR(priv->hsgmii_pcs))
		return PTR_ERR(priv->hsgmii_pcs);

	base = devm_platform_ioremap_resource_byname(pdev, "hsgmii_rate_adp");
	if (IS_ERR(base))
		return PTR_ERR(base);

	syscon_config.name = "hsgmii_rate_adp";
	priv->hsgmii_rate_adp = devm_regmap_init_mmio(dev, base, &syscon_config);
	if (IS_ERR(priv->hsgmii_rate_adp))
		return PTR_ERR(priv->hsgmii_rate_adp);

	base = devm_platform_ioremap_resource_byname(pdev, "multi_sgmii");
	if (IS_ERR(base))
		return PTR_ERR(base);

	syscon_config.name = "multi_sgmii";
	priv->multi_sgmii = devm_regmap_init_mmio(dev, base, &syscon_config);
	if (IS_ERR(priv->multi_sgmii))
		return PTR_ERR(priv->multi_sgmii);

	base = devm_platform_ioremap_resource_byname(pdev, "usxgmii");
	if (IS_ERR(base) && PTR_ERR(base) != -ENOENT)
		return PTR_ERR(base);

	syscon_config.name = "usxgmii";
	priv->usxgmii_pcs = devm_regmap_init_mmio(dev, base, &syscon_config);
	if (IS_ERR(priv->usxgmii_pcs))
		return PTR_ERR(priv->usxgmii_pcs);

	base = devm_platform_ioremap_resource_byname(pdev, "xfi_pma");
	if (IS_ERR(base) && PTR_ERR(base) != -ENOENT)
		return PTR_ERR(base);

	syscon_config.name = "xfi_pma";
	priv->xfi_pma = devm_regmap_init_mmio(dev, base, &syscon_config);
	if (IS_ERR(priv->xfi_pma))
		return PTR_ERR(priv->xfi_pma);

	base = devm_platform_ioremap_resource_byname(pdev, "xfi_ana");
	if (IS_ERR(base) && PTR_ERR(base) != -ENOENT)
		return PTR_ERR(base);

	syscon_config.name = "xfi_ana";
	priv->xfi_ana = devm_regmap_init_mmio(dev, base, &syscon_config);
	if (IS_ERR(priv->xfi_ana))
		return PTR_ERR(priv->xfi_ana);

	/* SCU is used to toggle XFI or HSGMII in global SoC registers */
	priv->scu = syscon_regmap_lookup_by_phandle(dev->of_node, "airoha,scu");
	if (IS_ERR(priv->scu))
		return PTR_ERR(priv->scu);

	priv->rsts[0].id = "mac";
	priv->rsts[1].id = "phy";
	ret = devm_reset_control_bulk_get_exclusive(dev, ARRAY_SIZE(priv->rsts),
						    priv->rsts);
	if (ret)
		return dev_err_probe(dev, ret, "failed to get bulk reset lines\n");

	priv->xfi_rst = devm_reset_control_get_optional_exclusive(dev, "xfi");
	if (IS_ERR(priv->xfi_rst))
		return dev_err_probe(dev, PTR_ERR(priv->xfi_rst), "failed to get xfi reset lines\n");

	/* For Ethernet PCS, read the AN7581 SoC revision to check if
	 * manual rx calibration is needed. This is only limited to
	 * any SoC revision before E2.
	 */
	if (device_is_compatible(dev, "airoha,an7581-pcs-eth") &&
	    data->port_type == AIROHA_PCS_ETH) {
		u32 val;

		ret = regmap_read(priv->scu, AIROHA_SCU_PDIDR, &val);
		if (ret)
			return ret;

		if (FIELD_GET(AIROHA_SCU_PRODUCT_ID, val) < 0x2)
			priv->manual_rx_calib = true;
	}

	platform_set_drvdata(pdev, priv);

	return 0;
}

static const struct airoha_pcs_match_data an7581_pcs_eth = {
	.port_type = AIROHA_PCS_ETH,
	.hibernation_workaround = true,
	.usxgmii_ber_time_fixup = true,
	.bringup = an7581_pcs_bringup,
	.link_up = an7581_pcs_phya_link_up,
	.rxlock_workaround = an7581_pcs_rxlock_workaround,
};

static const struct airoha_pcs_match_data an7581_pcs_pon = {
	.port_type = AIROHA_PCS_PON,
	.hibernation_workaround = true,
	.usxgmii_ber_time_fixup = true,
	.bringup = an7581_pcs_bringup,
	.link_up = an7581_pcs_phya_link_up,
};

static const struct airoha_pcs_match_data an7583_pcs_eth = {
	.port_type = AIROHA_PCS_ETH,
	.usxgmii_rx_gb_out_vld_tweak = true,
	.usxgmii_xfi_mode_sel = true,
	.bringup = an7583_pcs_common_phya_bringup,
	.link_up = an7583_pcs_common_phya_link_up,
};

static const struct airoha_pcs_match_data an7583_pcs_pon = {
	.port_type = AIROHA_PCS_PON,
	.usxgmii_rx_gb_out_vld_tweak = true,
	.usxgmii_xfi_mode_sel = true,
	.bringup = an7583_pcs_common_phya_bringup,
	.link_up = an7583_pcs_common_phya_link_up,
};

static const struct of_device_id airoha_pcs_of_table[] = {
	{ .compatible = "airoha,an7581-pcs-eth", .data = &an7581_pcs_eth },
	{ .compatible = "airoha,an7581-pcs-pon", .data = &an7581_pcs_pon },
	{ .compatible = "airoha,an7583-pcs-eth", .data = &an7583_pcs_eth },
	{ .compatible = "airoha,an7583-pcs-pon", .data = &an7583_pcs_pon },
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, airoha_pcs_of_table);

static struct platform_driver airoha_pcs_driver = {
	.driver = {
		.name	 = "airoha-pcs",
		.of_match_table = airoha_pcs_of_table,
	},
	.probe = airoha_pcs_probe,
};
module_platform_driver(airoha_pcs_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Airoha PCS driver");
MODULE_AUTHOR("Christian Marangi <ansuelsmth@gmail.com>");
