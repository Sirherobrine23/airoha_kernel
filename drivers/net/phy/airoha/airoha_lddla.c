// SPDX-License-Identifier: GPL-2.0
/*
 * Shared core for the Airoha xPON LDDLA (laser-diode driver + limiting
 * amplifier) controllers (EN7570, EN7571).
 *
 * Provides the common I2C transport, the bounded ADC/I2C lock, the calibration
 * firmware store, the hwmon registration, the virtual SFP bus and the single
 * module entry point that registers whichever chip drivers are configured in.
 * The per-chip drivers supply a struct airoha_lddla_ops for the live readback.
 *
 * Bus attachment
 * ==============
 * The driver sits on two I2C buses with opposite roles:
 *
 *  - On the real I2C bus it is a *client*.  The LDDLA is a physical slave at
 *    7-bit address 0x70, described in the device tree as "airoha,en7570" or
 *    "airoha,en7571" (or matched by the i2c_device_id table); the per-chip
 *    i2c_driver binds to it and the transport helpers below issue master
 *    transactions to 0x70.
 *
 *  - To publish its diagnostics it registers a *virtual* I2C adapter
 *    (lddla_sfp_init()) on which it plays the *slave* side.  The adapter's
 *    master_xfer emulates the two EEPROM-style slaves that a pluggable SFP
 *    exposes - the MSA serial-ID ROM at 0x50 and the SFF-8472 diagnostics at
 *    0x51 - so the standard SFP / phylink stack (or userspace i2cdev) can read
 *    this soldered-down module as if it were a real SFP.
 *
 * Locking: every top-level entry point (probe/init, the 1 Hz worker, hwmon and
 * debugfs callbacks) holds lddla->lock for the whole operation, which keeps the
 * multi-step channel-select/latch/read ADC sequence atomic.
 */
#include <linux/debugfs.h>
#include <linux/export.h>
#include <linux/phy/phy-airoha-xpon.h>
#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/hwmon.h>
#include <linux/i2c.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/sched/signal.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/unaligned.h>

#include "airoha_lddla.h"

/* ------------------------------------------------------------------ */
/* I2C transport							      */
/* ------------------------------------------------------------------ */

/*
 * I2C register addressing
 * =======================
 * The LDDLA is a 7-bit I2C slave at address 0x70.  Its register file is reached
 * through a 16-bit register pointer that is sent as the first two data bytes of
 * every transaction, most-significant byte first (big-endian on the wire):
 *
 *	S 0x70+w  ptr[15:8] ptr[7:0]  Sr 0x70+r  d0 [d1 [d2 d3]]  P   (read)
 *	S 0x70+w  ptr[15:8] ptr[7:0]     d0 [d1 [d2 d3]]          P   (write)
 *
 * Registers are 32-bit and word aligned, but the pointer is a *byte* address,
 * so an individual byte lane of a 32-bit register is reached at "reg + lane".
 * Register data on the wire is little-endian, so a 4-byte access maps as:
 *
 *	buf[0] = bits [7:0]	(reg + 0, least-significant byte)
 *	buf[1] = bits [15:8]	(reg + 1)
 *	buf[2] = bits [23:16]	(reg + 2)
 *	buf[3] = bits [31:24]	(reg + 3, most-significant byte)
 *
 * Accesses are byte-granular bursts of 1 to 4 bytes.  Most register updates are
 * a read-modify-write of a single byte lane (lddla_update8()), which is why the
 * chip code frequently addresses "reg + 1/2/3" to reach a specific lane.
 */

/**
 * lddla_rd() - read a byte range from the register file.
 * @lddla: device
 * @addr:  16-bit register byte address (a register base, or base + byte lane)
 * @buf:   destination, filled little-endian (buf[0] = least-significant byte)
 * @len:   number of bytes to read (1 to 4)
 *
 * Writes the 2-byte big-endian register pointer, then reads @len data bytes
 * after a repeated start.  Return: 0 on success, negative errno on failure.
 */
int lddla_rd(struct airoha_lddla *lddla, u16 addr, u8 *buf, int len)
{
	u8 ptr[2] = { addr >> 8, addr & 0xff };
	struct i2c_msg msg[2] = {
		{ .addr = lddla->client->addr, .flags = 0, .len = 2, .buf = ptr },
		{ .addr = lddla->client->addr, .flags = I2C_M_RD, .len = len,
		  .buf = buf },
	};
	int ret;

	ret = i2c_transfer(lddla->client->adapter, msg, 2);
	if (ret < 0)
		return ret;
	return ret == 2 ? 0 : -EIO;
}

/**
 * lddla_wr() - write a byte range to the register file.
 * @lddla: device
 * @addr:  16-bit register byte address (a register base, or base + byte lane)
 * @buf:   source, little-endian (buf[0] = least-significant byte)
 * @len:   number of bytes to write (1 to 4)
 *
 * Sends the 2-byte big-endian register pointer followed by @len data bytes in
 * a single I2C message.  Return: 0 on success, negative errno on failure.
 */
int lddla_wr(struct airoha_lddla *lddla, u16 addr, const u8 *buf, int len)
{
	u8 tmp[2 + 4];
	int ret;

	if (len > 4)
		return -EINVAL;

	tmp[0] = addr >> 8;
	tmp[1] = addr & 0xff;
	memcpy(&tmp[2], buf, len);

	ret = i2c_master_send(lddla->client, tmp, len + 2);
	if (ret < 0)
		return ret;
	return ret == len + 2 ? 0 : -EIO;
}

/* Read a single byte lane. */
int lddla_rd8(struct airoha_lddla *lddla, u16 addr, u8 *val)
{
	return lddla_rd(lddla, addr, val, 1);
}

/* Write a single byte lane. */
int lddla_wr8(struct airoha_lddla *lddla, u16 addr, u8 val)
{
	return lddla_wr(lddla, addr, &val, 1);
}

/**
 * lddla_update8() - read-modify-write a single register byte lane.
 * @lddla: device
 * @addr:  16-bit byte address of the lane
 * @mask:  bits to preserve (1 = keep the existing bit)
 * @set:   bits to set after masking
 *
 * Return: 0 on success, negative errno on failure.
 */
int lddla_update8(struct airoha_lddla *lddla, u16 addr, u8 mask, u8 set)
{
	u8 val;
	int ret;

	ret = lddla_rd8(lddla, addr, &val);
	if (ret)
		return ret;
	val = (val & mask) | set;
	return lddla_wr8(lddla, addr, val);
}

/* Read two byte lanes as a little-endian 16-bit value. */
int lddla_rd16(struct airoha_lddla *lddla, u16 addr, u16 *val)
{
	u8 b[2];
	int ret;

	ret = lddla_rd(lddla, addr, b, 2);
	if (ret)
		return ret;
	*val = b[0] | (b[1] << 8);
	return 0;
}

/* Read four byte lanes as a little-endian 32-bit value. */
int lddla_rd32(struct airoha_lddla *lddla, u16 addr, u32 *val)
{
	u8 b[4];
	int ret;

	ret = lddla_rd(lddla, addr, b, 4);
	if (ret)
		return ret;
	*val = b[0] | (b[1] << 8) | (b[2] << 16) | (b[3] << 24);
	return 0;
}

/**
 * lddla_lock() - bounded acquire of the I2C/ADC lock.
 * @lddla: device
 *
 * The control loops and diagnostic paths must not block forever on contention,
 * so cap the wait at ~100 ms.  Return: 0 once held, -ETIMEDOUT on timeout, or
 * -ERESTARTSYS if a signal arrives in a user-context caller.
 */
int lddla_lock(struct airoha_lddla *lddla)
{
	unsigned long deadline = jiffies + msecs_to_jiffies(100);

	while (!mutex_trylock(&lddla->lock)) {
		if (signal_pending(current))
			return -ERESTARTSYS;
		if (time_after(jiffies, deadline))
			return -ETIMEDOUT;
		usleep_range(100, 200);
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* Calibration store						      */
/* ------------------------------------------------------------------ */

/**
 * lddla_flash_read() - read a calibration word from the in-memory mirror.
 * @lddla: device
 * @off:   byte offset into the blob (a multiple of 4)
 *
 * Return: the 32-bit word, or AIROHA_LDDLA_FLASH_ERASED if @off is out of range.
 */
u32 lddla_flash_read(struct airoha_lddla *lddla, u32 off)
{
	u32 idx = off >> 2;

	if (idx >= AIROHA_LDDLA_FLASH_WORDS)
		return AIROHA_LDDLA_FLASH_ERASED;
	return lddla->flash[idx];
}

/* Fill the first 40 words with the "erased" pattern. */
void lddla_flash_defaults(struct airoha_lddla *lddla)
{
	int i;

	for (i = 0; i < 40; i++)
		lddla->flash[i] = AIROHA_LDDLA_FLASH_ERASED;
}

/**
 * lddla_flash_load() - load the calibration NVM into the in-memory mirror.
 * @lddla: device
 *
 * A missing firmware blob is non-fatal: the erased defaults are installed.
 * Return: 0 (always; load failures fall back to defaults).
 */
int lddla_flash_load(struct airoha_lddla *lddla)
{
	const struct firmware *fw;
	size_t words, i;
	int ret;

	ret = request_firmware(&fw, lddla->fw_name, lddla->dev);
	if (ret) {
		dev_warn(lddla->dev,
			 "calibration NVM '%s' not found (%d); using defaults\n",
			 lddla->fw_name, ret);
		lddla_flash_defaults(lddla);
		return 0;
	}

	memset32(lddla->flash, AIROHA_LDDLA_FLASH_ERASED, AIROHA_LDDLA_FLASH_WORDS);
	words = min_t(size_t, fw->size / sizeof(u32), AIROHA_LDDLA_FLASH_WORDS);
	for (i = 0; i < words; i++)
		lddla->flash[i] = get_unaligned_le32(fw->data + i * sizeof(u32));

	release_firmware(fw);
	dev_dbg(lddla->dev, "loaded %zu calibration words from '%s'\n",
		words, lddla->fw_name);
	return 0;
}

/* ------------------------------------------------------------------ */
/* hwmon								      */
/* ------------------------------------------------------------------ */

static umode_t airoha_lddla_hwmon_is_visible(const void *data,
					    enum hwmon_sensor_types type,
					    u32 attr, int channel)
{
	return 0444;
}

/* Convert an SFF-8472 threshold word to the hwmon unit for an attribute. */
static long airoha_lddla_hwmon_thld(enum hwmon_sensor_types type, u32 attr)
{
	switch (type) {
	case hwmon_temp:	/* 1/256 degC -> milli-degC */
		return (attr == hwmon_temp_min ? (s16)AIROHA_TEMP_LOW_THLD
					       : (s16)AIROHA_TEMP_HIGH_THLD)
			* 1000 / 256;
	case hwmon_in:		/* 100 uV -> mV */
		return (attr == hwmon_in_min ? AIROHA_VOLT_LOW_THLD
					     : AIROHA_VOLT_HIGH_THLD) / 10;
	case hwmon_curr:	/* 2 uA -> mA */
		return (attr == hwmon_curr_min ? AIROHA_TX_BIAS_LOW_THLD
					       : AIROHA_TX_BIAS_HIGH_THLD)
			* 2 / 1000;
	default:
		return 0;
	}
}

static int airoha_lddla_hwmon_read(struct device *dev,
				  enum hwmon_sensor_types type,
				  u32 attr, int channel, long *val)
{
	struct airoha_lddla *lddla = dev_get_drvdata(dev);
	const struct airoha_lddla_ops *ops = lddla->ops;
	int ret;
	u16 word;

	ret = lddla_lock(lddla);
	if (ret)
		return ret;

	switch (type) {
	case hwmon_temp:
		switch (attr) {
		case hwmon_temp_input:
			*val = ops->temp_refresh(lddla);
			break;
		case hwmon_temp_min:
		case hwmon_temp_max:
			*val = airoha_lddla_hwmon_thld(type, attr);
			break;
		case hwmon_temp_min_alarm:
			*val = (s16)lddla->ddmi_temperature < (s16)AIROHA_TEMP_LOW_THLD;
			break;
		case hwmon_temp_max_alarm:
			*val = (s16)lddla->ddmi_temperature > (s16)AIROHA_TEMP_HIGH_THLD;
			break;
		default:
			ret = -EOPNOTSUPP;
		}
		break;
	case hwmon_in:
		switch (attr) {
		case hwmon_in_input:
			*val = ops->vcc_refresh(lddla) / 10;	/* 100 uV -> mV */
			break;
		case hwmon_in_min:
		case hwmon_in_max:
			*val = airoha_lddla_hwmon_thld(type, attr);
			break;
		case hwmon_in_min_alarm:
			*val = lddla->ddmi_voltage < AIROHA_VOLT_LOW_THLD;
			break;
		case hwmon_in_max_alarm:
			*val = lddla->ddmi_voltage > AIROHA_VOLT_HIGH_THLD;
			break;
		default:
			ret = -EOPNOTSUPP;
		}
		break;
	case hwmon_curr:
		switch (attr) {
		case hwmon_curr_input:
			*val = ops->bias_refresh(lddla) * 2 / 1000;	/* 2 uA -> mA */
			break;
		case hwmon_curr_min:
		case hwmon_curr_max:
			*val = airoha_lddla_hwmon_thld(type, attr);
			break;
		case hwmon_curr_min_alarm:
			*val = lddla->ddmi_current < AIROHA_TX_BIAS_LOW_THLD;
			break;
		case hwmon_curr_max_alarm:
			*val = lddla->ddmi_current > AIROHA_TX_BIAS_HIGH_THLD;
			break;
		default:
			ret = -EOPNOTSUPP;
		}
		break;
	case hwmon_power:
		/* channel 0 = Tx optical power, channel 1 = Rx optical power. */
		switch (attr) {
		case hwmon_power_input:
			word = channel ? ops->rx_power_refresh(lddla)
				       : ops->tx_power_refresh(lddla);
			*val = word / 10;		/* 0.1 uW -> uW */
			break;
		case hwmon_power_min:
			*val = (channel ? AIROHA_RX_PWR_LOW_THLD
					: AIROHA_TX_PWR_LOW_THLD) / 10;
			break;
		case hwmon_power_max:
			*val = (channel ? AIROHA_RX_PWR_HIGH_THLD
					: AIROHA_TX_PWR_HIGH_THLD) / 10;
			break;
		case hwmon_power_min_alarm:
			*val = !!(lddla->alarm & (channel ? AIROHA_ALARM_RX_LOW_POWER
							 : AIROHA_ALARM_TX_LOW_POWER));
			break;
		case hwmon_power_max_alarm:
			*val = !!(lddla->alarm & (channel ? AIROHA_ALARM_RX_HIGH_POWER
							 : AIROHA_ALARM_TX_HIGH_POWER));
			break;
		default:
			ret = -EOPNOTSUPP;
		}
		break;
	default:
		ret = -EOPNOTSUPP;
	}

	mutex_unlock(&lddla->lock);
	return ret;
}

static const struct hwmon_channel_info * const airoha_lddla_hwmon_info[] = {
	HWMON_CHANNEL_INFO(temp,
			   HWMON_T_INPUT | HWMON_T_MIN | HWMON_T_MAX |
			   HWMON_T_MIN_ALARM | HWMON_T_MAX_ALARM),
	HWMON_CHANNEL_INFO(in,
			   HWMON_I_INPUT | HWMON_I_MIN | HWMON_I_MAX |
			   HWMON_I_MIN_ALARM | HWMON_I_MAX_ALARM),
	HWMON_CHANNEL_INFO(curr,
			   HWMON_C_INPUT | HWMON_C_MIN | HWMON_C_MAX |
			   HWMON_C_MIN_ALARM | HWMON_C_MAX_ALARM),
	HWMON_CHANNEL_INFO(power,
			   HWMON_P_INPUT | HWMON_P_MIN | HWMON_P_MAX |
			   HWMON_P_MIN_ALARM | HWMON_P_MAX_ALARM,
			   HWMON_P_INPUT | HWMON_P_MIN | HWMON_P_MAX |
			   HWMON_P_MIN_ALARM | HWMON_P_MAX_ALARM),
	NULL,
};

static const struct hwmon_ops airoha_lddla_hwmon_ops = {
	.is_visible = airoha_lddla_hwmon_is_visible,
	.read = airoha_lddla_hwmon_read,
};

static const struct hwmon_chip_info airoha_lddla_hwmon_chip_info = {
	.ops = &airoha_lddla_hwmon_ops,
	.info = airoha_lddla_hwmon_info,
};

/**
 * lddla_hwmon_register() - register the device with hwmon.
 * @lddla: device
 *
 * Exposes temperature, supply voltage, Tx bias current and Tx/Rx optical power
 * (channels 0/1) using the cached DDMI words.  Return: 0 or a negative errno.
 */
int lddla_hwmon_register(struct airoha_lddla *lddla)
{
	lddla->hwmon = devm_hwmon_device_register_with_info(lddla->dev,
							   lddla->ops->name, lddla,
							   &airoha_lddla_hwmon_chip_info,
							   NULL);
	return PTR_ERR_OR_ZERO(lddla->hwmon);
}

/**
 * airoha_lddla_tx_rearm() - rearm an LDDLA optical transmitter
 * @dev: LDDLA I2C device
 *
 * The caller must invoke this after external TX_DISABLE has been released.
 *
 * Return: 0 on success or a negative errno.
 */
int airoha_lddla_tx_rearm(struct device *dev)
{
	struct airoha_lddla *lddla = dev_get_drvdata(dev);
	int ret;

	if (!lddla || lddla->dev != dev || !lddla->ops->tx_rearm)
		return -EOPNOTSUPP;

	ret = lddla_lock(lddla);
	if (ret)
		return ret;

	ret = lddla->ops->tx_rearm(lddla);
	mutex_unlock(&lddla->lock);

	return ret;
}
EXPORT_SYMBOL_GPL(airoha_lddla_tx_rearm);

/* ------------------------------------------------------------------ */
/* debugfs								      */
/* ------------------------------------------------------------------ */

static int airoha_lddla_diag_show(struct seq_file *s, void *unused)
{
	struct airoha_lddla *lddla = s->private;
	int ret;

	ret = lddla_lock(lddla);
	if (ret)
		return ret;
	seq_printf(s, "mode:        %s\n",
		   lddla->pon_mode == AIROHA_PON_GPON ? "GPON" :
		   lddla->pon_mode == AIROHA_PON_EPON ? "EPON" : "unknown");
	seq_printf(s, "ddmi temp:   0x%04x\n", lddla->ddmi_temperature);
	seq_printf(s, "ddmi vcc:    0x%04x\n", lddla->ddmi_voltage);
	seq_printf(s, "ddmi bias:   0x%04x\n", lddla->ddmi_current);
	seq_printf(s, "ddmi tx_pwr: 0x%04x\n", lddla->ddmi_tx_power);
	seq_printf(s, "ddmi rx_pwr: 0x%04x\n", lddla->ddmi_rx_power);
	seq_printf(s, "alarm:       0x%04x\n", lddla->alarm);
	if (lddla->ops->diag_show)
		lddla->ops->diag_show(lddla, s);
	mutex_unlock(&lddla->lock);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(airoha_lddla_diag);

static int airoha_lddla_flash_show(struct seq_file *s, void *unused)
{
	struct airoha_lddla *lddla = s->private;
	int i, ret;

	ret = lddla_lock(lddla);
	if (ret)
		return ret;
	for (i = 0; i < AIROHA_LDDLA_FLASH_WORDS; i++)
		seq_printf(s, "0x%03x = 0x%08x\n", i * 4, lddla->flash[i]);
	mutex_unlock(&lddla->lock);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(airoha_lddla_flash);

static ssize_t airoha_lddla_tx_rearm_write(struct file *file,
					   const char __user *buf, size_t count,
					   loff_t *ppos)
{
	struct airoha_lddla *lddla = file->private_data;
	bool rearm;
	int ret;

	ret = kstrtobool_from_user(buf, count, &rearm);
	if (ret)
		return ret;
	if (!rearm)
		return -EINVAL;

	ret = airoha_lddla_tx_rearm(lddla->dev);
	if (ret)
		return ret;

	return count;
}

static const struct file_operations airoha_lddla_tx_rearm_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = airoha_lddla_tx_rearm_write,
	.llseek = noop_llseek,
};

/**
 * lddla_debugfs_init() - create the per-device debugfs directory.
 * @lddla: device
 *
 * Adds the common "diag" (state dump, including the chip-specific @diag_show)
 * and "flash" (calibration mirror) files.  Chips may add further files under
 * lddla->debugfs after calling this.
 */
void lddla_debugfs_init(struct airoha_lddla *lddla)
{
	lddla->debugfs = debugfs_create_dir(dev_name(lddla->dev), NULL);
	debugfs_create_file("diag", 0444, lddla->debugfs, lddla,
			    &airoha_lddla_diag_fops);
	debugfs_create_file("flash", 0444, lddla->debugfs, lddla,
			    &airoha_lddla_flash_fops);
	if (lddla->ops->tx_rearm)
		debugfs_create_file("tx_rearm", 0200, lddla->debugfs,
				    lddla, &airoha_lddla_tx_rearm_fops);
}

/* Tear down the debugfs directory and everything under it. */
void lddla_debugfs_remove(struct airoha_lddla *lddla)
{
	debugfs_remove_recursive(lddla->debugfs);
}

/* ------------------------------------------------------------------ */
/* Virtual SFP MSA / SFF-8472 bus					      */
/* ------------------------------------------------------------------ */

#define AIROHA_SFP_ROM_ADDR	0x50	/* 8-bit 0xA0: SFP MSA serial ID */
#define AIROHA_SFP_DDM_ADDR	0x51	/* 8-bit 0xA2: SFF-8472 diagnostics */
#define AIROHA_SFP_PAGE_SIZE	256

struct airoha_sfp {
	struct i2c_adapter adapter;
	struct airoha_lddla *lddla;
	u8 rom[AIROHA_SFP_PAGE_SIZE];
	u8 ddm[AIROHA_SFP_PAGE_SIZE];
	u8 rom_ptr;
	u8 ddm_ptr;
};

static void put_be16(u8 *buf, int off, u16 val)
{
	buf[off] = val >> 8;
	buf[off + 1] = val & 0xff;
}

/* Copy an ASCII field, space-padded to @len (no NUL terminator). */
static void put_ascii(u8 *buf, int off, const char *s, int len)
{
	int i;

	for (i = 0; i < len; i++)
		buf[off + i] = *s ? *s++ : ' ';
}

static u8 sfp_checksum(const u8 *buf, int first, int last)
{
	u8 sum = 0;
	int i;

	for (i = first; i <= last; i++)
		sum += buf[i];
	return sum;
}

static void airoha_sfp_build_rom(struct airoha_sfp *sfp)
{
	const struct airoha_lddla_ops *ops = sfp->lddla->ops;
	bool gpon = sfp->lddla->pon_mode == AIROHA_PON_GPON;
	u8 *r = sfp->rom;

	memset(r, 0, AIROHA_SFP_PAGE_SIZE);

	r[0] = 0x03;			/* Identifier: SFP/SFP+ */
	r[1] = 0x04;			/* Ext. identifier: 2-wire serial ID */
	r[2] = 0x01;			/* Connector: SC */
	r[11] = 0x03;			/* Encoding: NRZ */
	r[12] = gpon ? 0x19 : 0x0d;	/* BR nominal: ~2.5G GPON / 1.25G EPON */
	put_be16(r, 14, 1490);		/* downstream wavelength (nm) */

	put_ascii(r, 20, "Airoha", 16);			/* vendor name */
	r[37] = 0x00;					/* vendor OUI */
	r[38] = 0x0c;
	r[39] = 0xe7;
	put_ascii(r, 40, ops->part_number, 16);		/* vendor part number */
	put_ascii(r, 56, "0001", 4);			/* vendor revision */
	put_be16(r, 60, 1490);				/* wavelength (nm) */
	r[63] = sfp_checksum(r, 0, 62);			/* CC_BASE */

	r[65] = 0x1a;			/* Options: TX_DISABLE | TX_FAULT | LOS */
	put_ascii(r, 68, ops->serial, 16);		/* vendor serial number */
	put_ascii(r, 84, ops->date_code, 8);		/* date code */
	r[92] = 0x68;			/* DDM: implemented, internally cal, avg Rx */
	r[93] = 0xf0;			/* enhanced options: alarms, soft control */
	r[94] = 0x06;			/* SFF-8472 compliance (rev 11.3) */
	r[95] = sfp_checksum(r, 64, 94);		/* CC_EXT */
}

static void airoha_sfp_build_ddm_thresholds(struct airoha_sfp *sfp)
{
	u8 *d = sfp->ddm;

	memset(d, 0, AIROHA_SFP_PAGE_SIZE);

	put_be16(d, 0, AIROHA_TEMP_HIGH_THLD);
	put_be16(d, 2, AIROHA_TEMP_LOW_THLD);
	put_be16(d, 4, AIROHA_TEMP_HIGH_THLD);
	put_be16(d, 6, AIROHA_TEMP_LOW_THLD);
	put_be16(d, 8, AIROHA_VOLT_HIGH_THLD);
	put_be16(d, 10, AIROHA_VOLT_LOW_THLD);
	put_be16(d, 12, AIROHA_VOLT_HIGH_THLD);
	put_be16(d, 14, AIROHA_VOLT_LOW_THLD);
	put_be16(d, 16, AIROHA_TX_BIAS_HIGH_THLD);
	put_be16(d, 18, AIROHA_TX_BIAS_LOW_THLD);
	put_be16(d, 20, AIROHA_TX_BIAS_HIGH_THLD);
	put_be16(d, 22, AIROHA_TX_BIAS_LOW_THLD);
	put_be16(d, 24, AIROHA_TX_PWR_HIGH_THLD);
	put_be16(d, 26, AIROHA_TX_PWR_LOW_THLD);
	put_be16(d, 28, AIROHA_TX_PWR_HIGH_THLD);
	put_be16(d, 30, AIROHA_TX_PWR_LOW_THLD);
	put_be16(d, 32, AIROHA_RX_PWR_HIGH_THLD);
	put_be16(d, 34, AIROHA_RX_PWR_LOW_THLD);
	put_be16(d, 36, AIROHA_RX_PWR_HIGH_THLD);
	put_be16(d, 38, AIROHA_RX_PWR_LOW_THLD);

	/* Internally-calibrated module: unity slopes, zero offsets. */
	put_be16(d, 56, 1);
	put_be16(d, 80, 1);
	put_be16(d, 84, 1);
	put_be16(d, 88, 1);
	put_be16(d, 90, 1);
	d[95] = sfp_checksum(d, 0, 94);		/* CC_DMI */
}

/* Refresh the live SFF-8472 diagnostics from the cached DDMI words. */
static void airoha_sfp_refresh_ddm(struct airoha_sfp *sfp)
{
	struct airoha_lddla *lddla = sfp->lddla;
	u8 *d = sfp->ddm;
	u32 alarm;

	mutex_lock(&lddla->lock);
	put_be16(d, 96, lddla->ddmi_temperature);
	put_be16(d, 98, lddla->ddmi_voltage);
	put_be16(d, 100, lddla->ddmi_current);
	put_be16(d, 102, lddla->ddmi_tx_power);
	put_be16(d, 104, lddla->ddmi_rx_power);
	alarm = lddla->alarm;
	mutex_unlock(&lddla->lock);

	d[110] = 0x00;		/* Data_Ready_Bar cleared = ready */

	d[112] = (alarm & AIROHA_ALARM_HIGH_TEMP   ? 0x80 : 0) |
		 (alarm & AIROHA_ALARM_LOW_TEMP    ? 0x40 : 0) |
		 (alarm & AIROHA_ALARM_HIGH_VOLT   ? 0x20 : 0) |
		 (alarm & AIROHA_ALARM_LOW_VOLT    ? 0x10 : 0) |
		 (alarm & AIROHA_ALARM_TX_HIGH_BIAS  ? 0x08 : 0) |
		 (alarm & AIROHA_ALARM_TX_LOW_BIAS   ? 0x04 : 0) |
		 (alarm & AIROHA_ALARM_TX_HIGH_POWER ? 0x02 : 0) |
		 (alarm & AIROHA_ALARM_TX_LOW_POWER  ? 0x01 : 0);
	d[113] = (alarm & AIROHA_ALARM_RX_HIGH_POWER ? 0x80 : 0) |
		 (alarm & AIROHA_ALARM_RX_LOW_POWER  ? 0x40 : 0);
	d[116] = d[112];
	d[117] = d[113];
}

static int airoha_sfp_xfer(struct i2c_adapter *adap, struct i2c_msg *msgs,
			   int num)
{
	struct airoha_sfp *sfp = i2c_get_adapdata(adap);
	u8 *page, *ptr;
	int i, j;

	for (i = 0; i < num; i++) {
		struct i2c_msg *msg = &msgs[i];

		if (msg->addr == AIROHA_SFP_ROM_ADDR) {
			page = sfp->rom;
			ptr = &sfp->rom_ptr;
		} else if (msg->addr == AIROHA_SFP_DDM_ADDR) {
			page = sfp->ddm;
			ptr = &sfp->ddm_ptr;
		} else {
			return -ENXIO;
		}

		if (msg->flags & I2C_M_RD) {
			if (page == sfp->ddm)
				airoha_sfp_refresh_ddm(sfp);
			for (j = 0; j < msg->len; j++)
				msg->buf[j] = page[(*ptr)++];
		} else {
			if (msg->len >= 1)
				*ptr = msg->buf[0];	/* set word pointer */
		}
	}

	return num;
}

static u32 airoha_sfp_func(struct i2c_adapter *adap)
{
	return I2C_FUNC_I2C | I2C_FUNC_SMBUS_EMUL;
}

static const struct i2c_algorithm airoha_sfp_algo = {
	.master_xfer = airoha_sfp_xfer,
	.functionality = airoha_sfp_func,
};

/**
 * lddla_sfp_init() - publish the diagnostics as a virtual SFP module.
 * @lddla: device
 *
 * Registers an emulated I2C adapter carrying the two SFP slaves (MSA serial-ID
 * at 0x50, SFF-8472 diagnostics at 0x51) so the standard SFP/phylink and
 * ethtool tooling can read this soldered-down module.  Return: 0 or errno.
 */
int lddla_sfp_init(struct airoha_lddla *lddla)
{
	struct airoha_sfp *sfp;
	int ret;

	sfp = devm_kzalloc(lddla->dev, sizeof(*sfp), GFP_KERNEL);
	if (!sfp)
		return -ENOMEM;

	sfp->lddla = lddla;
	airoha_sfp_build_rom(sfp);
	airoha_sfp_build_ddm_thresholds(sfp);

	sfp->adapter.owner = THIS_MODULE;
	sfp->adapter.algo = &airoha_sfp_algo;
	sfp->adapter.dev.parent = lddla->dev;
	sfp->adapter.dev.of_node =
		of_get_child_by_name(lddla->dev->of_node, "i2c-sfp");
	sfp->adapter.class = 0;
	strscpy(sfp->adapter.name, "Airoha LDDLA virtual SFP",
		sizeof(sfp->adapter.name));
	i2c_set_adapdata(&sfp->adapter, sfp);

	ret = i2c_add_adapter(&sfp->adapter);
	if (ret)
		return ret;

	lddla->sfp = sfp;
	dev_info(lddla->dev, "virtual SFP bus %d: ROM @0x%02x, DDM @0x%02x\n",
		 sfp->adapter.nr, AIROHA_SFP_ROM_ADDR, AIROHA_SFP_DDM_ADDR);
	return 0;
}

/* Remove the virtual SFP adapter. */
void lddla_sfp_remove(struct airoha_lddla *lddla)
{
	if (lddla->sfp) {
		i2c_del_adapter(&lddla->sfp->adapter);
		of_node_put(lddla->sfp->adapter.dev.of_node);
		lddla->sfp = NULL;
	}
}

/* ------------------------------------------------------------------ */
/* Module: register the configured chip drivers			      */
/* ------------------------------------------------------------------ */

#if IS_ENABLED(CONFIG_EN7570_PHY)
extern struct i2c_driver en7570_i2c_driver;
#endif
#if IS_ENABLED(CONFIG_EN7571_PHY)
extern struct i2c_driver en7571_i2c_driver;
#endif
#if IS_ENABLED(CONFIG_EN7572_PHY)
extern struct i2c_driver en7572_i2c_driver;
#endif

static int __init airoha_lddla_init(void)
{
	int ret = 0;

#if IS_ENABLED(CONFIG_EN7570_PHY)
	ret = i2c_add_driver(&en7570_i2c_driver);
	if (ret)
		return ret;
#endif
#if IS_ENABLED(CONFIG_EN7571_PHY)
	ret = i2c_add_driver(&en7571_i2c_driver);
	if (ret)
		goto err_7571;
#endif
#if IS_ENABLED(CONFIG_EN7572_PHY)
	ret = i2c_add_driver(&en7572_i2c_driver);
	if (ret)
		goto err_7572;
#endif
	return ret;

#if IS_ENABLED(CONFIG_EN7572_PHY)
err_7572:
#if IS_ENABLED(CONFIG_EN7571_PHY)
	i2c_del_driver(&en7571_i2c_driver);
#endif
#endif
#if IS_ENABLED(CONFIG_EN7571_PHY)
err_7571:
#if IS_ENABLED(CONFIG_EN7570_PHY)
	i2c_del_driver(&en7570_i2c_driver);
#endif
#endif
	return ret;
}
module_init(airoha_lddla_init);

static void __exit airoha_lddla_exit(void)
{
#if IS_ENABLED(CONFIG_EN7572_PHY)
	i2c_del_driver(&en7572_i2c_driver);
#endif
#if IS_ENABLED(CONFIG_EN7571_PHY)
	i2c_del_driver(&en7571_i2c_driver);
#endif
#if IS_ENABLED(CONFIG_EN7570_PHY)
	i2c_del_driver(&en7570_i2c_driver);
#endif
}
module_exit(airoha_lddla_exit);

MODULE_DESCRIPTION("Airoha EN7570/EN7571/EN7572 xPON LDDLA controller driver");
MODULE_AUTHOR("Benjamin Larsson <benjamin.larsson@genexis.eu>");
MODULE_LICENSE("GPL");
