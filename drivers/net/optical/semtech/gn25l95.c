// SPDX-License-Identifier: GPL-2.0-only
/*
 * Semtech GN25L9x/GN28L9x xPON laser driver / limiting amplifier.
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

#define GN25L98_TABLE2_TX_CTRL	0xa2
#define GN25L98_TX_OPEN		BIT(5)

#define GN28L96_TABLE_CONTROL		0x80
#define GN28L96_TABLE_DIAGNOSTICS		0x81
#define GN28L96_TABLE_ID			0xff
#define GN28L96_TX_DIS_CTRL		0x80
#define GN28L96_SOFT_TX_DIS_INHIBIT	BIT(2)
#define GN28L96_TX_DISABLE_REPORT_MODE	BIT(3)
#define GN28L96_TX_DISABLE_POL		BIT(4)
#define GN28L96_EYE_SAFETY_CTRL		0x9a
#define GN28L96_TX_FAULT_POL		BIT(0)
#define GN28L96_TX_FAULT_STATE_SOURCE	BIT(4)
#define GN28L96_RX_LOS_CTRL		0xed
#define GN28L96_LOS_POLARITY		BIT(0)
#define GN28L96_SAFE_MODE_STARTUP		0x88
#define GN28L96_DIG_STATUS		0x89
#define GN28L96_DIG_STATUS_MCU_MODE	BIT(0)
#define GN28L96_DIG_STATUS_DDMI_READY	BIT(1)
#define GN28L96_EEPROM_CTRL		0x8d
#define GN28L96_EE_UPDATE_DIS		BIT(0)
#define GN28L96_SAFE_MODE_CUSTOM_ADDR	0xc5
#define GN28L96_READY_TIMEOUT_MS		1000

/* A0, A2, then 128-byte tables 80..83, optionally followed by 84..86. */
#define GN28L96_HWCONFIG_SIZE		0x400
#define GN28L96_HWCONFIG_EXT_SIZE		0x580
#define GN28L96_HWCONFIG_A2		0x100
#define GN28L96_HWCONFIG_TABLE80		0x200
#define GN28L96_HWCONFIG_TABLE81		0x280
#define GN28L96_DEFAULT_FW_NAME		"semtech/gn28l96-hwconfig.bin"

enum gn25l9x_variant {
	GN25L9X_VARIANT_95,
	GN25L9X_VARIANT_98,
	GN28L9X_VARIANT_96,
	GN28L9X_VARIANT_98,
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
	bool tx_disable_inverted;
	bool tx_fault_inverted;
	bool rx_los_inverted;
	char revision[3];
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
		.protocols = OPTICAL_FRONTEND_PROTO_EPON |
			     OPTICAL_FRONTEND_PROTO_GPON,
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
		.protocols = OPTICAL_FRONTEND_PROTO_EPON |
			     OPTICAL_FRONTEND_PROTO_GPON,
		.thresholds = &gn25l95_thresholds,
		.telemetry_cache_ms = 100,
	},
	.default_i2c_addr = GN25L95_I2C_A2_ADDR,
	.default_fw_name = GN25L98_DEFAULT_FW_NAME,
	.variant = GN25L9X_VARIANT_98,
};

static const struct gn25l95_chip_data gn28l96_data = {
	.frontend = {
		.name = "gn28l96",
		.type = "lddla",
		.vendor_name = "Semtech",
		.vendor_oui = { 0x00, 0x00, 0x00 },
		.part_number = "GN28L96",
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
		.protocols = OPTICAL_FRONTEND_PROTO_XEPON |
			     OPTICAL_FRONTEND_PROTO_XGPON |
			     OPTICAL_FRONTEND_PROTO_XGSPON,
		.telemetry_cache_ms = 100,
	},
	.default_i2c_addr = GN25L95_I2C_A2_ADDR,
	.default_fw_name = GN28L96_DEFAULT_FW_NAME,
	.variant = GN28L9X_VARIANT_96,
};

static const struct gn25l95_chip_data gn28l98_data = {
	.frontend = {
		.name = "gn28l98",
		.type = "lddla",
		.vendor_name = "Semtech",
		.vendor_oui = { 0x00, 0x00, 0x00 },
		.part_number = "GN28L98",
		.serial = "UNKNOWN",
		.date_code = "000000",
		.capabilities = OPTICAL_FRONTEND_CAP_TEMPERATURE |
				OPTICAL_FRONTEND_CAP_VOLTAGE |
				OPTICAL_FRONTEND_CAP_BIAS |
				OPTICAL_FRONTEND_CAP_TX_POWER |
				OPTICAL_FRONTEND_CAP_RX_POWER |
				OPTICAL_FRONTEND_CAP_ALARMS |
				OPTICAL_FRONTEND_CAP_TX_FAULT |
				OPTICAL_FRONTEND_CAP_RX_LOS |
				OPTICAL_FRONTEND_CAP_BURST_TX |
				OPTICAL_FRONTEND_CAP_APD,
		.protocols = OPTICAL_FRONTEND_PROTO_XGSPON,
		.telemetry_cache_ms = 100,
	},
	.default_i2c_addr = GN25L95_I2C_A2_ADDR,
	.default_fw_name = NULL,
	.variant = GN28L9X_VARIANT_98,
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

	if (priv->data->variant == GN28L9X_VARIANT_96) {
		msgs[0].addr = priv->client->addr ^ 1;
		msgs[1].addr = msgs[0].addr;
	}

	ret = i2c_transfer(adap, msgs, ARRAY_SIZE(msgs));
	if (ret < 0)
		return ret;

	return ret == ARRAY_SIZE(msgs) ? 0 : -EIO;
}

/* Caller holds priv->lock, or the frontend has not been registered yet. */
static int gn28l96_read_table(struct gn25l95_priv *priv, u8 table, u8 reg,
			      void *buf, size_t len)
{
	unsigned int old_table;
	int ret, restore;

	ret = regmap_read(priv->regmap, GN25L95_TABLE_SELECT, &old_table);
	if (ret)
		return ret;

	ret = regmap_write(priv->regmap, GN25L95_TABLE_SELECT, table);
	if (!ret)
		ret = regmap_bulk_read(priv->regmap, reg, buf, len);

	restore = regmap_write(priv->regmap, GN25L95_TABLE_SELECT, old_table);
	return ret ? ret : restore;
}

static int gn28l96_wait_ready(struct gn25l95_priv *priv)
{
	unsigned long deadline = jiffies +
		msecs_to_jiffies(GN28L96_READY_TIMEOUT_MS);
	unsigned int status;
	int ret;

	do {
		ret = regmap_read(priv->regmap, GN25L95_STATUS_CTRL, &status);
		if (ret)
			return ret;
		if (!(status & GN25L95_STATUS_DATA_READY_BAR))
			return 0;
		msleep(GN25L95_READY_POLL_MS);
	} while (time_before(jiffies, deadline));

	return -ETIMEDOUT;
}

/*
 * Use aligned two-byte writes for DACs and calibration words (MSB first).
 * Never cross the default eight-byte write-page boundary. There is no
 * EEPROM behind the device in this path; EEPROM update is also inhibited.
 */
static int gn28l96_write_range(struct gn25l95_priv *priv, u8 reg,
			       const u8 *data, size_t len)
{
	size_t chunk;
	int ret;

	while (len) {
		chunk = !(reg & 1) && len > 1 ? 2 : 1;
		ret = regmap_bulk_write(priv->regmap, reg, data, chunk);
		if (ret)
			return ret;
		reg += chunk;
		data += chunk;
		len -= chunk;
	}

	return 0;
}

struct gn28l96_config_range {
	u8 first;
	u8 last;
};

/* Writable configuration only: omit live readbacks and reserved bytes. */
static const struct gn28l96_config_range gn28l96_control_ranges[] = {
	{ 0x80, 0x8d }, { 0x8f, 0x91 }, { 0x94, 0x96 },
	{ 0x9a, 0xa0 }, { 0xa2, 0xa3 }, { 0xa5, 0xa5 },
	{ 0xa8, 0xb1 }, { 0xb6, 0xc1 }, { 0xca, 0xcd },
	{ 0xd0, 0xd8 }, { 0xdc, 0xe5 }, { 0xe8, 0xf0 },
	{ 0xf2, 0xf4 }, { 0xfe, 0xfe },
};

static const struct gn28l96_config_range gn28l96_diag_ranges[] = {
	{ 0xa0, 0xb7 }, { 0xb9, 0xb9 }, { 0xbc, 0xdf },
};

static int gn28l96_write_config(struct gn25l95_priv *priv, const u8 *data,
				const struct gn28l96_config_range *ranges,
				size_t count)
{
	size_t i;
	int ret;

	for (i = 0; i < count; i++) {
		ret = gn28l96_write_range(priv, ranges[i].first,
					  data + ranges[i].first - 0x80,
					  ranges[i].last - ranges[i].first + 1);
		if (ret)
			return ret;
	}

	return 0;
}

static int gn28l96_validate_hwconfig(const u8 *data, size_t size)
{
	u8 startup;

	if (size != GN28L96_HWCONFIG_SIZE &&
	    size != GN28L96_HWCONFIG_EXT_SIZE)
		return -EINVAL;

	startup = data[GN28L96_HWCONFIG_TABLE81 + 0x08];
	if (startup != GN25L95_SAFE_MODE_NORMAL &&
	    startup != GN28L96_SAFE_MODE_CUSTOM_ADDR)
		return -EINVAL;

	/* Software TX_DISABLE must remain effective throughout programming. */
	if (data[GN28L96_HWCONFIG_TABLE80] & GN28L96_SOFT_TX_DIS_INHIBIT)
		return -EINVAL;

	/* Tables 84..86 are required when any of their LUTs are enabled. */
	if (size == GN28L96_HWCONFIG_SIZE &&
	    (data[GN28L96_HWCONFIG_TABLE81 + 0x36] & GENMASK(4, 0)))
		return -EINVAL;

	return 0;
}

static int gn28l96_program_luts(struct gn25l95_priv *priv, const u8 *data,
				size_t size)
{
	unsigned int table, reg;
	size_t offset;
	int ret;

	for (table = 0x82; table <= (size == GN28L96_HWCONFIG_SIZE ?
				   0x83 : 0x86); table++) {
		offset = GN28L96_HWCONFIG_TABLE80 + (table - 0x80) * 0x80;
		ret = regmap_write(priv->regmap, GN25L95_TABLE_SELECT, table);
		if (ret)
			return ret;

		if (table < 0x84) {
			ret = gn28l96_write_range(priv, 0x80, data + offset, 0x80);
			if (ret)
				return ret;
			continue;
		}

		/* Tables 84..86 use only even bytes; table 86 ends at 0xbf. */
		for (reg = 0x80; reg < (table == 0x86 ? 0xc0 : 0x100);
		     reg += 2) {
			ret = regmap_write(priv->regmap, reg,
					   data[offset + reg - 0x80]);
			if (ret)
				return ret;
		}
	}

	return 0;
}

static int gn28l96_verify_config(struct gn25l95_priv *priv, const u8 *data,
				 const struct gn28l96_config_range *ranges,
				 size_t count)
{
	u8 values[0x80];
	size_t i, len;
	int ret;

	for (i = 0; i < count; i++) {
		len = ranges[i].last - ranges[i].first + 1;
		ret = regmap_bulk_read(priv->regmap, ranges[i].first, values, len);
		if (ret)
			return ret;
		if (memcmp(values, data + ranges[i].first - 0x80, len))
			return -EIO;
	}

	return 0;
}

static int gn28l96_verify_control(struct gn25l95_priv *priv, const u8 *data)
{
	static const u8 registers[] = { 0x80, 0x87, 0x9a, 0xed };
	static const u8 masks[] = { 0x1f, 0x73, 0x1f, 0x7f };
	unsigned int value;
	size_t i;
	int ret;

	for (i = 0; i < ARRAY_SIZE(registers); i++) {
		ret = regmap_read(priv->regmap, registers[i], &value);
		if (ret)
			return ret;
		if ((value & masks[i]) != (data[registers[i] - 0x80] & masks[i]))
			return -EIO;
	}

	return 0;
}

static int gn28l96_program_hwconfig(struct gn25l95_priv *priv, const u8 *data,
				    size_t size)
{
	unsigned int old_table, table, reg, mask;
	u8 values[0x80];
	size_t offset;
	int ret, restore;

	ret = gn28l96_validate_hwconfig(data, size);
	if (ret)
		return ret;

	ret = regmap_read(priv->regmap, GN25L95_TABLE_SELECT, &old_table);
	if (ret)
		return ret;

	ret = regmap_update_bits(priv->regmap, GN25L95_STATUS_CTRL,
				 GN25L95_STATUS_SOFT_TX_DISABLE,
				 GN25L95_STATUS_SOFT_TX_DISABLE);
	if (ret)
		return ret;
	msleep(GN25L95_SOFT_TX_DELAY_MS);

	ret = regmap_write(priv->regmap, GN25L95_TABLE_SELECT,
			   GN28L96_TABLE_DIAGNOSTICS);
	if (ret)
		goto out_disable;
	ret = regmap_write(priv->regmap, GN28L96_EEPROM_CTRL,
			   GN28L96_EE_UPDATE_DIS);
	if (ret)
		goto out_disable;
	ret = regmap_write(priv->regmap, GN28L96_SAFE_MODE_STARTUP, 0);
	if (ret)
		goto out_disable;

	ret = gn28l96_write_range(priv, 0, data + GN28L96_HWCONFIG_A2, 0x30);
	if (ret)
		goto out_disable;
	ret = gn28l96_write_range(priv, 0x38,
				  data + GN28L96_HWCONFIG_A2 + 0x38, 0x24);
	if (ret)
		goto out_disable;
	ret = regmap_write(priv->regmap, 0x6f, data[GN28L96_HWCONFIG_A2 + 0x6f]);
	if (ret)
		goto out_disable;
	ret = regmap_write(priv->regmap, 0x73, data[GN28L96_HWCONFIG_A2 + 0x73]);
	if (ret)
		goto out_disable;
	ret = gn28l96_write_range(priv, 0x78,
				  data + GN28L96_HWCONFIG_A2 + 0x78, 3);
	if (ret)
		goto out_disable;

	ret = regmap_write(priv->regmap, GN25L95_TABLE_SELECT, GN25L95_TABLE_0);
	if (ret)
		goto out_disable;
	ret = gn28l96_write_range(priv, 0xf8,
				  data + GN28L96_HWCONFIG_A2 + 0xf8, 2);
	if (ret)
		goto out_disable;
	ret = gn28l96_write_range(priv, 0xfc,
				  data + GN28L96_HWCONFIG_A2 + 0xfc, 2);
	if (ret)
		goto out_disable;

	ret = gn28l96_program_luts(priv, data, size);
	if (ret)
		goto out_disable;

	ret = regmap_write(priv->regmap, GN25L95_TABLE_SELECT,
			   GN28L96_TABLE_CONTROL);
	if (ret)
		goto out_disable;
	ret = gn28l96_write_config(priv, data + GN28L96_HWCONFIG_TABLE80,
				   gn28l96_control_ranges,
				   ARRAY_SIZE(gn28l96_control_ranges));
	if (ret)
		goto out_disable;
	ret = gn28l96_verify_control(priv, data + GN28L96_HWCONFIG_TABLE80);
	if (ret)
		goto out_disable;

	ret = regmap_write(priv->regmap, GN25L95_TABLE_SELECT,
			   GN28L96_TABLE_DIAGNOSTICS);
	if (ret)
		goto out_disable;
	ret = gn28l96_write_config(priv, data + GN28L96_HWCONFIG_TABLE81,
				   gn28l96_diag_ranges,
				   ARRAY_SIZE(gn28l96_diag_ranges));
	if (ret)
		goto out_disable;
	ret = gn28l96_verify_config(priv, data + GN28L96_HWCONFIG_TABLE81,
				    gn28l96_diag_ranges,
				    ARRAY_SIZE(gn28l96_diag_ranges));
	if (ret)
		goto out_disable;

	/* Verify LUTs before permitting normal startup; reserved bits read as zero. */
	for (table = 0x82; table <= (size == GN28L96_HWCONFIG_SIZE ?
				   0x83 : 0x86); table++) {
		offset = GN28L96_HWCONFIG_TABLE80 + (table - 0x80) * 0x80;
		ret = regmap_write(priv->regmap, GN25L95_TABLE_SELECT, table);
		if (ret)
			goto out_disable;
		ret = regmap_bulk_read(priv->regmap, 0x80, values, sizeof(values));
		if (ret)
			goto out_disable;
		if (table < 0x84) {
			if (memcmp(values, data + offset, sizeof(values))) {
				ret = -EIO;
				goto out_disable;
			}
			continue;
		}
		for (reg = 0; reg < (table == 0x86 ? 0x40 : 0x80); reg += 2) {
			mask = table == 0x84 ? 0x3f : table == 0x86 ? 0x07 :
				reg < 0x40 ? 0x1f : 0x7f;
			if ((values[reg] & mask) != (data[offset + reg] & mask)) {
				ret = -EIO;
				goto out_disable;
			}
		}
	}

	ret = regmap_write(priv->regmap, GN25L95_TABLE_SELECT,
			   GN28L96_TABLE_DIAGNOSTICS);
	if (ret)
		goto out_disable;
	/* Keep the current I2C address and host access permissions unchanged. */
	ret = regmap_write(priv->regmap, GN28L96_SAFE_MODE_STARTUP,
			   data[GN28L96_HWCONFIG_TABLE81 + 0x08]);
	if (ret)
		goto out_disable;
	ret = regmap_read(priv->regmap, GN28L96_SAFE_MODE_STARTUP, &reg);
	if (ret)
		goto out_disable;
	if (reg != data[GN28L96_HWCONFIG_TABLE81 + 0x08]) {
		ret = -EIO;
		goto out_disable;
	}
	ret = gn28l96_wait_ready(priv);

out_disable:
	/* Only the protocol consumer may enable TX after successful calibration. */
	restore = regmap_update_bits(priv->regmap, GN25L95_STATUS_CTRL,
				     GN25L95_STATUS_SOFT_TX_DISABLE,
				     GN25L95_STATUS_SOFT_TX_DISABLE);
	if (!ret)
		ret = restore;
	restore = regmap_write(priv->regmap, GN25L95_TABLE_SELECT, old_table);
	return ret ? ret : restore;
}

static int gn28l96_prepare(struct gn25l95_priv *priv)
{
	const struct firmware *fw;
	u8 identity[10], digital, startup, tx_control, fault_control, los_control;
	u8 password_bytes[4];
	u32 password;
	int ret;

	if (device_property_present(priv->dev, "semtech,password")) {
		ret = device_property_read_u32(priv->dev, "semtech,password", &password);
		if (ret)
			return ret;
		put_unaligned_be32(password, password_bytes);
		ret = regmap_bulk_write(priv->regmap, 0x7b, password_bytes,
					sizeof(password_bytes));
		if (ret)
			return ret;
	}

	ret = gn28l96_read_table(priv, GN28L96_TABLE_ID, 0x80, identity,
				 sizeof(identity));
	if (ret)
		return ret;
	if (memcmp(identity, "GN28L96", 7))
		return dev_err_probe(priv->dev, -ENODEV,
				     "GN28L96 chip ID unavailable or mismatched; check password\n");
	memcpy(priv->revision, identity + 8, 2);

	ret = gn28l96_read_table(priv, GN28L96_TABLE_DIAGNOSTICS,
				 GN28L96_DIG_STATUS, &digital, 1);
	if (ret)
		return ret;
	priv->eeprom_present = !(digital & GN28L96_DIG_STATUS_MCU_MODE);

	if (priv->eeprom_present) {
		/* Preserve the factory EEPROM and its already-loaded calibration. */
		if (device_property_present(priv->dev, "firmware-name"))
			return dev_err_probe(priv->dev, -EOPNOTSUPP,
					     "host calibration requires GN28L96 MCU mode\n");
		ret = gn28l96_read_table(priv, GN28L96_TABLE_DIAGNOSTICS,
					 GN28L96_SAFE_MODE_STARTUP, &startup, 1);
		if (ret)
			return ret;
		if (startup != GN25L95_SAFE_MODE_NORMAL &&
		    startup != GN28L96_SAFE_MODE_CUSTOM_ADDR)
			return dev_err_probe(priv->dev, -EINVAL,
					     "GN28L96 EEPROM calibration is not valid\n");
		ret = gn28l96_wait_ready(priv);
	} else {
		if (priv->client->addr != GN25L95_I2C_A2_ADDR)
			return dev_err_probe(priv->dev, -EOPNOTSUPP,
					     "GN28L96 MCU calibration requires default A2 address\n");
		ret = request_firmware(&fw, priv->fw_name, priv->dev);
		if (ret)
			return dev_err_probe(priv->dev, ret,
					     "calibration '%s' not found\n", priv->fw_name);
		ret = gn28l96_program_hwconfig(priv, fw->data, fw->size);
		release_firmware(fw);
	}
	if (ret)
		return ret;

	ret = gn28l96_read_table(priv, GN28L96_TABLE_CONTROL,
				 GN28L96_TX_DIS_CTRL, &tx_control, 1);
	if (ret)
		return ret;
	if (tx_control & GN28L96_SOFT_TX_DIS_INHIBIT)
		return dev_err_probe(priv->dev, -EOPNOTSUPP,
				     "GN28L96 software TX_DISABLE is inhibited\n");
	priv->tx_disable_inverted = (tx_control & GN28L96_TX_DISABLE_POL) &&
		!(tx_control & GN28L96_TX_DISABLE_REPORT_MODE);
	ret = gn28l96_read_table(priv, GN28L96_TABLE_CONTROL,
				 GN28L96_EYE_SAFETY_CTRL, &fault_control, 1);
	if (ret)
		return ret;
	priv->tx_fault_inverted = (fault_control & GN28L96_TX_FAULT_POL) &&
		!(fault_control & GN28L96_TX_FAULT_STATE_SOURCE);
	ret = gn28l96_read_table(priv, GN28L96_TABLE_CONTROL,
				 GN28L96_RX_LOS_CTRL, &los_control, 1);
	if (ret)
		return ret;
	priv->rx_los_inverted = !!(los_control & GN28L96_LOS_POLARITY);
	priv->calibrated = true;

	dev_info(priv->dev, "GN28L96 revision %.2s, %s calibration\n",
		 priv->revision, priv->eeprom_present ? "EEPROM" : "host");
	return 0;
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

static unsigned int gn25l95_normalize_status(struct gn25l95_priv *priv,
					     unsigned int status)
{
	if (priv->tx_disable_inverted)
		status ^= GN25L95_STATUS_TX_DISABLE;
	if (priv->tx_fault_inverted)
		status ^= GN25L95_STATUS_TX_FAULT;
	if (priv->rx_los_inverted)
		status ^= GN25L95_STATUS_RX_LOS;

	return status;
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

	status = gn25l95_normalize_status(priv, status);

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

	status = gn25l95_normalize_status(priv, status);
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

static int gn25l98_tx_enable_locked(struct gn25l95_priv *priv, bool enable)
{
	unsigned int old_table;
	int ret;

	ret = regmap_read(priv->regmap, GN25L95_TABLE_SELECT, &old_table);
	if (ret)
		return ret;

	ret = gn25l98_select_table(priv, GN25L95_TABLE_2);
	if (ret)
		return ret;

	ret = regmap_update_bits(priv->regmap,
				 GN25L98_TABLE2_TX_CTRL,
				 GN25L98_TX_OPEN,
				 enable ? GN25L98_TX_OPEN : 0);

	gn25l98_select_table(priv, old_table);

	return ret;
}

static int gn25l95_frontend_tx_enable(struct optical_frontend *frontend, bool enable)
{
	struct gn25l95_priv *priv =
		optical_frontend_get_drvdata(frontend);
	int ret = 0;

	mutex_lock(&priv->lock);

	if (priv->data->variant == GN25L9X_VARIANT_98)
		ret = gn25l98_tx_enable_locked(priv, enable);
	else if (priv->data->variant == GN28L9X_VARIANT_96) {
		if (enable)
			ret = gn28l96_wait_ready(priv);
		if (!ret) {
			ret = regmap_update_bits(priv->regmap, GN25L95_STATUS_CTRL,
						 GN25L95_STATUS_SOFT_TX_DISABLE,
						 enable ? 0 :
						 GN25L95_STATUS_SOFT_TX_DISABLE);
			if (!ret)
				msleep(GN25L95_SOFT_TX_DELAY_MS);
		}
	}

	mutex_unlock(&priv->lock);

	return ret;
}

static const struct optical_frontend_ops gn25l95_frontend_ops = {
	.get_telemetry = gn25l95_frontend_get_telemetry,
	.get_state = gn25l95_frontend_get_state,
	.tx_enable = gn25l95_frontend_tx_enable,
	.tx_rearm = gn25l95_frontend_tx_rearm,
};

/* GN28L98 vendor calibration/control registers still need documentation. */
static const struct optical_frontend_ops gn28l98_frontend_ops = {
	.get_telemetry = gn25l95_frontend_get_telemetry,
	.get_state = gn25l95_frontend_get_state,
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

	seq_printf(s, "chip:        %s\n", priv->data->frontend.part_number);
	if (priv->data->variant == GN28L9X_VARIANT_96)
		seq_printf(s, "revision:    %.2s\n", priv->revision);
	seq_printf(s, "native A0:   %s\n", priv->native_a0 ? "yes" : "no");
	seq_printf(s, "ext EEPROM:  %s\n",
		   priv->data->variant == GN28L9X_VARIANT_98 ? "unknown" :
		   priv->eeprom_present ? "yes" : "no");
	seq_printf(s, "calibrated:  %s\n", priv->calibrated ? "yes" : "no");
	if (priv->fw_name &&
	    (!priv->eeprom_present || priv->data->variant == GN25L9X_VARIANT_98))
		seq_printf(s, "hwconfig:    %s\n", priv->fw_name);
	if (priv->data->variant == GN28L9X_VARIANT_98)
		seq_puts(s, "support:     experimental, preconfigured SFF diagnostics only\n");
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
	if (priv->data->frontend.capabilities & OPTICAL_FRONTEND_CAP_TX_REARM)
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

	if (priv->data->variant == GN28L9X_VARIANT_98) {
		if (device_property_present(dev, "firmware-name"))
			return dev_err_probe(dev, -EOPNOTSUPP,
					     "GN28L98 host calibration is not documented\n");
		ret = gn28l96_wait_ready(priv);
		if (ret)
			return dev_err_probe(dev, ret,
					     "GN28L98 requires a preconfigured SFF interface\n");
		dev_warn(dev, "GN28L98 support is experimental: SFF diagnostics only\n");
		goto register_frontend;
	}

	if (priv->data->variant == GN28L9X_VARIANT_96) {
		ret = gn28l96_prepare(priv);
		if (ret)
			return dev_err_probe(dev, ret, "failed to prepare GN28L96\n");
		goto read_a0;
	}

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

read_a0:
	priv->native_a0 = !gn25l95_read_a0(priv, 0, &a0_id, 1);
	if (priv->native_a0)
		dev_info(dev, "native SFF A0/A2 interface detected (A0 id 0x%02x)\n",
			 a0_id);
	else
		dev_info(dev, "A2-only interface detected; host-calibrated mode\n");

register_frontend:
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
		dev, &priv->data->frontend,
		priv->data->variant == GN28L9X_VARIANT_98 ?
		&gn28l98_frontend_ops : &gn25l95_frontend_ops, priv);
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
	{ .compatible = "semtech,gn28l96", .data = &gn28l96_data },
	{ .compatible = "semtech,gn28l98", .data = &gn28l98_data },
	{ }
};
MODULE_DEVICE_TABLE(of, gn25l95_of_match);

static const struct i2c_device_id gn25l95_i2c_ids[] = {
	{ "gn25l95", (kernel_ulong_t)&gn25l95_data },
	{ "gn25l98", (kernel_ulong_t)&gn25l98_data },
	{ "gn28l96", (kernel_ulong_t)&gn28l96_data },
	{ "gn28l98", (kernel_ulong_t)&gn28l98_data },
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
MODULE_FIRMWARE(GN28L96_DEFAULT_FW_NAME);
MODULE_DESCRIPTION("Semtech GN25L9x/GN28L9x xPON optical frontend driver");
MODULE_LICENSE("GPL");
