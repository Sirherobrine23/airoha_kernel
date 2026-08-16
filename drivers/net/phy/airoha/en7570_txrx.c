// SPDX-License-Identifier: GPL-2.0
/*
 * EN7570 transmitter and receiver subsystems.
 *
 * Transmitter: bias/modulation current, APC/Auto-ER (ERC) control, burst
 * timing (TGEN), TX_SD, the APD reverse-bias DAC, safety latches and Tx alarms.
 *
 * Receiver: TIA gain / TIAMUX, the limiting-amplifier RSSI front end, LOS/SD
 * detection and RSSI auto-ranging.  LOS_init programs the ADC revision enables
 * in SVADC_PD bytes 2 (REV1) and 3 (REV2) and the LOS_CTRL1/2 byte lanes; RSSI
 * calibration captures Vref/V with the cal-enable and V-mode bits.
 *
 * Locking: callers hold priv->lddla.lock.
 */
#include <linux/kernel.h>
#include <linux/math64.h>

#include "en7570.h"

static void en7570_alarm_set(struct en7570_priv *priv, u32 bit, bool on)
{
	if (on)
		priv->lddla.alarm |= bit;
	else
		priv->lddla.alarm &= ~bit;
}

/* ------------------------------------------------------------------ */
/* Transmitter							      */
/* ------------------------------------------------------------------ */

/* Write a 10-bit current code to a P{0,1}_PWR_CTRL_CS2 register (2 bytes). */
static void en7570_write_current(struct en7570_priv *priv, u16 reg, u32 code)
{
	u8 b[2] = { code & 0xff, (code >> 8) & 0xff };

	lddla_wr(&priv->lddla, reg, b, 2);
}

/*
 * CDR control and the PRBS calibration pattern live in the PHY core layer
 * (PHY CSRs reached through IO_GPHYREG / IO_SPHYREG), which is outside this
 * device's register file.  They are stubbed here so the bring-up flow stays
 * intact; on real silicon the PHY init layer drives the line-rate and
 * polarity.
 */
void en7570_cdr(struct en7570_priv *priv, bool enable)
{
	dev_dbg(priv->lddla.dev, "CDR %s (PHY-layer)\n", enable ? "lock-to-data" : "lock-to-ref");
}

int en7570_pattern_start(struct en7570_priv *priv, int mode)
{
	priv->pattern_enabled = 1;
	dev_dbg(priv->lddla.dev, "start PRBS pattern mode %d (PHY-layer)\n", mode);
	return 0;
}

void en7570_pattern_stop(struct en7570_priv *priv)
{
	priv->pattern_enabled = 0;
	dev_dbg(priv->lddla.dev, "stop PRBS pattern (PHY-layer)\n");
}

/*
 * Read back a live current / MPD-target code.  Ibias/Imod are 12-bit
 * readbacks in bytes 2-3 of P{0,1}_PWR_CTRL_CS3; the MPD targets live in
 * register 0x0004 with MPDH in bytes 0-1 and MPDL in bytes 2-3.
 */
u32 en7570_info(struct en7570_priv *priv, u8 sel)
{
	u8 b[4] = { 0 };

	switch (sel) {
	case EN7570_INFO_IMOD:
		lddla_rd(&priv->lddla, EN7570_P1_PWR_CTRL_CS3, b, 4);
		priv->mod_code = b[2] | ((b[3] & 0x0f) << 8);
		return priv->mod_code;
	case EN7570_INFO_P0:	/* MPDL: register 0x0004 bytes 2-3 */
		lddla_rd(&priv->lddla, EN7570_MPDH, b, 4);
		priv->mpdl = b[2] | ((b[3] & 0x03) << 8);
		return priv->mpdl;
	case EN7570_INFO_P1:	/* MPDH: register 0x0004 bytes 0-1 */
		lddla_rd(&priv->lddla, EN7570_MPDH, b, 4);
		priv->mpdh = b[0] | ((b[1] & 0x03) << 8);
		return priv->mpdh;
	default:		/* EN7570_INFO_IBIAS */
		lddla_rd(&priv->lddla, EN7570_P0_PWR_CTRL_CS3, b, 4);
		priv->bias_code = b[2] | ((b[3] & 0x0f) << 8);
		return priv->bias_code;
	}
}

/* Program the initial Ibias / Imod from flash, or from the LUT if absent. */
void en7570_tx_load_init_current(struct en7570_priv *priv)
{
	int idx = en7570_temp_index(priv->bosa_temp_mc);
	u32 ibias, imod;

	ibias = lddla_flash_read(&priv->lddla, EN7570_FL_IBIAS_INIT);
	imod = lddla_flash_read(&priv->lddla, EN7570_FL_IMOD_INIT);

	if (ibias == EN7570_FLASH_ERASED)
		ibias = priv->lut[idx][0];
	if (imod == EN7570_FLASH_ERASED)
		imod = priv->lut[idx][1];

	/* Bias/modulation current codes are 12-bit. */
	en7570_write_current(priv, EN7570_P0_PWR_CTRL_CS2, ibias & 0xfff);
	en7570_write_current(priv, EN7570_P1_PWR_CTRL_CS2, imod & 0xfff);
}

/*
 * Load the MPD targets and start the ERC loops.  Register 0x0004 holds MPDH
 * (= flash_P1_target) in bytes 0-1 and MPDL (= flash_P0_target) in bytes 2-3;
 * both are 10-bit.
 */
void en7570_tx_load_mpd_targets(struct en7570_priv *priv)
{
	u32 p0 = lddla_flash_read(&priv->lddla, EN7570_FL_P0_TARGET);
	u32 p1 = lddla_flash_read(&priv->lddla, EN7570_FL_P1_TARGET);
	u8 b[4];

	if (p0 == EN7570_FLASH_ERASED || p1 == EN7570_FLASH_ERASED)
		return;

	priv->mpdh = p1 & 0x3ff;	/* bytes 0-1 */
	priv->mpdl = p0 & 0x3ff;	/* bytes 2-3 */

	b[0] = priv->mpdh & 0xff;
	b[1] = (priv->mpdh >> 8) & 0x03;
	b[2] = priv->mpdl & 0xff;
	b[3] = (priv->mpdl >> 8) & 0x03;
	lddla_wr(&priv->lddla, EN7570_MPDH, b, 4);

	/* Start P0 and P1 ERC. */
	lddla_update8(&priv->lddla, EN7570_P0_PWR_CTRL_CS3, EN7570_ERC_START_MASK,
		       EN7570_ERC_START);
	lddla_update8(&priv->lddla, EN7570_P1_PWR_CTRL_CS3, EN7570_ERC_START_MASK,
		       EN7570_ERC_START);
}

/**
 * en7570_tx_sd_cal() - TX_SD calibration mux dance.
 * @priv: device
 *
 * Routes the SD path to the ADC, measures the photocurrent floor with the
 * threshold at zero, and returns the (tiaflt - TIASD_0) delta used to derive
 * the TX_SD threshold.
 */
int en7570_tx_sd_cal(struct en7570_priv *priv)
{
	u16 tiaflt = 0, floor = 0;

	/*
	 * 1. TIAMUX -> b'100; 2. ADC_select_SD in SVADC_PD byte 2 (0x10);
	 * 3. TxPW inmux in SVADC_PD byte 0 (0x04).
	 */
	lddla_update8(&priv->lddla, EN7570_TIAMUX, EN7570_TIA_MUX_MASK, EN7570_TIA_MUX_DEFAULT);
	lddla_update8(&priv->lddla, EN7570_SVADC_PD + 2, ~EN7570_ADC_SELECT_SD & 0xff,
		       EN7570_ADC_SELECT_SD);
	lddla_update8(&priv->lddla, EN7570_SVADC_PD, EN7570_ADC_INMUX_MASK,
		       EN7570_ADC_TXPW_ENABLE);

	/* 4. latch and read the photocurrent. */
	lddla_update8(&priv->lddla, EN7570_PROBE_CONTROL + 1, EN7570_ADC_LATCH_MASK,
		       EN7570_ADC_LATCH);
	lddla_rd16(&priv->lddla, EN7570_ADC_PROBE_STATUS, &tiaflt);

	/* 5. TIAMUX -> b'001 (SD path), zero TIASD, latch, read the floor. */
	lddla_update8(&priv->lddla, EN7570_TIAMUX, EN7570_TIA_MUX_MASK,
		       EN7570_TIA_MUX_SELECT_SD);
	lddla_wr8(&priv->lddla, EN7570_TIASD, 0);
	lddla_update8(&priv->lddla, EN7570_TIASD + 1, EN7570_TIASD_UPPER_MASK, 0);
	lddla_update8(&priv->lddla, EN7570_PROBE_CONTROL + 1, EN7570_ADC_LATCH_MASK,
		       EN7570_ADC_LATCH);
	lddla_rd16(&priv->lddla, EN7570_ADC_PROBE_STATUS, &floor);

	/* 6. restore SD/inmux and the normal TIA mux. */
	lddla_update8(&priv->lddla, EN7570_SVADC_PD + 2, ~EN7570_ADC_SELECT_SD & 0xff, 0);
	lddla_update8(&priv->lddla, EN7570_SVADC_PD, EN7570_ADC_INMUX_MASK, 0);
	lddla_update8(&priv->lddla, EN7570_TIAMUX, EN7570_TIA_MUX_MASK, EN7570_TIA_MUX_DEFAULT);

	return (s32)tiaflt - (s32)floor;
}

/* Program the TX_SD threshold. */
void en7570_tx_sd_level(struct en7570_priv *priv)
{
	s32 delta = en7570_tx_sd_cal(priv);
	u32 mpdh, mpdl, thr;
	u8 b[4];

	if (delta < 0) {
		dev_dbg(priv->lddla.dev, "Tx SD error (delta < 0)\n");
		return;
	}

	/* Read live MPDH (bytes 0-1) / MPDL (bytes 2-3) from register 0x0004. */
	lddla_rd(&priv->lddla, EN7570_MPDH, b, 4);
	mpdh = (b[0] | (b[1] << 8)) & 0x3ff;
	mpdl = (b[2] | (b[3] << 8)) & 0x3ff;

	/* tia_sd = 0.05*(MPDH + MPDL/4) + (1.4/0.6)*delta, 9-bit field. */
	thr = (mpdh + (mpdl >> 2)) / 20 + (delta * 7) / 3;

	lddla_wr8(&priv->lddla, EN7570_TIASD, thr & 0xff);
	lddla_update8(&priv->lddla, EN7570_TIASD + 1, 0xfe, (thr >> 8) & 0x01);
}

/* Capture the MPD dark current (laser off) as the calibration baseline. */
void en7570_mpd_dark(struct en7570_priv *priv)
{
	u32 code = 0;

	/* TIAMUX -> b'100, sample the LDD/MPD probe once. */
	lddla_update8(&priv->lddla, EN7570_TIAMUX, EN7570_TIA_MUX_MASK, EN7570_TIA_MUX_DEFAULT);
	en7570_adc_sample(priv, EN7570_ADC_CH_MPD, 1, &code);

	priv->mpd_current_offset = code;
	priv->mpd_current = code;
}

/**
 * en7570_mpd_current() - measure the MPD photocurrent.
 * @priv: device
 *
 * Samples the MPD rail with TIAMUX at b'000, takes the maximum and floors it
 * at the dark-current offset.
 * The vEN7570==0 dual-slope compensation path is not implemented.
 */
s32 en7570_mpd_current(struct en7570_priv *priv)
{
	int n = (priv->variant == 0 && priv->tec_switch) ? 10 : 1;
	u32 code, maxc = 0;
	int i;

	lddla_update8(&priv->lddla, EN7570_TIAMUX, EN7570_TIA_MUX_MASK, 0);
	for (i = 0; i < n; i++) {
		code = 0;
		en7570_adc_sample(priv, EN7570_ADC_CH_MPD, 1, &code);
		maxc = max(maxc, code);
	}
	lddla_update8(&priv->lddla, EN7570_TIAMUX, EN7570_TIA_MUX_MASK, EN7570_TIA_MUX_DEFAULT);

	/* No Tx power: floor the reading at the dark-current offset. */
	priv->mpd_current = (maxc < (u32)priv->mpd_current_offset)
			    ? priv->mpd_current_offset : (s32)maxc;
	return priv->mpd_current;
}

/* Program the ERC digital-loop filter coefficients once. */
void en7570_erc_filter(struct en7570_priv *priv)
{
	static const u8 taps[3] = {
		EN7570_ERC_FILTER_B0, EN7570_ERC_FILTER_B1, EN7570_ERC_FILTER_B2,
	};

	/* Bytes 0-2 are the taps; byte 3 bit 0 is the filter enable (RMW). */
	lddla_wr(&priv->lddla, EN7570_ERC_FILTER_CTRL, taps, sizeof(taps));
	lddla_update8(&priv->lddla, EN7570_ERC_FILTER_CTRL + 3, 0xfe,
		       EN7570_ERC_FILTER_B3);
}

/* Strict P0-clear / P1-clear / P0-set / P1-set ERC restart. */
void en7570_erc_restart(struct en7570_priv *priv)
{
	lddla_update8(&priv->lddla, EN7570_P0_PWR_CTRL_CS3, EN7570_ERC_START_MASK, 0);
	lddla_update8(&priv->lddla, EN7570_P1_PWR_CTRL_CS3, EN7570_ERC_START_MASK, 0);
	lddla_update8(&priv->lddla, EN7570_P0_PWR_CTRL_CS3, EN7570_ERC_START_MASK,
		       EN7570_ERC_START);
	lddla_update8(&priv->lddla, EN7570_P1_PWR_CTRL_CS3, EN7570_ERC_START_MASK,
		       EN7570_ERC_START);
}

/* Single-channel ERC restart for the bias rail (P0). */
void en7570_erc_restart_p0(struct en7570_priv *priv)
{
	lddla_update8(&priv->lddla, EN7570_P0_PWR_CTRL_CS3, EN7570_ERC_START_MASK, 0);
	lddla_update8(&priv->lddla, EN7570_P0_PWR_CTRL_CS3, EN7570_ERC_START_MASK,
		       EN7570_ERC_START);
}

/**
 * en7570_tgen() - calibrate the upstream burst timing.
 * @priv:   device
 * @mode: PON mode (EN7570_PON_*)
 *
 * Sweeps the T0/T1 timer reset points 32 times with the line clock locked to
 * the reference clock, then loads the calibrated (or default) delay value.
 */
int en7570_tgen(struct en7570_priv *priv, int mode)
{
	u32 t0t1 = lddla_flash_read(&priv->lddla, EN7570_FL_T0T1_DELAY);
	u8 ptr[4];
	u16 t0c;
	int i;

	en7570_cdr(priv, false);			/* lock to reference clock */
	en7570_pattern_start(priv, 5);		/* internal PRBS23 */

	for (i = 0; i < 32; i++) {
		lddla_rd(&priv->lddla, EN7570_T1DELAY, ptr, 4);
		ptr[3] &= EN7570_ERC_ENABLE_MASK;	/* clear ERC enable */
		ptr[0] = EN7570_T1_T0_SETTING1;		/* try delay code */
		ptr[1] = EN7570_TIMER_RESET_VALUE;	/* T1 timer reset */
		ptr[2] = EN7570_TIMER_RESET_VALUE;	/* T0 timer reset */
		lddla_wr(&priv->lddla, EN7570_T1DELAY, ptr, 4);

		/* TGEN method-2 reset: pulse the T1/T0 reset, set method2. */
		ptr[3] = (ptr[3] & EN7570_TGEN_RESET_MASK) | EN7570_TGEN_RESET_T1T0;
		ptr[3] = (ptr[3] & EN7570_TGEN_METHOD2_MASK) | EN7570_TGEN_METHOD2_ENABLE;
		lddla_wr(&priv->lddla, EN7570_T1DELAY, ptr, 4);

		lddla_rd16(&priv->lddla, EN7570_T0C, &t0c);	/* sample T0C/T1C */
	}

	/*
	 * Load the final delay value.  The empty-flash defaults are 0x9a (GPON)
	 * / 0x47 (EPON).
	 */
	lddla_rd(&priv->lddla, EN7570_T1DELAY, ptr, 4);
	if (t0t1 != EN7570_FLASH_ERASED)
		ptr[0] = t0t1 & 0xff;
	else
		ptr[0] = (mode == EN7570_PON_GPON) ? EN7570_T1_T0_DELAY_GPON
						   : EN7570_T1_T0_DELAY_EPON;
	ptr[3] |= EN7570_ERC_ENABLE;		/* re-enable ERC */
	lddla_wr(&priv->lddla, EN7570_T1DELAY, ptr, 4);

	en7570_cdr(priv, true);			/* back to lock-to-data */
	en7570_pattern_stop(priv);
	return 0;
}

/* APD soft-start (byte2 bit5) and control-enable (byte1 bit0). */
void en7570_apd_init(struct en7570_priv *priv)
{
	lddla_update8(&priv->lddla, EN7570_APD_DAC_CODE + 2, ~EN7570_APD_SOFTSTART_ENABLE & 0xff,
		       EN7570_APD_SOFTSTART_ENABLE);
	lddla_update8(&priv->lddla, EN7570_APD_DAC_CODE + 1, ~EN7570_APD_CONTROL_ENABLE & 0xff,
		       EN7570_APD_CONTROL_ENABLE);
}

void en7570_apd_write(struct en7570_priv *priv, u8 code)
{
	lddla_wr8(&priv->lddla, EN7570_APD_DAC_CODE, code);
}

/* Convert an APD voltage (mV) to a DAC code, four-segment or legacy. */
static int en7570_apd_voltage_to_code(struct en7570_priv *priv, s32 v_mv)
{
	u32 w1 = lddla_flash_read(&priv->lddla, EN7570_FL_APD_VOLTAGE_1);
	u32 w2 = lddla_flash_read(&priv->lddla, EN7570_FL_APD_VOLTAGE_2);
	s32 step, code;

	/*
	 * Erased flash reads as all ones, which passes a test for the upper
	 * half being populated and then decodes as four identical 6553.5V
	 * anchors. Treat it as absent so a board whose factory data omits the
	 * anchors falls back to the legacy slope instead of leaving the APD
	 * bias unprogrammed.
	 */
	if (w1 != EN7570_FLASH_ERASED && w2 != EN7570_FLASH_ERASED &&
	    (w1 & 0xffff0000) && (w2 & 0xffff0000)) {
		/* Four-segment piecewise-linear over 64 codes per segment. */
		s32 v00 = (s32)(w1 >> 16) * 100;
		s32 v40 = (s32)(w1 & 0xffff) * 100;
		s32 v80 = (s32)(w2 >> 16) * 100;
		s32 vc0 = (s32)(w2 & 0xffff) * 100;

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
	} else {
		/* Legacy single-slope mode. */
		s32 zero_mv = EN7570_APD_ZERO_CODE_MV_DEF;
		s32 step_uv = EN7570_APD_STEP_UV_DEF;
		u32 fstep = lddla_flash_read(&priv->lddla, EN7570_FL_VOLTAGE_SLOPE);

		if (fstep != EN7570_FLASH_ERASED && fstep)
			step_uv = fstep;	/* flash_APD_voltage_step (uV/code) */

		code = div_s64((s64)(v_mv - zero_mv) * 1000, step_uv);
	}

	return clamp(code, 0, 0xff);
}

/* Recompute and program the APD voltage from BOSA temperature. */
void en7570_apd_update(struct en7570_priv *priv)
{
	s32 v_mv, contrib;
	int code;

	if (priv->bosa_temp_mc > EN7570_APD_KNEE_TEMP_MC) {
		contrib = div_s64((s64)priv->apd_slope_up_uv *
				  (priv->bosa_temp_mc - EN7570_APD_KNEE_TEMP_MC),
				  1000000);
		v_mv = priv->apd_knee_mv + contrib;
	} else {
		contrib = div_s64((s64)priv->apd_slope_dn_uv *
				  (EN7570_APD_KNEE_TEMP_MC - priv->bosa_temp_mc),
				  1000000);
		v_mv = priv->apd_knee_mv - contrib;
	}

	code = en7570_apd_voltage_to_code(priv, v_mv);
	if (code < 0)
		return;

	priv->apd_voltage_mv = v_mv;
	en7570_apd_write(priv, code);
}

void en7570_rogue_clear(struct en7570_priv *priv)
{
	lddla_update8(&priv->lddla, EN7570_ROGUE_ONU_DET_CTRL + 1, EN7570_ROGUE_ONU_MASK,
		       EN7570_ROGUE_ONU_CLEAR);
}

/* Clear the safe-circuit latch and the APD OVP latch. */
void en7570_safe_reset(struct en7570_priv *priv)
{
	lddla_update8(&priv->lddla, EN7570_SAFE_PROTECT + 1, EN7570_SAFE_CIRCUIT_MASK,
		       EN7570_SAFE_CIRCUIT_RESET);
}

/* Compare cached Tx-power / bias DDMI words against alarm thresholds. */
void en7570_tx_alarms(struct en7570_priv *priv)
{
	u16 p = priv->lddla.ddmi_tx_power;
	u16 b = priv->lddla.ddmi_current;

	en7570_alarm_set(priv, AIROHA_ALARM_TX_LOW_POWER, p < EN7570_TX_PWR_LOW_THLD);
	en7570_alarm_set(priv, AIROHA_ALARM_TX_HIGH_POWER, p > EN7570_TX_PWR_HIGH_THLD);
	en7570_alarm_set(priv, AIROHA_ALARM_TX_LOW_BIAS, b < EN7570_TX_BIAS_LOW_THLD);
	en7570_alarm_set(priv, AIROHA_ALARM_TX_HIGH_BIAS, b > EN7570_TX_BIAS_HIGH_THLD);
}

/* ------------------------------------------------------------------ */
/* Receiver							      */
/* ------------------------------------------------------------------ */

/* RSSI front-end gain table: LA_PWD byte2 [2:0] -> multiplicative factor. */
static const u32 en7570_rssi_gain_factor[6] = { 1, 4, 16, 64, 128, 256 };

/* Program the Rx TIA gain from flash into TIAMUX byte1 [7:6]. */
void en7570_rx_tia_gain(struct en7570_priv *priv)
{
	u32 gain = lddla_flash_read(&priv->lddla, EN7570_FL_TIAGAIN);

	if (gain == EN7570_FLASH_ERASED)
		return;				/* reference leaves TIAMUX untouched */
	gain &= 0x3;

	lddla_update8(&priv->lddla, EN7570_TIAMUX + 1, 0x3f, gain << EN7570_TIA_GAIN_SHIFT);
}

/* Default the RSSI front-end to the largest gain (LA_PWD byte2 [2:0] = 5). */
void en7570_rssi_gain_init(struct en7570_priv *priv)
{
	lddla_update8(&priv->lddla, EN7570_LA_PWD + 2, EN7570_RSSI_GAIN_MASK,
		       EN7570_RSSI_GAIN_DEFAULT);
}

/* Sample the RSSI rail at the given front-end gain, averaging 4 latches. */
static u32 en7570_rssi_sample(struct en7570_priv *priv, int gain)
{
	u32 code = 0;

	lddla_update8(&priv->lddla, EN7570_LA_PWD + 2, EN7570_RSSI_GAIN_MASK, gain & 0x07);
	en7570_adc_sample(priv, EN7570_ADC_CH_RSSI, 4, &code);
	return code;
}

/**
 * en7570_rssi_cal() - capture the RSSI reference.
 * @priv: device
 *
 * Vref is read with RSSI V-mode enabled, V with it disabled, both with the
 * RSSI calibration path on.
 */
int en7570_rssi_cal(struct en7570_priv *priv)
{
	s32 term_uv;
	u32 code;

	/* RSSI calibration on (LA_PWD byte1 bit 4). */
	lddla_update8(&priv->lddla, EN7570_LA_PWD + 1, EN7570_RSSI_CAL_EN_MASK,
		       EN7570_RSSI_CAL_EN);

	/* V-mode on -> Vref. */
	lddla_update8(&priv->lddla, EN7570_LA_PWD + 1, 0xbf, 0x40);
	en7570_adc_sample(priv, EN7570_ADC_CH_RSSI, 1, &code);
	priv->rssi_vref = code;

	/* V-mode off -> V. */
	lddla_update8(&priv->lddla, EN7570_LA_PWD + 1, 0xbf, 0);
	en7570_adc_sample(priv, EN7570_ADC_CH_RSSI, 1, &code);
	priv->rssi_v = code;

	/* RSSI calibration off. */
	lddla_update8(&priv->lddla, EN7570_LA_PWD + 1, EN7570_RSSI_CAL_EN_MASK, 0);

	if (priv->rssi_v <= priv->rssi_vref) {
		dev_warn(priv->lddla.dev, "RSSI calibration fail (V <= Vref)\n");
		return 0;
	}

	/*
	 * rssi_factor is diagnostic-only (exposed via debugfs); the runtime
	 * current calculation uses the gain-table factor instead.
	 */
	term_uv = div_s64(priv->adc_slope_nv * (priv->rssi_v - priv->rssi_vref),
			  1000) + priv->adc_offset_uv;
	priv->rssi_factor = div_s64((s64)term_uv * 1000, 350);
	return 0;
}

/**
 * en7570_rssi_current() - six-stage gain sweep.
 * @priv: device
 *
 * Sweeps the front-end gain from the largest down, breaking once the measured
 * voltage falls more than the noise-defend margin below Vref, then scales the
 * surviving reading by the corresponding gain factor.
 */
s32 en7570_rssi_current(struct en7570_priv *priv)
{
	u32 voltage = 0, factor = 1;
	int g;

	for (g = 5; g >= 0; g--) {
		voltage = en7570_rssi_sample(priv, g);
		factor = en7570_rssi_gain_factor[g];
		if ((s32)voltage < priv->rssi_vref - EN7570_RSSI_DEFEND_NOISE_THRESH)
			break;
	}

	/* Restore the default (largest) gain. */
	lddla_update8(&priv->lddla, EN7570_LA_PWD + 2, EN7570_RSSI_GAIN_MASK,
		       EN7570_RSSI_GAIN_DEFAULT);

	if (priv->rssi_vref >= (s32)voltage)
		priv->rssi_current = (priv->rssi_vref - (s32)voltage) * factor;
	else
		priv->rssi_current = 0;

	return priv->rssi_current;
}

/* Characterise the RSSI noise floor: gain pinned to 0, 64-sample average. */
s32 en7570_rx_dark(struct en7570_priv *priv)
{
	u32 code = 0;

	lddla_update8(&priv->lddla, EN7570_LA_PWD + 2, EN7570_RSSI_GAIN_MASK, 0);
	en7570_adc_sample(priv, EN7570_ADC_CH_RSSI, 64, &code);
	lddla_update8(&priv->lddla, EN7570_LA_PWD + 2, EN7570_RSSI_GAIN_MASK,
		       EN7570_RSSI_GAIN_DEFAULT);
	return code;
}

/* LOS calibration init sequence. */
void en7570_los_init(struct en7570_priv *priv)
{
	/* LOS_CTRL1: trigger (byte0) and analog-input stable counter (byte1). */
	lddla_update8(&priv->lddla, EN7570_LOS_CTRL1, EN7570_LOS_CAL_TRIG_MASK,
		       EN7570_LOS_CAL_TRIG);
	lddla_update8(&priv->lddla, EN7570_LOS_CTRL1 + 1, EN7570_LOS_AIN_STABLE_MASK,
		       EN7570_LOS_AIN_STABLE_SET);

	/* ADC REV2 (SVADC_PD byte3) and REV1 (SVADC_PD byte2). */
	lddla_update8(&priv->lddla, EN7570_SVADC_PD + 3, EN7570_LOS_ADCREV2_MASK,
		       EN7570_LOS_ADCREV2_ENABLE);
	lddla_update8(&priv->lddla, EN7570_SVADC_PD + 2, EN7570_LOS_ADCREV1_MASK,
		       EN7570_LOS_ADCREV1_ENABLE);

	/* LOS_CTRL2: confidence (byte1) and LOS/SD counter (byte0). */
	lddla_update8(&priv->lddla, EN7570_LOS_CTRL2 + 1, EN7570_LOS_CONFIDENCE_MASK,
		       EN7570_LOS_CONFIDENCE_SET);
	lddla_update8(&priv->lddla, EN7570_LOS_CTRL2, EN7570_LOS_CNT_MASK,
		       EN7570_LOS_CNT_SET);
}

/* Master LOS init: calibrate, then load thresholds. */
void en7570_los_level(struct en7570_priv *priv)
{
	u32 high = lddla_flash_read(&priv->lddla, EN7570_FL_LOS_HIGH_THLD);
	u32 low = lddla_flash_read(&priv->lddla, EN7570_FL_LOS_LOW_THLD);

	en7570_los_init(priv);

	if (high == EN7570_FLASH_ERASED || low == EN7570_FLASH_ERASED) {
		high = EN7570_LOS_COMP_THLD_H_DEF;
		low = EN7570_LOS_COMP_THLD_L_DEF;
	}

	/* LOS_CTRL1 byte2 = high threshold, byte3 = low threshold. */
	lddla_update8(&priv->lddla, EN7570_LOS_CTRL1 + 2, ~EN7570_LOS_THRESH_MASK & 0xff,
		       high & EN7570_LOS_THRESH_MASK);
	lddla_update8(&priv->lddla, EN7570_LOS_CTRL1 + 3, ~EN7570_LOS_THRESH_MASK & 0xff,
		       low & EN7570_LOS_THRESH_MASK);
}

/* Manual LOS recalibration with explicit high/low thresholds. */
void en7570_los_cal(struct en7570_priv *priv, u8 high, u8 low)
{
	en7570_los_init(priv);
	lddla_update8(&priv->lddla, EN7570_LOS_CTRL1 + 2, ~EN7570_LOS_THRESH_MASK & 0xff,
		       high & EN7570_LOS_THRESH_MASK);
	lddla_update8(&priv->lddla, EN7570_LOS_CTRL1 + 3, ~EN7570_LOS_THRESH_MASK & 0xff,
		       low & EN7570_LOS_THRESH_MASK);
}

/* Compare the cached Rx-power DDMI word against alarm thresholds. */
void en7570_rx_alarms(struct en7570_priv *priv)
{
	u16 p = priv->lddla.ddmi_rx_power;

	en7570_alarm_set(priv, AIROHA_ALARM_RX_LOW_POWER, p < EN7570_RX_PWR_LOW_THLD);
	en7570_alarm_set(priv, AIROHA_ALARM_RX_HIGH_POWER, p > EN7570_RX_PWR_HIGH_THLD);
}
