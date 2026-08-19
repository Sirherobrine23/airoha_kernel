// SPDX-License-Identifier: GPL-2.0
/*
 * EN7571 transmitter and receiver subsystems.
 *
 * Transmitter / power control: the laser power is held by a dual-closed-loop
 * (DCL) power-control block (PWR_CTRL_0..E).  This brings that loop up
 * (en7571_reg_init), loads the Iav/Imod/Pav/P1 set-points from flash, runs in
 * force mode, generates the GPON/EPON burst timing (TGEN), maintains the APD
 * reverse-bias DAC, clears the safe-circuit / rogue-ONU latches and evaluates
 * the Tx alarms.
 *
 * Receiver: RSSI front end (calibration, gain init, auto-ranging current read,
 * dark-current sum), LOS/SD detection and the TIA signal-detect threshold.
 * RSSI uses a hardware accumulator (1024 samples) gated by RG_ADLCH_CTRL, with
 * a six-step gain sweep to keep the converter above its noise floor.
 *
 * Locking: callers hold priv->lddla.lock.
 */
#include <linux/delay.h>
#include <linux/kernel.h>
#include <linux/math64.h>

#include "en7571.h"

static void en7571_alarm_set(struct en7571_priv *priv, u32 bit, bool on)
{
	if (on)
		priv->lddla.alarm |= bit;
	else
		priv->lddla.alarm &= ~bit;
}

/* ------------------------------------------------------------------ */
/* Transmitter / power control					      */
/* ------------------------------------------------------------------ */

/* --- Mode / reset primitives --- */

/* Enter "7571" power-control mode (PWR_CTRL_0 byte 0 rg_pwr_ctrl_en). */
void en7571_7571_enable(struct en7571_priv *priv)
{
	lddla_update8(&priv->lddla, EN7571_PWR_CTRL_0, EN7571_PWR_CTRL_EN_MASK,
		       EN7571_PWR_CTRL_EN);
}

/* Pulse the Tx-fault / safe-protect reset (SAFE_PROTECT byte 1). */
void en7571_safe_reset(struct en7571_priv *priv)
{
	lddla_update8(&priv->lddla, EN7571_SAFE_PROTECT + 1, EN7571_SAFE_CIRCUIT_MASK,
		       EN7571_SAFE_CIRCUIT_RESET);
}

/* Run / halt the dual-closed loop (PWR_CTRL_0 byte 1 rg_pwr_ctrl_rst_b). */
void en7571_dcl_start(struct en7571_priv *priv)
{
	lddla_update8(&priv->lddla, EN7571_PWR_CTRL_0 + 1, EN7571_DCL_RST_B_MASK,
		       EN7571_DCL_RST_B);
}

void en7571_dcl_stop(struct en7571_priv *priv)
{
	lddla_update8(&priv->lddla, EN7571_PWR_CTRL_0 + 1, EN7571_DCL_RST_B_MASK, 0);
}

/* Force Pav/P1 from the cal registers (PWR_CTRL_C byte 3). */
void en7571_force_mode(struct en7571_priv *priv)
{
	lddla_update8(&priv->lddla, EN7571_PWR_CTRL_C + 3, EN7571_PAV_P1_FORCE_MASK,
		       EN7571_PAV_P1_FORCE);
}

/* Let the loop drive Pav/P1 (PWR_CTRL_C byte 3). */
void en7571_auto_lock_mode(struct en7571_priv *priv)
{
	lddla_update8(&priv->lddla, EN7571_PWR_CTRL_C + 3, EN7571_PAV_P1_FORCE_MASK, 0);
}

/*
 * Enable / freeze the MPDH step sizes (rev-2 link control).  ENABLE writes the
 * configured steps; DISABLE clears the step field so MPDH stays frozen.
 */
void en7571_mpdh_stepsize(struct en7571_priv *priv, bool enable)
{
	if (enable) {
		lddla_update8(&priv->lddla, EN7571_PWR_CTRL_3 + 2, EN7571_STEPSIZE_MASK,
			       EN7571_STEPSIZE1);
		lddla_update8(&priv->lddla, EN7571_PWR_CTRL_4 + 2, EN7571_STEPSIZE_MASK,
			       EN7571_STEPSIZE5);
		lddla_update8(&priv->lddla, EN7571_PWR_CTRL_4, EN7571_STEPSIZE_MASK,
			       EN7571_STEPSIZE3);
	} else {
		lddla_update8(&priv->lddla, EN7571_PWR_CTRL_3 + 2, EN7571_STEPSIZE_MASK, 0);
		lddla_update8(&priv->lddla, EN7571_PWR_CTRL_4 + 2, EN7571_STEPSIZE_MASK, 0);
		lddla_update8(&priv->lddla, EN7571_PWR_CTRL_4, EN7571_STEPSIZE_MASK, 0);
	}
}

/* Burst-control gate (DUMMY byte 1): assert burst-control-off. */
void en7571_burst_ctrl(struct en7571_priv *priv)
{
	lddla_update8(&priv->lddla, EN7571_DUMMY + 1, EN7571_BURST_CTRL_MASK,
		       EN7571_BURST_CTRL_OFF);
}

/*
 * Link-register setup.  On rev-2 silicon, enabling the link disables the MPDH
 * step (and vice-versa), asserts burst-control and sets the HW KT select;
 * earlier silicon only toggles the HW KT select.
 */
void en7571_link_reg(struct en7571_priv *priv, bool enable)
{
	if (priv->ver == 2) {
		/*
		 * Match xpon_en757x/v1: rev-2 controls MPDH, TGEN and the
		 * burst gate here, but leaves the HW-KT select cleared.
		 * Temperature compensation is handled by the software KT loop.
		 */
		en7571_mpdh_stepsize(priv, !enable);
		en7571_t1delay_setting(priv, !enable);
		en7571_burst_ctrl(priv);
	} else {
		en7571_hwkt(priv, enable);
		en7571_t1delay_setting(priv, !enable);
	}
}

/*
 * CDR lock control lives in the companion PHY core layer (PHY CSRs reached
 * through IO_GPHYREG / IO_SPHYREG), outside this device's register file.  It
 * is stubbed so the bring-up flow stays intact; on real silicon the PHY init
 * layer drives the line-rate, polarity and the PRBS calibration pattern.
 */
void en7571_cdr(struct en7571_priv *priv, bool enable)
{
	dev_dbg(priv->lddla.dev, "CDR %s (PHY-layer)\n",
		enable ? "lock-to-data" : "lock-to-ref");
}

/* --- Power-loop register init --- */

void en7571_reg_init(struct en7571_priv *priv)
{
	u8 b[4];

	/* TIA default: clear the TIA gain / mux bits in TIAMUX byte 1. */
	lddla_update8(&priv->lddla, EN7571_TIAMUX + 1, ~0xce & 0xff, 0);

	/*
	 * The v1 EN7571 driver does not source these limiter registers from
	 * calibration offsets 0x070/0x074.  It always programs the production
	 * defaults into the minimum-current field while preserving the other
	 * lanes.  Loading the complete flash words here can clear both limits
	 * and clamp the live Ibias/Imod readbacks to zero.
	 */
	lddla_rd(&priv->lddla, EN7571_PWR_LIMITER_0, b, 4);
	b[3] = (b[3] & 0xf0) | ((EN7571_DA_IBIAS_MIN >> 8) & 0x0f);
	b[2] = EN7571_DA_IBIAS_MIN & 0xff;
	lddla_wr(&priv->lddla, EN7571_PWR_LIMITER_0, b, 4);

	lddla_rd(&priv->lddla, EN7571_PWR_LIMITER_2, b, 4);
	b[3] = (b[3] & 0xf0) | ((EN7571_DA_IMOD_MIN >> 8) & 0x0f);
	b[2] = EN7571_DA_IMOD_MIN & 0xff;
	lddla_wr(&priv->lddla, EN7571_PWR_LIMITER_2, b, 4);

	/* Imax = 0x0fff0000 in both PWR_CTRL_6 and PWR_CTRL_7. */
	b[0] = 0; b[1] = 0; b[2] = EN7571_IMAX & 0xff; b[3] = (EN7571_IMAX >> 8) & 0x0f;
	lddla_wr(&priv->lddla, EN7571_PWR_CTRL_6, b, 4);
	lddla_wr(&priv->lddla, EN7571_PWR_CTRL_7, b, 4);

	/* Initial Iav (bytes 0-1) / Imod (bytes 2-3), preserving top nibbles. */
	lddla_rd(&priv->lddla, EN7571_PWR_CTRL_9, b, 4);
	b[3] = (b[3] & 0xf0) | ((EN7571_CMD_IMOD_SETTING >> 8) & 0x0f);
	b[2] = EN7571_CMD_IMOD_SETTING & 0xff;
	b[1] = (b[1] & 0xf0) | ((EN7571_CMD_IAV_SETTING >> 8) & 0x0f);
	b[0] = EN7571_CMD_IAV_SETTING & 0xff;
	lddla_wr(&priv->lddla, EN7571_PWR_CTRL_9, b, 4);

	/* step-mu select + Pav/MPDH step sizes 0-2 (preserve masked bits). */
	lddla_rd(&priv->lddla, EN7571_PWR_CTRL_3, b, 4);
	b[0] = (b[0] & EN7571_STEPMU_SEL_MASK) | EN7571_STEPMU_SEL;
	b[3] = (b[3] & EN7571_STEPSIZE_MASK) | EN7571_STEPSIZE2;
	b[2] = (b[2] & EN7571_STEPSIZE_MASK) | EN7571_STEPSIZE1;
	b[1] = (b[1] & EN7571_STEPSIZE_MASK) | EN7571_STEPSIZE0;
	lddla_wr(&priv->lddla, EN7571_PWR_CTRL_3, b, 4);

	/* step sizes 3-5 (byte 3 untouched). */
	lddla_rd(&priv->lddla, EN7571_PWR_CTRL_4, b, 4);
	b[2] = (b[2] & EN7571_STEPSIZE_MASK) | EN7571_STEPSIZE5;
	b[1] = (b[1] & EN7571_STEPSIZE_MASK) | EN7571_STEPSIZE4;
	b[0] = (b[0] & EN7571_STEPSIZE_MASK) | EN7571_STEPSIZE3;
	lddla_wr(&priv->lddla, EN7571_PWR_CTRL_4, b, 4);

	/* Iav/Imod variation + MPDH updatePd (byte 2 untouched). */
	lddla_rd(&priv->lddla, EN7571_PWR_CTRL_8, b, 4);
	b[3] = (b[3] & EN7571_MPDH_UPDATEPD_MASK) | EN7571_MPDH_UPDATEPD;
	b[1] = EN7571_DELTA_IMOD_MAX;
	b[0] = EN7571_DELTA_IAV_MAX;
	lddla_wr(&priv->lddla, EN7571_PWR_CTRL_8, b, 4);

	/* Force-mode loop (auto-lock is not used). */
	en7571_force_mode(priv);

	/* Phase-1 timer (bytes 0-1, preserve top 6 bits of byte 1). */
	lddla_rd(&priv->lddla, EN7571_PWR_CTRL_5, b, 4);
	b[1] = (b[1] & 0xfc) | ((EN7571_TIMER_PHZ1_NUM >> 8) & 0x03);
	b[0] = EN7571_TIMER_PHZ1_NUM & 0xff;
	lddla_wr(&priv->lddla, EN7571_PWR_CTRL_5, b, 4);

	/* MPDH P1 step size (byte 3) and MPDx average shift (byte 0). */
	lddla_update8(&priv->lddla, EN7571_PWR_CTRL_1 + 3, EN7571_P1_STEPSIZE_MASK,
		       EN7571_P1_STEPSIZE);
	lddla_update8(&priv->lddla, EN7571_PWR_CTRL_1, EN7571_MPDX_SHTBIT_MASK,
		       EN7571_MPDX_SHTBIT);

	/* PWRADC averaging (byte 3). */
	lddla_update8(&priv->lddla, EN7571_PWR_CTRL_E + 3, EN7571_PAVG_SHTBIT_MASK,
		       EN7571_PAVG_SHTBIT_64);

	/* Adjustment intervals: P1 (byte 3) and Pav (byte 2) = 0. */
	lddla_wr8(&priv->lddla, EN7571_PWR_CTRL_B + 3, 0);
	lddla_wr8(&priv->lddla, EN7571_PWR_CTRL_B + 2, 0);

	/* Disable temperature-compensation scale (PWR_CTRL_A byte 2). */
	lddla_update8(&priv->lddla, EN7571_PWR_CTRL_A + 2, EN7571_IMOD_IAVSCALE_MASK, 0);

	/* Clear the HW KT Imod-adjust select (PWR_CTRL_2 byte 0). */
	lddla_update8(&priv->lddla, EN7571_PWR_CTRL_2, EN7571_IMOD_ADJ_SEL_MASK, 0);
}

/* --- Load calibrated set-points from flash --- */

void en7571_load_tx_cal_data(struct en7571_priv *priv)
{
	u32 w;
	u8 b[4];

	w = lddla_flash_read(&priv->lddla, EN7571_FL_IAV_IMOD);
	if (w != EN7571_FLASH_ERASED) {
		u32 iav = (w & EN7571_FL_IAV_MASK) >> 16;
		u32 imod = w & EN7571_FL_IMOD_MASK;

		b[0] = iav & 0xff;
		b[1] = (iav >> 8) & 0x0f;
		b[2] = imod & 0xff;
		b[3] = (imod >> 8) & 0x0f;
		lddla_wr(&priv->lddla, EN7571_PWR_CTRL_9, b, 4);
		lddla_wr(&priv->lddla, EN7571_PWR_CTRL_FLASH_1, b, 4);
		dev_dbg(priv->lddla.dev, "initial current loaded (Iav 0x%03x Imod 0x%03x)\n",
			iav, imod);
	}

	w = lddla_flash_read(&priv->lddla, EN7571_FL_PAV_P1);
	if (w != EN7571_FLASH_ERASED) {
		u32 pav = (w & EN7571_FL_PAV_MASK) >> 16;
		u32 p1 = w & EN7571_FL_P1_MASK;

		/* PWR_CTRL_D: Pav in bytes 0-1, P1 in bytes 2-3. */
		b[0] = pav & 0xff;
		b[1] = (pav >> 8) & 0x0f;
		b[2] = p1 & 0xff;
		b[3] = (p1 >> 8) & 0x03;
		lddla_wr(&priv->lddla, EN7571_PWR_CTRL_D, b, 4);

		/* PWR_CTRL_FLASH_2: byte-swapped (P1 in bytes 0-1, Pav in 2-3). */
		b[0] = p1 & 0xff;
		b[1] = (p1 >> 8) & 0x03;
		b[2] = pav & 0xff;
		b[3] = (pav >> 8) & 0x0f;
		lddla_wr(&priv->lddla, EN7571_PWR_CTRL_FLASH_2, b, 4);
		dev_dbg(priv->lddla.dev, "PWR/ER loaded (Pav 0x%03x P1 0x%03x)\n", pav, p1);
	}

	/* Open-loop Ibias/Imod seed (flash 0x008) -> P0/P1 coarse DACs. */
	w = lddla_flash_read(&priv->lddla, EN7571_FL_OPEN_LOOP_SEED);
	if (w != EN7571_FLASH_ERASED) {
		u32 ibias = (w & EN7571_FL_IAV_MASK) >> 16;
		u32 imod = w & EN7571_FL_IMOD_MASK;

		b[0] = ibias & 0xff;
		b[1] = (ibias >> 8) & 0x0f;
		lddla_wr(&priv->lddla, EN7571_P0_PWR_CTRL_CS2, b, 2);
		b[0] = imod & 0xff;
		b[1] = (imod >> 8) & 0x0f;
		lddla_wr(&priv->lddla, EN7571_P1_PWR_CTRL_CS2, b, 2);
	}

	/* TIA gain (bits 14-15) / bandwidth (bits 9-11) from flash 0x048. */
	w = lddla_flash_read(&priv->lddla, EN7571_FL_TIA_SETTING);
	if (w != EN7571_FLASH_ERASED)
		lddla_update8(&priv->lddla, EN7571_TIAMUX + 1, ~0xce & 0xff,
			       (((w >> 16) & 0x3) << 6) | ((w & 0x7) << 1));
}

/* --- Loop set-point readback --- */

u32 en7571_info(struct en7571_priv *priv, u8 select)
{
	/*
	 * The live "NOW" readbacks momentarily freeze the closed loop by
	 * zeroing the Iav/Imod variation in PWR_CTRL_8 bytes 0-1, take the
	 * sample, then restore the variation limits.
	 */
	static const u8 freeze[2] = { 0, 0 };
	static const u8 thaw[2] = { EN7571_DELTA_IAV_MAX, EN7571_DELTA_IMOD_MAX };
	u8 b[4] = { 0 };

	switch (select) {
	case EN7571_SELECT_PAV_CAL:
		lddla_rd(&priv->lddla, EN7571_PWR_CTRL_D, b, 4);
		priv->pav_cal = (b[0] | (b[1] << 8)) & 0xfff;
		return priv->pav_cal;
	case EN7571_SELECT_P1_CAL:
		lddla_rd(&priv->lddla, EN7571_PWR_CTRL_D, b, 4);
		priv->p1_cal = (b[2] | (b[3] << 8)) & 0x3ff;
		return priv->p1_cal;
	case EN7571_SELECT_IAV_NOW:
		lddla_wr(&priv->lddla, EN7571_PWR_CTRL_8, freeze, 2);
		lddla_rd(&priv->lddla, EN7571_RO_PWR_CTRL_3, b, 4);
		lddla_wr(&priv->lddla, EN7571_PWR_CTRL_8, thaw, 2);
		priv->iav_now = (b[0] | (b[1] << 8)) & 0xfff;
		return priv->iav_now;
	case EN7571_SELECT_IBIAS_NOW:
		lddla_wr(&priv->lddla, EN7571_PWR_CTRL_8, freeze, 2);
		lddla_rd(&priv->lddla, EN7571_P0_PWR_CTRL_CS3, b, 4);
		lddla_wr(&priv->lddla, EN7571_PWR_CTRL_8, thaw, 2);
		priv->ibias_now = (b[2] | (b[3] << 8)) & 0xfff;
		return priv->ibias_now;
	case EN7571_SELECT_IMOD_NOW:
		lddla_wr(&priv->lddla, EN7571_PWR_CTRL_8, freeze, 2);
		lddla_rd(&priv->lddla, EN7571_P1_PWR_CTRL_CS3, b, 4);
		lddla_wr(&priv->lddla, EN7571_PWR_CTRL_8, thaw, 2);
		priv->imod_now = (b[2] | (b[3] << 8)) & 0xfff;
		return priv->imod_now;
	case EN7571_SELECT_P1_NOW:
		lddla_rd(&priv->lddla, EN7571_RO_PWR_CTRL_0, b, 4);
		return (b[2] | (b[3] << 8)) & 0x3ff;
	default:
		return 0;
	}
}

/* --- Burst timing generation --- */

/* Program the per-mode T0/T1 burst delay (T1DELAY byte 0). */
void en7571_set_t0t1_delay(struct en7571_priv *priv, u8 delay)
{
	lddla_wr8(&priv->lddla, EN7571_T1DELAY, delay);
}

/*
 * Link-state T1 delay: enable loads the per-mode burst delay; disable clears
 * the T0 delay nibble back to normal (keeping the T1 nibble).
 */
void en7571_t1delay_setting(struct en7571_priv *priv, bool enable)
{
	if (enable)
		en7571_set_t0t1_delay(priv,
				      priv->lddla.pon_mode == EN7571_PON_GPON ?
				      EN7571_T1_T0_DELAY_GPON : EN7571_T1_T0_DELAY_EPON);
	else
		lddla_update8(&priv->lddla, EN7571_T1DELAY, EN7571_T1_DELAY_MASK,
			       EN7571_T1_DELAY_NORMAL);
}

/*
 * Recall the burst T0C/T1C timing from flash and re-arm the burst generator.
 * The per-burst maximum search is not run at boot; the calibrated T0C/T1C
 * limits captured at production time are reloaded instead.
 */
void en7571_tgen_recall(struct en7571_priv *priv)
{
	u32 t0ct1c = lddla_flash_read(&priv->lddla, EN7571_FL_T0CT1C);
	u8 t0c = 0, t1c = 0;

	if (t0ct1c != EN7571_FLASH_ERASED) {
		t0c = (t0ct1c >> 16) & 0xff;
		t1c = (t0ct1c >> 24) & 0xff;
	}

	lddla_wr8(&priv->lddla, EN7571_T1DELAY + 1, t1c);
	lddla_wr8(&priv->lddla, EN7571_T1DELAY + 2, t0c);
	mdelay(10);

	/* Pulse the T1/T0 timer reset (T1DELAY byte 3 bit 5). */
	lddla_update8(&priv->lddla, EN7571_T1DELAY + 3, EN7571_TGEN_RESET_MASK,
		       EN7571_TGEN_RESET_T1T0);
	mdelay(10);
	lddla_update8(&priv->lddla, EN7571_T1DELAY + 3, EN7571_TGEN_RESET_MASK, 0);
	mdelay(10);

	/* Enable ERC. */
	lddla_update8(&priv->lddla, EN7571_T1DELAY + 3, EN7571_ERC_ENABLE_MASK,
		       EN7571_ERC_ENABLE);
}

/* --- APD reverse-bias control --- */

void en7571_apd_init(struct en7571_priv *priv)
{
	lddla_update8(&priv->lddla, EN7571_APD_DAC_CODE + 2, ~EN7571_APD_SOFTSTART_ENABLE & 0xff,
		       EN7571_APD_SOFTSTART_ENABLE);
	lddla_update8(&priv->lddla, EN7571_APD_DAC_CODE + 1, ~EN7571_APD_CONTROL_ENABLE & 0xff,
		       EN7571_APD_CONTROL_ENABLE);
}

void en7571_apd_write(struct en7571_priv *priv, u8 code)
{
	lddla_wr8(&priv->lddla, EN7571_APD_DAC_CODE, code);
}

/* Convert an APD voltage (mV) to a DAC code via the 4-point piecewise fit. */
static int en7571_apd_voltage_to_code(struct en7571_priv *priv, s32 v_mv)
{
	u32 w1 = lddla_flash_read(&priv->lddla, EN7571_FL_APD_VOLTAGE_1);
	u32 w2 = lddla_flash_read(&priv->lddla, EN7571_FL_APD_VOLTAGE_2);
	s32 v00 = EN7571_APD_V00_MV_DEF, v40 = EN7571_APD_V40_MV_DEF;
	s32 v80 = EN7571_APD_V80_MV_DEF, vc0 = EN7571_APD_VC0_MV_DEF;
	s32 step, code;

	/* Flash anchors are stored as volts x10; scale to mV. */
	if (w1 != EN7571_FLASH_ERASED) {
		v00 = (s32)(w1 >> 16) * 100;
		v40 = (s32)(w1 & 0xffff) * 100;
	}
	if (w2 != EN7571_FLASH_ERASED) {
		v80 = (s32)(w2 >> 16) * 100;
		vc0 = (s32)(w2 & 0xffff) * 100;
	}

	if (!(v40 > v00 && v80 > v40 && vc0 > v80)) {
		dev_warn(priv->lddla.dev, "APD slope error; not writing DAC\n");
		return -EINVAL;
	}

	if (v_mv < v80) {
		if (v_mv < v40) {
			step = (v40 - v00) / 64;
			code = 0x00 + (step ? (v_mv - v00) / step : 0);
		} else {
			step = (v80 - v40) / 64;
			code = 0x40 + (step ? (v_mv - v40) / step : 0);
		}
	} else {
		step = (vc0 - v80) / 64;
		if (v_mv < vc0)
			code = 0x80 + (step ? (v_mv - v80) / step : 0);
		else
			code = 0xc0 + (step ? (v_mv - vc0) / step : 0);
	}

	return clamp(code, 0, 0xff);
}

/* Recompute and program the APD voltage from the BOSA temperature. */
void en7571_apd_control(struct en7571_priv *priv)
{
	s32 v_mv, contrib;
	int code;

	if (priv->bosa_temp_mc > EN7571_APD_KNEE_TEMP_MC) {
		contrib = div_s64((s64)priv->apd_slope_up_uv *
				  (priv->bosa_temp_mc - EN7571_APD_KNEE_TEMP_MC),
				  1000000);
		v_mv = priv->apd_knee_mv + contrib;
	} else {
		contrib = div_s64((s64)priv->apd_slope_dn_uv *
				  (EN7571_APD_KNEE_TEMP_MC - priv->bosa_temp_mc),
				  1000000);
		v_mv = priv->apd_knee_mv - contrib;
	}

	code = en7571_apd_voltage_to_code(priv, v_mv);
	if (code < 0)
		return;

	priv->apd_voltage_mv = v_mv;
	en7571_apd_write(priv, code);
}

/* Clear the rogue-ONU latch (ROGUE_ONU_DET_CTRL+1 byte 0). */
void en7571_rogue_clear(struct en7571_priv *priv)
{
	lddla_update8(&priv->lddla, EN7571_ROGUE_ONU_DET_CTRL + 1, EN7571_ROGUE_ONU_MASK,
		       EN7571_ROGUE_ONU_CLEAR);
}

/* Compare cached Tx-power / bias DDMI words against the alarm thresholds. */
void en7571_tx_alarms(struct en7571_priv *priv)
{
	u16 p = priv->lddla.ddmi_tx_power;
	u16 b = priv->lddla.ddmi_current;

	en7571_alarm_set(priv, AIROHA_ALARM_TX_LOW_POWER, p < EN7571_TX_PWR_LOW_THLD);
	en7571_alarm_set(priv, AIROHA_ALARM_TX_HIGH_POWER, p > EN7571_TX_PWR_HIGH_THLD);
	en7571_alarm_set(priv, AIROHA_ALARM_TX_LOW_BIAS, b < EN7571_TX_BIAS_LOW_THLD);
	en7571_alarm_set(priv, AIROHA_ALARM_TX_HIGH_BIAS, b > EN7571_TX_BIAS_HIGH_THLD);
}

/* ------------------------------------------------------------------ */
/* Receiver							      */
/* ------------------------------------------------------------------ */

/* RSSI front-end gain table: LA_PWD byte 2 [2:0] -> multiplicative factor. */
static const u32 en7571_rssi_gain_factor[6] = { 1, 4, 16, 64, 128, 256 };

/* Default the RSSI front-end to the largest gain (LA_PWD byte 2 [2:0] = 5). */
void en7571_rssi_gain_init(struct en7571_priv *priv)
{
	lddla_update8(&priv->lddla, EN7571_LA_PWD + 2, EN7571_RSSI_GAIN_MASK,
		       EN7571_RSSI_GAIN_DEFAULT);
}

/**
 * en7571_rssi_cal() - capture the RSSI reference / zero levels.
 * @priv: device
 *
 * Vref is read with RSSI "V mode" enabled, V with it disabled, both with the
 * RSSI calibration path on.
 */
int en7571_rssi_cal(struct en7571_priv *priv)
{
	s32 term_uv;

	/* RSSI calibration on (LA_PWD byte 1 bit 4). */
	lddla_update8(&priv->lddla, EN7571_LA_PWD + 1, EN7571_RSSI_CAL_MASK,
		       EN7571_RSSI_CAL_EN);
	/* RSSI V mode on (byte 1 bit 6). */
	lddla_update8(&priv->lddla, EN7571_LA_PWD + 1, EN7571_RSSI_VMODE_MASK,
		       EN7571_RSSI_VMODE_EN);
	/* Route the ADC to RSSI. */
	lddla_update8(&priv->lddla, EN7571_SVADC_PD, EN7571_ADC_SELECT_MASK,
		       EN7571_ADC_RSSI_ENABLE);

	priv->rssi_vref = en7571_svadc_get(priv);

	/* RSSI V mode off -> V. */
	lddla_update8(&priv->lddla, EN7571_LA_PWD + 1, EN7571_RSSI_VMODE_MASK, 0);
	priv->rssi_v = en7571_svadc_get(priv);

	/* RSSI calibration off, restore the SVADC mux. */
	lddla_update8(&priv->lddla, EN7571_LA_PWD + 1, EN7571_RSSI_CAL_MASK, 0);
	lddla_update8(&priv->lddla, EN7571_SVADC_PD, EN7571_ADC_SELECT_MASK, 0);

	if (priv->rssi_v <= priv->rssi_vref) {
		dev_warn(priv->lddla.dev, "RSSI calibration fail (V <= Vref)\n");
		return 0;
	}

	/* Diagnostic-only factor against the ideal IR (0.35). */
	term_uv = en7571_adc_code_to_uv(priv, priv->rssi_v - priv->rssi_vref);
	priv->rssi_factor = div_s64((s64)term_uv * 100, EN7571_RSSI_IDEAL_IR_X100);
	return 0;
}

/**
 * en7571_rssi_current() - auto-ranging RSSI current read.
 * @priv: device
 *
 * Accumulates 1024 samples in hardware and sweeps the front-end gain from the
 * largest down, breaking once the averaged voltage falls more than the
 * noise-defend margin below Vref, then scales by the surviving gain factor.
 */
s32 en7571_rssi_current(struct en7571_priv *priv)
{
	u32 voltage = 0, factor = 1;
	int g;

	/* Route the ADC to RSSI and arm the 1024-sample accumulator. */
	lddla_update8(&priv->lddla, EN7571_SVADC_PD, EN7571_ADC_SELECT_MASK,
		       EN7571_ADC_RSSI_ENABLE);
	lddla_update8(&priv->lddla, EN7571_RG_ADLCH_CTRL + 3, EN7571_ADLCH_COUNT_MASK,
		       EN7571_ADLCH_COUNT_1024);

	for (g = 5; g >= 0; g--) {
		u8 b[4] = { 0 };
		u32 sum;

		lddla_update8(&priv->lddla, EN7571_LA_PWD + 2, EN7571_RSSI_GAIN_MASK, g & 0x07);
		/* Trigger the accumulate latch (byte 3 bit 7). */
		lddla_update8(&priv->lddla, EN7571_RG_ADLCH_CTRL + 3, 0xff, EN7571_ADLCH_TRIG);
		msleep(20);

		lddla_rd(&priv->lddla, EN7571_RG_ADLCH_CTRL, b, 4);
		if (!(b[2] >> 7))		/* accumulate valid bit */
			continue;
		sum = ((b[2] << 16) | (b[1] << 8) | b[0]) & EN7571_ADLCH_SUM_MASK;
		voltage = sum >> 10;		/* average over 1024 samples */
		factor = en7571_rssi_gain_factor[g];

		if ((s32)voltage < priv->rssi_vref - EN7571_ADC_RSSI_DEFEND_NOISE)
			break;
	}

	if ((s32)voltage < EN7571_ADC_0V5)
		dev_dbg(priv->lddla.dev, "RSSI below 0.5 V, reading not precise\n");

	/* Restore the accumulate default and the largest gain, release the mux. */
	lddla_update8(&priv->lddla, EN7571_RG_ADLCH_CTRL + 3, EN7571_ADLCH_COUNT_MASK, 0);
	lddla_update8(&priv->lddla, EN7571_LA_PWD + 2, EN7571_RSSI_GAIN_MASK,
		       EN7571_RSSI_GAIN_DEFAULT);
	lddla_update8(&priv->lddla, EN7571_SVADC_PD, EN7571_ADC_SELECT_MASK, 0);

	if (priv->rssi_vref >= (s32)voltage)
		priv->rssi_current = (priv->rssi_vref - (s32)voltage) * factor;
	else
		priv->rssi_current = 0;

	return priv->rssi_current;
}

/* Sum 64 raw RSSI samples with the gain pinned to 0 (APD breakdown search). */
u32 en7571_dark_current(struct en7571_priv *priv)
{
	u32 sum = 0;
	int i;

	lddla_update8(&priv->lddla, EN7571_LA_PWD + 2, EN7571_RSSI_GAIN_MASK, 0);
	lddla_update8(&priv->lddla, EN7571_SVADC_PD, EN7571_ADC_SELECT_MASK,
		       EN7571_ADC_RSSI_ENABLE);
	for (i = 0; i < 64; i++)
		sum += en7571_svadc_get(priv);
	lddla_update8(&priv->lddla, EN7571_SVADC_PD, EN7571_ADC_SELECT_MASK, 0);
	lddla_update8(&priv->lddla, EN7571_LA_PWD + 2, EN7571_RSSI_GAIN_MASK,
		       EN7571_RSSI_GAIN_DEFAULT);
	return sum;
}

/* LOS calibration init sequence. */
void en7571_los_init(struct en7571_priv *priv)
{
	/* LOS_CTRL1: cal trigger (byte 0) and analog-input stable counter (byte 1). */
	lddla_update8(&priv->lddla, EN7571_LOS_CTRL1, EN7571_LOS_CAL_TRIG_MASK,
		       EN7571_LOS_CAL_TRIG);
	lddla_update8(&priv->lddla, EN7571_LOS_CTRL1 + 1, EN7571_LOS_AIN_STABLE_MASK,
		       EN7571_LOS_AIN_STABLE_SET);

	/* LOS ADC revision enables (SVADC_PD byte 3 = REV2, byte 2 = REV1). */
	lddla_update8(&priv->lddla, EN7571_SVADC_PD + 3, EN7571_LOS_ADCREV2_MASK,
		       EN7571_LOS_ADCREV2_ENABLE);
	lddla_update8(&priv->lddla, EN7571_SVADC_PD + 2, EN7571_LOS_ADCREV1_MASK,
		       EN7571_LOS_ADCREV1_ENABLE);

	/* LOS_CTRL2: confidence (byte 1) and LOS/SD counter (byte 0). */
	lddla_update8(&priv->lddla, EN7571_LOS_CTRL2 + 1, EN7571_LOS_CONFIDENCE_MASK,
		       EN7571_LOS_CONFIDENCE_SET);
	lddla_update8(&priv->lddla, EN7571_LOS_CTRL2, EN7571_LOS_CNT_MASK,
		       EN7571_LOS_CNT_SET);
}

/* Master LOS init: calibrate, then load the Rx-LOS / Rx-SD thresholds. */
void en7571_los_level(struct en7571_priv *priv)
{
	u32 thld = lddla_flash_read(&priv->lddla, EN7571_FL_LOS_THLD);
	u8 rx_los, rx_sd;

	en7571_los_init(priv);

	if (thld != EN7571_FLASH_ERASED) {
		rx_los = thld & EN7571_FL_RX_LOS_MASK;
		rx_sd = (thld & EN7571_FL_RX_SD_MASK) >> 16;
	} else {
		rx_los = EN7571_LOS_COMP_THLD_L_DEF;
		rx_sd = EN7571_LOS_COMP_THLD_H_DEF;
	}

	/* LOS_CTRL1 byte 3 = Rx LOS, byte 2 = Rx SD (both keep bit 7). */
	lddla_update8(&priv->lddla, EN7571_LOS_CTRL1 + 3, EN7571_LOS_COMP_THLD_MASK,
		       rx_los & 0x7f);
	lddla_update8(&priv->lddla, EN7571_LOS_CTRL1 + 2, EN7571_LOS_COMP_THLD_MASK,
		       rx_sd & 0x7f);
}

/* Trigger one PWRADC conversion and return the 10-bit instantaneous value. */
static u16 en7571_pwradc_data10(struct en7571_priv *priv)
{
	u16 v = 0;

	lddla_update8(&priv->lddla, EN7571_RG_PWRADC_DATA2 + 3, 0xff, EN7571_PWRADC_TRIG);
	lddla_rd16(&priv->lddla, EN7571_RG_PWRADC_DATA, &v);
	return v & 0x3ff;
}

/* Average eight 10-bit PWRADC samples (round to nearest). */
static s32 en7571_pwradc_avg8(struct en7571_priv *priv)
{
	u32 sum = 0;
	int i;

	for (i = 0; i < 8; i++)
		sum += en7571_pwradc_data10(priv);
	return ((sum >> 2) + 1) >> 1;
}

/**
 * en7571_txsd_level() - program the TIA signal-detect threshold.
 * @priv: device
 *
 * Measures the TIA flat-band (A) and signal-detect (B) PWRADC levels and
 * combines them with the flashed Pav (D, dark-offset corrected) as
 *   tia_sd = c*D + (2.8/1.8)*(A - B) + 6
 * carried in Q20.  The D coefficient depends on the high TIA gain bit.
 */
void en7571_txsd_level(struct en7571_priv *priv)
{
	u32 pav_p1 = lddla_flash_read(&priv->lddla, EN7571_FL_PAV_P1);
	s32 txsd_offset = priv->pwradc_offset >> 6;
	s32 a, b, d, tia_sd, coeff_d;
	u8 tiamux0, tiamux1;

	/* Zero the TIASD threshold (bits 0-8). */
	lddla_wr8(&priv->lddla, EN7571_TIASD, 0);
	lddla_update8(&priv->lddla, EN7571_TIASD + 1, EN7571_TIASD_UPPER_MASK, 0);

	/* Save the TIA mux field and select the flat-band path. */
	lddla_rd8(&priv->lddla, EN7571_TIAMUX, &tiamux0);
	lddla_update8(&priv->lddla, EN7571_TIAMUX, EN7571_TIA_MUX_MASK, EN7571_TIA_MUX_TIAFLT);
	mdelay(5);
	a = en7571_pwradc_avg8(priv);

	/* Select the signal-detect path. */
	lddla_update8(&priv->lddla, EN7571_TIAMUX, EN7571_TIA_MUX_MASK, EN7571_TIA_MUX_TIASD);
	b = en7571_pwradc_avg8(priv);

	/* D: flashed Pav minus the dark offset. */
	d = (s32)((pav_p1 & EN7571_FL_PAV_MASK) >> 18) - txsd_offset;

	lddla_rd8(&priv->lddla, EN7571_TIAMUX + 1, &tiamux1);
	coeff_d = (tiamux1 & 0x80) ? 108741 : 163112;	/* 0.1*(2.8/1.8), /1.5 if gain[1] */
	tia_sd = (s32)(((s64)coeff_d * d + 1631118LL * (a - b) + (6LL << 20)) >> 20);
	if (tia_sd < 0)
		tia_sd = 0;

	lddla_wr8(&priv->lddla, EN7571_TIASD, tia_sd & 0xff);
	lddla_update8(&priv->lddla, EN7571_TIASD + 1, EN7571_TIASD_UPPER_MASK,
		       (tia_sd >> 8) & 0x01);

	/* Restore the saved TIA mux field. */
	lddla_update8(&priv->lddla, EN7571_TIAMUX, EN7571_TIA_MUX_MASK, tiamux0 & ~EN7571_TIA_MUX_MASK);
}

/* Compare the cached Rx-power DDMI word against the alarm thresholds. */
void en7571_rx_alarms(struct en7571_priv *priv)
{
	u16 p = priv->lddla.ddmi_rx_power;

	en7571_alarm_set(priv, AIROHA_ALARM_RX_LOW_POWER, p < EN7571_RX_PWR_LOW_THLD);
	en7571_alarm_set(priv, AIROHA_ALARM_RX_HIGH_POWER, p > EN7571_RX_PWR_HIGH_THLD);
}
