// SPDX-License-Identifier: GPL-2.0
/*
 * Airoha EN7572 / AN8901 xPON LDDLA controller: I2C transport, MD32 firmware
 * loader, reset / detect, the per-chip ops table and the 1 Hz worker.
 *
 * The device exposes two I2C slaves on the same bus: the A0 page (0x50, used
 * for MD32 program/data-memory loading and the SFF-8472 serial-ID mirror) and
 * the A2 page (0x51, the firmware mailbox, the hardware CSRs and the SFF-8472
 * digital diagnostics).  All transactions carry a 16-bit big-endian register
 * pointer; mailbox/CSR data is little-endian.
 *
 * Locking: the 1 Hz worker, hwmon and debugfs callbacks hold lddla->lock for
 * the whole operation.  Probe-time firmware loading runs single-threaded
 * before the worker is scheduled and does not take the lock.
 */
#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/i2c.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/property.h>
#include <linux/seq_file.h>
#include <linux/slab.h>

#include "en7572.h"

/* ------------------------------------------------------------------ */
/* I2C transport							      */
/* ------------------------------------------------------------------ */

/**
 * en7572_rd() - read a byte range from an A0/A2 register.
 * @priv: device
 * @dev:  7-bit slave (EN7572_DEV_A0 or EN7572_DEV_A2)
 * @reg:  16-bit register pointer
 * @buf:  destination
 * @len:  number of bytes to read
 *
 * Return: 0 on success, negative errno on failure.
 */
int en7572_rd(struct en7572_priv *priv, u8 dev, u16 reg, u8 *buf, int len)
{
	u8 ptr[2] = { reg >> 8, reg & 0xff };
	struct i2c_msg msg[2] = {
		{ .addr = dev, .flags = 0, .len = 2, .buf = ptr },
		{ .addr = dev, .flags = I2C_M_RD, .len = len, .buf = buf },
	};
	int ret;

	ret = i2c_transfer(priv->lddla.client->adapter, msg, 2);
	if (ret < 0)
		return ret;
	return ret == 2 ? 0 : -EIO;
}

/**
 * en7572_wr() - write a byte range to an A0/A2 register.
 * @priv: device
 * @dev:  7-bit slave (EN7572_DEV_A0 or EN7572_DEV_A2)
 * @reg:  16-bit register pointer
 * @buf:  source
 * @len:  number of bytes to write (1 to 40)
 *
 * Return: 0 on success, negative errno on failure.
 */
int en7572_wr(struct en7572_priv *priv, u8 dev, u16 reg, const u8 *buf, int len)
{
	u8 tmp[2 + 40];
	struct i2c_msg msg = { .addr = dev, .flags = 0 };
	int ret;

	if (len > 40)
		return -EINVAL;

	tmp[0] = reg >> 8;
	tmp[1] = reg & 0xff;
	memcpy(&tmp[2], buf, len);
	msg.len = len + 2;
	msg.buf = tmp;

	ret = i2c_transfer(priv->lddla.client->adapter, &msg, 1);
	if (ret < 0)
		return ret;
	return ret == 1 ? 0 : -EIO;
}

/* Read a single byte; returns 0 on I2C error. */
u8 en7572_byte_rd(struct en7572_priv *priv, u8 dev, u16 reg)
{
	u8 v = 0;

	en7572_rd(priv, dev, reg, &v, 1);
	return v;
}

/* Read a little-endian 16-bit word; returns 0 on I2C error. */
u16 en7572_word_rd(struct en7572_priv *priv, u8 dev, u16 reg)
{
	u8 b[2] = { 0 };

	en7572_rd(priv, dev, reg, b, 2);
	return b[0] | (b[1] << 8);
}

void en7572_byte_wr(struct en7572_priv *priv, u8 dev, u16 reg, u8 val)
{
	en7572_wr(priv, dev, reg, &val, 1);
}

void en7572_word_wr(struct en7572_priv *priv, u8 dev, u16 reg, u16 val)
{
	u8 b[2] = { val & 0xff, val >> 8 };

	en7572_wr(priv, dev, reg, b, 2);
}

/**
 * en7572_bit_rd() - read a bit-field from a 32-bit A2-page CSR.
 * @priv:  device
 * @reg:   16-bit CSR pointer (device 0x51)
 * @start: least-significant bit of the field
 * @end:   most-significant bit of the field
 *
 * Return: the right-justified field value (0 on I2C error).
 */
u32 en7572_bit_rd(struct en7572_priv *priv, u16 reg, int start, int end)
{
	u8 b[4] = { 0 };
	u32 val, mask;

	if (end < start)
		swap(start, end);

	en7572_rd(priv, EN7572_DEV_A2, reg, b, 4);
	val = b[0] | (b[1] << 8) | (b[2] << 16) | (b[3] << 24);
	mask = (end - start == 31) ? 0xffffffff : (BIT(end - start + 1) - 1);
	return (val >> start) & mask;
}

/**
 * en7572_bit_wr() - read-modify-write a bit-field in a 32-bit A2-page CSR.
 * @priv:  device
 * @reg:   16-bit CSR pointer (device 0x51)
 * @start: least-significant bit of the field
 * @end:   most-significant bit of the field
 * @val:   value to place in the field
 */
void en7572_bit_wr(struct en7572_priv *priv, u16 reg, int start, int end, u32 val)
{
	u8 b[4] = { 0 };
	u32 cur, mask;

	if (end < start)
		swap(start, end);

	mask = ((end - start == 31) ? 0xffffffff : (BIT(end - start + 1) - 1)) << start;

	en7572_rd(priv, EN7572_DEV_A2, reg, b, 4);
	cur = b[0] | (b[1] << 8) | (b[2] << 16) | (b[3] << 24);
	cur = (cur & ~mask) | ((val << start) & mask);

	b[0] = cur;
	b[1] = cur >> 8;
	b[2] = cur >> 16;
	b[3] = cur >> 24;
	en7572_wr(priv, EN7572_DEV_A2, reg, b, 4);
}

/* ------------------------------------------------------------------ */
/* Detection							      */
/* ------------------------------------------------------------------ */

/**
 * en7572_detect() - confirm an EN7572/AN8901 is present.
 * @priv: device
 *
 * Return: 0 if the identity word reads back as expected, -ENODEV otherwise.
 */
int en7572_detect(struct en7572_priv *priv)
{
	u16 id = en7572_a2_word(priv, EN7572_CSR_CHIP_ID);

	if (id != EN7572_CHIP_ID) {
		dev_info(priv->lddla.dev, "no EN7572 (id 0x%04x)\n", id);
		return -ENODEV;
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* MD32 firmware loading						      */
/* ------------------------------------------------------------------ */

/* Stream one firmware image into MD32 memory, 32 bits at a time. */
static void en7572_load_block(struct en7572_priv *priv, u16 cfg, u16 addr_reg,
			      u16 data_reg, const u8 *data, size_t size, u32 base)
{
	size_t i;

	en7572_bit_wr(priv, cfg, 0, 0, 1);
	en7572_bit_wr(priv, addr_reg, 0, 31, base);
	for (i = 0; i + 4 <= size; i += 4)
		en7572_wr(priv, EN7572_DEV_A0, data_reg, &data[i], 4);
}

/* Load a firmware image of the expected size, or return an error. */
static int en7572_request_image(struct en7572_priv *priv, const char *name,
				u8 *dst, size_t want)
{
	const struct firmware *fw;
	int ret;

	ret = request_firmware(&fw, name, priv->lddla.dev);
	if (ret) {
		dev_err(priv->lddla.dev, "firmware '%s' not found (%d)\n", name, ret);
		return ret;
	}
	if (fw->size < want) {
		dev_err(priv->lddla.dev, "firmware '%s' too small (%zu < %zu)\n",
			name, fw->size, want);
		release_firmware(fw);
		return -EINVAL;
	}
	memcpy(dst, fw->data, want);
	release_firmware(fw);
	return 0;
}

/**
 * en7572_load_firmware() - load MD32 program/data memory and the BOB table.
 * @priv: device
 *
 * The PM and DM images are mandatory; the BOB calibration table is optional
 * (an uncalibrated module still runs).  Return: 0 on success, negative errno
 * if a mandatory image is missing.
 */
static int en7572_load_firmware(struct en7572_priv *priv)
{
	u8 *pm, *dm;
	int ret;

	pm = kmalloc(EN7572_PM_SIZE, GFP_KERNEL);
	dm = kmalloc(EN7572_DM_SIZE, GFP_KERNEL);
	if (!pm || !dm) {
		ret = -ENOMEM;
		goto out;
	}

	ret = en7572_request_image(priv, priv->fw_pm, pm, EN7572_PM_SIZE);
	if (ret)
		goto out;
	ret = en7572_request_image(priv, priv->fw_dm, dm, EN7572_DM_SIZE);
	if (ret)
		goto out;

	en7572_load_block(priv, EN7572_MD32_PM_CFG, EN7572_MD32_PM_ADDR,
			  EN7572_MD32_PM_DATA, pm, EN7572_PM_SIZE, 0);
	en7572_load_block(priv, EN7572_MD32_DM_CFG, EN7572_MD32_DM_ADDR,
			  EN7572_MD32_DM_DATA, dm, EN7572_DM_SIZE, 0);
	dev_dbg(priv->lddla.dev, "MD32 PM/DM loaded\n");

	if (en7572_request_image(priv, priv->fw_bob, priv->bob, EN7572_BOB_SIZE)) {
		dev_warn(priv->lddla.dev,
			 "no BOB calibration table; running uncalibrated\n");
	} else {
		en7572_load_block(priv, EN7572_MD32_DM_CFG, EN7572_MD32_DM_ADDR,
				  EN7572_MD32_DM_DATA, priv->bob, EN7572_BOB_SIZE,
				  EN7572_MD32_BOB_DM_OFFSET);
		priv->bob_valid = true;
		dev_dbg(priv->lddla.dev, "BOB table loaded\n");
	}
	ret = 0;
out:
	kfree(pm);
	kfree(dm);
	return ret;
}

/* SFF-8472 alarm/warning thresholds programmed when the page is unset. */
static const u8 en7572_aw_thld[40] = {
	0x64, 0x00, 0xce, 0x00, 0x64, 0x00, 0xce, 0x00,
	0x90, 0x88, 0x71, 0x48, 0x8e, 0x94, 0x73, 0x3c,
	0xa6, 0x05, 0x01, 0xf4, 0x9c, 0x40, 0x02, 0xee,
	0xff, 0xff, 0x00, 0x00, 0xff, 0xff, 0x00, 0x00,
	0x31, 0x24, 0x00, 0x01, 0x27, 0x10, 0x00, 0x03,
};

/* True if the diagnostics threshold page has not been programmed yet. */
static bool en7572_thld_unset(struct en7572_priv *priv)
{
	u8 b[4] = { 0 };

	en7572_rd(priv, EN7572_DEV_A2, 0x00, b, 4);
	return b[0] == 0xff && b[1] == 0xff && b[2] == 0xff && b[3] == 0xff;
}

/**
 * en7572_init() - reset the device, load firmware and release the MD32.
 * @priv: device
 *
 * Holds the MCU in reset, disables OCP and the APD bias, pulses the core
 * reset, streams the firmware/BOB into MD32 memory, programs the SFF-8472
 * threshold page if blank, clears stale alarm flags and finally releases the
 * MCU.  Return: 0 on success, negative errno on a firmware-load failure.
 */
int en7572_init(struct en7572_priv *priv)
{
	int ret;

	en7572_bit_wr(priv, EN7572_MD32_EN_CFG, 0, 0, 0);	/* hold MCU */
	en7572_bit_wr(priv, EN7572_RG_OCP_CTRL, 30, 30, 0);	/* OCP off */
	en7572_bit_wr(priv, EN7572_RG_APD_DAC_CODE, 8, 8, 0);	/* APD off */
	msleep(100);

	en7572_bit_wr(priv, EN7572_RG_SYS_RESET, 30, 31, 0);
	en7572_bit_wr(priv, EN7572_RG_SYS_RESET, 30, 31, 3);

	ret = en7572_load_firmware(priv);
	if (ret)
		return ret;

	if (en7572_thld_unset(priv))
		en7572_wr(priv, EN7572_DEV_A2, 0x00, en7572_aw_thld,
			  sizeof(en7572_aw_thld));

	/* Clear stale SFF-8472 alarm / warning flags. */
	en7572_word_wr(priv, EN7572_DEV_A2, EN7572_A2_ALARM_FLAGS, 0);
	en7572_word_wr(priv, EN7572_DEV_A2, EN7572_A2_WARN_FLAGS, 0);

	en7572_bit_wr(priv, EN7572_MD32_EN_CFG, 0, 0, 1);	/* release MCU */
	priv->mcu_ready = true;

	/* Arm the control loops: first pass after BEN comes up. */
	priv->ri_first_boot = true;
	priv->ri_wait_s = 5;
	priv->ap_wait_s = 1;

	dev_info(priv->lddla.dev, "MD32 firmware running\n");
	return 0;
}

/* ------------------------------------------------------------------ */
/* Periodic worker						      */
/* ------------------------------------------------------------------ */

/**
 * en7572_tick() - one pass of the 1 Hz worker.
 * @priv: device
 *
 * Refreshes the cached SFF-8472 diagnostics (for hwmon and the virtual SFP),
 * re-evaluates the alarm bitmap, and services the firmware-assist control
 * loops (each self-gates on its enable bit and cadence).  Called with
 * lddla->lock held.
 */
void en7572_tick(struct en7572_priv *priv)
{
	struct airoha_lddla *lddla = &priv->lddla;

	en7572_temp_refresh(lddla);
	en7572_vcc_refresh(lddla);
	en7572_bias_refresh(lddla);
	en7572_tx_power_refresh(lddla);
	en7572_rx_power_refresh(lddla);
	en7572_alarms(priv);

	en7572_reduce_imod(priv);
	en7572_adaptive_pav(priv);

	priv->cnt++;
}

static void en7572_tick_work(struct work_struct *work)
{
	struct en7572_priv *priv = container_of(to_delayed_work(work),
						struct en7572_priv, tick_work);

	if (lddla_lock(&priv->lddla) == 0) {
		en7572_tick(priv);
		mutex_unlock(&priv->lddla.lock);
	}
	schedule_delayed_work(&priv->tick_work, HZ);
}

/* ------------------------------------------------------------------ */
/* Per-chip ops table						      */
/* ------------------------------------------------------------------ */

static const struct airoha_lddla_ops en7572_ops = {
	.name		= "en7572",
	.part_number	= "EN7572",
	.serial		= "0000000000000000",
	.date_code	= "000000",
	.temp_refresh	= en7572_temp_refresh,
	.vcc_refresh	= en7572_vcc_refresh,
	.bias_refresh	= en7572_bias_refresh,
	.tx_power_refresh = en7572_tx_power_refresh,
	.rx_power_refresh = en7572_rx_power_refresh,
	.diag_show	= en7572_diag_show,
};

/* ------------------------------------------------------------------ */
/* Probe / remove						      */
/* ------------------------------------------------------------------ */

static const struct {
	const char *pm, *dm, *bob;
} en7572_fw[] = {
	[EN7572_VARIANT_EN7572] = {
		"airoha/en7572-pm.bin", "airoha/en7572-dm.bin", "airoha/en7572-bob.bin",
	},
	[EN7572_VARIANT_AN8901] = {
		"airoha/an8901-pm.bin", "airoha/an8901-dm.bin", "airoha/an8901-bob.bin",
	},
};

static int en7572_probe(struct i2c_client *client)
{
	struct en7572_priv *priv;
	int ret;

	priv = devm_kzalloc(&client->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->lddla.client = client;
	priv->lddla.dev = &client->dev;
	priv->lddla.ops = &en7572_ops;
	priv->lddla.pon_mode = AIROHA_PON_GPON;
	mutex_init(&priv->lddla.lock);
	i2c_set_clientdata(client, priv);

	priv->variant = (uintptr_t)i2c_get_match_data(client);
	if (priv->variant >= ARRAY_SIZE(en7572_fw))
		priv->variant = EN7572_VARIANT_EN7572;
	priv->fw_pm = en7572_fw[priv->variant].pm;
	priv->fw_dm = en7572_fw[priv->variant].dm;
	priv->fw_bob = en7572_fw[priv->variant].bob;

	ret = en7572_detect(priv);
	if (ret)
		return ret;

	ret = en7572_init(priv);
	if (ret)
		return ret;

	/* Load the default Tx eye (eye 0) from the BOB table, if calibrated. */
	if (priv->bob_valid)
		en7572_adaptive_pon(priv, 0);

	ret = lddla_hwmon_register(&priv->lddla);
	if (ret)
		return ret;
	lddla_debugfs_init(&priv->lddla);

	ret = lddla_sfp_init(&priv->lddla);
	if (ret) {
		lddla_debugfs_remove(&priv->lddla);
		return ret;
	}

	INIT_DELAYED_WORK(&priv->tick_work, en7572_tick_work);
	schedule_delayed_work(&priv->tick_work, HZ);
	return 0;
}

static void en7572_remove(struct i2c_client *client)
{
	struct en7572_priv *priv = i2c_get_clientdata(client);

	cancel_delayed_work_sync(&priv->tick_work);
	lddla_sfp_remove(&priv->lddla);
	lddla_debugfs_remove(&priv->lddla);
}

static const struct i2c_device_id en7572_id[] = {
	{ "en7572", EN7572_VARIANT_EN7572 },
	{ "an8901", EN7572_VARIANT_AN8901 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, en7572_id);

static const struct of_device_id en7572_of_match[] = {
	{ .compatible = "airoha,en7572", .data = (void *)EN7572_VARIANT_EN7572 },
	{ .compatible = "airoha,an8901", .data = (void *)EN7572_VARIANT_AN8901 },
	{ }
};
MODULE_DEVICE_TABLE(of, en7572_of_match);

struct i2c_driver en7572_i2c_driver = {
	.driver = {
		.name = "en7572",
		.of_match_table = en7572_of_match,
	},
	.probe = en7572_probe,
	.remove = en7572_remove,
	.id_table = en7572_id,
};
