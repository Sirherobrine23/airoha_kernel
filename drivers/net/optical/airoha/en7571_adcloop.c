// SPDX-License-Identifier: GPL-2.0
/*
 * EN7571 ADC subsystem and loop control.
 *
 * ADC / temperature sensor: the SVADC is a successive-approximation ADC whose
 * input is selected through a multiplexer in SVADC_PD; a latch in
 * PROBE_CONTROL captures one conversion into ADC_PROBE_STATUS.  Every
 * per-channel read follows the same pattern: select channel, latch, read the
 * 16-bit sample, average, restore the mux.  The monitor-photodiode power uses a
 * separate PWRADC with a hardware accumulator (1024 samples) gated by
 * RG_PWRADC_DATA2.
 *
 * Loop control: temperature-compensation (KT) and feature configuration.  KT
 * compensates the laser modulation current for temperature.  The software loop
 * reads the present average-current deviation from the calibrated value and
 * writes a temperature factor into the hardware KT coefficient register; the
 * on-die loop then adjusts Imod.  The factor is taken from the low-temp or
 * high-temp half of the calibration word depending on the sign of the
 * deviation.  The fast Imod writers target the Imod DAC directly.  The rev-2
 * cross path can bracket the DAC write in an IRQ-off MBI window; that window is
 * a SoC/PHY-layer facility and is not reachable from this device's register
 * file, so the DAC write is issued directly here.
 *
 * Locking: callers hold priv->lddla.lock, which keeps the multi-step
 * channel-select/latch/read sequence atomic (the SVADC mux is a single shared
 * resource).
 */
#include <linux/delay.h>
#include <linux/kernel.h>
#include <linux/math64.h>

#include "en7571.h"

/* ------------------------------------------------------------------ */
/* ADC / temperature sensor / PWRADC				      */
/* ------------------------------------------------------------------ */

/* Latch one SVADC conversion and return the raw 16-bit code. */
u32 en7571_svadc_get(struct en7571_priv *priv)
{
	u16 code = 0;

	/* Latch: PROBE_CONTROL+1 byte 0 bit 4. */
	lddla_update8(&priv->lddla, EN7571_PROBE_CONTROL + 1, EN7571_ADC_LATCH_MASK,
		       EN7571_ADC_LATCH);
	lddla_rd16(&priv->lddla, EN7571_ADC_PROBE_STATUS, &code);
	return code;
}

/* Select an SVADC channel in SVADC_PD (byte 0, or byte 1 for the 0.875 V ref). */
static void en7571_adc_select(struct en7571_priv *priv, u8 channel)
{
	if (channel == EN7571_ADC_SELECT_BG_0V875)
		lddla_update8(&priv->lddla, EN7571_SVADC_PD + 1, EN7571_ADC_BG0V875_MASK,
			       channel & ~EN7571_ADC_BG0V875_MASK);
	else
		lddla_update8(&priv->lddla, EN7571_SVADC_PD, EN7571_ADC_SELECT_MASK,
			       channel & ~EN7571_ADC_SELECT_MASK);
}

/* Restore the SVADC_PD channel field to its default (cleared). */
static void en7571_adc_restore(struct en7571_priv *priv, u8 channel)
{
	if (channel == EN7571_ADC_SELECT_BG_0V875)
		lddla_update8(&priv->lddla, EN7571_SVADC_PD + 1, EN7571_ADC_BG0V875_MASK, 0);
	else
		lddla_update8(&priv->lddla, EN7571_SVADC_PD, EN7571_ADC_SELECT_MASK, 0);
}

/**
 * en7571_adc_sample() - select a channel and average a few latched reads.
 * @priv:    device
 * @channel: EN7571_ADC_SELECT_* value
 * @samples: number of latches to average (rounded to nearest)
 * @out:     averaged 16-bit ADC code
 */
int en7571_adc_sample(struct en7571_priv *priv, u8 channel, int samples,
		      u32 *out)
{
	u32 sum = 0;
	int i;

	if (samples < 1)
		samples = 1;

	en7571_adc_select(priv, channel);
	for (i = 0; i < samples; i++)
		sum += en7571_svadc_get(priv);
	en7571_adc_restore(priv, channel);

	*out = (sum + samples / 2) / samples;	/* round to nearest */
	return 0;
}

/* Convert a raw ADC code to microvolts using the two-point calibration. */
s64 en7571_adc_code_to_uv(struct en7571_priv *priv, u32 code)
{
	return div_s64(priv->adc_slope_nv * code, 1000) + priv->adc_offset_uv;
}

/**
 * en7571_adc_calibrate() - two-point bandgap calibration.
 * @priv: device
 *
 * Establishes a linear ADC-code -> voltage conversion from the on-die 1.76 V
 * and 0.875 V bandgap references.  Rerun periodically to track slow drift.
 * Must not run too soon after a SW reset; the bandgaps need to settle.
 */
int en7571_adc_calibrate(struct en7571_priv *priv)
{
	u32 bg1v76, bg0v875;
	s32 delta;

	en7571_adc_sample(priv, EN7571_ADC_SELECT_BG_1V76, 8, &bg1v76);
	en7571_adc_sample(priv, EN7571_ADC_SELECT_BG_0V875, 8, &bg0v875);

	delta = (s32)bg1v76 - (s32)bg0v875;
	if (delta > EN7571_ADC_BANDGAP_MIN_DELTA) {
		priv->adc_slope_nv =
			div_s64(EN7571_BG_1V76_NV - EN7571_BG_0V875_NV, delta);
	} else {
		dev_warn(priv->lddla.dev, "ADC calibration fail (spread %d)\n", delta);
		priv->adc_slope_nv = EN7571_DEFAULT_SLOPE_NV;
		return -EIO;
	}

	priv->adc_offset_uv = 1760000 - div_s64(priv->adc_slope_nv * bg1v76, 1000);
	return 0;
}

/* On-die temperature ADC code (single latched read). */
u16 en7571_adc_temp(struct en7571_priv *priv)
{
	u32 code = 0;

	en7571_adc_sample(priv, EN7571_ADC_SELECT_TEMPERATURE, 1, &code);
	return code;
}

/* Supply-voltage ADC code; the Rx termination is forced high-Z while sampling. */
u16 en7571_adc_vcc(struct en7571_priv *priv)
{
	u32 code = 0;

	lddla_update8(&priv->lddla, EN7571_LA_PWD, EN7571_LA_RX_HIGHZ_MASK,
		       EN7571_LA_RX_HIGHZ_ENABLE);
	en7571_adc_sample(priv, EN7571_ADC_SELECT_VOLTAGE, 1, &code);
	lddla_update8(&priv->lddla, EN7571_LA_PWD, EN7571_LA_RX_HIGHZ_MASK, 0);
	return code;
}

/**
 * en7571_efuse_temp() - read the eFuse temperature correction.
 * @priv: device
 *
 * Pulses a read trigger, waits for the eFuse to produce a value, then decodes
 * the signed 8-bit code at 0.3 degC/step into priv->efuse_offset_mc.
 */
void en7571_efuse_temp(struct en7571_priv *priv)
{
	u8 read = 0;

	/* RG_eFuse_Temp byte 2 bit 0: trigger a read pulse. */
	lddla_update8(&priv->lddla, EN7571_RG_EFUSE_TEMP + 2, 0xfe, 0x01);
	msleep(50);
	lddla_rd8(&priv->lddla, EN7571_RG_EFUSE_TEMP, &read);

	if (read < EN7571_EFUSE_SIGN_THRESHOLD)
		priv->efuse_offset_mc = read * EN7571_EFUSE_STEP_MC;
	else
		priv->efuse_offset_mc = -((256 - read) * EN7571_EFUSE_STEP_MC);
}

/**
 * en7571_temp_update() - refresh the IC / BOSA temperatures.
 * @priv: device
 *
 * Voltage-domain linear fit:
 *   sensor_voltage = ADC_slope * code + ADC_offset
 *   IC_temp        = 495.8 - 327.5 * sensor_voltage - eFuse_offset - 3.44
 *   BOSA_temp      = IC_temp - BOSA_offset
 * with T_V_offset / T_V_slope held x10 (default 4958 / 3275 = 495.8 / 327.5).
 */
void en7571_temp_update(struct en7571_priv *priv)
{
	u32 raw = en7571_adc_temp(priv);
	s64 sensor_uv = en7571_adc_code_to_uv(priv, raw);

	priv->ic_temp_mc = 100 * priv->temp_offset_x10 -
		div_s64((s64)priv->temp_slope_x10 * sensor_uv, 10000) -
		priv->efuse_offset_mc - EN7571_EFUSE_FIXED_OFFSET_MC;
	priv->bosa_temp_mc = priv->ic_temp_mc - priv->bosa_temp_offset_mc;
	priv->env_temp_mc = priv->ic_temp_mc - priv->env_temp_offset_mc;
}

/*
 * Run one PWRADC accumulate-1024 conversion and return the averaged code
 * (20-bit sum >> 4).  RG_PWRK1 selection and the TIA path are set up by the
 * caller; this helper only drives RG_PWRADC_DATA2.
 */
static u32 en7571_pwradc_acc(struct en7571_priv *priv)
{
	u8 b[4] = { 0 };
	u32 sum;

	/* Accumulate = 1024 (byte 3 [2:0]). */
	lddla_update8(&priv->lddla, EN7571_RG_PWRADC_DATA2 + 3, EN7571_PWRADC_COUNT_MASK,
		       EN7571_PWRADC_COUNT_1024);
	/* Trigger (byte 3 bit 7) and wait for the conversion to settle. */
	lddla_update8(&priv->lddla, EN7571_RG_PWRADC_DATA2 + 3, 0xff, EN7571_PWRADC_TRIG);
	msleep(20);

	lddla_rd(&priv->lddla, EN7571_RG_PWRADC_DATA2, b, 4);
	sum = ((b[2] << 16) | (b[1] << 8) | b[0]) & EN7571_PWRADC_SUM_MASK;

	/* Restore the accumulate count to its reset (zero) field. */
	lddla_update8(&priv->lddla, EN7571_RG_PWRADC_DATA2 + 3, EN7571_PWRADC_COUNT_MASK, 0);

	if (!(b[2] & EN7571_PWRADC_VALID)) {
		dev_dbg(priv->lddla.dev, "PWRADC sample not valid\n");
		return 0;
	}
	return sum >> 4;
}

/* Capture the MPD ADC value with the laser off into priv->pwradc_offset. */
void en7571_pwradc_calibration(struct en7571_priv *priv)
{
	/* RG_PWRK1 = 1 (RG_ANA_CTRL1 byte 3). */
	lddla_update8(&priv->lddla, EN7571_RG_ANA_CTRL1 + 3, EN7571_RG_PWRK1_MASK,
		       EN7571_RG_PWRK1_1);
	priv->pwradc_offset = en7571_pwradc_acc(priv);
	/* RG_PWRK1 = 0. */
	lddla_update8(&priv->lddla, EN7571_RG_ANA_CTRL1 + 3, EN7571_RG_PWRK1_MASK, 0);
}

/* Route the MPD photodiode to the PWRADC and enable its block. */
void en7571_pwradc_enable(struct en7571_priv *priv)
{
	lddla_update8(&priv->lddla, EN7571_TIAMUX, EN7571_TIA_MUX_MASK, 0);	/* RG_TIAMUX=0 */
	lddla_update8(&priv->lddla, EN7571_RG_ANA_CTRL1 + 3, EN7571_RG_PWRK1_MASK, 0);
	lddla_update8(&priv->lddla, EN7571_RG_PWR_CTRL_BEN_0, EN7571_PWR_CTRL_BEN_MASK,
		       EN7571_PWR_CTRL_BEN_SET);
}

/**
 * en7571_pwradc_get() - read the monitor-photodiode power ADC.
 * @priv: device
 *
 * Returns the dark offset when no light could be present, else the latest
 * accumulated reading floored at the dark offset.
 */
u32 en7571_pwradc_get(struct en7571_priv *priv)
{
	u32 adc;

	/*
	 * When the line is idle (Tx off) the reading should fall back to the
	 * dark offset.  Traffic state lives in the PHY core layer, outside this
	 * device, so always sample and floor at the dark offset instead.
	 */
	adc = en7571_pwradc_acc(priv);
	priv->pwradc = (adc < priv->pwradc_offset) ? priv->pwradc_offset : adc;
	return priv->pwradc;
}

/* ------------------------------------------------------------------ */
/* Loop control / KT					              */
/* ------------------------------------------------------------------ */

/* --- Feature configuration --- */

void en7571_config(struct en7571_priv *priv)
{
	u32 w;

	/* Internal DDMI mode. */
	w = lddla_flash_read(&priv->lddla, EN7571_FL_INTERNAL_DDMI);
	if (w == EN7571_DDMI_OFF) {
		priv->internal_ddmi = EN7571_DDMI_OFF;
		priv->fast_ddmi = 0;
	} else {
		priv->internal_ddmi = EN7571_DDMI_ON;
		priv->fast_ddmi = (w == EN7571_DDMI_FAST);
	}

	/*
	 * Loop select: single-closed-loop (1) and open-loop (2) build a
	 * per-temperature bias/mod LUT and track it; everything else runs the
	 * KT temperature-compensation loop on the dual-closed loop.
	 */
	w = lddla_flash_read(&priv->lddla, EN7571_FL_CL_SWITCH);
	if (w == EN7571_LOOP_SCL) {
		priv->scl = 1;
		en7571_single_cl_mode(priv);
		en7571_lut_recover(priv);
		en7571_temp_update(priv);
		en7571_lut_tracking(priv);
	} else if (w == EN7571_LOOP_OPEN) {
		priv->dol = 1;
		en7571_open_loop_mode(priv, true);
		en7571_lut_recover(priv);
		en7571_temp_update(priv);
		en7571_lut_tracking(priv);
	} else {
		priv->kt = EN7571_KT_NORMAL;

		w = lddla_flash_read(&priv->lddla, EN7571_FL_KT_SWITCH);
		if (w == EN7571_KT_INVERSE)
			priv->kt = EN7571_KT_INVERSE;
		else if (w == EN7571_KT_ENHANCED)
			priv->kt = EN7571_KT_ENHANCED;
	}

	/* Disable the cross-Imod MBI fast path if flash clears it. */
	if (lddla_flash_read(&priv->lddla, EN7571_FL_CRS) == 0)
		priv->cross = 0;

	/* MBI settle delay used by the cross-Imod path. */
	w = lddla_flash_read(&priv->lddla, EN7571_FL_MBI_DELAY);
	if (w != EN7571_FLASH_ERASED)
		priv->mbi_delay = w;
}

/* --- HW KT select (PWR_CTRL_2 byte 0) --- */

void en7571_hwkt(struct en7571_priv *priv, bool enable)
{
	lddla_update8(&priv->lddla, EN7571_PWR_CTRL_2, EN7571_IMOD_ADJ_SEL_MASK,
		       enable ? EN7571_IMOD_ADJ_SEL_KT : 0);
}

/* --- KT coefficient tune (PWR_CTRL_2) --- */

/*
 * Write the 10-bit KT factor into PWR_CTRL_2.  The byte indexing is unusual
 * (byte 2 takes the high bits computed from byte 1, byte 1 takes the low byte):
 * it is reproduced as the silicon expects.
 */
void en7571_tune_kt(struct en7571_priv *priv, u16 input)
{
	u8 b[4] = { 0 };

	lddla_rd(&priv->lddla, EN7571_PWR_CTRL_2, b, 4);
	b[2] = (b[1] & 0xfc) | (input >> 8);
	b[1] = input & 0xff;
	lddla_wr(&priv->lddla, EN7571_PWR_CTRL_2, b, 4);
}

/* --- Imod / MPDH writers --- */

/* Write a 12-bit Imod DAC value to the phase-1 coarse register. */
void en7571_change_imod(struct en7571_priv *priv, u32 v)
{
	u8 b[4] = { 0 };

	lddla_rd(&priv->lddla, EN7571_P1_PWR_CTRL_CS2, b, 4);
	b[1] = (b[1] & 0xf0) | ((v >> 8) & 0x0f);
	b[0] = v & 0xff;
	lddla_wr(&priv->lddla, EN7571_P1_PWR_CTRL_CS2, b, 4);
}

/* Write a 12-bit Ibias DAC value to the phase-0 coarse register. */
void en7571_change_ibias(struct en7571_priv *priv, u32 v)
{
	u8 b[4] = { 0 };

	lddla_rd(&priv->lddla, EN7571_P0_PWR_CTRL_CS2, b, 4);
	b[1] = (b[1] & 0xf0) | ((v >> 8) & 0x0f);
	b[0] = v & 0xff;
	lddla_wr(&priv->lddla, EN7571_P0_PWR_CTRL_CS2, b, 4);
}

/*
 * Fast Imod write.  With the cross path enabled the DAC write would normally be
 * bracketed by an IRQ-off MBI window; that window is a SoC-layer facility, so
 * the write is issued directly and the configured MBI settle delay is honoured.
 */
void en7571_cross_imod(struct en7571_priv *priv, u32 v)
{
	u8 b[2] = { v & 0xff, (v >> 8) & 0x0f };

	lddla_wr(&priv->lddla, EN7571_P1_PWR_CTRL_CS2, b, 2);
	if (priv->cross && priv->mbi_delay)
		udelay(priv->mbi_delay);
}

/* Move the MPDH set-point (PWR_CTRL_D bytes 2-3), freezing Imod variation. */
void en7571_change_mpdh(struct en7571_priv *priv, u32 v)
{
	u8 b[2] = { v & 0xff, (v >> 8) & 0x03 };

	lddla_wr8(&priv->lddla, EN7571_PWR_CTRL_8 + 1, 0);		/* freeze Imod var */
	lddla_wr(&priv->lddla, EN7571_PWR_CTRL_D + 2, b, 2);
	lddla_wr8(&priv->lddla, EN7571_PWR_CTRL_8 + 1, EN7571_DELTA_IMOD_MAX);
}

/* --- SW KT --- */

/*
 * Read the present average-current deviation from the calibrated Iav and load
 * the high- or low-temperature KT factor accordingly.  The hardware loop then
 * drives Imod toward the compensated target.
 */
void en7571_swkt(struct en7571_priv *priv)
{
	u32 iav_imod = lddla_flash_read(&priv->lddla, EN7571_FL_IAV_IMOD);
	u32 kt_flash = lddla_flash_read(&priv->lddla, EN7571_FL_KT);
	s32 iav_cal, ratio_now;
	u16 factor_ht, factor_lt;

	if (iav_imod == EN7571_FLASH_ERASED)
		return;

	/* Unprovisioned KT word: use the compiled-in default factors. */
	if (kt_flash == EN7571_FLASH_ERASED)
		kt_flash = EN7571_FL_KT_DEFAULT;

	iav_cal = (iav_imod & EN7571_FL_IAV_MASK) >> 16;
	if (!iav_cal)
		return;

	factor_ht = kt_flash & 0xffff;
	factor_lt = (kt_flash >> 16) & 0xffff;

	ratio_now = 100 * (s32)en7571_info(priv, EN7571_SELECT_IAV_NOW) / iav_cal - 100;

	en7571_tune_kt(priv, ratio_now >= 0 ? factor_ht : factor_lt);
}

/* --- Single-closed / open-loop (LUT) modes --- */

/* Switch to single-closed-loop mode (freeze MPDH, reset the loop). */
void en7571_single_cl_mode(struct en7571_priv *priv)
{
	en7571_dcl_stop(priv);
	en7571_mpdh_stepsize(priv, false);
	en7571_hw_reset(priv);
	en7571_dcl_start(priv);
}

/* Force both phases open-loop (or release them) via P0/P1_PWR_CTRL_CS3. */
void en7571_open_loop_mode(struct en7571_priv *priv, bool enable)
{
	u8 v = enable ? EN7571_ERC_OPEN_LOOP : 0;

	lddla_update8(&priv->lddla, EN7571_P0_PWR_CTRL_CS3, EN7571_ERC_OPEN_LOOP_MASK, v);
	lddla_update8(&priv->lddla, EN7571_P1_PWR_CTRL_CS3, EN7571_ERC_OPEN_LOOP_MASK, v);
}

/* Read one flash LUT anchor (12-bit Ibias in [27:16], Imod in [11:0]). */
static s32 en7571_lut_anchor(struct en7571_priv *priv, int fidx, bool imod)
{
	u32 off = EN7571_FL_LUT_BASE + fidx * 4;
	u32 w;

	if (off > EN7571_FL_LUT_END)
		off = EN7571_FL_LUT_END;
	w = lddla_flash_read(&priv->lddla, off);
	return imod ? (w & 0x0fff) : ((w >> 16) & 0x0fff);
}

/* Linear interpolation of an anchor pair at temperature @temp (degC). */
static s32 en7571_lut_interp(struct en7571_priv *priv, s32 temp, bool imod)
{
	int fidx = (temp + 40) / 10;
	s32 dn, up;

	fidx = clamp(fidx, 0, 15);
	dn = en7571_lut_anchor(priv, fidx, imod);
	up = en7571_lut_anchor(priv, min(fidx + 1, 15), imod);
	return dn + (temp + 40 - fidx * 10) * (up - dn) / 10;
}

/*
 * Rebuild the 64-entry bias/mod LUT from the 16 flash anchors, offset so the
 * interpolated value at the calibration temperature matches the calibrated
 * open-loop currents (flash 0x008).
 */
void en7571_lut_recover(struct en7571_priv *priv)
{
	u32 seed = lddla_flash_read(&priv->lddla, EN7571_FL_OPEN_LOOP_SEED);
	u32 envw = lddla_flash_read(&priv->lddla, EN7571_FL_ENV_OFFSET);
	s32 tcal, ibias_off, imod_off;
	int index;

	if (envw == EN7571_FLASH_ERASED || seed == EN7571_FLASH_ERASED) {
		dev_warn(priv->lddla.dev, "open-loop LUT not provisioned\n");
		return;
	}

	tcal = (s32)envw / 10;
	if (tcal < -40 || tcal > 110) {
		dev_warn(priv->lddla.dev, "env temperature out of range (%d)\n", tcal);
		return;
	}

	ibias_off = (s32)((seed & EN7571_FL_IAV_MASK) >> 16) -
		    en7571_lut_interp(priv, tcal, false);
	imod_off = (s32)(seed & EN7571_FL_IMOD_MASK) -
		   en7571_lut_interp(priv, tcal, true);

	for (index = 0; index < 64; index++) {
		s32 temp = (index * 25 - 400) / 10;

		priv->lut[index][0] = en7571_lut_interp(priv, temp, false) + ibias_off;
		priv->lut[index][1] = en7571_lut_interp(priv, temp, true) + imod_off;
	}
}

/* Open-loop tick: drive Ibias/Imod from the LUT at the ambient temperature. */
void en7571_lut_tracking(struct en7571_priv *priv)
{
	int idx;

	if (!priv->dol)			/* single-closed loop tracks in hardware */
		return;

	idx = ((priv->env_temp_mc / 1000) + 40) * 2 / 5;
	idx = clamp(idx, 0, 63);

	if (priv->lut[idx][0] == 0xfff || priv->lut[idx][1] == 0xfff)
		return;
	en7571_change_ibias(priv, priv->lut[idx][0]);
	en7571_change_imod(priv, priv->lut[idx][1]);
}
