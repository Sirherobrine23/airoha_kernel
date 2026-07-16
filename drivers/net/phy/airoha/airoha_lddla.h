/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Shared core for the Airoha xPON LDDLA controller drivers (EN7570, EN7571).
 *
 * The two chips share the same host integration: an I2C-attached register
 * file at slave 0x70 with a 2-byte big-endian pointer and little-endian data,
 * a 100-word calibration firmware blob, a 1 Hz control worker, and SFF-8472
 * digital diagnostics exported through hwmon and a virtual SFP bus.
 *
 * This header defines the common per-device state object, the per-chip
 * operations table the shared code calls back into, the shared I2C transport,
 * the calibration store, and the shared SFF-8472 unit constants.  Each chip
 * embeds struct airoha_lddla as the first member of its private structure.
 *
 * Fixed-point unit conventions:
 *   - temperature    : milli-degrees Celsius (m degC)
 *   - supply voltage : microvolts (uV)
 *   - current        : microamps (uA)
 *   - optical power  : tenths of a microwatt (0.1 uW), matching SFF-8472
 */
#ifndef _AIROHA_LDDLA_H
#define _AIROHA_LDDLA_H

#include <linux/bits.h>
#include <linux/i2c.h>
#include <linux/minmax.h>
#include <linux/mutex.h>
#include <linux/types.h>

struct dentry;
struct seq_file;
struct airoha_sfp;
struct airoha_lddla;

/* xPON mode (shared values). */
#define AIROHA_PON_UNKNOWN	(-1)
#define AIROHA_PON_EPON		0
#define AIROHA_PON_GPON		1

/* Calibration NVM: 100 little-endian 32-bit words; 0xffffffff == erased. */
#define AIROHA_LDDLA_FLASH_WORDS	100
#define AIROHA_LDDLA_FLASH_ERASED	0xffffffff

/* DDMI alarm bits (host-visible). */
#define AIROHA_ALARM_TX_LOW_POWER	BIT(0)
#define AIROHA_ALARM_TX_HIGH_POWER	BIT(1)
#define AIROHA_ALARM_TX_LOW_BIAS	BIT(2)
#define AIROHA_ALARM_TX_HIGH_BIAS	BIT(3)
#define AIROHA_ALARM_RX_LOW_POWER	BIT(4)
#define AIROHA_ALARM_RX_HIGH_POWER	BIT(5)
#define AIROHA_ALARM_LOW_VOLT		BIT(6)
#define AIROHA_ALARM_HIGH_VOLT		BIT(7)
#define AIROHA_ALARM_LOW_TEMP		BIT(8)
#define AIROHA_ALARM_HIGH_TEMP		BIT(9)

/* SFF-8472 alarm/warning thresholds (word units), shared by both chips. */
#define AIROHA_TX_PWR_LOW_THLD		0x2710	/* 0.0 dBm   (0.1 uW units) */
#define AIROHA_TX_PWR_HIGH_THLD		0x8a99	/* +5.5 dBm */
#define AIROHA_TX_BIAS_LOW_THLD		0x01f4	/* 1 mA      (2 uA units) */
#define AIROHA_TX_BIAS_HIGH_THLD	0xc350	/* 100 mA */
#define AIROHA_RX_PWR_LOW_THLD		0x000a	/* -30 dBm   (0.1 uW units) */
#define AIROHA_RX_PWR_HIGH_THLD		0x09cf	/* -6 dBm */
#define AIROHA_VOLT_LOW_THLD		0x7148	/* 2.9 V     (100 uV units) */
#define AIROHA_VOLT_HIGH_THLD		0x9088	/* 3.7 V */
#define AIROHA_TEMP_LOW_THLD		0xfb00	/* -5 degC   (1/256 degC) */
#define AIROHA_TEMP_HIGH_THLD		0x5500	/* +85 degC */

/**
 * struct airoha_lddla_ops - per-chip callbacks invoked by the shared core.
 * @name: hwmon / i2c driver name ("en7570" / "en7571").
 * @part_number: SFP MSA vendor part-number string.
 * @serial: SFP MSA vendor serial-number string.
 * @date_code: SFP MSA date code (6 chars).
 * @temp_refresh: refresh the temperature DDMI word; return IC temp (m degC).
 * @vcc_refresh: refresh and return the cached supply-voltage word.
 * @bias_refresh: refresh and return the cached Tx bias-current word.
 * @tx_power_refresh: refresh and return the cached Tx optical-power word.
 * @rx_power_refresh: refresh and return the cached Rx optical-power word.
 * @diag_show: chip-specific debugfs diagnostic dump (optional).
 * @tx_rearm: rearm the optical transmitter after TX_DISABLE is released.
 *
 * The refreshers update the matching airoha_lddla.ddmi_* cache and the alarm
 * bitmap; the shared hwmon and SFP layers only read the cache.
 */
struct airoha_lddla_ops {
	const char *name;
	const char *part_number;
	const char *serial;
	const char *date_code;

	s32 (*temp_refresh)(struct airoha_lddla *lddla);
	u16 (*vcc_refresh)(struct airoha_lddla *lddla);
	u16 (*bias_refresh)(struct airoha_lddla *lddla);
	u16 (*tx_power_refresh)(struct airoha_lddla *lddla);
	u16 (*rx_power_refresh)(struct airoha_lddla *lddla);
	void (*diag_show)(struct airoha_lddla *lddla, struct seq_file *s);
	int (*tx_rearm)(struct airoha_lddla *lddla);
};

/**
 * struct airoha_lddla - shared per-device state, embedded first in each chip priv.
 * @client: backing I2C client (slave 0x70).
 * @dev: device for logging and firmware/sysfs registration.
 * @ops: per-chip operations table.
 * @lock: serialises every I2C transaction so the multi-step shared-ADC
 *	sequence (channel-select, latch, status-read) stays atomic.
 * @hwmon: registered hwmon device.
 * @debugfs: per-device debugfs directory.
 * @sfp: virtual SFP bus exposing the diagnostics (may be NULL).
 * @fw_name: calibration firmware blob name.
 * @flash: in-memory mirror of the calibration NVM.
 * @pon_mode: AIROHA_PON_* operating mode.
 * @alarm: AIROHA_ALARM_* bitmap of active DDMI alarms.
 * @ddmi_temperature: cached SFF-8472 temperature word (1/256 degC).
 * @ddmi_voltage: cached SFF-8472 supply-voltage word (100 uV units).
 * @ddmi_current: cached SFF-8472 Tx bias-current word (2 uA units).
 * @ddmi_tx_power: cached SFF-8472 Tx optical-power word (0.1 uW units).
 * @ddmi_rx_power: cached SFF-8472 Rx optical-power word (0.1 uW units).
 */
struct airoha_lddla {
	struct i2c_client *client;
	struct device *dev;
	const struct airoha_lddla_ops *ops;

	struct mutex lock;

	struct device *hwmon;
	struct dentry *debugfs;
	struct airoha_sfp *sfp;

	const char *fw_name;
	u32 flash[AIROHA_LDDLA_FLASH_WORDS];

	int pon_mode;
	u32 alarm;

	u16 ddmi_temperature;
	u16 ddmi_voltage;
	u16 ddmi_current;
	u16 ddmi_tx_power;
	u16 ddmi_rx_power;
};

/* --- I2C transport (airoha_lddla.c) --- */
int lddla_rd(struct airoha_lddla *lddla, u16 addr, u8 *buf, int len);
int lddla_wr(struct airoha_lddla *lddla, u16 addr, const u8 *buf, int len);
int lddla_rd8(struct airoha_lddla *lddla, u16 addr, u8 *val);
int lddla_wr8(struct airoha_lddla *lddla, u16 addr, u8 val);
int lddla_update8(struct airoha_lddla *lddla, u16 addr, u8 mask, u8 set);
int lddla_rd16(struct airoha_lddla *lddla, u16 addr, u16 *val);
int lddla_rd32(struct airoha_lddla *lddla, u16 addr, u32 *val);
int lddla_lock(struct airoha_lddla *lddla);

/* --- Calibration store (airoha_lddla.c) --- */
int lddla_flash_load(struct airoha_lddla *lddla);
u32 lddla_flash_read(struct airoha_lddla *lddla, u32 off);
void lddla_flash_defaults(struct airoha_lddla *lddla);

/* --- hwmon + virtual SFP + debugfs (airoha_lddla.c) --- */
int lddla_hwmon_register(struct airoha_lddla *lddla);
void lddla_debugfs_init(struct airoha_lddla *lddla);
void lddla_debugfs_remove(struct airoha_lddla *lddla);
int lddla_sfp_init(struct airoha_lddla *lddla);
void lddla_sfp_remove(struct airoha_lddla *lddla);

#endif /* _AIROHA_LDDLA_H */
