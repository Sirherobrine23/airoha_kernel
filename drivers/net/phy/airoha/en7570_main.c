// SPDX-License-Identifier: GPL-2.0
/*
 * Airoha EN7570 xPON LDDLA controller: detection, bring-up, the 1 Hz control
 * worker and the probe glue onto the shared airoha_lddla core.
 *
 * The EN7570 is an I2C-attached burst-mode laser driver and limiting
 * post-amplifier for GPON/EPON ONU/ONT modules.  It brings the device up from a
 * calibration blob and runs the on-die control loops (APC/Auto-ER, LOS, RSSI,
 * APD bias) plus the ETC/TEC eye-tracking from a 1 Hz state machine.
 *
 * Locking: every top-level entry point holds lddla.lock for the whole operation,
 * which keeps the multi-step channel-select/latch/read ADC sequence atomic.
 */
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/i2c.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/property.h>
#include <linux/seq_file.h>

#include "en7570.h"

#define EN7570_DEFAULT_FW	"airoha/en7570_cal.bin"

/* ------------------------------------------------------------------ */
/* Detection and reset						      */
/* ------------------------------------------------------------------ */

/**
 * en7570_detect() - probe for EN7570 silicon.
 * @priv: device
 *
 * Matches the ID register and stashes the silicon variant byte.
 * Return: 1 if present, 0 if not, negative errno on I2C error.
 */
int en7570_detect(struct en7570_priv *priv)
{
	u8 id = 0, var = 0;
	int ret;

	ret = lddla_rd8(&priv->lddla, EN7570_FT_ADC_CLK_CLR, &id);
	if (ret)
		return ret;
	if (id != EN7570_EN7570_ID)
		return 0;

	/* Stash the silicon variant byte from the DUMMY register. */
	lddla_rd8(&priv->lddla, EN7570_DUMMY, &var);
	priv->variant = var;
	return 1;
}

void en7570_sw_reset(struct en7570_priv *priv)
{
	lddla_wr8(&priv->lddla, EN7570_SW_RESET, EN7570_SW_RESET_TRIG);
	/* Bandgaps need to stabilise before ADC calibration. */
	usleep_range(1000, 2000);
}

/* ------------------------------------------------------------------ */
/* Init / bring-up						      */
/* ------------------------------------------------------------------ */

/* Pull the per-device calibration constants out of the loaded flash blob. */
static void en7570_load_config(struct en7570_priv *priv)
{
	u32 w;

	/* Temperature slope (upper 16, x10) and offset (lower 16, x10). */
	w = lddla_flash_read(&priv->lddla, EN7570_FL_TEMP_K_SLOPE_OFFSET);
	if (((w >> 16) & 0xffff) != 0xffff)
		priv->temp_slope_x10 = (w >> 16) & 0xffff;
	if ((w & 0xffff) != 0xffff)
		priv->temp_offset_x10 = w & 0xffff;

	/* BOSA offset (lower 16, whole degC) and Env offset (upper 16, x10). */
	w = lddla_flash_read(&priv->lddla, EN7570_FL_TEMPERATURE_OFFSET);
	if ((w & 0xffff) != 0xffff)
		priv->bosa_temp_offset_mc = (w & 0xffff) * 1000;
	if (((w >> 16) & 0xffff) != 0xffff)
		priv->env_temp_offset_mc = ((w >> 16) & 0xffff) * 100;

	w = lddla_flash_read(&priv->lddla, EN7570_FL_APD_SLOPE1);
	if (w != EN7570_FLASH_ERASED)
		priv->apd_slope_dn_uv = w * 10000;	/* V/degC x100 -> uV/degC */
	w = lddla_flash_read(&priv->lddla, EN7570_FL_APD_SLOPE2);
	if (w != EN7570_FLASH_ERASED)
		priv->apd_slope_up_uv = w * 10000;
	w = lddla_flash_read(&priv->lddla, EN7570_FL_APD_CHANGE_POINT);
	if (w != EN7570_FLASH_ERASED)
		priv->apd_knee_mv = w * 10;		/* V x100 -> mV */

	w = lddla_flash_read(&priv->lddla, EN7570_FL_T_APD);
	priv->t_apd = (w != EN7570_FLASH_ERASED) ? max_t(u32, w, EN7570_T_APD_MIN)
						 : EN7570_T_APD_DEFAULT;

	w = lddla_flash_read(&priv->lddla, EN7570_FL_TEC);
	if (w != EN7570_FLASH_ERASED) {
		priv->bosa_lth_ua = w;
		priv->tec_en = (w != 0);
	}

	w = lddla_flash_read(&priv->lddla, EN7570_FL_ETC);
	priv->etc_mode = (w != EN7570_FLASH_ERASED) ? (w & 0x3) : EN7570_ETC_DISABLED;

	w = lddla_flash_read(&priv->lddla, EN7570_FL_INTERNAL_DDMI);
	priv->internal_ddmi = (w != EN7570_FLASH_ERASED) ? w : EN7570_DDMI_ON;
	priv->fast_ddmi = (priv->internal_ddmi == EN7570_DDMI_FAST);

	w = lddla_flash_read(&priv->lddla, EN7570_FL_RESERVED1);
	priv->scl = (w == EN7570_LOOP_SCL);
	priv->dol = (w == EN7570_LOOP_DOL);
}

/**
 * en7570_init() - bring the EN7570 up from the calibration blob.
 * @priv: device
 *
 * Resets the part, loads the LUT and per-device calibration, calibrates the
 * ADCs, then for a provisioned PON mode programs the burst timing, Tx
 * currents/targets, APD bias and the configured loop-control mode.
 * Return: 0 on success, -EIO if ADC calibration fails.
 */
int en7570_init(struct en7570_priv *priv)
{
	u32 magic;
	int ret;

	en7570_sw_reset(priv);
	en7570_load_config(priv);
	en7570_lut_default(priv);

	en7570_rx_tia_gain(priv);		/* must precede MPD calibration */
	en7570_erc_filter(priv);
	en7570_mpd_dark(priv);

	ret = en7570_adc_calibrate(priv);
	if (ret)
		return -EIO;
	en7570_temp_update(priv);
	en7570_rssi_cal(priv);

	/* Branch on the PON-mode magic number. */
	magic = lddla_flash_read(&priv->lddla, EN7570_FL_MAGIC);
	if (magic == EN7570_MAGIC_GPON) {
		priv->lddla.pon_mode = EN7570_PON_GPON;
	} else if (magic == EN7570_MAGIC_EPON) {
		priv->lddla.pon_mode = EN7570_PON_EPON;
	} else {
		dev_warn(priv->lddla.dev, "unknown PON magic 0x%08x; DDMI disabled\n",
			 magic);
		priv->internal_ddmi = EN7570_DDMI_OFF;
		return 0;	/* leave the optical chain in its default state */
	}

	en7570_rssi_gain_init(priv);
	en7570_los_level(priv);
	en7570_tgen(priv, priv->lddla.pon_mode);
	en7570_tx_load_init_current(priv);
	en7570_tx_load_mpd_targets(priv);
	en7570_tx_sd_level(priv);

	if (priv->lddla.pon_mode == EN7570_PON_GPON) {
		en7570_apd_init(priv);
		en7570_apd_update(priv);
	}

	/* Engage the configured loop-control mode. */
	if (priv->scl) {
		en7570_mode_single_cl(priv);
	} else if (priv->dol) {
		en7570_mode_open(priv);
		en7570_lut_recover(priv);
	} else {
		en7570_mode_dual_cl(priv);
	}

	en7570_rogue_clear(priv);
	en7570_safe_reset(priv);

	if (priv->etc_mode == EN7570_ETC_BIAS_TRACK)
		en7570_lut_recover(priv);

	dev_info(priv->lddla.dev, "EN7570 initialised: %s, %s-loop, ETC%d, DDMI%d\n",
		 priv->lddla.pon_mode == EN7570_PON_GPON ? "GPON" : "EPON",
		 priv->scl ? "single-closed" : priv->dol ? "dual-open" : "dual-closed",
		 priv->etc_mode, priv->internal_ddmi);
	return 0;
}

/* Restart ERC to prevent eye failure on a fiber hot-plug. */
void en7570_fiber_plug(struct en7570_priv *priv)
{
	en7570_erc_restart(priv);
}

/* ------------------------------------------------------------------ */
/* 1 Hz periodic state machine					      */
/* ------------------------------------------------------------------ */

/**
 * en7570_tick() - 1 Hz control and DDMI-refresh state machine.
 * @priv: device
 *
 * Retunes the APD voltage every T_APD seconds (GPON), staggers the DDMI word
 * refresh, and runs the loop-mode handlers (ETC / bias-tracking / TEC eye
 * correction / open-loop LUT) plus periodic ADC re-calibration.
 */
void en7570_tick(struct en7570_priv *priv)
{
	u32 c = priv->cnt7570;
	bool dual_cl = !priv->scl && !priv->dol;
	bool fast = priv->fast_ddmi;

	/* APD voltage tracking (GPON only) every T_APD seconds. */
	if (priv->lddla.pon_mode == EN7570_PON_GPON &&
	    (c % priv->t_apd) == priv->t_apd - 1) {
		en7570_temp_update(priv);
		en7570_apd_update(priv);
	}

	/* Staggered DDMI refresh (only when internal DDMI is enabled). */
	if (priv->internal_ddmi != EN7570_DDMI_OFF) {
		if (c % 10 == 0 || fast)
			en7570_vcc_ddmi(priv);
		en7570_bias_ddmi(priv);			/* every tick */
		if (c % 10 == 4 || fast)
			en7570_temp_ddmi(priv);
		if (c % 10 == 6 || fast) {
			en7570_tx_power_ddmi(priv);
			en7570_tx_alarms(priv);
		}
		if (c % 10 == 8 || fast) {
			en7570_rx_power_ddmi(priv);
			en7570_rx_alarms(priv);
		}
	}

	/* Loop-mode handlers. */
	if (priv->scl && c % 29 == 0 && priv->etc_mode == EN7570_ETC_BIAS_TRACK)
		en7570_bias_track(priv);
	if (priv->dol && c % 5 == 0)
		en7570_loop_open_track(priv);
	if (dual_cl && c % 30 == 29) {
		if (priv->etc_mode == EN7570_ETC_STANDARD)
			en7570_etc_std(priv);
		else if (priv->etc_mode == EN7570_ETC_BIAS_TRACK &&
			 (priv->pattern_enabled || priv->bias_track_sw))
			en7570_bias_track(priv);
	}

	/* Eye-failure protection when bias falls below the BOSA threshold. */
	if (dual_cl && priv->tec_en && priv->tec_switch)
		en7570_tec(priv);

	/* Fiber hot-plug protection (dual-closed-loop, pattern active). */
	if (dual_cl && priv->lddla.pon_mode != EN7570_PON_UNKNOWN &&
	    priv->fiber_plug && priv->pattern_enabled)
		en7570_fiber_plug(priv);

	/* Track slow ADC drift. */
	if (c % 120 == 119 && priv->lddla.pon_mode != EN7570_PON_UNKNOWN)
		en7570_adc_calibrate(priv);

	priv->cnt7570++;
}

static void en7570_tick_work(struct work_struct *work)
{
	struct en7570_priv *priv = container_of(to_delayed_work(work),
						struct en7570_priv, tick_work);

	/* If a diagnostic path is holding the lock, skip this tick. */
	if (!lddla_lock(&priv->lddla)) {
		en7570_tick(priv);
		mutex_unlock(&priv->lddla.lock);
	}

	schedule_delayed_work(&priv->tick_work, HZ);
}

/* ------------------------------------------------------------------ */
/* Shared-core operations + chip debugfs			      */
/* ------------------------------------------------------------------ */

static s32 en7570_op_temp(struct airoha_lddla *lddla)
{
	struct en7570_priv *priv = container_of(lddla, struct en7570_priv, lddla);

	en7570_temp_ddmi(priv);
	return priv->ic_temp_mc;
}

static s32 en7570_op_bosa_temp(struct airoha_lddla *lddla)
{
	struct en7570_priv *priv = container_of(lddla, struct en7570_priv, lddla);

	en7570_temp_ddmi(priv);
	return priv->bosa_temp_mc;
}

static u16 en7570_op_vcc(struct airoha_lddla *lddla)
{
	return en7570_vcc_ddmi(container_of(lddla, struct en7570_priv, lddla));
}

static u16 en7570_op_bias(struct airoha_lddla *lddla)
{
	return en7570_bias_ddmi(container_of(lddla, struct en7570_priv, lddla));
}

static u16 en7570_op_tx_power(struct airoha_lddla *lddla)
{
	return en7570_tx_power_ddmi(container_of(lddla, struct en7570_priv, lddla));
}

static u16 en7570_op_rx_power(struct airoha_lddla *lddla)
{
	return en7570_rx_power_ddmi(container_of(lddla, struct en7570_priv, lddla));
}

static int en7570_op_tx_rearm(struct airoha_lddla *lddla)
{
	if (lddla->pon_mode != AIROHA_PON_GPON &&
	    lddla->pon_mode != AIROHA_PON_EPON)
		return -ENODATA;

	return lddla_update8(lddla, EN7570_SAFE_PROTECT + 1,
			     EN7570_SAFE_CIRCUIT_MASK,
			     EN7570_SAFE_CIRCUIT_RESET);
}

static void en7570_op_diag(struct airoha_lddla *lddla, struct seq_file *s)
{
	struct en7570_priv *priv = container_of(lddla, struct en7570_priv, lddla);

	seq_printf(s, "variant:     0x%02x\n", priv->variant);
	seq_printf(s, "loop:        %s\n",
		   priv->scl ? "single-closed" : priv->dol ? "dual-open" : "dual-closed");
	seq_printf(s, "etc_mode:    %d\n", priv->etc_mode);
	seq_printf(s, "ic_temp:     %d.%03d degC\n",
		   priv->ic_temp_mc / 1000, abs(priv->ic_temp_mc % 1000));
	seq_printf(s, "bosa_temp:   %d.%03d degC\n",
		   priv->bosa_temp_mc / 1000, abs(priv->bosa_temp_mc % 1000));
	seq_printf(s, "apd_voltage: %d mV\n", priv->apd_voltage_mv);
	seq_printf(s, "ibias:       0x%03x\n", priv->bias_code);
	seq_printf(s, "imod:        0x%03x\n", priv->mod_code);
	seq_printf(s, "mpdl/mpdh:   0x%02x / 0x%02x\n", priv->mpdl, priv->mpdh);
	seq_printf(s, "adc slope:   %lld nV/code  offset %d uV\n",
		   priv->adc_slope_nv, priv->adc_offset_uv);
	seq_printf(s, "rssi factor: %d (x1000)\n", priv->rssi_factor);
}

static const struct airoha_lddla_ops en7570_ops = {
	.name = "en7570",
	.part_number = "EN7570-LDDLA",
	.serial = "EN7570SN00000001",
	.date_code = "260608",
	.temp_refresh = en7570_op_temp,
	.bosa_temp_refresh = en7570_op_bosa_temp,
	.vcc_refresh = en7570_op_vcc,
	.bias_refresh = en7570_op_bias,
	.tx_power_refresh = en7570_op_tx_power,
	.rx_power_refresh = en7570_op_rx_power,
	.diag_show = en7570_op_diag,
	.tx_rearm = en7570_op_tx_rearm,
};

static int en7570_lut_show(struct seq_file *s, void *unused)
{
	struct en7570_priv *priv = s->private;
	int t, ret;

	ret = lddla_lock(&priv->lddla);
	if (ret)
		return ret;
	for (t = 0; t < 64; t++)
		seq_printf(s, "[%2d] %+5d.%01u degC  ibias=0x%03x imod=0x%03x\n",
			   t, (t * 25 - 400) / 10, (t * 25) % 10,
			   priv->lut[t][0], priv->lut[t][1]);
	mutex_unlock(&priv->lddla.lock);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(en7570_lut);

/* ------------------------------------------------------------------ */
/* Probe / remove						      */
/* ------------------------------------------------------------------ */

static void en7570_set_defaults(struct en7570_priv *priv)
{
	priv->lddla.pon_mode = EN7570_PON_UNKNOWN;
	priv->internal_ddmi = EN7570_DDMI_ON;
	priv->tec_en = 1;
	priv->variant = 1;
	priv->t_apd = EN7570_T_APD_DEFAULT;
	priv->bosa_lth_ua = EN7570_BOSA_LTH_UA_DEF;
	priv->bosa_temp_offset_mc = EN7570_BOSA_TEMP_OFFSET_MC;
	priv->env_temp_offset_mc = 10000;	/* IC -> ambient delta, 10 degC */
	priv->temp_slope_x10 = EN7570_TEMP_SLOPE_X10_DEF;
	priv->temp_offset_x10 = EN7570_TEMP_OFFSET_X10_DEF;
	priv->apd_slope_up_uv = EN7570_APD_SLOPE_UP_UV_DEF;
	priv->apd_slope_dn_uv = EN7570_APD_SLOPE_DN_UV_DEF;
	priv->apd_knee_mv = EN7570_APD_KNEE_MV_DEF;
	priv->ic_temp_mc = 25000;
	priv->bosa_temp_mc = 20000;
	priv->env_temp_mc = 25000;
	priv->apd_voltage_mv = EN7570_APD_KNEE_MV_DEF;
}

static int en7570_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct en7570_priv *priv;
	int ret;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C))
		return -EOPNOTSUPP;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->lddla.client = client;
	priv->lddla.dev = dev;
	priv->lddla.ops = &en7570_ops;
	mutex_init(&priv->lddla.lock);
	INIT_DELAYED_WORK(&priv->tick_work, en7570_tick_work);
	i2c_set_clientdata(client, priv);
	en7570_set_defaults(priv);

	if (device_property_read_string(dev, "firmware-name", &priv->lddla.fw_name))
		priv->lddla.fw_name = EN7570_DEFAULT_FW;

	ret = en7570_detect(priv);
	if (ret < 0)
		return dev_err_probe(dev, ret, "I2C probe failed\n");
	if (!ret)
		return dev_err_probe(dev, -ENODEV, "EN7570 silicon not found\n");

	ret = lddla_flash_load(&priv->lddla);
	if (ret)
		return ret;

	mutex_lock(&priv->lddla.lock);
	ret = en7570_init(priv);
	mutex_unlock(&priv->lddla.lock);
	if (ret)
		return dev_err_probe(dev, ret, "device init failed\n");

	ret = lddla_hwmon_register(&priv->lddla);
	if (ret)
		return ret;

	lddla_debugfs_init(&priv->lddla);
	debugfs_create_file("lut", 0444, priv->lddla.debugfs, priv, &en7570_lut_fops);

	/* Expose the diagnostics as a virtual SFP module (non-fatal). */
	ret = lddla_sfp_init(&priv->lddla);
	if (ret)
		dev_warn(dev, "virtual SFP bus unavailable (%d)\n", ret);

	schedule_delayed_work(&priv->tick_work, HZ);
	return 0;
}

static void en7570_remove(struct i2c_client *client)
{
	struct en7570_priv *priv = i2c_get_clientdata(client);

	cancel_delayed_work_sync(&priv->tick_work);
	lddla_sfp_remove(&priv->lddla);
	lddla_debugfs_remove(&priv->lddla);
	mutex_destroy(&priv->lddla.lock);
}

static const struct of_device_id en7570_of_match[] = {
	{ .compatible = "airoha,en7570" },
	{ }
};
MODULE_DEVICE_TABLE(of, en7570_of_match);

static const struct i2c_device_id en7570_i2c_id[] = {
	{ "en7570" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, en7570_i2c_id);

struct i2c_driver en7570_i2c_driver = {
	.driver = {
		.name = "en7570",
		.of_match_table = en7570_of_match,
	},
	.probe = en7570_probe,
	.remove = en7570_remove,
	.id_table = en7570_i2c_id,
};

MODULE_FIRMWARE(EN7570_DEFAULT_FW);
