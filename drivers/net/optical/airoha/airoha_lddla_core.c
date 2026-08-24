// SPDX-License-Identifier: GPL-2.0
/*
 * Shared core for xPON LDDLA (laser-diode driver + limiting amplifier)
 * controllers used with Airoha/EcoNet xPON MACs.
 *
 * Provides the common I2C transport, the bounded ADC/I2C lock, the calibration
 * firmware store, hwmon registration and the single
 * module entry point that registers whichever chip drivers are configured in.
 * The per-chip drivers supply a struct airoha_lddla_ops for the live readback.
 *
 * Bus attachment
 * ==============
 * The driver is an I2C client.  The LDDLA is a physical slave at
 * 7-bit address 0x70, described in the device tree as "airoha,en7570" or
 * "airoha,en7571" (or matched by the i2c_device_id table); the per-chip
 * i2c_driver binds to it and the transport helpers below issue master
 * transactions to 0x70.
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
#include <linux/i2c.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/nvmem-consumer.h>
#include <linux/sched/signal.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/sysfs.h>
#include <linux/unaligned.h>

#include "airoha_lddla.h"

#define AIROHA_LDDLA_NVMEM_CELL "calibration"

/* ------------------------------------------------------------------ */
/* I2C transport							      */
/* ------------------------------------------------------------------ */

/*
 * EN7570/EN7571 I2C register addressing
 * ======================================
 * These LDDLA devices are 7-bit I2C slaves at address 0x70.  Their register
 * file is reached
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
/* Shared BOB / calibration store                                     */
/* ------------------------------------------------------------------ */

static bool lddla_bob_magic_valid(u32 magic)
{
	return (magic & AIROHA_LDDLA_BOB_MAGIC_COMMON_MASK) ==
	       AIROHA_LDDLA_BOB_MAGIC_COMMON_MIN;
}

static u8 lddla_bob_magic_chip_id(u32 magic)
{
	return magic & AIROHA_LDDLA_BOB_MAGIC_CHIP_MASK;
}

static u8 lddla_bob_magic_profile(u32 magic)
{
	return (magic & AIROHA_LDDLA_BOB_MAGIC_PROFILE_MASK) >> 24;
}

/*
 * Detect the byte order used by a factory BOB image from the PON magic at
 * byte 0x94.  Source images found in flash/NVMEM are known to exist in both
 * word orders, so never infer the format from the CPU endianness.
 */
static int lddla_bob_check(struct airoha_lddla *lddla, const u8 *data,
			   size_t len,
			   enum airoha_lddla_bob_endian *endian,
			   u32 *magic)
{
	u32 le, be;
	bool le_match, be_match;

	if (len < AIROHA_LDDLA_BOB_MAGIC_OFFSET + sizeof(u32))
		return -EINVAL;

	le = get_unaligned_le32(data + AIROHA_LDDLA_BOB_MAGIC_OFFSET);
	be = get_unaligned_be32(data + AIROHA_LDDLA_BOB_MAGIC_OFFSET);
	le_match = lddla_bob_magic_valid(le);
	be_match = lddla_bob_magic_valid(be);

	dev_dbg(lddla->dev,
		"BOB magic: little-endian 0x%08x, big-endian 0x%08x\n",
		le, be);

	if (le_match == be_match) {
		dev_err(lddla->dev,
			"unable to determine BOB byte order from Airoha signature at %#x\n",
			AIROHA_LDDLA_BOB_MAGIC_OFFSET);
		return -EINVAL;
	}

	if (le_match) {
		*endian = AIROHA_LDDLA_BOB_ENDIAN_LITTLE;
		*magic = le;
	} else {
		*endian = AIROHA_LDDLA_BOB_ENDIAN_BIG;
		*magic = be;
	}

	return 0;
}

static int lddla_bob_import(struct airoha_lddla *lddla, const u8 *data,
			    size_t len, const char *source)
{
	enum airoha_lddla_bob_endian endian;
	u32 magic;
	size_t expected = lddla->ops->bob_size;
	size_t i;
	int ret;

	if (!expected || expected > sizeof(lddla->bob))
		return -EINVAL;
	if (len < 0xff) {
		dev_err(lddla->dev, "%s BOB is too small (%zu < %zu bytes)\n",
			source, len, expected);
		return -EINVAL;
	}
	if (len > expected)
		dev_warn(lddla->dev,
			 "%s BOB has %zu extra bytes; ignoring them\n",
			 source, len - expected);

	ret = lddla_bob_check(lddla, data, expected, &endian, &magic);
	if (ret)
		return ret;

	memset(lddla->bob, 0xff, sizeof(lddla->bob));
	if (endian == AIROHA_LDDLA_BOB_ENDIAN_LITTLE) {
		memcpy(lddla->bob, data, expected);
	} else {
		/* Normalize every 32-bit factory word to little-endian bytes. */
		for (i = 0; i + sizeof(u32) <= expected; i += sizeof(u32))
			put_unaligned_le32(get_unaligned_be32(data + i),
					   lddla->bob + i);
		if (i < expected)
			memcpy(lddla->bob + i, data + i, expected - i);
	}

	lddla->bob_len = expected;
	lddla->bob_valid = true;
	lddla->bob_source_endian = endian;
	lddla->bob_magic = magic;
	lddla->bob_chip_id = lddla_bob_magic_chip_id(magic);
	lddla->bob_profile = lddla_bob_magic_profile(magic);

	dev_info(lddla->dev,
		 "loaded %zu-byte %s-endian BOB from %s: magic 0x%08x, chip-id 0x%02x, profile 0x%02x\n",
		 expected,
		 endian == AIROHA_LDDLA_BOB_ENDIAN_BIG ? "big" : "little",
		 source, magic, lddla->bob_chip_id, lddla->bob_profile);
	return 0;
}

#if IS_REACHABLE(CONFIG_NVMEM)
static int lddla_bob_load_nvmem(struct airoha_lddla *lddla)
{
	struct nvmem_cell *cell;
	void *buf;
	size_t len;
	int ret;

	cell = nvmem_cell_get(lddla->dev, AIROHA_LDDLA_NVMEM_CELL);
	if (IS_ERR(cell)) {
		ret = PTR_ERR(cell);
		if (ret == -ENOENT || ret == -EOPNOTSUPP)
			return -ENOENT;
		return ret;
	}

	buf = nvmem_cell_read(cell, &len);
	nvmem_cell_put(cell);
	if (IS_ERR(buf))
		return PTR_ERR(buf);

	ret = lddla_bob_import(lddla, buf, len, "NVMEM");
	kfree(buf);
	if (!ret)
		lddla->bob_source = AIROHA_LDDLA_BOB_SOURCE_NVMEM;
	return ret;
}
#else
static int lddla_bob_load_nvmem(struct airoha_lddla *lddla)
{
	return -ENOENT;
}
#endif

static int lddla_bob_load_firmware(struct airoha_lddla *lddla)
{
	const struct firmware *fw;
	int ret;

	if (!lddla->bob_fw_name)
		return -ENOENT;

	ret = request_firmware_direct(&fw, lddla->bob_fw_name, lddla->dev);
	if (ret)
		return ret;

	ret = lddla_bob_import(lddla, fw->data, fw->size,
			       lddla->bob_fw_name);
	release_firmware(fw);
	if (!ret)
		lddla->bob_source = AIROHA_LDDLA_BOB_SOURCE_FIRMWARE;
	return ret;
}

/**
 * lddla_bob_load() - load and normalize an Airoha factory BOB image.
 * @lddla: device
 *
 * NVMEM is preferred over a firmware file.  The magic at byte 0x94 identifies
 * both the EN757x family member and whether the source stores 32-bit words in
 * little or big endian.  The in-memory image is always normalized to
 * little-endian bytes, so the chip algorithms behave identically on LE and BE
 * hosts and no userspace byte-swap step is required.
 *
 * Missing calibration is non-fatal and leaves an erased mirror.  A present but
 * malformed or wrong-chip BOB is rejected.
 */
int lddla_bob_load(struct airoha_lddla *lddla)
{
	int ret;

	lddla_flash_defaults(lddla);

	ret = lddla_bob_load_nvmem(lddla);
	if (!ret)
		return 0;
	if (ret != -ENOENT)
		return ret;

	ret = lddla_bob_load_firmware(lddla);
	if (!ret)
		return 0;
	if (ret != -ENOENT && ret != -ENODEV) {
		dev_err(lddla->dev, "failed to load BOB firmware '%s': %d\n",
			lddla->bob_fw_name ?: "<none>", ret);
		return ret;
	}

	dev_warn(lddla->dev,
		 "calibration BOB '%s' not found (%d); using erased defaults\n",
		 lddla->bob_fw_name ?: "<none>", ret);
	return 0;
}

/**
 * lddla_flash_read() - read one normalized calibration word.
 * @lddla: device
 * @off: byte offset into the BOB (a multiple of four)
 *
 * Return: host-endian value of the canonical little-endian BOB word, or the
 * erased value if @off is outside the loaded image.
 */
u32 lddla_flash_read(struct airoha_lddla *lddla, u32 off)
{
	if (off & (sizeof(u32) - 1))
		return AIROHA_LDDLA_FLASH_ERASED;
	if (off + sizeof(u32) > lddla->bob_len)
		return AIROHA_LDDLA_FLASH_ERASED;

	return get_unaligned_le32(lddla->bob + off);
}

void lddla_flash_defaults(struct airoha_lddla *lddla)
{
	size_t size = lddla->ops ? lddla->ops->bob_size : 0;

	memset(lddla->bob, 0xff, sizeof(lddla->bob));
	lddla->bob_len = min_t(size_t, size, sizeof(lddla->bob));
	lddla->bob_valid = false;
	lddla->bob_source = AIROHA_LDDLA_BOB_SOURCE_NONE;
	lddla->bob_source_endian = AIROHA_LDDLA_BOB_ENDIAN_UNKNOWN;
}

const struct optical_frontend_thresholds airoha_lddla_default_thresholds = {
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

/* ------------------------------------------------------------------ */
/* Generic optical frontend bridge                                     */
/* ------------------------------------------------------------------ */

static int
airoha_lddla_frontend_set_mode(struct optical_frontend *frontend,
			       const struct optical_frontend_mode *mode)
{
	struct airoha_lddla *lddla = optical_frontend_get_drvdata(frontend);
	int pon_mode;
	int ret;

	switch (mode->protocol) {
	case OPTICAL_FRONTEND_PROTO_GPON:
		pon_mode = AIROHA_PON_GPON;
		break;
	case OPTICAL_FRONTEND_PROTO_EPON:
		pon_mode = AIROHA_PON_EPON;
		break;
	default:
		return -EOPNOTSUPP;
	}

	ret = lddla_lock(lddla);
	if (ret)
		return ret;

	/*
	 * EN757x calibration/bring-up is mode-specific. Do not silently
	 * reinterpret a frontend which was initialized for another PON mode.
	 */
	ret = lddla->pon_mode == pon_mode ? 0 : -EINVAL;
	mutex_unlock(&lddla->lock);

	return ret;
}

static int
airoha_lddla_frontend_get_telemetry(
	struct optical_frontend *frontend,
	struct optical_frontend_telemetry *telemetry)
{
	struct airoha_lddla *lddla = optical_frontend_get_drvdata(frontend);
	const struct airoha_lddla_ops *ops;
	int ret;

	if (!lddla || !telemetry)
		return -EINVAL;

	ops = lddla->ops;
	ret = lddla_lock(lddla);
	if (ret)
		return ret;

	memset(telemetry, 0, sizeof(*telemetry));
	if (ops->bosa_temp_refresh) {
		telemetry->temperature_mc = ops->bosa_temp_refresh(lddla);
		telemetry->valid |= OPTICAL_FRONTEND_TELEMETRY_F_TEMPERATURE;
	} else if (ops->temp_refresh) {
		telemetry->temperature_mc = ops->temp_refresh(lddla);
		telemetry->valid |= OPTICAL_FRONTEND_TELEMETRY_F_TEMPERATURE;
	}
	if (ops->vcc_refresh) {
		telemetry->voltage_uv = ops->vcc_refresh(lddla) * 100U;
		telemetry->valid |= OPTICAL_FRONTEND_TELEMETRY_F_VOLTAGE;
	}
	if (ops->bias_refresh) {
		telemetry->bias_ua = ops->bias_refresh(lddla) * 2U;
		telemetry->valid |= OPTICAL_FRONTEND_TELEMETRY_F_BIAS;
	}
	if (ops->tx_power_refresh) {
		telemetry->tx_power_nw = ops->tx_power_refresh(lddla) * 100U;
		telemetry->valid |= OPTICAL_FRONTEND_TELEMETRY_F_TX_POWER;
	}
	if (ops->rx_power_refresh) {
		telemetry->rx_power_nw = ops->rx_power_refresh(lddla) * 100U;
		telemetry->valid |= OPTICAL_FRONTEND_TELEMETRY_F_RX_POWER;
	}
	telemetry->alarms = lddla->alarm;
	telemetry->valid |= OPTICAL_FRONTEND_TELEMETRY_F_ALARMS;

	mutex_unlock(&lddla->lock);
	return telemetry->valid ? 0 : -ENODATA;
}

static int
airoha_lddla_frontend_get_state(struct optical_frontend *frontend,
				struct optical_frontend_state *state)
{
	struct airoha_lddla *lddla = optical_frontend_get_drvdata(frontend);

	if (!lddla || !state)
		return -EINVAL;

	memset(state, 0, sizeof(*state));
	state->present = true;
	state->ready = true;
	state->valid = OPTICAL_FRONTEND_STATE_F_PRESENT |
		       OPTICAL_FRONTEND_STATE_F_READY;

	return 0;
}

static int airoha_lddla_frontend_tx_rearm(struct optical_frontend *frontend)
{
	struct airoha_lddla *lddla = optical_frontend_get_drvdata(frontend);
	int ret;

	if (!lddla || !lddla->ops->tx_rearm)
		return -EOPNOTSUPP;

	ret = lddla_lock(lddla);
	if (ret)
		return ret;

	ret = lddla->ops->tx_rearm(lddla);
	mutex_unlock(&lddla->lock);

	return ret;
}

static const struct optical_frontend_ops airoha_lddla_frontend_ops = {
	.set_mode = airoha_lddla_frontend_set_mode,
	.get_telemetry = airoha_lddla_frontend_get_telemetry,
	.get_state = airoha_lddla_frontend_get_state,
	.tx_rearm = airoha_lddla_frontend_tx_rearm,
};

static struct airoha_lddla *airoha_lddla_from_frontend_dev(struct device *dev)
{
	struct optical_frontend *frontend = optical_frontend_from_dev(dev);

	return frontend ? optical_frontend_get_drvdata(frontend) : NULL;
}

static ssize_t valid_show(struct device *dev,
			  struct device_attribute *attr, char *buf)
{
	struct airoha_lddla *lddla = airoha_lddla_from_frontend_dev(dev);

	return lddla ? sysfs_emit(buf, "%u\n", lddla->bob_valid) : -ENODEV;
}
static DEVICE_ATTR_RO(valid);

static ssize_t source_show(struct device *dev,
			   struct device_attribute *attr, char *buf)
{
	struct airoha_lddla *lddla = airoha_lddla_from_frontend_dev(dev);
	const char *source;

	if (!lddla)
		return -ENODEV;

	switch (lddla->bob_source) {
	case AIROHA_LDDLA_BOB_SOURCE_NVMEM:
		source = "nvmem";
		break;
	case AIROHA_LDDLA_BOB_SOURCE_FIRMWARE:
		source = "firmware";
		break;
	case AIROHA_LDDLA_BOB_SOURCE_NONE:
	default:
		source = "none";
		break;
	}

	return sysfs_emit(buf, "%s\n", source);
}
static DEVICE_ATTR_RO(source);

static ssize_t endian_show(struct device *dev,
			   struct device_attribute *attr, char *buf)
{
	struct airoha_lddla *lddla = airoha_lddla_from_frontend_dev(dev);
	const char *endian;

	if (!lddla)
		return -ENODEV;

	switch (lddla->bob_source_endian) {
	case AIROHA_LDDLA_BOB_ENDIAN_LITTLE:
		endian = "little";
		break;
	case AIROHA_LDDLA_BOB_ENDIAN_BIG:
		endian = "big";
		break;
	case AIROHA_LDDLA_BOB_ENDIAN_UNKNOWN:
	default:
		endian = "unknown";
		break;
	}

	return sysfs_emit(buf, "%s\n", endian);
}
static DEVICE_ATTR_RO(endian);

static ssize_t size_show(struct device *dev,
			 struct device_attribute *attr, char *buf)
{
	struct airoha_lddla *lddla = airoha_lddla_from_frontend_dev(dev);

	return lddla ? sysfs_emit(buf, "%zu\n", lddla->bob_len) : -ENODEV;
}
static DEVICE_ATTR_RO(size);

static ssize_t magic_show(struct device *dev,
			  struct device_attribute *attr, char *buf)
{
	struct airoha_lddla *lddla = airoha_lddla_from_frontend_dev(dev);

	if (!lddla)
		return -ENODEV;
	if (!lddla->bob_valid)
		return -ENODATA;
	return sysfs_emit(buf, "0x%08x\n", lddla->bob_magic);
}
static DEVICE_ATTR_RO(magic);

static ssize_t chip_id_show(struct device *dev,
			    struct device_attribute *attr, char *buf)
{
	struct airoha_lddla *lddla = airoha_lddla_from_frontend_dev(dev);

	if (!lddla)
		return -ENODEV;
	if (!lddla->bob_valid)
		return -ENODATA;
	return sysfs_emit(buf, "0x%02x\n", lddla->bob_chip_id);
}
static DEVICE_ATTR_RO(chip_id);

static ssize_t profile_show(struct device *dev,
			    struct device_attribute *attr, char *buf)
{
	struct airoha_lddla *lddla = airoha_lddla_from_frontend_dev(dev);

	if (!lddla)
		return -ENODEV;
	if (!lddla->bob_valid)
		return -ENODATA;
	return sysfs_emit(buf, "0x%02x\n", lddla->bob_profile);
}
static DEVICE_ATTR_RO(profile);

static struct attribute *airoha_lddla_bob_attrs[] = {
	&dev_attr_valid.attr,
	&dev_attr_source.attr,
	&dev_attr_endian.attr,
	&dev_attr_size.attr,
	&dev_attr_magic.attr,
	&dev_attr_chip_id.attr,
	&dev_attr_profile.attr,
	NULL,
};

static const struct attribute_group airoha_lddla_bob_group = {
	.name = "bob",
	.attrs = airoha_lddla_bob_attrs,
};

static const struct attribute_group *airoha_lddla_frontend_groups[] = {
	&airoha_lddla_bob_group,
	NULL,
};

int lddla_frontend_register(struct airoha_lddla *lddla)
{
	const struct airoha_lddla_ops *ops = lddla->ops;
	struct optical_frontend_desc *desc = &lddla->frontend_desc;

	memset(desc, 0, sizeof(*desc));
	desc->name = ops->name;
	desc->type = "lddla";
	desc->vendor_name = ops->vendor_name ?: "Airoha";
	if (ops->vendor_name) {
		memcpy(desc->vendor_oui, ops->vendor_oui,
		       sizeof(desc->vendor_oui));
	} else {
		desc->vendor_oui[0] = 0x00;
		desc->vendor_oui[1] = 0x0c;
		desc->vendor_oui[2] = 0xe7;
	}
	desc->part_number = ops->part_number;
	desc->serial = ops->serial;
	desc->date_code = ops->date_code;
	desc->protocols = ops->protocols;
	desc->thresholds = ops->thresholds;
	desc->telemetry_cache_ms = 100;
	desc->groups = airoha_lddla_frontend_groups;

	if (ops->temp_refresh || ops->bosa_temp_refresh)
		desc->capabilities |= OPTICAL_FRONTEND_CAP_TEMPERATURE;
	if (ops->vcc_refresh)
		desc->capabilities |= OPTICAL_FRONTEND_CAP_VOLTAGE;
	if (ops->bias_refresh)
		desc->capabilities |= OPTICAL_FRONTEND_CAP_BIAS;
	if (ops->tx_power_refresh)
		desc->capabilities |= OPTICAL_FRONTEND_CAP_TX_POWER;
	if (ops->rx_power_refresh)
		desc->capabilities |= OPTICAL_FRONTEND_CAP_RX_POWER;
	desc->capabilities |= OPTICAL_FRONTEND_CAP_ALARMS |
			      OPTICAL_FRONTEND_CAP_BURST_TX;
	if (ops->tx_rearm)
		desc->capabilities |= OPTICAL_FRONTEND_CAP_TX_REARM;

	lddla->frontend = devm_optical_frontend_register(
		lddla->dev, desc, &airoha_lddla_frontend_ops, lddla);
	if (IS_ERR(lddla->frontend))
		return PTR_ERR(lddla->frontend);

	return devm_optical_frontend_hwmon_register(lddla->frontend);
}

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
	seq_printf(s, "bob:         %s, %zu bytes, source %s-endian\n",
		   lddla->bob_valid ? "valid" : "not loaded", lddla->bob_len,
		   lddla->bob_source_endian == AIROHA_LDDLA_BOB_ENDIAN_BIG ?
		   "big" : lddla->bob_source_endian == AIROHA_LDDLA_BOB_ENDIAN_LITTLE ?
		   "little" : "unknown");
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
	for (i = 0; i + sizeof(u32) <= lddla->bob_len; i += sizeof(u32))
		seq_printf(s, "0x%03x = 0x%08x\n", i,
			   lddla_flash_read(lddla, i));
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

	ret = optical_frontend_tx_rearm(lddla->frontend);
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

MODULE_DESCRIPTION("Airoha EN757x xPON optical frontend drivers");
MODULE_AUTHOR("Benjamin Larsson <benjamin.larsson@genexis.eu>");
MODULE_LICENSE("GPL");
