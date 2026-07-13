// SPDX-License-Identifier: GPL-2.0
/*
 * EN7571 Digital Diagnostic Monitoring.
 *
 * Formats the on-die ADC / loop measurements into SFF-8472 monitoring words
 * and caches them for the host.  All conversions use the fixed-point unit
 * conventions documented in en7571.h with s64 intermediates to avoid overflow.
 *
 * Locking: callers hold priv->lddla.lock.
 */
#include <linux/kernel.h>
#include <linux/math64.h>

#include "en7571.h"

/* SFF-8472 temperature word: 1/256 degC, two's complement. */
u16 en7571_temp_ddmi(struct en7571_priv *priv)
{
	s32 t256;

	en7571_temp_update(priv);
	t256 = div_s64((s64)priv->ic_temp_mc * 256, 1000);
	priv->lddla.ddmi_temperature = (u16)t256;
	return priv->lddla.ddmi_temperature;
}

/* SFF-8472 supply-voltage word: 100 uV/LSB, with the on-die 2x divider. */
u16 en7571_vcc_ddmi(struct en7571_priv *priv)
{
	u32 code = en7571_adc_vcc(priv);
	s64 uv = en7571_adc_code_to_uv(priv, code);

	priv->lddla.ddmi_voltage = (u16)div_s64(2 * uv, 100);
	return priv->lddla.ddmi_voltage;
}

/* SFF-8472 Tx bias-current word (2 uA/LSB), internal or external calibration. */
u16 en7571_bias_ddmi(struct en7571_priv *priv)
{
	u32 code = en7571_info(priv, EN7571_SELECT_IBIAS_NOW);
	u32 ext = lddla_flash_read(&priv->lddla, EN7571_FL_EXTCAL_BIAS);
	s32 word;

	if (ext == EN7571_FLASH_ERASED) {
		/* Internal: 0.02442 mA/code over the 2 uA LSB. */
		word = (s32)code * 37500 / 3071;
	} else {
		u32 imod = en7571_info(priv, EN7571_SELECT_IMOD_NOW);

		word = (s32)((code * (ext & 0xffff)) >> 8) + (s16)(ext >> 16) +
		       (s32)(imod * 4650 / 4095);
	}
	priv->lddla.ddmi_current = clamp(word, 0, 0xffff);
	return priv->lddla.ddmi_current;
}

/* SFF-8472-style Tx modulation-current word (2 uA/LSB); not a standard field. */
u16 en7571_mod_ddmi(struct en7571_priv *priv)
{
	u32 code = en7571_info(priv, EN7571_SELECT_IMOD_NOW);
	u32 ext = lddla_flash_read(&priv->lddla, EN7571_FL_EXTCAL_MOD);
	s32 word;

	if (ext == EN7571_FLASH_ERASED)
		word = (s32)((code * 5627) >> 9);	/* internal: 0.02198 mA/code */
	else
		word = (s32)((code * (ext & 0xffff)) >> 8) + (s16)(ext >> 16);

	return clamp(word, 0, 0xffff);
}

/*
 * SFF-8472 Tx optical-power word: linear interpolation between the dark offset
 * (PWRADC_offset, power 0) and the single flash calibration point #1.  The
 * flash point holds Tx power in mW x100 in the upper half; scaling x100 reaches
 * the SFF-8472 0.1 uW unit.
 */
u16 en7571_tx_power_ddmi(struct en7571_priv *priv)
{
	u32 now = en7571_pwradc_get(priv);
	u32 dn = priv->pwradc_offset;
	u32 k1 = lddla_flash_read(&priv->lddla, EN7571_FL_DDMI_TX_P1);
	s32 pwradc_up = 0, tx_up = 0, tx_now = 0, div;

	if (k1 != EN7571_FLASH_ERASED) {
		pwradc_up = k1 & 0xffff;
		tx_up = (k1 >> 16) & 0xffff;	/* mW x100 */
	}

	div = pwradc_up - (s32)dn;
	if (div)
		tx_now = (s32)div_s64((s64)((s32)now - (s32)dn) * tx_up, div);
	else
		dev_dbg(priv->lddla.dev, "invalid Tx power point\n");

	/* mW x100 -> SFF-8472 0.1 uW; the word never reports zero. */
	priv->lddla.ddmi_tx_power = (tx_now == 0) ? 1 : (u16)(100 * tx_now);
	return priv->lddla.ddmi_tx_power;
}

/*
 * SFF-8472 Rx optical-power word.  Three calibration points hold the RSSI
 * current (lower half) and power in uW x100 (upper half, pre-divided by 10 to
 * reach the SFF-8472 0.1 uW unit).  The power is modelled as
 *   power = a*RSSI^2 + b*RSSI
 * i.e. power/RSSI is linear in RSSI; a and b are solved from the two
 * calibration points that bracket the present RSSI, carried in Q20.
 */
u16 en7571_rx_power_ddmi(struct en7571_priv *priv)
{
	u32 fp1 = lddla_flash_read(&priv->lddla, EN7571_FL_DDMI_RX_P1);
	u32 fp2 = lddla_flash_read(&priv->lddla, EN7571_FL_DDMI_RX_P2);
	u32 fp3 = lddla_flash_read(&priv->lddla, EN7571_FL_DDMI_RX_P3);
	s32 rssi1 = fp1 & 0xffff, k1 = ((fp1 >> 16) & 0xffff) / 10;
	s32 rssi2 = fp2 & 0xffff, k2 = ((fp2 >> 16) & 0xffff) / 10;
	s32 rssi3 = fp3 & 0xffff, k3 = ((fp3 >> 16) & 0xffff) / 10;
	u16 rssi = (u16)en7571_rssi_current(priv);
	s32 lo_r, lo_k, hi_r, hi_k;
	s64 y = 1, a, b, khi, klo;

	/* Pick the adjacent segment: points 2/3 below RSSI_2, else 1/2. */
	if (rssi <= rssi2) {
		lo_r = rssi2; lo_k = k2; hi_r = rssi3; hi_k = k3;
	} else {
		lo_r = rssi1; lo_k = k1; hi_r = rssi2; hi_k = k2;
	}

	if (hi_r && lo_r && hi_r != lo_r) {
		khi = div_s64((s64)hi_k << 20, hi_r);
		klo = div_s64((s64)lo_k << 20, lo_r);
		a = div_s64(khi - klo, hi_r - lo_r);
		b = khi - a * hi_r;
		y = (a * ((s64)rssi * rssi) + b * rssi) >> 19;
		y = (y + 1) >> 1;
	}

	y = clamp(y, (s64)1, (s64)65535);
	priv->lddla.ddmi_rx_power = y;
	return priv->lddla.ddmi_rx_power;
}
