/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Airoha EN7571 xPON LDDLA controller driver.
 *
 * Per-device state and internal API.  Shared transport / flash / hwmon / SFP
 * live in the airoha_lddla core; this holds the EN7571-specific state and logic.
 * All arithmetic is fixed-point integer (kernel space has no FPU).
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
#ifndef _EN7571_H
#define _EN7571_H

#include <linux/types.h>
#include <linux/workqueue.h>

#include "airoha_lddla.h"
#include "en7571_regs.h"

/* Driver version reported in diagnostics. */
#define EN7571_VERSION			3
#define EN7571_DEFAULT_FW		"airoha/en7571_bob.bin"

/*
 * Per-device state for one EN7571 LDDLA controller.  The shared airoha_lddla
 * object (transport / flash / hwmon / SFP) must be the first member.
 */
struct en7571_priv {
	struct airoha_lddla lddla;

	/* 1 Hz periodic state machine. */
	struct delayed_work tick_work;

	/* --- Mode flags --- */
	int internal_ddmi;	/* 0=off, 1=on, 2=fast */
	int fast_ddmi;		/* sample all DDMI every tick (not round-robin) */
	int ver;		/* silicon revision (2 = rev 2) */
	int kt;			/* EN7571_KT_* temperature-compensation mode */
	int scl;		/* single-closed-loop mode (flash LOOP_SEL=1) */
	int dol;		/* open-loop / LUT mode (flash LOOP_SEL=2) */
	int cross;		/* use the rev-2 MBI cross-Imod fast path */
	u32 mbi_delay;		/* cross-Imod MBI settle delay (us), flash 0x060 */
	int pattern_enabled;	/* calibration pattern is being transmitted */

	/* --- Calibration / measurement (fixed-point) --- */
	s64 adc_slope_nv;	/* nV per ADC code */
	s32 adc_offset_uv;	/* uV at ADC code 0 */
	s32 temp_slope_x10;	/* degC/V scale, x10 */
	s32 temp_offset_x10;	/* ADC code at 0 degC, x10 */
	s32 efuse_offset_mc;	/* eFuse temperature correction (m degC) */
	s32 rssi_factor;	/* RSSI calibration multiplier (x1000) */

	s32 ic_temp_mc;		/* on-die temperature (m degC) */
	s32 bosa_temp_mc;	/* BOSA temperature (m degC) */
	s32 env_temp_mc;	/* ambient temperature (m degC) */
	s32 bosa_temp_offset_mc;
	s32 env_temp_offset_mc;

	/* Open-loop / LUT mode: 64 temperature steps x {ibias, imod} codes. */
	u16 lut[64][2];

	s32 apd_voltage_mv;	/* last APD voltage written (mV) */
	s32 apd_slope_up_uv;	/* uV/degC above knee */
	s32 apd_slope_dn_uv;	/* uV/degC below knee */
	s32 apd_knee_mv;	/* APD voltage at 25 degC (mV) */

	/* --- Loop set-point readback cache (en7571_info) --- */
	u32 iav_now;
	u32 ibias_now;
	u32 imod_now;
	u32 pav_cal;
	u32 p1_cal;

	s32 rssi_vref;		/* RSSI Vref ADC code */
	s32 rssi_v;		/* RSSI measurement code at cal */
	s32 rssi_current;	/* RSSI photocurrent (raw) */
	u32 pwradc;		/* last MPD power ADC */
	u32 pwradc_offset;	/* MPD dark/offset (Tx off) */

	u32 t_apd;		/* APD update interval (s) */

	/* Free-running 1 Hz tick counter. */
	u32 cnt;
};

/* --- Init / reset (en7571_main.c) --- */
int en7571_detect(struct en7571_priv *priv);
void en7571_sw_reset(struct en7571_priv *priv);
void en7571_hw_reset(struct en7571_priv *priv);
int en7571_init(struct en7571_priv *priv);
void en7571_tick(struct en7571_priv *priv);

/* --- ADC (en7571_adcloop.c) --- */
u32 en7571_svadc_get(struct en7571_priv *priv);
int en7571_adc_calibrate(struct en7571_priv *priv);
int en7571_adc_sample(struct en7571_priv *priv, u8 channel, int samples, u32 *out);
s64 en7571_adc_code_to_uv(struct en7571_priv *priv, u32 code);
u16 en7571_adc_temp(struct en7571_priv *priv);
u16 en7571_adc_vcc(struct en7571_priv *priv);
void en7571_temp_update(struct en7571_priv *priv);
void en7571_efuse_temp(struct en7571_priv *priv);
void en7571_pwradc_calibration(struct en7571_priv *priv);
void en7571_pwradc_enable(struct en7571_priv *priv);
u32 en7571_pwradc_get(struct en7571_priv *priv);

/* --- Tx path / power-control loop (en7571_txrx.c) --- */
void en7571_7571_enable(struct en7571_priv *priv);
void en7571_safe_reset(struct en7571_priv *priv);
void en7571_dcl_start(struct en7571_priv *priv);
void en7571_dcl_stop(struct en7571_priv *priv);
void en7571_reg_init(struct en7571_priv *priv);
void en7571_force_mode(struct en7571_priv *priv);
void en7571_auto_lock_mode(struct en7571_priv *priv);
void en7571_mpdh_stepsize(struct en7571_priv *priv, bool enable);
void en7571_burst_ctrl(struct en7571_priv *priv);
void en7571_t1delay_setting(struct en7571_priv *priv, bool enable);
void en7571_link_reg(struct en7571_priv *priv, bool enable);
void en7571_single_cl_mode(struct en7571_priv *priv);
void en7571_open_loop_mode(struct en7571_priv *priv, bool enable);
void en7571_lut_recover(struct en7571_priv *priv);
void en7571_lut_tracking(struct en7571_priv *priv);
void en7571_cdr(struct en7571_priv *priv, bool enable);
void en7571_load_tx_cal_data(struct en7571_priv *priv);
void en7571_tgen_recall(struct en7571_priv *priv);
void en7571_set_t0t1_delay(struct en7571_priv *priv, u8 delay);
void en7571_apd_init(struct en7571_priv *priv);
void en7571_apd_control(struct en7571_priv *priv);
void en7571_apd_write(struct en7571_priv *priv, u8 code);
void en7571_rogue_clear(struct en7571_priv *priv);
void en7571_tx_alarms(struct en7571_priv *priv);
u32 en7571_info(struct en7571_priv *priv, u8 select);

/* --- Rx path (en7571_txrx.c) --- */
void en7571_rssi_gain_init(struct en7571_priv *priv);
int en7571_rssi_cal(struct en7571_priv *priv);
s32 en7571_rssi_current(struct en7571_priv *priv);
u32 en7571_dark_current(struct en7571_priv *priv);
void en7571_rx_alarms(struct en7571_priv *priv);
void en7571_los_init(struct en7571_priv *priv);
void en7571_los_level(struct en7571_priv *priv);
void en7571_txsd_level(struct en7571_priv *priv);

/* --- DDMI (en7571_ddmi.c) --- */
u16 en7571_temp_ddmi(struct en7571_priv *priv);
u16 en7571_vcc_ddmi(struct en7571_priv *priv);
u16 en7571_bias_ddmi(struct en7571_priv *priv);
u16 en7571_mod_ddmi(struct en7571_priv *priv);
u16 en7571_tx_power_ddmi(struct en7571_priv *priv);
u16 en7571_rx_power_ddmi(struct en7571_priv *priv);

/* --- Loop control / KT (en7571_adcloop.c) --- */
void en7571_config(struct en7571_priv *priv);
void en7571_hwkt(struct en7571_priv *priv, bool enable);
void en7571_tune_kt(struct en7571_priv *priv, u16 input);
void en7571_swkt(struct en7571_priv *priv);
void en7571_change_imod(struct en7571_priv *priv, u32 v);
void en7571_change_ibias(struct en7571_priv *priv, u32 v);
void en7571_cross_imod(struct en7571_priv *priv, u32 v);
void en7571_change_mpdh(struct en7571_priv *priv, u32 v);

#endif /* _EN7571_H */
