// SPDX-License-Identifier: GPL-2.0-only
/*
 * Xiamen UX UX3320B1 optical frontend.
 *
 * The A2 lower half is shared; register 0x7f selects the upper 128-byte
 * bank. Calibration uses the final register image exported by command 3
 * of the AOT4221NK stock bosa_ux3320 driver, not its cfg/modlut/biaslut
 * manufacturing records. The chip runs the calibrated loops itself.
 */
#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/i2c.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/nvmem-consumer.h>
#include <linux/optical_frontend.h>
#include <linux/property.h>
#include <linux/slab.h>
#include <linux/unaligned.h>

#define UX3320_PAGE_SELECT		0x7f
#define UX3320_PAGE_SIZE		128
#define UX3320_IMAGE_SIZE		(6 * UX3320_PAGE_SIZE)
#define UX3320_IMAGE_USER		0x080
#define UX3320_IMAGE_DEVICE		0x100
#define UX3320_IMAGE_BIAS		0x180
#define UX3320_IMAGE_MOD			0x200
#define UX3320_IMAGE_APD			0x280
#define UX3320_LUT_SIZE			105
#define UX3320_APD_LUT_SIZE		93
#define UX3320_STATUS			0x6e
#define UX3320_DATA_NOT_READY		BIT(0)
#define UX3320_RX_LOS			BIT(1)
#define UX3320_TX_FAULT			BIT(2)
#define UX3320_SOFT_TX_DISABLE		BIT(6)
#define UX3320_TX_DISABLE		BIT(7)
#define UX3320_CHIP_ID			0xf5
#define UX3320_CHIP_ID_SIZE		8
#define UX3320_LOOP_MODE			0x89
#define UX3320_LOOP_MODE_MASK		GENMASK(7, 5)
#define UX3320_SINGLE_LOOP		0xc0
#define UX3320_DOUBLE_LOOP		0xe0
#define UX3320_LUT_CTRL			0x82
#define UX3320_MOD_LUT_ENABLE		BIT(6)
#define UX3320_FW_NAME			"ux/ux3320b1-hwconfig.bin"

struct ux3320 {
	struct i2c_client *client;
	struct optical_frontend *frontend;
	/* Also protects the bank selector shared by all register accesses. */
	struct mutex lock;
	bool calibrated;
	bool mode_set;
	bool tx_requested;
};

static int ux3320_write(struct ux3320 *priv, u8 reg, u8 val)
{
	u8 buf[] = { reg, val };
	int ret;

	ret = i2c_master_send(priv->client, buf, sizeof(buf));
	return ret == sizeof(buf) ? 0 : ret < 0 ? ret : -EIO;
}

static int ux3320_read(struct ux3320 *priv, u8 reg, void *data, u16 len)
{
	struct i2c_msg msgs[] = {
		{ .addr = priv->client->addr, .len = 1, .buf = &reg },
		{ .addr = priv->client->addr, .flags = I2C_M_RD,
		  .len = len, .buf = data },
	};
	int ret;

	ret = i2c_transfer(priv->client->adapter, msgs, ARRAY_SIZE(msgs));
	return ret == ARRAY_SIZE(msgs) ? 0 : ret < 0 ? ret : -EIO;
}

/* The caller holds priv->lock, including throughout bank selection. */
static int ux3320_update(struct ux3320 *priv, u8 reg, u8 mask, u8 value)
{
	u8 old;
	int ret;

	ret = ux3320_read(priv, reg, &old, 1);
	if (ret)
		return ret;
	return ux3320_write(priv, reg, (old & ~mask) | (value & mask));
}

static int ux3320_tx_gate(struct ux3320 *priv, bool disable)
{
	return ux3320_update(priv, UX3320_STATUS, UX3320_SOFT_TX_DISABLE,
			      disable ? UX3320_SOFT_TX_DISABLE : 0);
}

static int ux3320_wait_ready(struct ux3320 *priv)
{
	u8 status;
	int ret, err;

	err = read_poll_timeout(ux3320_read, ret,
				ret || !(status & UX3320_DATA_NOT_READY),
				1000, 500000, false, priv, UX3320_STATUS,
				&status, 1);
	return ret ?: err;
}

static int ux3320_write_bank(struct ux3320 *priv, u8 bank,
			     const u8 *data, size_t len)
{
	size_t i;
	int ret;

	ret = ux3320_write(priv, UX3320_PAGE_SELECT, bank);
	if (ret)
		return ret;
	/* Use the one-byte transactions of the reference SMT_Write(). */
	for (i = 0; i < len; i++) {
		ret = ux3320_write(priv, 0x80 + i, data[i]);
		if (ret)
			return ret;
	}
	return 0;
}

static int ux3320_check_image(const u8 *data, size_t size)
{
	u8 mode;

	if (size != UX3320_IMAGE_SIZE)
		return -EINVAL;
	/* Reject erased data and foreign manufacturing-record formats. */
	if (data[UX3320_IMAGE_DEVICE] != 0xaa ||
	    !memcmp(data + UX3320_IMAGE_DEVICE + UX3320_CHIP_ID - 0x80,
		    "UX3320S", 7))
		return -EINVAL;
	mode = data[UX3320_IMAGE_DEVICE + UX3320_LOOP_MODE - 0x80] &
		UX3320_LOOP_MODE_MASK;
	if (mode != UX3320_SINGLE_LOOP && mode != UX3320_DOUBLE_LOOP)
		return -EINVAL;
	if (!memchr_inv(data + UX3320_IMAGE_BIAS, 0, UX3320_LUT_SIZE) ||
	    !memchr_inv(data + UX3320_IMAGE_BIAS, 0xff, UX3320_LUT_SIZE) ||
	    !memchr_inv(data + UX3320_IMAGE_MOD, 0, UX3320_LUT_SIZE) ||
	    !memchr_inv(data + UX3320_IMAGE_MOD, 0xff, UX3320_LUT_SIZE))
		return -EINVAL;
	return 0;
}

static int ux3320_verify_bank(struct ux3320 *priv, u8 bank,
			      const u8 *data, size_t len)
{
	u8 buf[16];
	size_t pos, n;
	int ret;

	ret = ux3320_write(priv, UX3320_PAGE_SELECT, bank);
	if (ret)
		return ret;
	for (pos = 0; pos < len; pos += n) {
		n = min_t(size_t, sizeof(buf), len - pos);
		ret = ux3320_read(priv, 0x80 + pos, buf, n);
		if (ret)
			return ret;
		if (memcmp(buf, data + pos, n))
			return -EIO;
	}
	return 0;
}

static int ux3320_apply_image(struct ux3320 *priv, const u8 *data, size_t size)
{
	static const struct {
		u8 bank;
		u16 offset;
		u8 len;
	} banks[] = {
		{ 0, UX3320_IMAGE_USER, UX3320_PAGE_SIZE },
		{ 4, UX3320_IMAGE_BIAS, UX3320_LUT_SIZE },
		{ 5, UX3320_IMAGE_MOD, UX3320_LUT_SIZE },
		{ 6, UX3320_IMAGE_APD, UX3320_APD_LUT_SIZE },
		{ 3, UX3320_IMAGE_DEVICE, UX3320_PAGE_SIZE },
	};
	u8 mode;
	size_t i;
	int ret;

	priv->calibrated = false;
	ret = ux3320_check_image(data, size);
	if (ret)
		return ret;
	ret = ux3320_tx_gate(priv, true);
	if (ret)
		return ret;
	ret = ux3320_wait_ready(priv);
	if (ret)
		goto disable;
	/* Native digitalDiagnosticsFlash covers 0x00..0x79, not 0x7f. */
	for (i = 0; i < 0x7a; i++) {
		u8 val = data[i];

		if (i == UX3320_STATUS)
			val |= UX3320_SOFT_TX_DISABLE;
		ret = ux3320_write(priv, i, val);
		if (ret)
			goto disable;
	}
	for (i = 0; i < ARRAY_SIZE(banks); i++) {
		ret = ux3320_write_bank(priv, banks[i].bank,
					data + banks[i].offset, banks[i].len);
		if (ret)
			goto disable;
	}
	mode = data[UX3320_IMAGE_DEVICE + UX3320_LOOP_MODE - 0x80] &
		UX3320_LOOP_MODE_MASK;
	if (mode == UX3320_SINGLE_LOOP) {
		/* Commit final per-unit LUTs after the device/loop controls. */
		ret = ux3320_write_bank(priv, 5, data + UX3320_IMAGE_MOD,
					UX3320_LUT_SIZE);
		if (ret)
			goto disable;
		ret = ux3320_write(priv, UX3320_PAGE_SELECT, 3);
		if (ret)
			goto disable;
		ret = ux3320_update(priv, UX3320_LUT_CTRL,
				    UX3320_MOD_LUT_ENABLE, UX3320_MOD_LUT_ENABLE);
		if (ret)
			goto disable;
		ret = ux3320_write_bank(priv, 4, data + UX3320_IMAGE_BIAS,
					UX3320_LUT_SIZE);
		if (ret)
			goto disable;
	}
	for (i = 0; i < ARRAY_SIZE(banks) - 1; i++) {
		ret = ux3320_verify_bank(priv, banks[i].bank,
					 data + banks[i].offset, banks[i].len);
		if (ret)
			goto disable;
	}
	ret = ux3320_write(priv, UX3320_PAGE_SELECT, 0);
	if (ret)
		goto disable;
	msleep(20);
	ret = ux3320_wait_ready(priv);
	if (!ret)
		priv->calibrated = true;
disable:
	/* Never release the laser while loading or on a partial I2C upload. */
	if (ux3320_tx_gate(priv, true) && !ret)
		ret = -EIO;
	if (ret)
		priv->calibrated = false;
	return ret;
}

static int ux3320_load_calibration(struct ux3320 *priv)
{
	struct device *dev = &priv->client->dev;
	const struct firmware *fw = NULL;
	const char *name = UX3320_FW_NAME;
	struct nvmem_cell *cell;
	const u8 *data;
	u8 *nvmem = NULL;
	size_t size;
	int ret;

	cell = nvmem_cell_get(dev, "calibration");
	if (!IS_ERR(cell)) {
		nvmem = nvmem_cell_read(cell, &size);
		nvmem_cell_put(cell);
		if (IS_ERR(nvmem))
			return PTR_ERR(nvmem);
		data = nvmem;
	} else {
		ret = PTR_ERR(cell);
		if (ret != -ENOENT && ret != -ENODEV && ret != -EOPNOTSUPP)
			return ret;
		/* A declared cell must not silently fall back to another unit's file. */
		if (device_property_match_string(dev, "nvmem-cell-names",
						 "calibration") >= 0)
			return ret;
		ret = device_property_read_string(dev, "firmware-name", &name);
		if (ret && device_property_present(dev, "firmware-name"))
			return ret;
		ret = request_firmware_direct(&fw, name, dev);
		if (ret == -ENOENT) {
			dev_warn(dev, "no calibrated register image; TX remains disabled\n");
			return 0;
		}
		if (ret)
			return ret;
		data = fw->data;
		size = fw->size;
	}
	ret = ux3320_apply_image(priv, data, size);
	release_firmware(fw);
	kfree(nvmem);
	return ret;
}

static int ux3320_set_mode(struct optical_frontend *frontend,
			   const struct optical_frontend_mode *mode)
{
	struct ux3320 *priv = optical_frontend_get_drvdata(frontend);
	int ret;

	/* Only the GPON configuration is established by the B1 reference. */
	if (mode->protocol != OPTICAL_FRONTEND_PROTO_GPON ||
	    mode->tx_rate != 1244160000ULL || mode->rx_rate != 2488320000ULL ||
	    mode->flags != OPTICAL_FRONTEND_MODE_BURST_TX)
		return -EOPNOTSUPP;
	mutex_lock(&priv->lock);
	priv->mode_set = false;
	priv->tx_requested = false;
	ret = ux3320_tx_gate(priv, true);
	if (!ret)
		priv->mode_set = true;
	mutex_unlock(&priv->lock);
	return ret;
}

static int ux3320_enable_tx(struct ux3320 *priv)
{
	u8 status;
	int ret;

	if (!priv->calibrated || !priv->mode_set)
		return -EACCES;
	/* The native driver pulses soft TX_DISABLE for 20 ms after setup. */
	ret = ux3320_tx_gate(priv, true);
	if (ret)
		return ret;
	msleep(20);
	ret = ux3320_wait_ready(priv);
	if (ret)
		return ret;
	ret = ux3320_tx_gate(priv, false);
	if (!ret) {
		msleep(20);
		ret = ux3320_read(priv, UX3320_STATUS, &status, 1);
		if (!ret && (status & (UX3320_DATA_NOT_READY |
				      UX3320_TX_FAULT | UX3320_SOFT_TX_DISABLE)))
			ret = -EIO;
		if (!ret)
			return 0;
	}
	ux3320_tx_gate(priv, true);
	return ret;
}

static int ux3320_tx_enable(struct optical_frontend *frontend, bool enable)
{
	struct ux3320 *priv = optical_frontend_get_drvdata(frontend);
	int ret;

	mutex_lock(&priv->lock);
	priv->tx_requested = false;
	ret = enable ? ux3320_enable_tx(priv) : ux3320_tx_gate(priv, true);
	if (!ret)
		priv->tx_requested = enable;
	mutex_unlock(&priv->lock);
	return ret;
}

static int ux3320_tx_rearm(struct optical_frontend *frontend)
{
	struct ux3320 *priv = optical_frontend_get_drvdata(frontend);
	int ret;

	mutex_lock(&priv->lock);
	if (priv->tx_requested) {
		priv->tx_requested = false;
		ret = ux3320_enable_tx(priv);
		if (!ret)
			priv->tx_requested = true;
	} else {
		/* An explicit future enable performs the pulse; keep TX off now. */
		ret = ux3320_tx_gate(priv, true);
	}
	mutex_unlock(&priv->lock);
	return ret;
}

static int ux3320_get_state(struct optical_frontend *frontend,
			    struct optical_frontend_state *state)
{
	struct ux3320 *priv = optical_frontend_get_drvdata(frontend);
	u8 status;
	int ret;

	mutex_lock(&priv->lock);
	ret = ux3320_read(priv, UX3320_STATUS, &status, 1);
	if (!ret) {
		state->valid = OPTICAL_FRONTEND_STATE_F_PRESENT |
			       OPTICAL_FRONTEND_STATE_F_READY |
			       OPTICAL_FRONTEND_STATE_F_RX_LOS |
			       OPTICAL_FRONTEND_STATE_F_TX_FAULT |
			       OPTICAL_FRONTEND_STATE_F_TX_ENABLED;
		state->present = true;
		state->ready = priv->calibrated && !(status & UX3320_DATA_NOT_READY);
		state->rx_los = !!(status & UX3320_RX_LOS);
		state->tx_fault = !!(status & UX3320_TX_FAULT);
		state->tx_enabled = !(status & (UX3320_TX_DISABLE | UX3320_SOFT_TX_DISABLE));
	}
	mutex_unlock(&priv->lock);
	return ret;
}

static int ux3320_get_telemetry(struct optical_frontend *frontend,
				struct optical_frontend_telemetry *telemetry)
{
	struct ux3320 *priv = optical_frontend_get_drvdata(frontend);
	u8 ddm[16];
	int ret;

	mutex_lock(&priv->lock);
	ret = ux3320_write(priv, UX3320_PAGE_SELECT, 0);
	if (!ret)
		ret = ux3320_read(priv, 0x60, ddm, sizeof(ddm));
	if (!ret && (ddm[UX3320_STATUS - 0x60] & UX3320_DATA_NOT_READY))
		ret = -EAGAIN;
	if (!ret) {
		telemetry->valid = OPTICAL_FRONTEND_TELEMETRY_F_TEMPERATURE |
			OPTICAL_FRONTEND_TELEMETRY_F_VOLTAGE |
			OPTICAL_FRONTEND_TELEMETRY_F_BIAS |
			OPTICAL_FRONTEND_TELEMETRY_F_TX_POWER |
			OPTICAL_FRONTEND_TELEMETRY_F_RX_POWER;
		telemetry->temperature_mc = (s16)get_unaligned_be16(ddm) * 1000 / 256;
		telemetry->voltage_uv = get_unaligned_be16(ddm + 2) * 100;
		telemetry->bias_ua = get_unaligned_be16(ddm + 4) * 2;
		telemetry->tx_power_nw = get_unaligned_be16(ddm + 6) * 100;
		telemetry->rx_power_nw = get_unaligned_be16(ddm + 8) * 100;
	}
	mutex_unlock(&priv->lock);
	return ret;
}

static const struct optical_frontend_desc ux3320_desc = {
	.name = "ux3320b1",
	.type = "lddla",
	.vendor_name = "Xiamen UX",
	.part_number = "UX3320B1",
	.protocols = OPTICAL_FRONTEND_PROTO_GPON,
	.capabilities = OPTICAL_FRONTEND_CAP_TEMPERATURE |
		OPTICAL_FRONTEND_CAP_VOLTAGE | OPTICAL_FRONTEND_CAP_BIAS |
		OPTICAL_FRONTEND_CAP_TX_POWER | OPTICAL_FRONTEND_CAP_RX_POWER |
		OPTICAL_FRONTEND_CAP_TX_DISABLE | OPTICAL_FRONTEND_CAP_TX_REARM |
		OPTICAL_FRONTEND_CAP_TX_FAULT | OPTICAL_FRONTEND_CAP_RX_LOS |
		OPTICAL_FRONTEND_CAP_BURST_TX,
	.telemetry_cache_ms = 1000,
};

static const struct optical_frontend_ops ux3320_ops = {
	.set_mode = ux3320_set_mode,
	.tx_enable = ux3320_tx_enable,
	.tx_rearm = ux3320_tx_rearm,
	.get_state = ux3320_get_state,
	.get_telemetry = ux3320_get_telemetry,
};

static void ux3320_quiesce(void *data)
{
	struct ux3320 *priv = data;
	int ret;

	mutex_lock(&priv->lock);
	priv->tx_requested = false;
	ret = ux3320_tx_gate(priv, true);
	mutex_unlock(&priv->lock);
	if (ret)
		dev_warn(&priv->client->dev, "failed to disable TX: %d\n", ret);
}

static int ux3320_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct ux3320 *priv;
	u8 id[UX3320_CHIP_ID_SIZE];
	int ret;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C))
		return -EOPNOTSUPP;
	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;
	priv->client = client;
	mutex_init(&priv->lock);
	i2c_set_clientdata(client, priv);
	ret = ux3320_tx_gate(priv, true);
	if (ret)
		return dev_err_probe(dev, ret, "failed to hold TX disabled\n");
	ret = devm_add_action_or_reset(dev, ux3320_quiesce, priv);
	if (ret)
		return ret;
	ret = ux3320_write(priv, UX3320_PAGE_SELECT, 3);
	if (!ret)
		ret = ux3320_read(priv, UX3320_CHIP_ID, id, sizeof(id));
	if (ret)
		return ret;
	/* B1 may return an unprogrammed ID; DT supplies the exact chip model. */
	if (!memcmp(id, "UX3320S", 7))
		return dev_err_probe(dev, -ENODEV, "UX3320S needs a separate backend\n");
	ret = ux3320_write(priv, UX3320_PAGE_SELECT, 0);
	if (ret)
		return ret;
	dev_dbg(dev, "bank 3 chip ID: %8phN\n", id);
	ret = ux3320_load_calibration(priv);
	if (ret)
		return dev_err_probe(dev, ret, "failed to load calibrated register image\n");
	priv->frontend = devm_optical_frontend_register(dev, &ux3320_desc,
							&ux3320_ops, priv);
	if (IS_ERR(priv->frontend))
		return PTR_ERR(priv->frontend);
	return devm_optical_frontend_hwmon_register(priv->frontend);
}

static void ux3320_shutdown(struct i2c_client *client)
{
	ux3320_quiesce(i2c_get_clientdata(client));
}

static const struct of_device_id ux3320_of_match[] = {
	{ .compatible = "ux,ux3320b1" },
	{ }
};
MODULE_DEVICE_TABLE(of, ux3320_of_match);

static const struct i2c_device_id ux3320_i2c_ids[] = {
	{ "ux3320b1" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, ux3320_i2c_ids);

static struct i2c_driver ux3320_driver = {
	.driver = {
		.name = "ux3320b1",
		.of_match_table = ux3320_of_match,
	},
	.probe = ux3320_probe,
	.shutdown = ux3320_shutdown,
	.id_table = ux3320_i2c_ids,
};
module_i2c_driver(ux3320_driver);

MODULE_FIRMWARE(UX3320_FW_NAME);
MODULE_DESCRIPTION("Xiamen UX UX3320B1 optical frontend");
MODULE_LICENSE("GPL");
