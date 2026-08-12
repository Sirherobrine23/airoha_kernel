// SPDX-License-Identifier: GPL-2.0-only
/*
 * Semtech GN25L95 xPON burst-mode laser driver / limiting amplifier.
 *
 * The GN25L95 exposes an SFF-8472-style host interface.  The lower A2 page
 * carries the live DDMI measurements and status at the standard 0x60..0x75
 * locations.  The chip normally responds to the SFF A0/A2 addresses 0x50 and
 * 0x51; in external-MCU mode only the A2 page is exposed.  This driver binds
 * to the A2 address and publishes the device through the existing Airoha
 * LDDLA telemetry/hwmon/virtual-SFP integration so either hardware mode works.
 *
 * Register pointers are one byte wide.  Multi-byte DDMI values are stored
 * MSB-first as required by SFF-8472; this is independent of CPU endianness.
 */

#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/jiffies.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/regmap.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/unaligned.h>
#include <linux/workqueue.h>

#include "airoha_lddla.h"

#define GN25L95_I2C_A0_ADDR		0x50
#define GN25L95_I2C_A2_ADDR		0x51

#define GN25L95_DDMI_TEMP		0x60
#define GN25L95_DDMI_VCC		0x62
#define GN25L95_DDMI_TX_BIAS		0x64
#define GN25L95_DDMI_TX_POWER		0x66
#define GN25L95_DDMI_RX_POWER		0x68
#define GN25L95_STATUS_CTRL		0x6e
#define GN25L95_ALARM_FLAGS_1		0x70
#define GN25L95_ALARM_FLAGS_2		0x71

#define GN25L95_STATUS_TX_DISABLE	BIT(7)
#define GN25L95_STATUS_SOFT_TX_DISABLE	BIT(6)
#define GN25L95_STATUS_ROGUE_ONU	BIT(5)
#define GN25L95_STATUS_TX_FAULT		BIT(2)
#define GN25L95_STATUS_RX_LOS		BIT(1)
#define GN25L95_STATUS_DATA_READY_BAR	BIT(0)

#define GN25L95_ALARM_TEMP_HIGH		BIT(7)
#define GN25L95_ALARM_TEMP_LOW		BIT(6)
#define GN25L95_ALARM_VCC_HIGH		BIT(5)
#define GN25L95_ALARM_VCC_LOW		BIT(4)
#define GN25L95_ALARM_TX_BIAS_HIGH	BIT(3)
#define GN25L95_ALARM_TX_BIAS_LOW	BIT(2)
#define GN25L95_ALARM_TX_POWER_HIGH	BIT(1)
#define GN25L95_ALARM_TX_POWER_LOW	BIT(0)
#define GN25L95_ALARM_RX_POWER_HIGH	BIT(7)
#define GN25L95_ALARM_RX_POWER_LOW	BIT(6)

#define GN25L95_READY_TIMEOUT_MS	250
#define GN25L95_READY_POLL_MS		20
#define GN25L95_SOFT_TX_DELAY_MS	110

struct gn25l95_priv {
	/* Must remain first: airoha_lddla_get_telemetry() consumes drvdata. */
	struct airoha_lddla lddla;
	struct regmap *regmap;
	struct delayed_work tick_work;
	bool native_a0;
};

static const struct regmap_config gn25l95_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = 0xff,
	.cache_type = REGCACHE_NONE,
};

static int gn25l95_read_a0(struct gn25l95_priv *priv, u8 reg, u8 *buf,
			   size_t len)
{
	struct i2c_adapter *adap = priv->lddla.client->adapter;
	struct i2c_msg msgs[2] = {
		{
			.addr = GN25L95_I2C_A0_ADDR,
			.len = 1,
			.buf = &reg,
		}, {
			.addr = GN25L95_I2C_A0_ADDR,
			.flags = I2C_M_RD,
			.len = len,
			.buf = buf,
		},
	};
	int ret;

	ret = i2c_transfer(adap, msgs, ARRAY_SIZE(msgs));
	if (ret < 0)
		return ret;

	return ret == ARRAY_SIZE(msgs) ? 0 : -EIO;
}

static void gn25l95_decode_alarms(struct airoha_lddla *lddla, u8 alarm1,
				  u8 alarm2)
{
	u32 alarm = 0;

	if (alarm1 & GN25L95_ALARM_TEMP_HIGH)
		alarm |= AIROHA_ALARM_HIGH_TEMP;
	if (alarm1 & GN25L95_ALARM_TEMP_LOW)
		alarm |= AIROHA_ALARM_LOW_TEMP;
	if (alarm1 & GN25L95_ALARM_VCC_HIGH)
		alarm |= AIROHA_ALARM_HIGH_VOLT;
	if (alarm1 & GN25L95_ALARM_VCC_LOW)
		alarm |= AIROHA_ALARM_LOW_VOLT;
	if (alarm1 & GN25L95_ALARM_TX_BIAS_HIGH)
		alarm |= AIROHA_ALARM_TX_HIGH_BIAS;
	if (alarm1 & GN25L95_ALARM_TX_BIAS_LOW)
		alarm |= AIROHA_ALARM_TX_LOW_BIAS;
	if (alarm1 & GN25L95_ALARM_TX_POWER_HIGH)
		alarm |= AIROHA_ALARM_TX_HIGH_POWER;
	if (alarm1 & GN25L95_ALARM_TX_POWER_LOW)
		alarm |= AIROHA_ALARM_TX_LOW_POWER;
	if (alarm2 & GN25L95_ALARM_RX_POWER_HIGH)
		alarm |= AIROHA_ALARM_RX_HIGH_POWER;
	if (alarm2 & GN25L95_ALARM_RX_POWER_LOW)
		alarm |= AIROHA_ALARM_RX_LOW_POWER;

	lddla->alarm = alarm;
}

static int gn25l95_refresh_all(struct gn25l95_priv *priv)
{
	struct airoha_lddla *lddla = &priv->lddla;
	unsigned int status;
	u8 ddmi[10], alarms[2];
	int ret;

	ret = regmap_read(priv->regmap, GN25L95_STATUS_CTRL, &status);
	if (ret)
		return ret;
	if (status & GN25L95_STATUS_DATA_READY_BAR)
		return -EAGAIN;

	ret = regmap_bulk_read(priv->regmap, GN25L95_DDMI_TEMP,
			       ddmi, sizeof(ddmi));
	if (ret)
		return ret;

	lddla->ddmi_temperature = get_unaligned_be16(&ddmi[0]);
	lddla->ddmi_voltage = get_unaligned_be16(&ddmi[2]);
	lddla->ddmi_current = get_unaligned_be16(&ddmi[4]);
	lddla->ddmi_tx_power = get_unaligned_be16(&ddmi[6]);
	lddla->ddmi_rx_power = get_unaligned_be16(&ddmi[8]);

	ret = regmap_bulk_read(priv->regmap, GN25L95_ALARM_FLAGS_1,
			       alarms, sizeof(alarms));
	if (!ret)
		gn25l95_decode_alarms(lddla, alarms[0], alarms[1]);

	return 0;
}

static s32 gn25l95_op_temp(struct airoha_lddla *lddla)
{
	struct gn25l95_priv *priv = container_of(lddla, struct gn25l95_priv,
						 lddla);

	gn25l95_refresh_all(priv);
	return (s16)lddla->ddmi_temperature * 1000 / 256;
}

static s32 gn25l95_op_bosa_temp(struct airoha_lddla *lddla)
{
	/* GN25L95 has one internal temperature sensor, not a separate BOSA one. */
	return gn25l95_op_temp(lddla);
}

static u16 gn25l95_op_vcc(struct airoha_lddla *lddla)
{
	struct gn25l95_priv *priv = container_of(lddla, struct gn25l95_priv,
						 lddla);

	gn25l95_refresh_all(priv);
	return lddla->ddmi_voltage;
}

static u16 gn25l95_op_bias(struct airoha_lddla *lddla)
{
	struct gn25l95_priv *priv = container_of(lddla, struct gn25l95_priv,
						 lddla);

	gn25l95_refresh_all(priv);
	return lddla->ddmi_current;
}

static u16 gn25l95_op_tx_power(struct airoha_lddla *lddla)
{
	struct gn25l95_priv *priv = container_of(lddla, struct gn25l95_priv,
						 lddla);

	gn25l95_refresh_all(priv);
	return lddla->ddmi_tx_power;
}

static u16 gn25l95_op_rx_power(struct airoha_lddla *lddla)
{
	struct gn25l95_priv *priv = container_of(lddla, struct gn25l95_priv,
						 lddla);

	gn25l95_refresh_all(priv);
	return lddla->ddmi_rx_power;
}

static int gn25l95_op_tx_rearm(struct airoha_lddla *lddla)
{
	struct gn25l95_priv *priv = container_of(lddla, struct gn25l95_priv,
						 lddla);
	unsigned int status;
	int ret;

	ret = regmap_read(priv->regmap, GN25L95_STATUS_CTRL, &status);
	if (ret)
		return ret;

	if (status & GN25L95_STATUS_DATA_READY_BAR) {
		dev_warn_ratelimited(lddla->dev,
				     "optical frontend is not configured/DDMI-ready\n");
		return -EAGAIN;
	}

	/*
	 * External-MCU mode powers up with SOFT_TX_DISABLE asserted.  Clearing it
	 * is sufficient for the normal start path.  If a fault is already latched,
	 * pulse the software disable first; the data sheet specifies up to 100 ms
	 * for a software disable transition, hence the conservative delay.
	 */
	if ((status & GN25L95_STATUS_TX_FAULT) &&
	    !(status & GN25L95_STATUS_SOFT_TX_DISABLE)) {
		ret = regmap_update_bits(priv->regmap, GN25L95_STATUS_CTRL,
					 GN25L95_STATUS_SOFT_TX_DISABLE,
					 GN25L95_STATUS_SOFT_TX_DISABLE);
		if (ret)
			return ret;
		msleep(GN25L95_SOFT_TX_DELAY_MS);
	}

	ret = regmap_update_bits(priv->regmap, GN25L95_STATUS_CTRL,
				 GN25L95_STATUS_SOFT_TX_DISABLE, 0);
	if (ret)
		return ret;

	msleep(GN25L95_SOFT_TX_DELAY_MS);
	return 0;
}

static void gn25l95_op_diag_show(struct airoha_lddla *lddla,
				 struct seq_file *s)
{
	struct gn25l95_priv *priv = container_of(lddla, struct gn25l95_priv,
						 lddla);
	unsigned int status = 0;
	u8 a0_id = 0;

	regmap_read(priv->regmap, GN25L95_STATUS_CTRL, &status);
	if (priv->native_a0)
		gn25l95_read_a0(priv, 0, &a0_id, 1);

	seq_printf(s, "native A0:   %s\n", priv->native_a0 ? "yes" : "no");
	if (priv->native_a0)
		seq_printf(s, "A0 id:       0x%02x\n", a0_id);
	seq_printf(s, "status:      0x%02x\n", status & 0xff);
	seq_printf(s, "tx_disable:  %u\n",
		   !!(status & GN25L95_STATUS_TX_DISABLE));
	seq_printf(s, "soft_tx_dis: %u\n",
		   !!(status & GN25L95_STATUS_SOFT_TX_DISABLE));
	seq_printf(s, "tx_fault:    %u\n",
		   !!(status & GN25L95_STATUS_TX_FAULT));
	seq_printf(s, "rx_los:      %u\n",
		   !!(status & GN25L95_STATUS_RX_LOS));
	seq_printf(s, "rogue_onu:   %u\n",
		   !!(status & GN25L95_STATUS_ROGUE_ONU));
	seq_printf(s, "data_ready:  %u\n",
		   !(status & GN25L95_STATUS_DATA_READY_BAR));
}

static const struct airoha_lddla_ops gn25l95_ops = {
	.name = "gn25l95",
	.vendor_name = "Semtech",
	.vendor_oui = { 0x00, 0x00, 0x00 },
	.part_number = "GN25L95",
	.serial = "UNKNOWN",
	.date_code = "000000",
	.temp_refresh = gn25l95_op_temp,
	.bosa_temp_refresh = gn25l95_op_bosa_temp,
	.vcc_refresh = gn25l95_op_vcc,
	.bias_refresh = gn25l95_op_bias,
	.tx_power_refresh = gn25l95_op_tx_power,
	.rx_power_refresh = gn25l95_op_rx_power,
	.diag_show = gn25l95_op_diag_show,
	.tx_rearm = gn25l95_op_tx_rearm,
};

static int
gn25l95_wait_accessible(struct gn25l95_priv *priv, unsigned int *status)
{
	unsigned long deadline = jiffies +
		msecs_to_jiffies(GN25L95_READY_TIMEOUT_MS);
	int ret = -ETIMEDOUT;

	do {
		ret = regmap_read(priv->regmap, GN25L95_STATUS_CTRL, status);
		if (!ret)
			return 0;

		msleep(GN25L95_READY_POLL_MS);
	} while (time_before(jiffies, deadline));

	return ret;
}

static void gn25l95_tick_work(struct work_struct *work)
{
	struct gn25l95_priv *priv =
		container_of(to_delayed_work(work), struct gn25l95_priv, tick_work);

	if (!lddla_lock(&priv->lddla)) {
		gn25l95_refresh_all(priv);
		mutex_unlock(&priv->lddla.lock);
	}

	schedule_delayed_work(&priv->tick_work, HZ);
}

static int gn25l95_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct gn25l95_priv *priv;
	u8 a0_id;
	unsigned int status;
	int ret;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C))
		return -EOPNOTSUPP;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->lddla.client = client;
	priv->lddla.dev = dev;
	priv->lddla.ops = &gn25l95_ops;
	priv->lddla.pon_mode = AIROHA_PON_GPON;
	mutex_init(&priv->lddla.lock);
	memset(priv->lddla.flash, 0xff, sizeof(priv->lddla.flash));
	INIT_DELAYED_WORK(&priv->tick_work, gn25l95_tick_work);
	i2c_set_clientdata(client, priv);

	if (client->addr != GN25L95_I2C_A2_ADDR)
		dev_info(dev, "using non-default A2 address 0x%02x\n",
			 client->addr);

	priv->regmap = devm_regmap_init_i2c(client, &gn25l95_regmap_config);
	if (IS_ERR(priv->regmap))
		return dev_err_probe(dev, PTR_ERR(priv->regmap),
				     "failed to create register map\n");

	ret = gn25l95_wait_accessible(priv, &status);
	if (ret)
		return dev_err_probe(dev, ret,
				     "GN25L95 A2 interface did not respond\n");

	priv->native_a0 = !gn25l95_read_a0(priv, 0, &a0_id, 1);
	if (priv->native_a0)
		dev_info(dev, "native SFF A0/A2 interface detected (A0 id 0x%02x)\n",
			 a0_id);
	else
		dev_info(dev, "A2-only interface detected; possible external-MCU mode\n");

	if (status & GN25L95_STATUS_DATA_READY_BAR) {
		dev_warn(dev,
			 "DDMI data is not ready; keeping transmitter disabled until configured\n");
	} else {
		ret = gn25l95_refresh_all(priv);
		if (ret)
			return dev_err_probe(dev, ret, "failed to read DDMI data\n");
	}

	ret = lddla_hwmon_register(&priv->lddla);
	if (ret)
		return dev_err_probe(dev, ret, "failed to register hwmon\n");

	lddla_debugfs_init(&priv->lddla);

	ret = lddla_sfp_init(&priv->lddla);
	if (ret) {
		lddla_debugfs_remove(&priv->lddla);
		return dev_err_probe(dev, ret,
				     "failed to register virtual SFP bus\n");
	}

	schedule_delayed_work(&priv->tick_work, HZ);

	dev_info(dev,
		 "GN25L95 optical frontend ready at A2 address 0x%02x\n",
		 client->addr);
	return 0;
}

static void gn25l95_remove(struct i2c_client *client)
{
	struct gn25l95_priv *priv = i2c_get_clientdata(client);

	cancel_delayed_work_sync(&priv->tick_work);
	lddla_sfp_remove(&priv->lddla);
	lddla_debugfs_remove(&priv->lddla);
}

static const struct of_device_id gn25l95_of_match[] = {
	{ .compatible = "semtech,gn25l95" },
	{ }
};
MODULE_DEVICE_TABLE(of, gn25l95_of_match);

static const struct i2c_device_id gn25l95_i2c_ids[] = {
	{ "gn25l95" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, gn25l95_i2c_ids);

struct i2c_driver gn25l95_i2c_driver = {
	.driver = {
		.name = "gn25l95",
		.of_match_table = gn25l95_of_match,
	},
	.probe = gn25l95_probe,
	.remove = gn25l95_remove,
	.id_table = gn25l95_i2c_ids,
};

MODULE_DESCRIPTION("Semtech GN25L95 xPON optical frontend driver");
MODULE_LICENSE("GPL");
