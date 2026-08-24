// SPDX-License-Identifier: GPL-2.0
/*
 * Airoha EN7571 xPON LDDLA controller: detection, bring-up, the 1 Hz control
 * worker and the probe glue onto the shared airoha_lddla core.
 *
 * The EN7571 is an I2C-attached analog/optical front-end (laser driver, TIA,
 * APD bias, ADCs, LOS/SD) for GPON/EPON ONU/ONT modules.  It brings the device
 * up from a calibration blob and runs the on-die dual-closed-loop power control
 * plus the KT temperature-compensation loop (and the single-closed / open-loop
 * LUT modes) from a 1 Hz state machine.
 *
 * Locking: every top-level entry point holds lddla.lock for the whole operation,
 * which keeps the multi-step shared-SVADC channel-select/latch/read sequence
 * atomic.
 */
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/i2c.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/property.h>
#include <linux/seq_file.h>
#include <linux/slab.h>

#include "en7571.h"

/* ------------------------------------------------------------------ */
/* Detection and reset						      */
/* ------------------------------------------------------------------ */

/**
 * en7571_detect() - probe for EN7571 silicon and its revision.
 * @priv: device
 *
 * Reads the two identification registers; on a match the part is recorded as
 * silicon rev 2 (its rev-2 burst / Imod paths are always used).
 * Return: 1 if present, 0 if not, negative errno on I2C error.
 */
int en7571_detect(struct en7571_priv *priv)
{
	u8 id1 = 0, id2 = 0;
	int ret;

	ret = lddla_rd8(&priv->lddla, EN7571_FT_ADC_CLK_CLR, &id1);
	if (ret)
		return ret;
	ret = lddla_rd8(&priv->lddla, EN7571_DUMMY, &id2);
	if (ret)
		return ret;

	if (id1 != EN7571_ID1 || id2 < EN7571_ID2_MIN)
		return 0;

	/*
	 * Every detected part is treated as silicon rev 2 once
	 * id1==0x03 && id2>=0x03; the rev-2 burst / Imod paths are therefore
	 * always taken.
	 */
	priv->ver = 2;
	return 1;
}

void en7571_sw_reset(struct en7571_priv *priv)
{
	lddla_update8(&priv->lddla, EN7571_SW_RESET, EN7571_SW_RESET_MASK,
		       EN7571_SW_RESET_SET);
	/* Bandgaps need to stabilise before ADC calibration. */
	usleep_range(1000, 2000);
}

/* Two-step HW-reset toggle on DUMMY byte 1: assert low, then release. */
void en7571_hw_reset(struct en7571_priv *priv)
{
	lddla_update8(&priv->lddla, EN7571_DUMMY + 1, EN7571_HWRESET_MASK, 0);
	lddla_update8(&priv->lddla, EN7571_DUMMY + 1, EN7571_HWRESET_MASK,
		       EN7571_HWRESET_SET);
}

/* ------------------------------------------------------------------ */
/* Init / bring-up						      */
/* ------------------------------------------------------------------ */

/* Pull the per-device calibration constants out of the loaded flash blob. */
static void en7571_load_config(struct en7571_priv *priv)
{
	u32 w;

	/*
	 * flash 0x024: low byte = signed BOSA temperature offset (degC),
	 * upper 16 = environment temperature offset (degC x10).
	 */
	w = lddla_flash_read(&priv->lddla, EN7571_FL_TEMP_OFFSET);
	if ((w & 0xffff) != 0xffff)
		priv->bosa_temp_offset_mc = (s8)(w & 0xff) * 1000;
	if (((w >> 16) & 0xffff) != 0xffff)
		priv->env_temp_offset_mc = ((w >> 16) & 0xffff) * 100;

	/* flash 0x028: SFF-8472 external temperature calibration (slope/offset). */
	w = lddla_flash_read(&priv->lddla, EN7571_FL_EXTCAL_TEMP);
	if ((w & 0xffff) != 0xffff)
		priv->temp_slope_x10 = priv->temp_slope_x10 * (w & 0xffff) / 256;
	if (((w >> 16) & 0xffff) != 0xffff)
		priv->temp_offset_x10 += (s16)(w >> 16) * 10;

	w = lddla_flash_read(&priv->lddla, EN7571_FL_APD_SLOPE_UP);
	if (w != EN7571_FLASH_ERASED)
		priv->apd_slope_up_uv = w * 10000;	/* V/degC x100 -> uV/degC */
	w = lddla_flash_read(&priv->lddla, EN7571_FL_APD_SLOPE_DN);
	if (w != EN7571_FLASH_ERASED)
		priv->apd_slope_dn_uv = w * 10000;
	w = lddla_flash_read(&priv->lddla, EN7571_FL_APD_CHANGE_POINT);
	if (w != EN7571_FLASH_ERASED)
		priv->apd_knee_mv = w * 10;		/* V x100 -> mV */

	w = lddla_flash_read(&priv->lddla, EN7571_FL_T_APD);
	priv->t_apd = (w != EN7571_FLASH_ERASED) ? max_t(u32, w, EN7571_T_APD_MIN)
						 : EN7571_T_APD_DEFAULT;
}

/**
 * en7571_init() - bring the EN7571 up from the calibration blob.
 * @priv: device
 *
 * Resets the part, calibrates the ADCs, recalls the burst timing and programs
 * the dual-closed-loop power control for the provisioned PON mode, then selects
 * the KT / single-closed / open-loop compensation mode.  Return: 0 (always; an
 * unprovisioned part is left in a minimal LOS-only state with DDMI disabled).
 */
int en7571_init(struct en7571_priv *priv)
{
	u32 magic;

	en7571_load_config(priv);

	en7571_sw_reset(priv);
	en7571_hw_reset(priv);
	en7571_7571_enable(priv);

	en7571_pwradc_calibration(priv);	/* MPD dark offset (Tx off) */
	en7571_pwradc_enable(priv);

	/*
	 * ADC calibration must follow the SW-reset settle.  A failure here is
	 * non-fatal: it leaves slope/offset at defaults and bring-up continues.
	 */
	if (en7571_adc_calibrate(priv))
		dev_warn(priv->lddla.dev, "ADC calibration failed; using default slope\n");
	en7571_rssi_cal(priv);
	en7571_efuse_temp(priv);

	/* Companion-PHY Tx polarity is a PHY-layer CSR, driven by the PHY init layer. */

	/* Branch on the PON-mode magic number. */
	magic = lddla_flash_read(&priv->lddla, EN7571_FL_MAGIC);
	if (magic == EN7571_MAGIC_GPON) {
		priv->lddla.pon_mode = EN7571_PON_GPON;
		en7571_tgen_recall(priv);
		en7571_set_t0t1_delay(priv, EN7571_T1_T0_DELAY_GPON);
		en7571_apd_init(priv);
		en7571_apd_control(priv);
	} else if (magic == EN7571_MAGIC_EPON) {
		priv->lddla.pon_mode = EN7571_PON_EPON;
		en7571_tgen_recall(priv);
		en7571_set_t0t1_delay(priv, EN7571_T1_T0_DELAY_EPON);
		/* EPON does not run APD control. */
	} else if (magic == EN7571_MAGIC_XPON) {
		/* Adapter mode: run as GPON but with the EPON burst delay. */
		priv->lddla.pon_mode = EN7571_PON_GPON;
		en7571_tgen_recall(priv);
		en7571_set_t0t1_delay(priv, EN7571_T1_T0_DELAY_EPON);
		en7571_apd_init(priv);
		en7571_apd_control(priv);
	} else {
		static const u8 zero[4] = { 0, 0, 0, 0 };

		dev_warn(priv->lddla.dev, "unknown PON magic 0x%08x; DDMI disabled\n",
			 magic);
		/* Quiesce the loop set-points, then minimal LOS-only mode. */
		lddla_wr(&priv->lddla, EN7571_PWR_CTRL_9, zero, 4);
		lddla_wr(&priv->lddla, EN7571_PWR_CTRL_D, zero, 4);
		en7571_los_init(priv);
		priv->internal_ddmi = EN7571_DDMI_OFF;
		return 0;
	}

	en7571_reg_init(priv);
	en7571_load_tx_cal_data(priv);
	en7571_txsd_level(priv);
	en7571_rssi_gain_init(priv);
	en7571_los_level(priv);
	en7571_rogue_clear(priv);
	en7571_safe_reset(priv);
	en7571_link_reg(priv, true);
	en7571_dcl_start(priv);
	en7571_config(priv);

	dev_info(priv->lddla.dev,
		 "EN7571 initialised: %s, rev %d, KT%d, DDMI%d\n",
		 priv->lddla.pon_mode == EN7571_PON_GPON ? "GPON" : "EPON",
		 priv->ver, priv->kt, priv->internal_ddmi);
	return 0;
}

/* ------------------------------------------------------------------ */
/* 1 Hz periodic state machine					      */
/* ------------------------------------------------------------------ */

/**
 * en7571_tick() - 1 Hz control and DDMI-refresh state machine.
 * @priv: device
 *
 * Round-robins the five DDMI words across a 10 s window (or all every tick in
 * fast mode), retunes the APD voltage every T_APD seconds (GPON only), and runs
 * either the KT temperature-compensation loop or the open-loop LUT tracking.
 */
void en7571_tick(struct en7571_priv *priv)
{
	u32 c = priv->cnt;
	bool fast = priv->fast_ddmi;

	if (priv->internal_ddmi == EN7571_DDMI_ON) {
		/*
		 * APD voltage tracking (GPON only) every T_APD seconds.  The
		 * BOSA temperature is refreshed by the temperature DDMI sample
		 * below, so no extra read is taken here.
		 */
		if (priv->lddla.pon_mode == EN7571_PON_GPON &&
		    (c % priv->t_apd) == priv->t_apd - 1)
			en7571_apd_control(priv);

		/*
		 * Round-robin DDMI refresh within a 10 s window (or all, fast).
		 * Cached words still at their reset value are re-sampled early
		 * so they converge during bring-up.
		 */
		if (c % 10 == 0 || fast)
			en7571_vcc_ddmi(priv);
		if (c % 10 == 2 || fast || priv->lddla.ddmi_current == 0)
			en7571_bias_ddmi(priv);
		if (c % 10 == 4 || fast)
			en7571_temp_ddmi(priv);
		if (c % 10 == 6 || fast || priv->lddla.ddmi_tx_power <= 1) {
			en7571_tx_power_ddmi(priv);
			en7571_tx_alarms(priv);
		}
		if (c % 10 == 8 || fast || priv->lddla.ddmi_rx_power <= 1) {
			en7571_rx_power_ddmi(priv);
			en7571_rx_alarms(priv);
		}
	}

	if (priv->scl || priv->dol) {
		/* Open-loop / single-closed-loop: track the bias/mod LUT. */
		if (c % EN7571_SWKT_PERIOD == EN7571_SWKT_PERIOD - 3)
			en7571_lut_tracking(priv);
	} else if (priv->kt != EN7571_KT_OFF) {
		/* Temperature-compensation (KT) runs every tick while up. */
		en7571_swkt(priv);
	}

	priv->cnt++;
}

static void en7571_tick_work(struct work_struct *work)
{
	struct en7571_priv *priv = container_of(to_delayed_work(work),
						struct en7571_priv, tick_work);

	/* If a diagnostic path is holding the lock, skip this tick. */
	if (!lddla_lock(&priv->lddla)) {
		en7571_tick(priv);
		mutex_unlock(&priv->lddla.lock);
	}

	schedule_delayed_work(&priv->tick_work, HZ);
}

/* ------------------------------------------------------------------ */
/* Shared-core operations					      */
/* ------------------------------------------------------------------ */

static s32 en7571_op_temp(struct airoha_lddla *lddla)
{
	struct en7571_priv *priv = container_of(lddla, struct en7571_priv, lddla);

	en7571_temp_ddmi(priv);
	return priv->ic_temp_mc;
}

static s32 en7571_op_bosa_temp(struct airoha_lddla *lddla)
{
	struct en7571_priv *priv = container_of(lddla, struct en7571_priv, lddla);

	en7571_temp_ddmi(priv);
	return priv->bosa_temp_mc;
}

static u16 en7571_op_vcc(struct airoha_lddla *lddla)
{
	return en7571_vcc_ddmi(container_of(lddla, struct en7571_priv, lddla));
}

static u16 en7571_op_bias(struct airoha_lddla *lddla)
{
	return en7571_bias_ddmi(container_of(lddla, struct en7571_priv, lddla));
}

static u16 en7571_op_tx_power(struct airoha_lddla *lddla)
{
	return en7571_tx_power_ddmi(container_of(lddla, struct en7571_priv, lddla));
}

static u16 en7571_op_rx_power(struct airoha_lddla *lddla)
{
	return en7571_rx_power_ddmi(container_of(lddla, struct en7571_priv, lddla));
}

static int en7571_op_tx_rearm(struct airoha_lddla *lddla)
{
	int ret;

	ret = lddla_update8(lddla, EN7571_PWR_CTRL_0 + 1,
			    EN7571_DCL_RST_B_MASK, EN7571_DCL_RST_B);
	if (ret)
		return ret;

	return lddla_update8(lddla, EN7571_SAFE_PROTECT + 1,
			     EN7571_SAFE_CIRCUIT_MASK,
			     EN7571_SAFE_CIRCUIT_RESET);
}

/* Chip-specific debugfs lines (the shared core prints the common ones). */
static void en7571_op_diag(struct airoha_lddla *lddla, struct seq_file *s)
{
	struct en7571_priv *priv = container_of(lddla, struct en7571_priv, lddla);
	u32 limiter0 = 0, limiter2 = 0, pwr_ctrl0 = 0;
	u32 pwr_ctrl9 = 0, pwr_ctrld = 0;
	u32 p0_cs2 = 0, p0_cs3 = 0, p1_cs2 = 0, p1_cs3 = 0;
	u32 tgen = 0, dummy = 0, safe = 0;

	lddla_rd32(lddla, EN7571_PWR_LIMITER_0, &limiter0);
	lddla_rd32(lddla, EN7571_PWR_LIMITER_2, &limiter2);
	lddla_rd32(lddla, EN7571_PWR_CTRL_0, &pwr_ctrl0);
	lddla_rd32(lddla, EN7571_PWR_CTRL_9, &pwr_ctrl9);
	lddla_rd32(lddla, EN7571_PWR_CTRL_D, &pwr_ctrld);
	lddla_rd32(lddla, EN7571_P0_PWR_CTRL_CS2, &p0_cs2);
	lddla_rd32(lddla, EN7571_P0_PWR_CTRL_CS3, &p0_cs3);
	lddla_rd32(lddla, EN7571_P1_PWR_CTRL_CS2, &p1_cs2);
	lddla_rd32(lddla, EN7571_P1_PWR_CTRL_CS3, &p1_cs3);
	lddla_rd32(lddla, EN7571_T1DELAY, &tgen);
	lddla_rd32(lddla, EN7571_DUMMY, &dummy);
	lddla_rd32(lddla, EN7571_SAFE_PROTECT, &safe);

	seq_printf(s, "version:     %d\n", EN7571_VERSION);
	seq_printf(s, "silicon rev: %d\n", priv->ver);
	seq_printf(s, "loop:        %s\n",
		   priv->scl ? "single-closed" : priv->dol ? "open" : "dual-closed");
	seq_printf(s, "kt:          %d\n", priv->kt);
	seq_printf(s, "ddmi mod:    0x%04x\n", en7571_mod_ddmi(priv));
	seq_printf(s, "ic_temp:     %d.%03d degC\n",
		   priv->ic_temp_mc / 1000, abs(priv->ic_temp_mc % 1000));
	seq_printf(s, "bosa_temp:   %d.%03d degC\n",
		   priv->bosa_temp_mc / 1000, abs(priv->bosa_temp_mc % 1000));
	seq_printf(s, "apd_voltage: %d mV\n", priv->apd_voltage_mv);
	seq_printf(s, "ibias:       0x%03x\n", priv->ibias_now);
	seq_printf(s, "imod:        0x%03x\n", priv->imod_now);
	seq_printf(s, "iav:         0x%03x\n", priv->iav_now);
	seq_printf(s, "pwradc:      0x%05x (offset 0x%05x)\n",
		   priv->pwradc, priv->pwradc_offset);
	seq_printf(s, "adc slope:   %lld nV/code  offset %d uV\n",
		   priv->adc_slope_nv, priv->adc_offset_uv);
	seq_printf(s, "efuse off:   %d m degC\n", priv->efuse_offset_mc);
	seq_printf(s, "rssi factor: %d (x1000)\n", priv->rssi_factor);
	seq_printf(s, "raw limiter: ibias=0x%08x imod=0x%08x\n",
		   limiter0, limiter2);
	seq_printf(s, "raw loop:    ctrl0=0x%08x setpoint=0x%08x pav_p1=0x%08x\n",
		   pwr_ctrl0, pwr_ctrl9, pwr_ctrld);
	seq_printf(s, "raw phase0:  cs2=0x%08x cs3=0x%08x\n",
		   p0_cs2, p0_cs3);
	seq_printf(s, "raw phase1:  cs2=0x%08x cs3=0x%08x\n",
		   p1_cs2, p1_cs3);
	seq_printf(s, "raw burst:   tgen=0x%08x dummy=0x%08x safe=0x%08x\n",
		   tgen, dummy, safe);
}

static const struct airoha_lddla_ops en7571_ops = {
	.name = "en7571",
	.part_number = "EN7571-LDDLA",
	.serial = "EN7571SN00000001",
	.date_code = "260609",
	.protocols = BIT(OPTICAL_FRONTEND_PROTO_EPON) |
		     BIT(OPTICAL_FRONTEND_PROTO_GPON),
	.thresholds = &airoha_lddla_default_thresholds,
	.bob_size_min = AIROHA_LDDLA_BOB_MIN_SIZE,
	.bob_size_max = AIROHA_LDDLA_BOB_MAX_SIZE,
	.temp_refresh = en7571_op_temp,
	.bosa_temp_refresh = en7571_op_bosa_temp,
	.vcc_refresh = en7571_op_vcc,
	.bias_refresh = en7571_op_bias,
	.tx_power_refresh = en7571_op_tx_power,
	.rx_power_refresh = en7571_op_rx_power,
	.diag_show = en7571_op_diag,
	.tx_rearm = en7571_op_tx_rearm,
};

/* ------------------------------------------------------------------ */
/* Probe / remove						      */
/* ------------------------------------------------------------------ */

static void en7571_set_defaults(struct en7571_priv *priv)
{
	priv->lddla.pon_mode = EN7571_PON_UNKNOWN;
	priv->internal_ddmi = EN7571_DDMI_ON;
	priv->ver = 1;
	priv->cross = 1;
	priv->kt = EN7571_KT_OFF;
	priv->t_apd = EN7571_T_APD_DEFAULT;
	priv->bosa_temp_offset_mc = EN7571_BOSA_TEMP_OFFSET_MC;
	priv->env_temp_offset_mc = 10000;	/* IC -> ambient delta, 10 degC */
	priv->temp_slope_x10 = EN7571_TEMP_SLOPE_X10_DEF;
	priv->temp_offset_x10 = EN7571_TEMP_OFFSET_X10_DEF;
	priv->apd_slope_up_uv = EN7571_APD_SLOPE_UP_UV_DEF;
	priv->apd_slope_dn_uv = EN7571_APD_SLOPE_DN_UV_DEF;
	priv->apd_knee_mv = EN7571_APD_KNEE_MV_DEF;
	priv->ic_temp_mc = 25000;
	priv->bosa_temp_mc = 20000;
	priv->apd_voltage_mv = EN7571_APD_KNEE_MV_DEF;
}

static int en7571_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct en7571_priv *priv;
	int ret;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C))
		return -EOPNOTSUPP;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->lddla.client = client;
	priv->lddla.dev = dev;
	priv->lddla.ops = &en7571_ops;
	mutex_init(&priv->lddla.lock);
	INIT_DELAYED_WORK(&priv->tick_work, en7571_tick_work);
	i2c_set_clientdata(client, priv);
	en7571_set_defaults(priv);

	if (device_property_read_string(dev, "firmware-name",
					&priv->lddla.bob_fw_name))
		priv->lddla.bob_fw_name = EN7571_DEFAULT_FW;

	ret = en7571_detect(priv);
	if (ret < 0)
		return dev_err_probe(dev, ret, "I2C probe failed\n");
	if (!ret)
		return dev_err_probe(dev, -ENODEV, "EN7571 silicon not found\n");

	ret = lddla_bob_load(&priv->lddla);
	if (ret)
		return dev_err_probe(dev, ret, "calibration load failed\n");

	mutex_lock(&priv->lddla.lock);
	ret = en7571_init(priv);
	mutex_unlock(&priv->lddla.lock);
	if (ret)
		return dev_err_probe(dev, ret, "device init failed\n");

	ret = lddla_frontend_register(&priv->lddla);
	if (ret)
		return ret;

	lddla_debugfs_init(&priv->lddla);


	schedule_delayed_work(&priv->tick_work, HZ);
	return 0;
}

static void en7571_remove(struct i2c_client *client)
{
	struct en7571_priv *priv = i2c_get_clientdata(client);

	cancel_delayed_work_sync(&priv->tick_work);
	lddla_debugfs_remove(&priv->lddla);
	mutex_destroy(&priv->lddla.lock);
}

static const struct of_device_id en7571_of_match[] = {
	{ .compatible = "airoha,en7571" },
	{ }
};
MODULE_DEVICE_TABLE(of, en7571_of_match);

static const struct i2c_device_id en7571_i2c_id[] = {
	{ "en7571" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, en7571_i2c_id);

struct i2c_driver en7571_i2c_driver = {
	.driver = {
		.name = "en7571",
		.of_match_table = en7571_of_match,
	},
	.probe = en7571_probe,
	.remove = en7571_remove,
	.id_table = en7571_i2c_id,
};

MODULE_FIRMWARE(EN7571_DEFAULT_FW);
