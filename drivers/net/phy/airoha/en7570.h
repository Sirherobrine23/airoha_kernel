/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Airoha EN7570 xPON LDDLA controller driver.
 *
 * Per-device state and internal API.  Shared transport / flash / hwmon / SFP
 * live in the airoha_lddla core; this holds the EN7570-specific state and logic.
 * All arithmetic is fixed-point integer.
 *
 * Fixed-point unit conventions used throughout the driver:
 *   - temperature     : milli-degrees Celsius (m degC)
 *   - supply voltage  : microvolts (uV)
 *   - current         : microamps (uA)
 *   - optical power   : tenths of a microwatt (0.1 uW), matching SFF-8472
 *   - APD voltage     : millivolts (mV)
 *   - ADC slope       : nanovolts per ADC code (nV/code)
 *   - ADC offset      : microvolts (uV)
 */
#ifndef _EN7570_H
#define _EN7570_H

#include <linux/types.h>
#include <linux/workqueue.h>

#include "airoha_lddla.h"
#include "en7570_regs.h"

/* ETC (eye-tracking-correction) modes (flash offset 0x07c). */
#define EN7570_ETC_DISABLED		0
#define EN7570_ETC_STANDARD		1
#define EN7570_ETC_VENDOR		2
#define EN7570_ETC_BIAS_TRACK		3

/* DDMI refresh modes (flash offset 0x02c). */
#define EN7570_DDMI_OFF			0
#define EN7570_DDMI_ON			1
#define EN7570_DDMI_FAST		2

/*
 * Per-device state for one EN7570 LDDLA controller.  The shared airoha_lddla
 * object (transport / flash / hwmon / SFP) must be the first member.
 */
struct en7570_priv {
	struct airoha_lddla lddla;

	/* 1 Hz periodic state machine. */
	struct delayed_work tick_work;

	/* Runtime look-up table: 64 temperature steps x {ibias, imod} codes. */
	u16 lut[64][2];

	/* --- Mode flags --- */
	int internal_ddmi;	/* 0=off, 1=on, 2=fast */
	int fast_ddmi;
	int etc_mode;		/* 0..3 */
	int tec_en;		/* eye-correction enable */
	int tec_switch;		/* manual TEC override */
	int variant;		/* silicon variant byte (vEN7570) */
	int fiber_plug;		/* set on fiber hot-plug detection */
	int pattern_enabled;	/* calibration pattern is being transmitted */
	bool scl;		/* single-closed-loop selected */
	bool dol;		/* dual-open-loop selected */
	bool bias_track_sw;	/* runtime bias-tracking override */

	/* --- Calibration / measurement (fixed-point) --- */
	s64 adc_slope_nv;	/* nV per ADC code */
	s32 adc_offset_uv;	/* uV at ADC code 0 */
	s32 temp_slope_x10;	/* ADC codes per degC, x10 */
	s32 temp_offset_x10;	/* ADC code at 0 degC, x10 */
	s32 rssi_factor;	/* RSSI calibration multiplier (x1000) */

	s32 ic_temp_mc;		/* on-die temperature (m degC) */
	s32 bosa_temp_mc;	/* BOSA temperature (m degC) */
	s32 env_temp_mc;	/* ambient temperature (m degC) */
	s32 bosa_temp_offset_mc;
	s32 env_temp_offset_mc;

	s32 apd_voltage_mv;	/* last APD voltage written (mV) */
	s32 apd_slope_up_uv;	/* uV/degC above knee */
	s32 apd_slope_dn_uv;	/* uV/degC below knee */
	s32 apd_knee_mv;	/* knee voltage (mV) */

	u32 bias_code;		/* last Ibias register code read */
	u32 mod_code;		/* last Imod register code read */
	u32 mpdl;		/* last MPDL written */
	u32 mpdh;		/* last MPDH written */

	s32 rssi_current;	/* RSSI photocurrent (raw) */
	s32 mpd_current;	/* MPD photocurrent (raw, post-offset) */
	s32 mpd_current_offset;	/* MPD dark current (calibration baseline) */
	s32 mpd_current_var;	/* drift variance used by ETC */
	s32 rssi_vref;		/* RSSI reference code (no optical signal) */
	s32 rssi_v;		/* RSSI measurement code at cal */

	u32 t_apd;		/* APD update interval (s) */
	u32 bosa_lth_ua;	/* BOSA threshold current (uA) */

	/* Free-running 1 Hz tick counter and helpers. */
	u32 cnt7570;
	s32 mpd_cal_cnt;
	int tec_cnt;
};

/*
 * Map a temperature (in milli-degC) to a LUT index: 64 entries spaced 2.5 degC
 * apart, starting at -40 degC, clamped to the valid range.
 */
static inline int en7570_temp_index(s32 temp_mc)
{
	int idx = (temp_mc + 40000) / 2500;

	return clamp(idx, 0, 63);
}

/* --- Init / reset (en7570_main.c) --- */
int en7570_detect(struct en7570_priv *priv);
void en7570_sw_reset(struct en7570_priv *priv);
int en7570_init(struct en7570_priv *priv);
void en7570_tick(struct en7570_priv *priv);
void en7570_fiber_plug(struct en7570_priv *priv);

/* --- ADC (en7570_adcloop.c) --- */
int en7570_adc_calibrate(struct en7570_priv *priv);
int en7570_adc_sample(struct en7570_priv *priv, u8 channel, int samples, u32 *out);
u16 en7570_adc_temp(struct en7570_priv *priv);
u16 en7570_adc_vcc(struct en7570_priv *priv);
s64 en7570_adc_code_to_uv(struct en7570_priv *priv, u32 code);
void en7570_temp_update(struct en7570_priv *priv);

/* --- Tx path (en7570_txrx.c) --- */
void en7570_tx_load_init_current(struct en7570_priv *priv);
void en7570_tx_load_mpd_targets(struct en7570_priv *priv);
void en7570_tx_sd_level(struct en7570_priv *priv);
int en7570_tx_sd_cal(struct en7570_priv *priv);
void en7570_mpd_dark(struct en7570_priv *priv);
s32 en7570_mpd_current(struct en7570_priv *priv);
u32 en7570_info(struct en7570_priv *priv, u8 channel);
int en7570_tgen(struct en7570_priv *priv, int mode);
void en7570_erc_filter(struct en7570_priv *priv);
void en7570_erc_restart(struct en7570_priv *priv);
void en7570_erc_restart_p0(struct en7570_priv *priv);
void en7570_cdr(struct en7570_priv *priv, bool enable);
int en7570_pattern_start(struct en7570_priv *priv, int mode);
void en7570_pattern_stop(struct en7570_priv *priv);
void en7570_apd_init(struct en7570_priv *priv);
void en7570_apd_update(struct en7570_priv *priv);
void en7570_apd_write(struct en7570_priv *priv, u8 code);
void en7570_rogue_clear(struct en7570_priv *priv);
void en7570_safe_reset(struct en7570_priv *priv);
void en7570_tx_alarms(struct en7570_priv *priv);

/* --- Rx path (en7570_txrx.c) --- */
void en7570_rssi_gain_init(struct en7570_priv *priv);
int en7570_rssi_cal(struct en7570_priv *priv);
s32 en7570_rssi_current(struct en7570_priv *priv);
s32 en7570_rx_dark(struct en7570_priv *priv);
void en7570_rx_alarms(struct en7570_priv *priv);
void en7570_los_level(struct en7570_priv *priv);
void en7570_los_init(struct en7570_priv *priv);
void en7570_los_cal(struct en7570_priv *priv, u8 high, u8 low);
void en7570_rx_tia_gain(struct en7570_priv *priv);

/* --- DDMI (en7570_ddmi.c) --- */
u16 en7570_temp_ddmi(struct en7570_priv *priv);
u16 en7570_vcc_ddmi(struct en7570_priv *priv);
u16 en7570_bias_ddmi(struct en7570_priv *priv);
u16 en7570_tx_power_ddmi(struct en7570_priv *priv);
u16 en7570_rx_power_ddmi(struct en7570_priv *priv);

/* --- Loop control / LUT / ETC (en7570_adcloop.c) --- */
void en7570_lut_default(struct en7570_priv *priv);
void en7570_mode_dual_cl(struct en7570_priv *priv);
void en7570_mode_single_cl(struct en7570_priv *priv);
void en7570_mode_open(struct en7570_priv *priv);
void en7570_loop_open_track(struct en7570_priv *priv);
void en7570_bias_track(struct en7570_priv *priv);
void en7570_lut_recover(struct en7570_priv *priv);
void en7570_lut_dump(struct en7570_priv *priv);
void en7570_etc_std(struct en7570_priv *priv);
void en7570_etc_sol(struct en7570_priv *priv);
void en7570_tec(struct en7570_priv *priv);

#endif /* _EN7570_H */
