// SPDX-License-Identifier: GPL-2.0
/*
 * EN7570 shared ADC subsystem and loop control.
 *
 * ADC / temperature sensor: a single auto-ranging ADC is multiplexed across
 * temperature, supply voltage, the monitor photodiode, RSSI and the bandgap
 * references.  Every per-channel read follows the same five-step pattern:
 * select channel, latch, read the 16-bit sample, average, restore the mux.
 * The channel select uses the discrete SVADC_PD byte-0 enable bits
 * (temperature 0x08, RSSI 0x02, MPD/TxPW 0x04, voltage field 0); the 0.875 V
 * bandgap is selected in byte 1 under mask 0xb3.  Temperature uses a
 * voltage-domain linear fit.
 *
 * Loop control: loop-control modes, look-up-table recovery and eye-tracking /
 * temperature compensation.  The MPD targets occupy register 0x0004 with MPDH
 * (= flash_P1_target) in bytes 0-1 and MPDL (= flash_P0_target) in bytes 2-3;
 * the per-temperature flash anchors pack Ibias in the upper 12 bits and Imod in
 * the lower 12 bits.
 *
 * Locking: callers hold priv->lddla.lock, which keeps the multi-step
 * channel-select/latch/read sequence atomic.
 */
#include <linux/kernel.h>
#include <linux/math64.h>

#include "en7570.h"

/* ------------------------------------------------------------------ */
/* ADC / temperature sensor					      */
/* ------------------------------------------------------------------ */

/* Select an ADC channel in SVADC_PD (byte 0, or byte 1 for the 0.875 V ref). */
static void en7570_adc_select(struct en7570_priv *priv, u8 channel)
{
	if (channel == EN7570_ADC_CH_BG0V875)
		lddla_update8(&priv->lddla, EN7570_SVADC_PD + 1, EN7570_ADC_BG0V875_MASK,
			       channel & ~EN7570_ADC_BG0V875_MASK);
	else
		lddla_update8(&priv->lddla, EN7570_SVADC_PD, EN7570_ADC_SELECT_MASK,
			       channel & ~EN7570_ADC_SELECT_MASK);
}

/* Restore the SVADC_PD channel field to its default (cleared). */
static void en7570_adc_restore(struct en7570_priv *priv, u8 channel)
{
	if (channel == EN7570_ADC_CH_BG0V875)
		lddla_update8(&priv->lddla, EN7570_SVADC_PD + 1, EN7570_ADC_BG0V875_MASK, 0);
	else
		lddla_update8(&priv->lddla, EN7570_SVADC_PD, EN7570_ADC_SELECT_MASK, 0);
}

/**
 * en7570_adc_sample() - run the generic five-step ADC read.
 * @priv:    device
 * @channel: EN7570_ADC_CH_* select value
 * @samples: number of latches to average (1, 4, 8 or 64)
 * @out:     averaged 16-bit ADC code (rounded to nearest)
 */
int en7570_adc_sample(struct en7570_priv *priv, u8 channel, int samples,
		      u32 *out)
{
	u32 sum = 0;
	int i, ret;
	u16 v;

	if (samples < 1)
		samples = 1;

	en7570_adc_select(priv, channel);

	for (i = 0; i < samples; i++) {
		/* Latch a fresh conversion (PROBE_CONTROL byte 1 bit 4). */
		ret = lddla_update8(&priv->lddla, EN7570_PROBE_CONTROL + 1,
				     EN7570_ADC_LATCH_MASK, EN7570_ADC_LATCH);
		if (ret)
			return ret;

		ret = lddla_rd16(&priv->lddla, EN7570_ADC_PROBE_STATUS, &v);
		if (ret)
			return ret;
		sum += v;
	}

	en7570_adc_restore(priv, channel);

	*out = (sum + samples / 2) / samples;	/* round to nearest */
	return 0;
}

/* Convert a raw ADC code to microvolts using the two-point calibration. */
s64 en7570_adc_code_to_uv(struct en7570_priv *priv, u32 code)
{
	return div_s64(priv->adc_slope_nv * code, 1000) + priv->adc_offset_uv;
}

/**
 * en7570_adc_calibrate() - two-point bandgap calibration.
 * @priv: device
 *
 * Establishes a linear ADC-code -> voltage conversion from the on-die 1.76 V
 * and 0.875 V bandgap references.  Rerun periodically to track slow drift.
 */
int en7570_adc_calibrate(struct en7570_priv *priv)
{
	u32 bg1v76, bg0v875;
	s32 delta;
	int ret;

	ret = en7570_adc_sample(priv, EN7570_ADC_CH_BG1V76, 8, &bg1v76);
	if (ret)
		return ret;
	ret = en7570_adc_sample(priv, EN7570_ADC_CH_BG0V875, 8, &bg0v875);
	if (ret)
		return ret;

	delta = (s32)bg1v76 - (s32)bg0v875;
	if (delta > EN7570_ADC_BANDGAP_MIN_DELTA) {
		priv->adc_slope_nv =
			div_s64(EN7570_BG_1V76_NV - EN7570_BG_0V875_NV, delta);
	} else {
		/* ADC stuck at bandgap: slope would be infinite, use default. */
		dev_warn(priv->lddla.dev,
			 "ADC bandgap spread too small (%d); using default slope\n",
			 delta);
		priv->adc_slope_nv = EN7570_DEFAULT_SLOPE_NV;
	}

	priv->adc_offset_uv = 1760000 - div_s64(priv->adc_slope_nv * bg1v76, 1000);
	return 0;
}

/* Averaged on-die temperature ADC code. */
u16 en7570_adc_temp(struct en7570_priv *priv)
{
	u32 code = 0;

	en7570_adc_sample(priv, EN7570_ADC_CH_TEMPERATURE, 8, &code);
	return code;
}

/* Averaged supply-voltage ADC code (toggles the Rx high-Z control). */
u16 en7570_adc_vcc(struct en7570_priv *priv)
{
	u32 code = 0;

	/* Disable Rx high-Z so the supply-voltage divider is sampled cleanly. */
	lddla_update8(&priv->lddla, EN7570_LA_PWD, EN7570_LA_RX_HIGHZ_MASK,
		       EN7570_LA_RX_HIGHZ);
	en7570_adc_sample(priv, EN7570_ADC_CH_VOLTAGE, 1, &code);
	lddla_update8(&priv->lddla, EN7570_LA_PWD, EN7570_LA_RX_HIGHZ_MASK, 0);
	return code;
}

/**
 * en7570_temp_update() - refresh the IC / BOSA / ambient temperatures.
 * @priv: device
 *
 * Voltage-domain linear fit:
 *   sensor_voltage = ADC_slope * code + ADC_offset
 *   IC_temp        = T_V_offset - T_V_slope * sensor_voltage
 * with T_V_offset / T_V_slope held x10 (default 4958 / 3275 = 495.8 / 327.5).
 */
void en7570_temp_update(struct en7570_priv *priv)
{
	u32 raw = en7570_adc_temp(priv);
	s64 sensor_uv = en7570_adc_code_to_uv(priv, raw);

	priv->ic_temp_mc = 100 * priv->temp_offset_x10 -
		div_s64((s64)priv->temp_slope_x10 * sensor_uv, 10000);
	priv->bosa_temp_mc = priv->ic_temp_mc - priv->bosa_temp_offset_mc;
	priv->env_temp_mc = priv->ic_temp_mc - priv->env_temp_offset_mc;
}

/* ------------------------------------------------------------------ */
/* Loop control						              */
/* ------------------------------------------------------------------ */

/* Write a 10-bit current code to a P{0,1}_PWR_CTRL_CS2 register (2 bytes). */
static void en7570_write_current(struct en7570_priv *priv, u16 reg, u32 code)
{
	u8 b[2] = { code & 0xff, (code >> 8) & 0xff };

	lddla_wr(&priv->lddla, reg, b, 2);
}

/* Write a 10-bit MPD target: byteoff 0 -> MPDH (bytes 0-1), 2 -> MPDL (2-3). */
static void en7570_write_mpd(struct en7570_priv *priv, u16 byteoff, u32 val)
{
	u8 b[2] = { val & 0xff, (val >> 8) & 0xff };

	lddla_wr(&priv->lddla, EN7570_MPDH + byteoff, b, 2);
}

/*
 * Built-in modulation/bias LUT, 64 entries from -40 degC in 2.5 degC steps.
 * The top entries are clamped to {0x666, 0x6ce} so an over-temperature event
 * cannot drive a destructive laser current.
 */
void en7570_lut_default(struct en7570_priv *priv)
{
	static const u16 lut[64][2] = {
		{ 0x106, 0x3b2 }, { 0x10a, 0x3b2 }, { 0x10f, 0x3b6 }, { 0x118, 0x3bb },
		{ 0x11d, 0x3bb }, { 0x121, 0x3c0 }, { 0x12a, 0x3c4 }, { 0x12f, 0x3c4 },
		{ 0x133, 0x3c9 }, { 0x13c, 0x3cd }, { 0x141, 0x3cd }, { 0x146, 0x3d2 },
		{ 0x14e, 0x3d6 }, { 0x157, 0x3d8 }, { 0x164, 0x3e4 }, { 0x171, 0x3ed },
		{ 0x17e, 0x3f6 }, { 0x18b, 0x3ff }, { 0x198, 0x408 }, { 0x1a5, 0x411 },
		{ 0x1ad, 0x416 }, { 0x1ba, 0x41f }, { 0x1c7, 0x428 }, { 0x1d4, 0x431 },
		{ 0x1e1, 0x43a }, { 0x1ee, 0x444 }, { 0x1fb, 0x44d }, { 0x214, 0x45a },
		{ 0x223, 0x468 }, { 0x236, 0x476 }, { 0x245, 0x488 }, { 0x258, 0x495 },
		{ 0x268, 0x4a3 }, { 0x277, 0x4b1 }, { 0x28a, 0x4c3 }, { 0x299, 0x4d1 },
		{ 0x2ac, 0x4de }, { 0x2bb, 0x4ec }, { 0x2cf, 0x4fe }, { 0x2fe, 0x510 },
		{ 0x332, 0x527 }, { 0x362, 0x53e }, { 0x396, 0x555 }, { 0x3c6, 0x56b },
		{ 0x3fa, 0x582 }, { 0x46b, 0x5b9 }, { 0x4dd, 0x5ef }, { 0x54e, 0x626 },
		{ 0x5c4, 0x65c }, { 0x635, 0x693 }, { 0x666, 0x6ce }, { 0x666, 0x6ce },
		{ 0x666, 0x6ce }, { 0x666, 0x6ce }, { 0x666, 0x6ce }, { 0x666, 0x6ce },
		{ 0x666, 0x6ce }, { 0x666, 0x6ce }, { 0x666, 0x6ce }, { 0x666, 0x6ce },
		{ 0x666, 0x6ce }, { 0x666, 0x6ce }, { 0x666, 0x6ce }, { 0x666, 0x6ce },
	};

	memcpy(priv->lut, lut, sizeof(priv->lut));
}

/* Read a flash LUT anchor: Ibias in upper 12 bits, Imod in lower 12 bits. */
static u32 en7570_anchor(struct en7570_priv *priv, int idx, bool imod)
{
	u32 w = lddla_flash_read(&priv->lddla, EN7570_FL_LUT_BASE + idx * 4);

	return imod ? (w & 0x0fff) : ((w >> 16) & 0x0fff);
}

/**
 * en7570_lut_recover() - rebuild the runtime LUT from 16 flash anchors
 *.
 * @priv: device
 *
 * DOL recovers both Ibias and Imod by direct interpolation; closed-loop
 * variants recover only Ibias, offset so the calibration anchor matches the
 * flash initial bias.
 */
void en7570_lut_recover(struct en7570_priv *priv)
{
	int index, fidx;
	s32 frac_x10, base, up;

	if (priv->dol) {
		for (index = 0; index < 64; index++) {
			fidx = clamp(index / 4, 0, 15);
			frac_x10 = index * 25 - fidx * 100;

			base = en7570_anchor(priv, fidx, false);
			up = en7570_anchor(priv, min(fidx + 1, 15), false);
			if (base == 0xfff || up == 0xfff)
				priv->lut[index][0] = 0xfff;
			else
				priv->lut[index][0] = base + (up - base) * frac_x10 / 100;

			base = en7570_anchor(priv, fidx, true);
			up = en7570_anchor(priv, min(fidx + 1, 15), true);
			if (base == 0xfff || up == 0xfff)
				priv->lut[index][1] = 0xfff;
			else
				priv->lut[index][1] = base + (up - base) * frac_x10 / 100;
		}
		/* Force exact anchor values at the 10 degC indices. */
		for (fidx = 0; fidx <= 15; fidx++) {
			if (en7570_anchor(priv, fidx, false) != 0xfff)
				priv->lut[fidx * 4][0] = en7570_anchor(priv, fidx, false);
			if (en7570_anchor(priv, fidx, true) != 0xfff)
				priv->lut[fidx * 4][1] = en7570_anchor(priv, fidx, true);
		}
	} else {
		s32 tcal_x10 = 250;	/* default 25.0 degC */
		s32 init_ibias, ibias_cal, offset;
		u32 etc1 = lddla_flash_read(&priv->lddla, EN7570_FL_RESERVED_ETC1);

		if (etc1 != EN7570_FLASH_ERASED)
			tcal_x10 = etc1 & 0xffff;	/* temp x10 */

		fidx = clamp((int)((tcal_x10 / 10 + 40) / 10), 0, 15);
		if (lddla_flash_read(&priv->lddla, EN7570_FL_LUT_BASE + fidx * 4) ==
		    EN7570_FLASH_ERASED) {
			dev_warn(priv->lddla.dev, "lack of bias-current table\n");
			return;
		}

		base = en7570_anchor(priv, fidx, false);
		up = en7570_anchor(priv, min(fidx + 1, 15), false);
		ibias_cal = base + (tcal_x10 - (100 * fidx - 400)) * (up - base) / 100;
		init_ibias = lddla_flash_read(&priv->lddla, EN7570_FL_IBIAS_INIT);
		if (init_ibias == EN7570_FLASH_ERASED)
			init_ibias = ibias_cal;
		offset = init_ibias - ibias_cal;

		for (index = 0; index < 64; index++) {
			fidx = clamp(index / 4, 0, 15);
			frac_x10 = index * 25 - fidx * 100;
			base = en7570_anchor(priv, fidx, false);
			up = en7570_anchor(priv, min(fidx + 1, 15), false);
			if (base == 0xfff || up == 0xfff) {
				priv->lut[index][0] = 0xfff;
				continue;
			}
			priv->lut[index][0] =
				base + (up - base) * frac_x10 / 100 + offset;
			if ((s32)priv->lut[index][0] <= 0) {
				dev_warn(priv->lddla.dev, "LUT recover failed\n");
				return;
			}
		}
		for (fidx = 0; fidx <= 15; fidx++)
			if (en7570_anchor(priv, fidx, false) != 0xfff)
				priv->lut[fidx * 4][0] =
					en7570_anchor(priv, fidx, false) + offset;
	}
}

void en7570_lut_dump(struct en7570_priv *priv)
{
	int t;

	for (t = 0; t < 64; t++)
		dev_info(priv->lddla.dev, "LUT[%2d] %+5d.%01u degC ibias=0x%03x imod=0x%03x\n",
			 t, (t * 25 - 400) / 10, abs(t * 25 - 400) % 10,
			 priv->lut[t][0], priv->lut[t][1]);
}

/* --- Loop-control modes --- */

/* Dual-closed-loop: clear open-loop, set ERC_start on P0 and P1. */
void en7570_mode_dual_cl(struct en7570_priv *priv)
{
	lddla_update8(&priv->lddla, EN7570_P0_PWR_CTRL_CS3, EN7570_ERC_OPEN_LOOP_MASK,
		       EN7570_ERC_START);
	lddla_update8(&priv->lddla, EN7570_P1_PWR_CTRL_CS3, EN7570_ERC_OPEN_LOOP_MASK,
		       EN7570_ERC_START);
}

/* Single-closed-loop: P0 open, P1 closed. */
void en7570_mode_single_cl(struct en7570_priv *priv)
{
	lddla_update8(&priv->lddla, EN7570_P0_PWR_CTRL_CS3, EN7570_ERC_OPEN_LOOP_MASK,
		       EN7570_ERC_OPEN_LOOP);
	lddla_update8(&priv->lddla, EN7570_P1_PWR_CTRL_CS3, EN7570_ERC_OPEN_LOOP_MASK,
		       EN7570_ERC_START);
}

/* Dual-open-loop: both rails open. */
void en7570_mode_open(struct en7570_priv *priv)
{
	lddla_update8(&priv->lddla, EN7570_P0_PWR_CTRL_CS3, EN7570_ERC_OPEN_LOOP_MASK,
		       EN7570_ERC_OPEN_LOOP);
	lddla_update8(&priv->lddla, EN7570_P1_PWR_CTRL_CS3, EN7570_ERC_OPEN_LOOP_MASK,
		       EN7570_ERC_OPEN_LOOP);
}

/* DOL tick handler: drive Ibias/Imod open-loop from the LUT. */
void en7570_loop_open_track(struct en7570_priv *priv)
{
	int idx = en7570_temp_index(priv->env_temp_mc);

	if (priv->lut[idx][0] == 0xfff || priv->lut[idx][1] == 0xfff)
		return;
	en7570_write_current(priv, EN7570_P0_PWR_CTRL_CS2, priv->lut[idx][0]);
	en7570_write_current(priv, EN7570_P1_PWR_CTRL_CS2, priv->lut[idx][1]);
}

/* Bias tracking: load LUT target Ibias and nudge MPDL toward it. */
void en7570_bias_track(struct en7570_priv *priv)
{
	int idx = en7570_temp_index(priv->env_temp_mc);
	u32 target = priv->lut[idx][0];
	u32 now = priv->bias_code;	/* last Ibias readback */
	u8 mode = 0;

	if (target == 0xfff)
		return;
	en7570_write_current(priv, EN7570_P0_PWR_CTRL_CS2, target);

	lddla_rd8(&priv->lddla, EN7570_P0_PWR_CTRL_CS3, &mode);
	if ((mode & 0x0f) != 0x05)	/* only in the dual-closed-loop signature */
		return;

	if (target > now && target - now > 0x14)
		en7570_write_mpd(priv, 0x02, en7570_info(priv, EN7570_INFO_P0) + 0x4);
	else if (target < now && now - target > 0x14)
		en7570_write_mpd(priv, 0x02, en7570_info(priv, EN7570_INFO_P0) - 0x4);
}

/* --- ETC / temperature compensation --- */

/* Hi-zone (ic > norm) MPD-target compensation for one delta byte. */
static s32 en7570_etc_hi(s32 ic, s32 norm, u8 d)
{
	if (ic > norm + 25)
		return d;
	return (ic - norm) * d / 25;
}

/* Lo-zone (ic < norm-20) MPD-target compensation for one delta byte. */
static s32 en7570_etc_lo(s32 ic, s32 norm, u8 d)
{
	if (ic < norm - 30)
		return -(s32)d;
	return (ic - (norm - 20)) * d / 10;
}

/* Standard ETC: compensate the MPD targets around a calibration anchor. */
void en7570_etc_std(struct en7570_priv *priv)
{
	u32 etc1, delta, p0t, p1t;
	s32 ic, norm = 40, p0comp = 0, p1comp = 0;
	u8 p0_hi, p1_hi, p0_lo, p1_lo;

	if (priv->scl || priv->dol)		/* skipped in SCL / DOL */
		return;

	etc1 = lddla_flash_read(&priv->lddla, EN7570_FL_RESERVED_ETC1);
	delta = lddla_flash_read(&priv->lddla, EN7570_FL_ETC_HI_LO_DELTA);
	if (etc1 != EN7570_FLASH_ERASED)
		norm = etc1 & 0xff;		/* norm_temp in degrees */

	p0_hi = delta & 0xff;
	p1_hi = (delta >> 8) & 0xff;
	p0_lo = (delta >> 16) & 0xff;
	p1_lo = (delta >> 24) & 0xff;

	ic = priv->ic_temp_mc / 1000;
	p0t = lddla_flash_read(&priv->lddla, EN7570_FL_P0_TARGET) & 0x3ff;
	p1t = lddla_flash_read(&priv->lddla, EN7570_FL_P1_TARGET) & 0x3ff;

	if (ic > norm && (p0_hi + p1_hi) != 0) {
		p0comp = en7570_etc_hi(ic, norm, p0_hi);
		p1comp = en7570_etc_hi(ic, norm, p1_hi);
	} else if (ic < norm - 20 && (p0_lo + p1_lo) != 0) {
		p0comp = en7570_etc_lo(ic, norm, p0_lo);
		p1comp = en7570_etc_lo(ic, norm, p1_lo);
	}

	/* MPDL (bytes 2-3) <- P0_target + comp; MPDH (bytes 0-1) <- P1_target. */
	en7570_write_mpd(priv, 0x02, p0t + p0comp);
	en7570_write_mpd(priv, 0x00, p1t + p1comp);
	en7570_tx_sd_level(priv);
}

/* Single-open-loop ETC: 4-segment ibias compensation. */
void en7570_etc_sol(struct en7570_priv *priv)
{
	u32 slopes = lddla_flash_read(&priv->lddla, EN7570_FL_IBIAS_SLOPE);
	u32 etc1 = lddla_flash_read(&priv->lddla, EN7570_FL_RESERVED_ETC1);
	s32 nt = (etc1 == EN7570_FLASH_ERASED) ? 40 : (etc1 & 0xff);
	s32 lt = nt - 30, ht = nt + 30;
	s8 s1 = slopes & 0xff, s2 = (slopes >> 8) & 0xff;
	s8 s3 = (slopes >> 16) & 0xff, s4 = (slopes >> 24) & 0xff;
	s32 ic = priv->ic_temp_mc / 1000, ibias;

	if (ic > ht)
		ibias = priv->lut[en7570_temp_index(priv->ic_temp_mc)][0] + s4 * (ic - ht);
	else if (ic > nt)
		ibias = priv->lut[en7570_temp_index(priv->ic_temp_mc)][0] + s3 * (ic - nt);
	else if (ic > lt)
		ibias = priv->lut[en7570_temp_index(priv->ic_temp_mc)][0] + s2 * (ic - lt);
	else
		ibias = priv->lut[en7570_temp_index(priv->ic_temp_mc)][0] - s1 * (lt - ic);

	/* Force P0 open-loop and clamp the bias to a safe floor. */
	lddla_update8(&priv->lddla, EN7570_P0_PWR_CTRL_CS3, EN7570_ERC_OPEN_LOOP_MASK,
		       EN7570_ERC_OPEN_LOOP);
	if (ibias < 0x25)
		ibias = 0x25;
	en7570_write_current(priv, EN7570_P0_PWR_CTRL_CS2, ibias & 0xfff);
}

/* Tx eye correction: rescue the eye when bias falls below BOSA_lth. */
void en7570_tec(struct en7570_priv *priv)
{
	u8 mode = 0;

	if (priv->tec_cnt > 7)			/* limited to 8 nudges */
		return;

	/* DDMI bias word is 2 uA/LSB; <<1 -> uA, compared to BOSA Ith (uA). */
	if (((s32)priv->lddla.ddmi_current << 1) >= (s32)priv->bosa_lth_ua)
		return;

	lddla_rd8(&priv->lddla, EN7570_P0_PWR_CTRL_CS3, &mode);
	if ((mode & 0x0f) != 0x05)		/* dual-closed-loop signature */
		return;

	en7570_write_mpd(priv, 0x02, en7570_info(priv, EN7570_INFO_P0) + 0x4);
	en7570_erc_restart_p0(priv);
	priv->tec_cnt++;
}
