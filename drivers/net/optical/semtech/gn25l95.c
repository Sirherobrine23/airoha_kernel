// SPDX-License-Identifier: GPL-2.0-only
/*
 * Semtech GN25L95/GN25L98 xPON burst-mode laser driver / limiting amplifier.
 *
 * The GN25L95 and GN25L98 expose an SFF-8472-style host interface. The lower
 * A2 page carries the live DDMI measurements and status at the standard
 * 0x60..0x75 locations. The chips normally respond to the SFF A0/A2 addresses
 * 0x50 and 0x51; in external-MCU mode only the A2 page is exposed. This driver
 * binds to the A2 address and registers an independent generic
 * optical_frontend provider so MAC, PHY, OMCI and diagnostic consumers can
 * share the device.
 *
 * Register pointers are one byte wide.  Multi-byte DDMI values are stored
 * MSB-first as required by SFF-8472; this is independent of CPU endianness.
 */

#include <linux/bitops.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/i2c.h>
#include <linux/jiffies.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/optical_frontend.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/unaligned.h>
#include <linux/workqueue.h>


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

#define GN25L95_TABLE_SELECT		0x7f
#define GN25L95_TABLE_0		0x00
#define GN25L95_TABLE_2		0x02
#define GN25L95_TABLE_4		0x04
#define GN25L95_TABLE_5		0x05
#define GN25L95_TABLE_6		0x06

#define GN25L95_TABLE2_SAFE_MODE_STARTUP	0xa0
#define GN25L98_TABLE2_SAFE_MODE_STARTUP	0x98
#define GN25L95_TABLE2_RX_LOS_LEVEL		0xbc
#define GN25L95_TABLE2_APD_DAC_CTRL		0xbd
#define GN25L95_TABLE2_APD_DAC			0xbe
#define GN25L95_TABLE2_STATUS_0		0xc0
#define GN25L95_TABLE2_EE_ACCESS		0xc4
#define GN25L95_TABLE2_EEPROM_CTRL		0xc7

#define GN25L95_STATUS0_DDMI_READY	BIT(1)
#define GN25L95_STATUS0_EEPROM_PRESENT	BIT(0)
#define GN25L95_APD_DAC_LUT_EN		BIT(0)
#define GN25L95_SAFE_MODE_NORMAL	0x6a

/* Vendor /bosa/hwconfig layout. */
#define GN25L95_HWCONFIG_SIZE		0x800
#define GN25L95_HWCONFIG_TABLE6		0x000
#define GN25L95_HWCONFIG_TABLE5		0x080
#define GN25L95_HWCONFIG_TABLE4		0x100
#define GN25L95_HWCONFIG_TABLE2		0x180
#define GN25L95_HWCONFIG_TABLE0		0x200
#define GN25L95_DEFAULT_FW_NAME	"semtech/gn25l95-hwconfig.bin"
#define GN25L98_DEFAULT_FW_NAME	"semtech/gn25l98-hwconfig.bin"
#define GN25L98_HWCONFIG_SIZE		0x280
#define GN25L98_HWCONFIG_TABLE6		0x000
#define GN25L98_HWCONFIG_TABLE5		0x080
#define GN25L98_HWCONFIG_TABLE4		0x100
#define GN25L98_HWCONFIG_TABLE2		0x180
#define GN25L98_HWCONFIG_TABLE0		0x200
#define GN25L95_VENDOR_WRITE_DELAY_MS	10
#define GN25L95_VENDOR_READ_DELAY_MS	1
#define GN25L95_RESET_DELAY_MS		150
#define GN25L95_READY_TIMEOUT_MS	250
#define GN25L95_READY_POLL_MS		20
#define GN25L95_SOFT_TX_DELAY_MS	110
#define GN25L98_TX_START_DELAY_MS	25

enum gn25l9x_variant {
	GN25L9X_VARIANT_95,
	GN25L9X_VARIANT_98,
};

struct gn25l95_chip_data {
	struct optical_frontend_desc frontend;
	u8 default_i2c_addr;
	const char *default_fw_name;
	enum gn25l9x_variant variant;
};

struct gn25l95_priv {
	struct device *dev;
	struct i2c_client *client;
	const struct gn25l95_chip_data *data;
	struct optical_frontend *frontend;
	struct regmap *regmap;
	struct mutex lock; /* serializes register access and calibration */
	struct delayed_work tick_work;
	struct dentry *debugfs;
	const char *fw_name;
	u32 alarm;
	u16 ddmi_temperature;
	u16 ddmi_voltage;
	u16 ddmi_current;
	u16 ddmi_tx_power;
	u16 ddmi_rx_power;
	bool native_a0;
	bool eeprom_present;
	bool calibrated;
};

static const struct regmap_config gn25l95_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = 0xff,
	.cache_type = REGCACHE_NONE,
};

static const struct optical_frontend_thresholds gn25l95_thresholds = {
	.valid = OPTICAL_FRONTEND_THRESHOLD_F_TEMPERATURE |
		 OPTICAL_FRONTEND_THRESHOLD_F_VOLTAGE |
		 OPTICAL_FRONTEND_THRESHOLD_F_BIAS |
		 OPTICAL_FRONTEND_THRESHOLD_F_TX_POWER |
		 OPTICAL_FRONTEND_THRESHOLD_F_RX_POWER,
	.temperature_low_mc = -5000,
	.temperature_high_mc = 85000,
	.voltage_low_uv = 2900000,
	.voltage_high_uv = 3700000,
	.bias_low_ua = 1000,
	.bias_high_ua = 100000,
	.tx_power_low_nw = 1000000,
	.tx_power_high_nw = 3548100,
	.rx_power_low_nw = 1000,
	.rx_power_high_nw = 251100,
};

static const struct gn25l95_chip_data gn25l95_data = {
	.frontend = {
		.name = "gn25l95",
		.type = "lddla",
		.vendor_name = "Semtech",
		.vendor_oui = { 0x00, 0x00, 0x00 },
		.part_number = "GN25L95",
		.serial = "UNKNOWN",
		.date_code = "000000",
		.capabilities = OPTICAL_FRONTEND_CAP_TEMPERATURE |
				OPTICAL_FRONTEND_CAP_VOLTAGE |
				OPTICAL_FRONTEND_CAP_BIAS |
				OPTICAL_FRONTEND_CAP_TX_POWER |
				OPTICAL_FRONTEND_CAP_RX_POWER |
				OPTICAL_FRONTEND_CAP_ALARMS |
				OPTICAL_FRONTEND_CAP_TX_REARM |
				OPTICAL_FRONTEND_CAP_TX_DISABLE |
				OPTICAL_FRONTEND_CAP_TX_FAULT |
				OPTICAL_FRONTEND_CAP_RX_LOS |
				OPTICAL_FRONTEND_CAP_BURST_TX |
				OPTICAL_FRONTEND_CAP_APD,
		.protocols = BIT(OPTICAL_FRONTEND_PROTO_EPON) |
			     BIT(OPTICAL_FRONTEND_PROTO_GPON),
		.thresholds = &gn25l95_thresholds,
		.telemetry_cache_ms = 100,
	},
	.default_i2c_addr = GN25L95_I2C_A2_ADDR,
	.default_fw_name = GN25L95_DEFAULT_FW_NAME,
	.variant = GN25L9X_VARIANT_95,
};

static const struct gn25l95_chip_data gn25l98_data = {
	.frontend = {
		.name = "gn25l98",
		.type = "lddla",
		.vendor_name = "Semtech",
		.vendor_oui = { 0x00, 0x00, 0x00 },
		.part_number = "GN25L98",
		.serial = "UNKNOWN",
		.date_code = "000000",
		.capabilities = OPTICAL_FRONTEND_CAP_TEMPERATURE |
				OPTICAL_FRONTEND_CAP_VOLTAGE |
				OPTICAL_FRONTEND_CAP_BIAS |
				OPTICAL_FRONTEND_CAP_TX_POWER |
				OPTICAL_FRONTEND_CAP_RX_POWER |
				OPTICAL_FRONTEND_CAP_ALARMS |
				OPTICAL_FRONTEND_CAP_TX_REARM |
				OPTICAL_FRONTEND_CAP_TX_DISABLE |
				OPTICAL_FRONTEND_CAP_TX_FAULT |
				OPTICAL_FRONTEND_CAP_RX_LOS |
				OPTICAL_FRONTEND_CAP_BURST_TX |
				OPTICAL_FRONTEND_CAP_APD,
		.protocols = BIT(OPTICAL_FRONTEND_PROTO_EPON) |
			     BIT(OPTICAL_FRONTEND_PROTO_GPON),
		.thresholds = &gn25l95_thresholds,
		.telemetry_cache_ms = 100,
	},
	.default_i2c_addr = GN25L95_I2C_A2_ADDR,
	.default_fw_name = GN25L98_DEFAULT_FW_NAME,
	.variant = GN25L9X_VARIANT_98,
};

static int gn25l95_vendor_write_byte(struct gn25l95_priv *priv, u8 reg,
				     u8 val)
{
	int ret;

	ret = regmap_write(priv->regmap, reg, val);
	if (ret)
		return ret;

	/* The stock SIF_X_Write path waits after every transaction. */
	msleep(GN25L95_VENDOR_WRITE_DELAY_MS);

	return 0;
}

static int gn25l95_select_table(struct gn25l95_priv *priv, u8 table)
{
	return gn25l95_vendor_write_byte(priv, GN25L95_TABLE_SELECT, table);
}

/*
 * The vendor driver never sends the calibration image as one long I2C write.
 * It advances in naturally aligned 1/2/4-byte transactions and waits 10 ms
 * after each transaction.  Keep that ordering here: apart from matching the
 * known-good firmware, it also preserves the write-latch semantics of the
 * adjacent APCSET/BIAS/MOD DAC registers in table 2.
 */
static int gn25l95_vendor_write_range(struct gn25l95_priv *priv, u8 reg,
				      const u8 *buf, size_t len)
{
	size_t chunk;
	int ret;

	while (len) {
		if (!(reg & 3) && len >= 4)
			chunk = 4;
		else if (!(reg & 1) && len >= 2)
			chunk = 2;
		else
			chunk = 1;

		ret = regmap_bulk_write(priv->regmap, reg, buf, chunk);
		if (ret)
			return ret;

		/* The vendor repeats the A8/AA writes before advancing. */
		if ((reg == 0xa8 && chunk == 4) ||
		    (reg == 0xaa && chunk == 2)) {
			msleep(GN25L95_VENDOR_WRITE_DELAY_MS);
			ret = regmap_bulk_write(priv->regmap, reg, buf, chunk);
			if (ret)
				return ret;
		}

		msleep(GN25L95_VENDOR_WRITE_DELAY_MS);
		reg += chunk;
		buf += chunk;
		len -= chunk;
	}

	return 0;
}

static int gn25l95_vendor_read_range(struct gn25l95_priv *priv, u8 reg,
				     u8 *buf, size_t len)
{
	unsigned int val;
	size_t i;
	int ret;

	for (i = 0; i < len; i++) {
		ret = regmap_read(priv->regmap, reg + i, &val);
		if (ret)
			return ret;

		buf[i] = val;
		/* Stock __transceiver_read_range() delays between bytes. */
		msleep(GN25L95_VENDOR_READ_DELAY_MS);
	}

	return 0;
}

static int gn25l95_soft_reset(struct gn25l95_priv *priv)
{
	static const u8 reset_seq[] = { 0x5d, 0x2c, 0x6a, 0xc9, 0x0b };
	int ret;

	ret = gn25l95_select_table(priv, GN25L95_TABLE_0);
	if (ret)
		return ret;

	ret = gn25l95_vendor_write_range(priv, 0x7b, reset_seq,
					 sizeof(reset_seq));
	if (ret)
		return ret;

	/* Stock pon_transceiver.ko waits 0x96 ms before probing the chip. */
	msleep(GN25L95_RESET_DELAY_MS);
	return 0;
}

static int gn25l95_read_eeprom_status(struct gn25l95_priv *priv, u8 *status)
{
	int ret;

	ret = gn25l95_select_table(priv, GN25L95_TABLE_2);
	if (ret)
		return ret;

	return gn25l95_vendor_read_range(priv, GN25L95_TABLE2_STATUS_0,
					   status, 1);
}

static int gn25l95_program_hwconfig(struct gn25l95_priv *priv,
				    const u8 *data, size_t size)
{
	struct device *dev = priv->dev;
	int ret;

	if (size != GN25L95_HWCONFIG_SIZE)
		return -EINVAL;

	/*
	 * Reproduce bob_readfile_init() from the stock pon_transceiver.ko.
	 * Only the ranges used by the stock boot path are written.  In
	 * particular table 6 0x80..0xbf is not part of that start sequence;
	 * the calibrated APD DAC LUT is table 6 0xc0..0xff (file + 0x40).
	 */
	ret = gn25l95_select_table(priv, GN25L95_TABLE_2);
	if (ret) {
		dev_err(dev, "cannot select table 2");
		return ret;
	}

	ret = gn25l95_vendor_write_byte(priv, GN25L95_TABLE2_EEPROM_CTRL, 0);
	if (ret) {
		dev_err(dev, "error on set 0 for eeprom controller");
		return ret;
	}

	ret = gn25l95_vendor_write_byte(priv, GN25L95_TABLE2_EE_ACCESS, 0);
	if (ret) {
		dev_err(dev, "error on set 0 for eeprom access");
		return ret;
	}

	ret = gn25l95_vendor_write_byte(priv,
					GN25L95_TABLE2_SAFE_MODE_STARTUP, 0);
	if (ret) {
		dev_err(dev, "error on set safe startup mode");
		return ret;
	}

	ret = gn25l95_select_table(priv, GN25L95_TABLE_0);
	if (ret) {
		dev_err(dev, "error on select table 0");
		return ret;
	}
	
	ret = gn25l95_vendor_write_range(priv, 0x00,
					 data + GN25L95_HWCONFIG_TABLE0, 0x7b);
	if (ret) {
		dev_err(dev, "error on write range for table 0");
		return ret;
	}

	ret = gn25l95_select_table(priv, GN25L95_TABLE_2);
	if (ret) {
		dev_err(dev, "error on select table 2 for write range");
		return ret;
	}

	ret = gn25l95_vendor_write_range(priv, 0x80,
					 data + GN25L95_HWCONFIG_TABLE2, 0x80);
	if (ret) {
		dev_err(dev, "error on write range in table 2");
		return ret;
	}
	
	ret = gn25l95_vendor_write_byte(priv,
					GN25L95_TABLE2_SAFE_MODE_STARTUP,
					  GN25L95_SAFE_MODE_NORMAL);
	if (ret) {
		dev_err(dev, "error on back to normal startup");
		return ret;
	}

	ret = gn25l95_select_table(priv, GN25L95_TABLE_4);
	if (ret) {
		dev_err(dev, "error on select table 4");
		return ret;
	}
	
	ret = gn25l95_vendor_write_range(priv, 0x80,
					 data + GN25L95_HWCONFIG_TABLE4, 0x80);
	if (ret) {
		dev_err(dev, "error on write range for table 4");
		return ret;
	}

	ret = gn25l95_select_table(priv, GN25L95_TABLE_5);
	if (ret) {
		dev_err(dev, "error on select table 5");
		return ret;
	}

	ret = gn25l95_vendor_write_range(priv, 0x80,
					 data + GN25L95_HWCONFIG_TABLE5, 0x80);
	if (ret) {
		dev_err(dev, "error on write range table 5");
		return ret;
	}

	ret = gn25l95_select_table(priv, GN25L95_TABLE_6);
	if (ret) {
		dev_err(dev, "error on select table 6");
		return ret;
	}

	ret = gn25l95_vendor_write_range(priv, 0xc0,
					 data + GN25L95_HWCONFIG_TABLE6 + 0x40,
					 0x40);
	if (ret) {
		dev_err(dev, "error on write to table 6");
		return ret;
	}

	/* Lower A2/DDMI does not depend on the upper-page selector. */
	return 0;
}

static int gn25l98_write_bytes(struct gn25l95_priv *priv, u8 reg,
			       const u8 *buf, size_t len)
{
	size_t i;
	int ret;

	for (i = 0; i < len; i++) {
		ret = regmap_write(priv->regmap, reg + i, buf[i]);
		if (ret)
			return ret;
	}

	return 0;
}

static int gn25l98_write_words(struct gn25l95_priv *priv, u8 reg,
			       const u8 *buf, size_t len)
{
	size_t i;
	int ret;

	if (len & 1)
		return -EINVAL;

	for (i = 0; i < len; i += 2) {
		ret = regmap_bulk_write(priv->regmap, reg + i, buf + i, 2);
		if (ret)
			return ret;
	}

	return 0;
}

static int gn25l98_select_table(struct gn25l95_priv *priv, u8 table)
{
	return regmap_write(priv->regmap, GN25L95_TABLE_SELECT, table);
}

static int gn25l98_wait_ddmi_ready(struct gn25l95_priv *priv)
{
	unsigned long deadline = jiffies +
		msecs_to_jiffies(GN25L95_READY_TIMEOUT_MS);
	unsigned int val;
	u8 status0;
	int ret;

	do {
		ret = gn25l98_select_table(priv, GN25L95_TABLE_2);
		if (ret)
			return ret;

		ret = regmap_read(priv->regmap, GN25L95_TABLE2_STATUS_0, &val);
		if (ret)
			return ret;
		status0 = val;
		if (status0 & GN25L95_STATUS0_DDMI_READY)
			return 0;

		msleep(GN25L95_READY_POLL_MS);
	} while (time_before(jiffies, deadline));

	return -ETIMEDOUT;
}

static int gn25l98_program_hwconfig(struct gn25l95_priv *priv,
				    const u8 *data, size_t size)
{
	struct device *dev = priv->dev;
	int ret;

	if (size != GN25L98_HWCONFIG_SIZE)
		return -EINVAL;

	/*
	 * The GN25L98 vendor driver waits for table 2 STATUS_0 bit 1 before
	 * touching the calibration tables. It then holds the transmitter in
	 * software-disable while tables 4, 5, 6, 0 and 2 are programmed.
	 */
	ret = gn25l98_wait_ddmi_ready(priv);
	if (ret)
		return dev_err_probe(dev, ret,
				     "GN25L98 DDMI interface did not become ready\n");

	ret = gn25l98_select_table(priv, GN25L95_TABLE_0);
	if (ret)
		return ret;

	ret = regmap_write(priv->regmap, GN25L95_STATUS_CTRL,
			   GN25L95_STATUS_SOFT_TX_DISABLE);
	if (ret)
		return ret;

	ret = gn25l98_select_table(priv, GN25L95_TABLE_4);
	if (ret)
		goto out_disable;

	ret = gn25l98_write_words(priv, 0x80,
				  data + GN25L98_HWCONFIG_TABLE4, 0x80);
	if (ret)
		goto out_disable;

	ret = gn25l98_select_table(priv, GN25L95_TABLE_5);
	if (ret)
		goto out_disable;

	ret = gn25l98_write_words(priv, 0x80,
				  data + GN25L98_HWCONFIG_TABLE5, 0x80);
	if (ret)
		goto out_disable;

	ret = gn25l95_select_table(priv, GN25L95_TABLE_6);
	if (ret)
		goto out_disable;

	/* The vendor image contains 32 16-bit APD LUT entries at +0x00. */
	ret = gn25l98_write_bytes(priv, 0xc0,
				  data + GN25L98_HWCONFIG_TABLE6, 0x40);
	if (ret)
		goto out_disable;

	ret = gn25l98_select_table(priv, GN25L95_TABLE_0);
	if (ret)
		goto out_disable;

	ret = gn25l98_write_bytes(priv, 0x00,
				  data + GN25L98_HWCONFIG_TABLE0, 0x80);
	if (ret)
		goto out_disable;

	/* Keep the vendor APD monitor windows used by the reference driver. */
	ret = regmap_write(priv->regmap, 0xf8, 0xff);
	if (ret)
		goto out_disable;
	ret = regmap_write(priv->regmap, 0xf9, 0xc0);
	if (ret)
		goto out_disable;
	ret = regmap_write(priv->regmap, 0xfc, 0xff);
	if (ret)
		goto out_disable;
	ret = regmap_write(priv->regmap, 0xfd, 0xc0);
	if (ret)
		goto out_disable;

	ret = gn25l98_select_table(priv, GN25L95_TABLE_2);
	if (ret)
		goto out_disable;

	ret = gn25l98_write_bytes(priv, 0x80,
				  data + GN25L98_HWCONFIG_TABLE2, 0x80);
	if (ret)
		goto out_disable;

	/* GN25L98 uses table 2 register 0x98 for the normal startup key. */
	ret = regmap_write(priv->regmap, GN25L98_TABLE2_SAFE_MODE_STARTUP,
			   GN25L95_SAFE_MODE_NORMAL);
	if (ret)
		goto out_disable;

	msleep(GN25L98_TX_START_DELAY_MS);

	ret = gn25l98_select_table(priv, GN25L95_TABLE_0);
	if (ret)
		goto out_disable;

	ret = regmap_write(priv->regmap, GN25L95_STATUS_CTRL, 0);
	if (!ret)
		return 0;

out_disable:
	/* Leave the transmitter disabled when calibration is incomplete. */
	gn25l98_select_table(priv, GN25L95_TABLE_0);
	regmap_write(priv->regmap, GN25L95_STATUS_CTRL,
		     GN25L95_STATUS_SOFT_TX_DISABLE);
	return ret;
}

static int gn25l95_verify_hwconfig(struct gn25l95_priv *priv, const u8 *data)
{
	struct device *dev = priv->dev;
	u8 lut_first, lut_last, safe_mode;
	u8 apd[3];
	int ret;

	ret = gn25l95_select_table(priv, GN25L95_TABLE_2);
	if (ret) {
		dev_err(dev, "error on select table 2");
		return ret;
	}

	ret = gn25l95_vendor_read_range(priv, GN25L95_TABLE2_RX_LOS_LEVEL,
					apd, sizeof(apd));
	if (ret) {
		dev_err(dev, "error read apd");
		return ret;
	}

	ret = gn25l95_vendor_read_range(priv,
					GN25L95_TABLE2_SAFE_MODE_STARTUP,
					  &safe_mode, 1);
	if (ret) {
		dev_err(dev, "error on read startup mode");
		return ret;
	}


	/*
	 * APD_DAC is a live register when APD_DAC_LUT_EN is set.  Once the
	 * state machine is running it is replaced with the temperature-indexed
	 * value from table 6, so only compare it with the image in direct mode.
	 */
	if (apd[0] != data[GN25L95_HWCONFIG_TABLE2 + 0x3c] ||
	    apd[1] != data[GN25L95_HWCONFIG_TABLE2 + 0x3d] ||
	    (!(apd[1] & GN25L95_APD_DAC_LUT_EN) &&
	     apd[2] != data[GN25L95_HWCONFIG_TABLE2 + 0x3e]) ||
	    safe_mode != GN25L95_SAFE_MODE_NORMAL) {
		dev_err(dev,
			"invalid config table 2: mode 0x%02x, [%d %d %d] != [%d %d %d]",
			safe_mode, apd[0], apd[1], apd[2],
			data[GN25L95_HWCONFIG_TABLE2 + 0x3c],
			data[GN25L95_HWCONFIG_TABLE2 + 0x3d],
			data[GN25L95_HWCONFIG_TABLE2 + 0x3e]);
		return -EIO;
	}

	ret = gn25l95_select_table(priv, GN25L95_TABLE_6);
	if (ret) {
		dev_err(dev, "error on select table 6");
		return ret;
	}

	ret = gn25l95_vendor_read_range(priv, 0xc0, &lut_first, 1);
	if (ret) {
		dev_err(dev, "error on read first lut");
		return ret;
	}

	ret = gn25l95_vendor_read_range(priv, 0xff, &lut_last, 1);
	if (ret) {
		dev_err(dev, "error on read last lut");
		return ret;
	}


	if (lut_first != data[GN25L95_HWCONFIG_TABLE6 + 0x40] ||
	    lut_last != data[GN25L95_HWCONFIG_TABLE6 + 0x7f]) {
		dev_err(dev, "invalid table 6: luts [%d %d] != [%d %d]",
			lut_first, lut_last, data[GN25L95_HWCONFIG_TABLE6 + 0x40],
			data[GN25L95_HWCONFIG_TABLE6 + 0x7f]);
		return -EIO;
	}

	dev_info(priv->dev,
		 "calibration verified: RX_LOS=0x%02x APD_CTRL=0x%02x APD=0x%02x APD_LUT=0x%02x..0x%02x\n",
		 apd[0], apd[1], apd[2], lut_first, lut_last);
	return 0;
}

static int gn25l98_verify_hwconfig(struct gn25l95_priv *priv, const u8 *data)
{
	u8 lut_first, lut_last, safe_mode;
	int ret;

	ret = gn25l95_select_table(priv, GN25L95_TABLE_2);
	if (ret)
		return ret;

	ret = gn25l95_vendor_read_range(priv,
					GN25L98_TABLE2_SAFE_MODE_STARTUP,
					 &safe_mode, 1);
	if (ret)
		return ret;

	if (safe_mode != GN25L95_SAFE_MODE_NORMAL) {
		dev_err(priv->dev, "invalid GN25L98 startup mode 0x%02x\n",
			safe_mode);
		return -EIO;
	}

	ret = gn25l98_select_table(priv, GN25L95_TABLE_6);
	if (ret)
		return ret;

	ret = gn25l95_vendor_read_range(priv, 0xc0, &lut_first, 1);
	if (ret)
		return ret;

	ret = gn25l95_vendor_read_range(priv, 0xff, &lut_last, 1);
	if (ret)
		return ret;

	if (lut_first != data[GN25L98_HWCONFIG_TABLE6] ||
	    lut_last != data[GN25L98_HWCONFIG_TABLE6 + 0x3f]) {
		dev_err(priv->dev,
			"invalid GN25L98 APD LUT: [0x%02x 0x%02x] != [0x%02x 0x%02x]\n",
			lut_first, lut_last, data[GN25L98_HWCONFIG_TABLE6],
			data[GN25L98_HWCONFIG_TABLE6 + 0x3f]);
		return -EIO;
	}

	dev_info(priv->dev,
		 "GN25L98 calibration verified: startup=0x%02x APD_LUT=0x%02x..0x%02x\n",
		 safe_mode, lut_first, lut_last);
	return 0;
}

static int gn25l95_load_hwconfig(struct gn25l95_priv *priv)
{
	const struct firmware *fw;
	size_t expected_size;
	int ret;

	expected_size = priv->data->variant == GN25L9X_VARIANT_98 ?
		GN25L98_HWCONFIG_SIZE : GN25L95_HWCONFIG_SIZE;

	ret = request_firmware(&fw, priv->fw_name, priv->dev);
	if (ret)
		return dev_err_probe(priv->dev, ret,
				     "calibration '%s' not found\n", priv->fw_name);

	if (fw->size != expected_size) {
		dev_err(priv->dev,
			"calibration '%s' has invalid size %zu (expected %zu)\n",
			priv->fw_name, fw->size, expected_size);
		ret = -EINVAL;
		goto out_release;
	}

	if (priv->data->variant == GN25L9X_VARIANT_98) {
		ret = gn25l98_program_hwconfig(priv, fw->data, fw->size);
		if (!ret)
			ret = gn25l98_verify_hwconfig(priv, fw->data);
	} else {
		ret = gn25l95_program_hwconfig(priv, fw->data, fw->size);
		if (!ret)
			ret = gn25l95_verify_hwconfig(priv, fw->data);
	}
	if (!ret)
		priv->calibrated = true;

out_release:
	release_firmware(fw);
	return ret;
}

static int gn25l95_read_a0(struct gn25l95_priv *priv, u8 reg, u8 *buf,
			   size_t len)
{
	struct i2c_adapter *adap = priv->client->adapter;
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

static void gn25l95_decode_alarms(struct gn25l95_priv *priv, u8 alarm1,
				  u8 alarm2)
{
	u32 alarm = 0;

	if (alarm1 & GN25L95_ALARM_TEMP_HIGH)
		alarm |= OPTICAL_FRONTEND_ALARM_HIGH_TEMP;
	if (alarm1 & GN25L95_ALARM_TEMP_LOW)
		alarm |= OPTICAL_FRONTEND_ALARM_LOW_TEMP;
	if (alarm1 & GN25L95_ALARM_VCC_HIGH)
		alarm |= OPTICAL_FRONTEND_ALARM_HIGH_VOLTAGE;
	if (alarm1 & GN25L95_ALARM_VCC_LOW)
		alarm |= OPTICAL_FRONTEND_ALARM_LOW_VOLTAGE;
	if (alarm1 & GN25L95_ALARM_TX_BIAS_HIGH)
		alarm |= OPTICAL_FRONTEND_ALARM_TX_HIGH_BIAS;
	if (alarm1 & GN25L95_ALARM_TX_BIAS_LOW)
		alarm |= OPTICAL_FRONTEND_ALARM_TX_LOW_BIAS;
	if (alarm1 & GN25L95_ALARM_TX_POWER_HIGH)
		alarm |= OPTICAL_FRONTEND_ALARM_TX_HIGH_POWER;
	if (alarm1 & GN25L95_ALARM_TX_POWER_LOW)
		alarm |= OPTICAL_FRONTEND_ALARM_TX_LOW_POWER;
	if (alarm2 & GN25L95_ALARM_RX_POWER_HIGH)
		alarm |= OPTICAL_FRONTEND_ALARM_RX_HIGH_POWER;
	if (alarm2 & GN25L95_ALARM_RX_POWER_LOW)
		alarm |= OPTICAL_FRONTEND_ALARM_RX_LOW_POWER;

	priv->alarm = alarm;
}

/* Caller holds priv->lock. */
static int gn25l95_refresh_all(struct gn25l95_priv *priv)
{
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

	priv->ddmi_temperature = get_unaligned_be16(&ddmi[0]);
	priv->ddmi_voltage = get_unaligned_be16(&ddmi[2]);
	priv->ddmi_current = get_unaligned_be16(&ddmi[4]);
	priv->ddmi_tx_power = get_unaligned_be16(&ddmi[6]);
	priv->ddmi_rx_power = get_unaligned_be16(&ddmi[8]);

	ret = regmap_bulk_read(priv->regmap, GN25L95_ALARM_FLAGS_1,
			       alarms, sizeof(alarms));
	if (!ret)
		gn25l95_decode_alarms(priv, alarms[0], alarms[1]);

	return 0;
}

static int
gn25l95_frontend_get_telemetry(
	struct optical_frontend *frontend,
	struct optical_frontend_telemetry *telemetry)
{
	struct gn25l95_priv *priv = optical_frontend_get_drvdata(frontend);
	int ret;

	mutex_lock(&priv->lock);
	ret = gn25l95_refresh_all(priv);
	if (!ret) {
		telemetry->temperature_mc =
			(s16)priv->ddmi_temperature * 1000 / 256;
		telemetry->voltage_uv = priv->ddmi_voltage * 100U;
		telemetry->bias_ua = priv->ddmi_current * 2U;
		telemetry->tx_power_nw = priv->ddmi_tx_power * 100U;
		telemetry->rx_power_nw = priv->ddmi_rx_power * 100U;
		telemetry->alarms = priv->alarm;
		telemetry->valid = OPTICAL_FRONTEND_TELEMETRY_F_TEMPERATURE |
				   OPTICAL_FRONTEND_TELEMETRY_F_VOLTAGE |
				   OPTICAL_FRONTEND_TELEMETRY_F_BIAS |
				   OPTICAL_FRONTEND_TELEMETRY_F_TX_POWER |
				   OPTICAL_FRONTEND_TELEMETRY_F_RX_POWER |
				   OPTICAL_FRONTEND_TELEMETRY_F_ALARMS;
	}
	mutex_unlock(&priv->lock);

	return ret;
}

static int
gn25l95_frontend_get_state(struct optical_frontend *frontend,
			     struct optical_frontend_state *state)
{
	struct gn25l95_priv *priv = optical_frontend_get_drvdata(frontend);
	unsigned int status;
	int ret;

	mutex_lock(&priv->lock);
	ret = regmap_read(priv->regmap, GN25L95_STATUS_CTRL, &status);
	mutex_unlock(&priv->lock);
	if (ret)
		return ret;

	state->present = true;
	state->ready = !(status & GN25L95_STATUS_DATA_READY_BAR);
	state->rx_los = !!(status & GN25L95_STATUS_RX_LOS);
	state->tx_fault = !!(status & GN25L95_STATUS_TX_FAULT);
	state->tx_enabled = !(status & (GN25L95_STATUS_TX_DISABLE |
				       GN25L95_STATUS_SOFT_TX_DISABLE));
	state->valid = OPTICAL_FRONTEND_STATE_F_PRESENT |
		       OPTICAL_FRONTEND_STATE_F_READY |
		       OPTICAL_FRONTEND_STATE_F_RX_LOS |
		       OPTICAL_FRONTEND_STATE_F_TX_FAULT |
		       OPTICAL_FRONTEND_STATE_F_TX_ENABLED;

	return 0;
}

/* Caller holds priv->lock. */
static int gn25l95_tx_rearm_locked(struct gn25l95_priv *priv)
{
	unsigned int status;
	int ret;

	ret = regmap_read(priv->regmap, GN25L95_STATUS_CTRL, &status);
	if (ret)
		return ret;

	if (status & GN25L95_STATUS_DATA_READY_BAR) {
		dev_warn_ratelimited(priv->dev,
				     "optical frontend is not configured/DDMI-ready\n");
		return -EAGAIN;
	}

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

static int gn25l95_frontend_tx_rearm(struct optical_frontend *frontend)
{
	struct gn25l95_priv *priv = optical_frontend_get_drvdata(frontend);
	int ret;

	mutex_lock(&priv->lock);
	ret = gn25l95_tx_rearm_locked(priv);
	mutex_unlock(&priv->lock);

	return ret;
}

static const struct optical_frontend_ops gn25l95_frontend_ops = {
	.get_telemetry = gn25l95_frontend_get_telemetry,
	.get_state = gn25l95_frontend_get_state,
	.tx_rearm = gn25l95_frontend_tx_rearm,
};

static int gn25l95_diag_show(struct seq_file *s, void *unused)
{
	struct gn25l95_priv *priv = s->private;
	unsigned int status = 0;
	u8 a0_id = 0;

	mutex_lock(&priv->lock);
	regmap_read(priv->regmap, GN25L95_STATUS_CTRL, &status);
	if (priv->native_a0)
		gn25l95_read_a0(priv, 0, &a0_id, 1);

	seq_printf(s, "native A0:   %s\n", priv->native_a0 ? "yes" : "no");
	seq_printf(s, "ext EEPROM:  %s\n", priv->eeprom_present ? "yes" : "no");
	seq_printf(s, "calibrated:  %s\n", priv->calibrated ? "yes" : "no");
	if (!priv->eeprom_present ||
	    priv->data->variant == GN25L9X_VARIANT_98)
		seq_printf(s, "hwconfig:    %s\n", priv->fw_name);
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
	mutex_unlock(&priv->lock);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(gn25l95_diag);

static ssize_t gn25l95_tx_rearm_write(struct file *file,
				      const char __user *buf, size_t count,
				      loff_t *ppos)
{
	struct gn25l95_priv *priv = file->private_data;
	bool rearm;
	int ret;

	ret = kstrtobool_from_user(buf, count, &rearm);
	if (ret)
		return ret;
	if (!rearm)
		return -EINVAL;

	ret = optical_frontend_tx_rearm(priv->frontend);
	return ret ? ret : count;
}

static const struct file_operations gn25l95_tx_rearm_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = gn25l95_tx_rearm_write,
	.llseek = noop_llseek,
};

static void gn25l95_debugfs_init(struct gn25l95_priv *priv)
{
	priv->debugfs = debugfs_create_dir(dev_name(priv->dev), NULL);
	debugfs_create_file("diag", 0444, priv->debugfs, priv,
			    &gn25l95_diag_fops);
	debugfs_create_file("tx_rearm", 0200, priv->debugfs, priv,
			    &gn25l95_tx_rearm_fops);
}

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

	mutex_lock(&priv->lock);
	if (!gn25l95_refresh_all(priv)) {
		mutex_unlock(&priv->lock);
		optical_frontend_invalidate_telemetry(priv->frontend);
	} else {
		mutex_unlock(&priv->lock);
	}

	schedule_delayed_work(&priv->tick_work, HZ);
}

static int gn25l95_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct gn25l95_priv *priv;
	const char *part_number;
	u8 a0_id, status0;
	unsigned int status;
	bool load_hwconfig;
	int ret;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C))
		return -EOPNOTSUPP;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->client = client;
	priv->dev = dev;
	priv->data = i2c_get_match_data(client);
	if (!priv->data)
		priv->data = &gn25l95_data;
	part_number = priv->data->frontend.part_number;
	mutex_init(&priv->lock);
	INIT_DELAYED_WORK(&priv->tick_work, gn25l95_tick_work);
	i2c_set_clientdata(client, priv);

	if (client->addr != priv->data->default_i2c_addr)
		dev_info(dev, "using non-default A2 address 0x%02x\n",
			 client->addr);

	priv->regmap = devm_regmap_init_i2c(client, &gn25l95_regmap_config);
	if (IS_ERR(priv->regmap))
		return dev_err_probe(dev, PTR_ERR(priv->regmap),
				     "failed to create register map\n");

	ret = gn25l95_wait_accessible(priv, &status);
	if (ret)
		return dev_err_probe(dev, ret,
				     "%s A2 interface did not respond\n",
				     part_number);

	ret = device_property_read_string(dev, "firmware-name", &priv->fw_name);
	if (ret)
		priv->fw_name = priv->data->default_fw_name;

	if (priv->data->variant == GN25L9X_VARIANT_95) {
		ret = gn25l95_soft_reset(priv);
		if (ret)
			return dev_err_probe(dev, ret,
					     "%s soft reset failed\n",
					     part_number);

		ret = gn25l95_wait_accessible(priv, &status);
		if (ret)
			return dev_err_probe(dev, ret,
					     "%s did not return after soft reset\n",
					     part_number);
	}

	ret = gn25l95_read_eeprom_status(priv, &status0);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to read EEPROM status\n");

	priv->eeprom_present = !!(status0 & GN25L95_STATUS0_EEPROM_PRESENT);
	dev_info(dev,
		 "%s %s external EEPROM (STATUS_0=0x%02x, DDMI_READY=%u)\n",
		 part_number,
		 priv->eeprom_present ? "has" : "has no", status0,
		 !!(status0 & GN25L95_STATUS0_DDMI_READY));

	/* The reference GN25L98 driver always applies its BOSA table image. */
	load_hwconfig = !priv->eeprom_present ||
		priv->data->variant == GN25L9X_VARIANT_98;
	if (load_hwconfig) {
		dev_info(dev, "loading board BOSA calibration from %s\n",
			 priv->fw_name);
		ret = gn25l95_load_hwconfig(priv);
		if (ret)
			return dev_err_probe(dev, ret, "error on loading hwconfig error code %d",
					     ret);
		dev_info(dev, "board BOSA calibration programmed\n");
	}

	priv->native_a0 = !gn25l95_read_a0(priv, 0, &a0_id, 1);
	if (priv->native_a0)
		dev_info(dev, "native SFF A0/A2 interface detected (A0 id 0x%02x)\n",
			 a0_id);
	else
		dev_info(dev, "A2-only interface detected; host-calibrated mode\n");

	ret = regmap_read(priv->regmap, GN25L95_STATUS_CTRL, &status);
	if (ret)
		return dev_err_probe(dev, ret, "failed to read frontend status\n");

	if (status & GN25L95_STATUS_DATA_READY_BAR) {
		dev_warn(dev,
			 "DDMI data is not ready; keeping transmitter disabled until configured\n");
	} else {
		mutex_lock(&priv->lock);
		ret = gn25l95_refresh_all(priv);
		mutex_unlock(&priv->lock);
		if (ret)
			return dev_err_probe(dev, ret, "failed to read DDMI data\n");
	}

	priv->frontend = devm_optical_frontend_register(
		dev, &priv->data->frontend, &gn25l95_frontend_ops, priv);
	if (IS_ERR(priv->frontend))
		return dev_err_probe(dev, PTR_ERR(priv->frontend),
				     "failed to register optical frontend\n");

	ret = devm_optical_frontend_hwmon_register(priv->frontend);
	if (ret)
		return dev_err_probe(dev, ret, "failed to register hwmon\n");


	gn25l95_debugfs_init(priv);
	schedule_delayed_work(&priv->tick_work, HZ);

	dev_info(dev,
		 "%s optical frontend ready at A2 address 0x%02x\n",
		 part_number, client->addr);
	return 0;
}

static void gn25l95_remove(struct i2c_client *client)
{
	struct gn25l95_priv *priv = i2c_get_clientdata(client);

	cancel_delayed_work_sync(&priv->tick_work);
	debugfs_remove_recursive(priv->debugfs);
}

static const struct of_device_id gn25l95_of_match[] = {
	{ .compatible = "semtech,gn25l95", .data = &gn25l95_data },
	{ .compatible = "semtech,gn25l98", .data = &gn25l98_data },
	{ }
};
MODULE_DEVICE_TABLE(of, gn25l95_of_match);

static const struct i2c_device_id gn25l95_i2c_ids[] = {
	{ "gn25l95", (kernel_ulong_t)&gn25l95_data },
	{ "gn25l98", (kernel_ulong_t)&gn25l98_data },
	{ }
};
MODULE_DEVICE_TABLE(i2c, gn25l95_i2c_ids);

static struct i2c_driver gn25l95_i2c_driver = {
	.driver = {
		.name = "gn25l95",
		.of_match_table = gn25l95_of_match,
	},
	.probe = gn25l95_probe,
	.remove = gn25l95_remove,
	.id_table = gn25l95_i2c_ids,
};
module_i2c_driver(gn25l95_i2c_driver);

MODULE_FIRMWARE(GN25L95_DEFAULT_FW_NAME);
MODULE_FIRMWARE(GN25L98_DEFAULT_FW_NAME);
MODULE_DESCRIPTION("Semtech GN25L95/GN25L98 xPON optical frontend driver");
MODULE_LICENSE("GPL");
