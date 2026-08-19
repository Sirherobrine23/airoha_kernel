// SPDX-License-Identifier: GPL-2.0
/*
 * EN7572 / AN8901 runtime control loops.
 *
 * The MD32 firmware runs the closed-loop laser control on its own; these are
 * the host-side assists the vendor driver layers on top:
 *
 *  - en7572_adaptive_pon(): load one of the two calibrated Tx "eyes" from the
 *    BOB table into the hardware on a PON / Tx-mode change.
 *  - en7572_reduce_imod(): cap the modulation current at high temperature so a
 *    hot module cannot drive a destructive current once Iav/Imod saturate.
 *  - en7572_adaptive_pav(): trim the average optical power down when Imod hits
 *    its ceiling, and restore it when headroom returns.
 *
 * The two periodic loops keep their cadence with a per-loop seconds countdown
 * decremented by the 1 Hz worker, reproducing the vendor's variable msleep().
 *
 * Locking: callers hold lddla->lock.
 */
#include <linux/kernel.h>

#include "en7572.h"

/* BOB calibration-table halves: eye 1 (A0) and eye 0 (A2). */
static u8 bob_a0(struct en7572_priv *priv, int off)
{
	return priv->lddla.bob[EN7572_BOB_A0(off)];
}

static u8 bob_a2(struct en7572_priv *priv, int off)
{
	return priv->lddla.bob[EN7572_BOB_A2(off)];
}

/* Program one calibrated Tx eye into the loop-control CSRs. */
static void en7572_load_eye(struct en7572_priv *priv,
			    u8 (*bob)(struct en7572_priv *, int), int tssi_off)
{
	en7572_bit_wr(priv, EN7572_RG_TX_CROSSING, 2, 3, 2);	/* BEN off */

	en7572_bit_wr(priv, EN7572_DCL_CTRL_2, 16, 27,
		      (bob(priv, EN7572_BOB_IMOD_CAL + 1) << 8) |
		      bob(priv, EN7572_BOB_IMOD_CAL));
	en7572_bit_wr(priv, EN7572_DCL_CTRL_2, 0, 12,
		      (bob(priv, EN7572_BOB_IAV_CAL + 1) << 8) |
		      bob(priv, EN7572_BOB_IAV_CAL));

	en7572_bit_wr(priv, EN7572_RG_APC_ANA_VMON_SEL, 8, 15,
		      bob(priv, EN7572_BOB_APC_CAL));
	en7572_bit_wr(priv, EN7572_RG_RESERVE_TIA, 0, 0,
		      bob(priv, EN7572_BOB_TIA_CUR));
	en7572_bit_wr(priv, EN7572_RG_IMPD_SINK, 8, 15,
		      bob(priv, EN7572_BOB_ERC_CDAC));
	en7572_bit_wr(priv, EN7572_RG_IMPD_SINK, 16, 27,
		      (bob(priv, EN7572_BOB_ERC_DAC + 1) << 8) |
		      bob(priv, EN7572_BOB_ERC_DAC));
	en7572_bit_wr(priv, EN7572_RG_RESERVE_TIA, 8, 13,
		      (bob(priv, EN7572_BOB_TIA_BW) << 3) |
		      bob(priv, EN7572_BOB_TIA_GAIN));
	en7572_bit_wr(priv, EN7572_RG_REP_PH_CAP_SEL, 16, 18,
		      bob(priv, EN7572_BOB_PGA_GAIN));
	en7572_bit_wr(priv, EN7572_RG_REP_PH_CAP_SEL, 5, 6,
		      bob(priv, EN7572_BOB_PGA_CAP));

	/* Refresh the firmware's Tx-power DDMI cal point for this eye. */
	en7572_wr(priv, EN7572_DEV_A2, EN7572_BOB_TSSI_CAL_1,
		  &priv->lddla.bob[tssi_off], 4);

	en7572_bit_wr(priv, EN7572_RG_LOOP_EN, 0, 0, 0);	/* restart loop */
	en7572_bit_wr(priv, EN7572_RG_LOOP_EN, 0, 0, 1);
	en7572_bit_wr(priv, EN7572_RG_TX_CROSSING, 2, 3, 0);	/* BEN normal */
}

/**
 * en7572_adaptive_pon() - load the Tx eye for a PON / Tx-mode change.
 * @priv: device
 * @mode: -1 report the active eye, 0 load eye 0 (A2 half), 1 load eye 1 (A0)
 *
 * Return: true once handled.
 */
bool en7572_adaptive_pon(struct en7572_priv *priv, int mode)
{
	if (mode == 0)
		en7572_load_eye(priv, bob_a2, EN7572_BOB_A2(EN7572_BOB_TSSI_CAL_1));
	else if (mode == 1)
		en7572_load_eye(priv, bob_a0, EN7572_BOB_A0(EN7572_BOB_TSSI_CAL_1));
	else
		dev_dbg(priv->lddla.dev, "adaptive PON: report (apc 0x%x)\n",
			en7572_bit_rd(priv, EN7572_RG_APC_ANA_VMON_SEL, 8, 15));

	return true;
}

/* BOSA temperature in degC = (SFF temp word / 256) - BOSA offset. */
static s16 en7572_bosa_temp(struct en7572_priv *priv)
{
	u16 w = (en7572_a2_byte(priv, EN7572_A2_DDMI_TEMP) << 8) |
		en7572_a2_byte(priv, EN7572_A2_DDMI_TEMP + 1);

	return ((s16)w >> 8) - en7572_a2_byte(priv, EN7572_A2_BOSA_TEMP_OFFSET);
}

/**
 * en7572_reduce_imod() - high-temperature modulation-current limiter.
 * @priv: device
 *
 * Watches the live Iav/Imod codes against their ceilings; once both the
 * temperature rises past the calibration point and a rail saturates, it caps
 * Imod (scaled down with temperature) and restores it when the module cools.
 * Self-gates on the reduce-Imod custom-function bit and reschedules itself.
 */
void en7572_reduce_imod(struct en7572_priv *priv)
{
	u16 iav_code, imod_code, iav_max, imod_max;
	s16 temperature;
	u32 delay_s = 5;

	if (priv->ri_wait_s) {
		priv->ri_wait_s--;
		return;
	}
	/* Custom-function bit: enable == 0, disable == 1. */
	if (en7572_bit_rd(priv, EN7572_A2_CUSTOM_FUNC, 0, 0) == 1) {
		priv->ri_wait_s = 5;
		return;
	}

	iav_code = en7572_bit_rd(priv, EN7572_CSR_IAV, 0, 12);
	imod_code = en7572_bit_rd(priv, EN7572_CSR_IBIAS_IMOD, 16, 27);
	iav_max = (en7572_a2_byte(priv, EN7572_A2_IAV_MAX) << 5) | 0x1e;
	imod_max = (en7572_a2_byte(priv, EN7572_A2_IMOD_MAX) << 4) | 0xf;
	temperature = en7572_bosa_temp(priv);

	if (priv->ri_first_boot) {
		u8 ht_th;

		if (en7572_bit_rd(priv, EN7572_RG_BEN_STATUS, 0, 0) == 0) {
			priv->ri_wait_s = 5;
			return;
		}
		priv->ri_first_boot = false;
		ht_th = en7572_a2_byte(priv, EN7572_A2_APC_RED_LMT);

		if (ht_th != 0xff) {
			priv->ri_temp_org = (s8)ht_th -
				en7572_a2_byte(priv, EN7572_A2_BOSA_TEMP_OFFSET);
		} else if (iav_max < 0x17fe && imod_max < 0xfff) {
			priv->ri_temp_org = 80;
		} else {
			priv->ri_wait_s = 5;
			return;
		}

		if (temperature > priv->ri_temp_org &&
		    (iav_code == iav_max || imod_code == imod_max)) {
			priv->ri_assert = true;
			priv->ri_imod_max_new = (imod_max > imod_code) ? imod_code
								       : imod_max;
			if (iav_max > iav_code)
				en7572_bit_wr(priv, EN7572_RG_IAV_LIMIT, 16, 28,
					      iav_code);
		}
		delay_s = 20;
	}

	if (priv->ri_assert) {
		u16 tune = priv->ri_imod_max_new -
			   en7572_a2_byte(priv, EN7572_A2_SLOPE_TIME) *
			   (temperature - priv->ri_temp_org);

		if (tune > priv->ri_imod_max_new)
			tune = priv->ri_imod_max_new;
		else if (tune < priv->ri_imod_max_new / 2)
			tune = priv->ri_imod_max_new / 2;
		en7572_bit_wr(priv, EN7572_RG_IMOD_MAX, 0, 11, tune);
	}

	if (temperature < priv->ri_temp_org && priv->ri_assert) {
		priv->ri_cnt2++;
		delay_s = 10;
	} else if (temperature >= priv->ri_temp_org && priv->ri_assert) {
		priv->ri_cnt2 = 0;
		delay_s = 20;
	} else if (iav_code < iav_max && imod_code < imod_max && !priv->ri_assert) {
		priv->ri_cnt1 = 0;
		delay_s = (iav_max - iav_code < 50 || imod_max - imod_code < 50) ? 5 : 30;
	} else if ((iav_code == iav_max || imod_code == imod_max) && !priv->ri_assert) {
		priv->ri_cnt1++;
		delay_s = 5;
	}

	if (priv->ri_cnt1 >= 4 && !priv->ri_assert) {		/* assert */
		priv->ri_assert = true;
		priv->ri_cnt1 = 0;
		priv->ri_cnt2 = 0;
		priv->ri_temp_org = temperature;
		priv->ri_imod_max_new = (imod_max > imod_code) ? imod_code : imod_max;
		if (iav_max > iav_code)
			en7572_bit_wr(priv, EN7572_RG_IAV_LIMIT, 16, 28, iav_code);
		priv->ri_wait_s = delay_s;
		return;
	}
	if (priv->ri_cnt2 >= 4 && priv->ri_assert) {		/* de-assert */
		priv->ri_assert = false;
		priv->ri_cnt1 = 0;
		priv->ri_cnt2 = 0;
		en7572_bit_wr(priv, EN7572_RG_IAV_LIMIT, 16, 28,
			      (en7572_a2_byte(priv, EN7572_A2_IAV_MAX) << 5) | 0x1e);
		en7572_bit_wr(priv, EN7572_RG_IMOD_MAX, 0, 11,
			      (en7572_a2_byte(priv, EN7572_A2_IMOD_MAX) << 4) | 0xf);
		priv->ri_wait_s = delay_s;
		return;
	}

	priv->ri_wait_s = delay_s;
}

/**
 * en7572_adaptive_pav() - trim average optical power under Imod saturation.
 * @priv: device
 *
 * When Imod is pinned at its ceiling the loop nudges the average-power (Pav)
 * set-point down (bounded by the APC reduction limit); once Imod has headroom
 * again for several cycles it nudges Pav back up toward the calibrated APC.
 * Self-gates on the adaptive-Pav custom-function bit and reschedules itself.
 */
void en7572_adaptive_pav(struct en7572_priv *priv)
{
	int apc, apc_cal, apc_red_lmt;
	u16 imod_code, imod_max;

	if (priv->ap_wait_s) {
		priv->ap_wait_s--;
		return;
	}
	if (en7572_bit_rd(priv, EN7572_A2_CUSTOM_FUNC, 1, 1) == 1) {
		priv->ap_wait_s = 10;
		return;
	}

	apc_cal = en7572_a2_byte(priv, EN7572_A2_APC_CAL);
	apc = en7572_bit_rd(priv, EN7572_RG_APC_ANA_VMON_SEL, 8, 15);
	apc_red_lmt = en7572_a2_byte(priv, EN7572_A2_APC_RED_LMT);
	imod_code = en7572_bit_rd(priv, EN7572_CSR_IBIAS_IMOD, 16, 27);
	imod_max = (en7572_a2_byte(priv, EN7572_A2_IMOD_MAX) << 4) | 0xf;

	if (apc == apc_cal)
		priv->ap_assert = false;

	if (en7572_bit_rd(priv, EN7572_RG_BEN_STATUS, 0, 0) == 0) {
		priv->ap_wait_s = 1;
		return;
	}

	if (imod_code == imod_max) {			/* reduce Pav */
		priv->ap_assert = true;
		priv->ap_inc_cnt = 0;
		if (apc > apc_cal - apc_red_lmt)
			en7572_byte_wr(priv, EN7572_DEV_A2, EN7572_A2_GPL, 0x1f);
	} else if (imod_code < imod_max && priv->ap_assert) {	/* restore Pav */
		if (++priv->ap_inc_cnt >= 5) {
			priv->ap_inc_cnt = 5;
			if (apc < apc_cal)
				en7572_byte_wr(priv, EN7572_DEV_A2,
					       EN7572_A2_GPL, 0x11);
		}
	}

	priv->ap_wait_s = en7572_a2_byte(priv, EN7572_A2_SLOPE_TIME);
}
