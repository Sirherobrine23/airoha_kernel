// SPDX-License-Identifier: GPL-2.0
/*
 * EN7570 Digital Diagnostic Monitoring.
 *
 * Formats the on-die ADC measurements into SFF-8472 monitoring words and
 * caches them for the host.  All conversions use the fixed-point unit
 * conventions documented in en7570.h with s64 intermediates to avoid overflow.
 *
 * Locking: callers hold priv->lddla.lock.
 */
#include <linux/kernel.h>
#include <linux/math64.h>

#include "en7570.h"

/* SFF-8472 temperature word: 1/256 degC, two's complement. */
u16 en7570_temp_ddmi(struct en7570_priv *priv)
{
	s32 t256;

	en7570_temp_update(priv);
	t256 = div_s64((s64)priv->ic_temp_mc * 256, 1000);
	priv->lddla.ddmi_temperature = (u16)t256;
	return priv->lddla.ddmi_temperature;
}

/* SFF-8472 supply-voltage word: 100 uV/LSB, with the on-die 2x divider. */
u16 en7570_vcc_ddmi(struct en7570_priv *priv)
{
	u32 code = en7570_adc_vcc(priv);
	s64 uv = en7570_adc_code_to_uv(priv, code);

	priv->lddla.ddmi_voltage = (u16)div_s64(2 * uv, 100);
	return priv->lddla.ddmi_voltage;
}

/*
 * SFF-8472 Tx bias-current word (2 uA/LSB), and the fiber-plug detector
 *: a large positive jump in bias between ticks marks an
 * optical hot-plug; while plugged but the bias has not recovered the flag
 * is held.
 */
u16 en7570_bias_ddmi(struct en7570_priv *priv)
{
	u32 code = en7570_info(priv, EN7570_INFO_IBIAS);
	s32 ibias_ua = (s32)code * EN7570_BIAS_UA_PER_CODE_X100 / 100;
	u16 word = ibias_ua / EN7570_BIAS_8472_UA_PER_LSB;
	s32 delta = (s32)word - (s32)priv->lddla.ddmi_current;

	/*
	 * A large positive jump in bias between ticks marks an optical
	 * hot-plug.  While plugged but the bias has not recovered (< 1 mA)
	 * the flag is simply held set.
	 */
	if (delta > 500)
		priv->fiber_plug = 1;

	priv->lddla.ddmi_current = word;
	return word;
}

/*
 * SFF-8472 Tx optical-power word: two-point K-point interpolation.  The flash
 * K-points hold Tx power in mW x100 in the upper half; the result is scaled
 * x100 to reach the SFF-8472 0.1 uW unit.  The lower K-point defaults to the
 * MPD dark offset.
 */
u16 en7570_tx_power_ddmi(struct en7570_priv *priv)
{
	s32 mpd = en7570_mpd_current(priv);
	u32 k1 = lddla_flash_read(&priv->lddla, EN7570_FL_TX_K1);
	u32 k2 = lddla_flash_read(&priv->lddla, EN7570_FL_TX_K2);
	s32 mpd_up = 0, tx_up = 0;
	s32 mpd_dn = priv->mpd_current_offset, tx_dn = 0;
	s32 tx_power;

	if (k1 != EN7570_FLASH_ERASED) {
		mpd_up = k1 & 0xffff;
		tx_up = (k1 >> 16) & 0xffff;
	}
	if (k2 != EN7570_FLASH_ERASED) {
		mpd_dn = k2 & 0xffff;
		tx_dn = (k2 >> 16) & 0xffff;
	}

	if (mpd_up == mpd_dn)
		return priv->lddla.ddmi_tx_power;	/* avoid divide-by-zero */

	tx_power = tx_dn + div_s64((s64)(mpd - mpd_dn) * (tx_up - tx_dn),
				   mpd_up - mpd_dn);
	priv->lddla.ddmi_tx_power = clamp(100 * tx_power, 0, 0xffff);
	return priv->lddla.ddmi_tx_power;
}

/* Three-point Lagrange interpolation through (r,p) at RSSI value r. */
static s32 en7570_lagrange3(s32 r, s32 r0, s32 p0, s32 r1, s32 p1,
			    s32 r2, s32 p2)
{
	s64 d0 = (s64)(r0 - r1) * (r0 - r2);
	s64 d1 = (s64)(r1 - r0) * (r1 - r2);
	s64 d2 = (s64)(r2 - r0) * (r2 - r1);
	s64 acc = 0;

	if (d0)
		acc += div_s64((s64)p0 * (r - r1) * (r - r2), d0);
	if (d1)
		acc += div_s64((s64)p1 * (r - r0) * (r - r2), d1);
	if (d2)
		acc += div_s64((s64)p2 * (r - r0) * (r - r1), d2);
	return acc;
}

/* SFF-8472 Rx optical-power word: 3-segment linear or quadratic. */
u16 en7570_rx_power_ddmi(struct en7570_priv *priv)
{
	s32 rssi = en7570_rssi_current(priv);
	u32 k1 = lddla_flash_read(&priv->lddla, EN7570_FL_RX_K1);
	u32 k2 = lddla_flash_read(&priv->lddla, EN7570_FL_RX_K2);
	u32 k3 = lddla_flash_read(&priv->lddla, EN7570_FL_RX_K3);
	u32 k4 = lddla_flash_read(&priv->lddla, EN7570_FL_RX_K4);
	/* K-point order: K1 = up, K2 = md, K3 = dn. */
	s32 rssi_up = k1 & 0xffff, pwr_up = (k1 >> 16) & 0xffff;
	s32 rssi_md = k2 & 0xffff, pwr_md = (k2 >> 16) & 0xffff;
	s32 rssi_dn = k3 & 0xffff, pwr_dn = (k3 >> 16) & 0xffff;
	s32 rx_power;

	if ((k4 & 0xf) == 1) {
		/* Quadratic (Lagrange) fit over the three K-points. */
		rx_power = en7570_lagrange3(rssi, rssi_dn, pwr_dn, rssi_md, pwr_md,
					    rssi_up, pwr_up);
	} else if (rssi <= rssi_dn) {
		rx_power = rssi_dn ? (s32)div_s64((s64)rssi * pwr_dn, rssi_dn) : 0;
	} else if (rssi < rssi_md) {
		rx_power = pwr_dn + (s32)div_s64((s64)(rssi - rssi_dn) *
						 (pwr_md - pwr_dn),
						 rssi_md - rssi_dn);
	} else {
		rx_power = pwr_md + (s32)div_s64((s64)(rssi - rssi_md) *
						 (pwr_up - pwr_md),
						 rssi_up - rssi_md ?: 1);
	}

	rx_power = clamp(rx_power / 10, 0, 0xffff);
	priv->lddla.ddmi_rx_power = rx_power;
	return priv->lddla.ddmi_rx_power;
}
