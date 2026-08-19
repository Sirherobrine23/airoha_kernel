/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Shared Airoha EN757x xPON laser-driver / limiting-amplifier core.
 *
 * EN7570/EN7571 use the shared 0x70 transport helpers below. EN7572 keeps its
 * own transport but reuses the Airoha family state and provider bridge. Generic
 * telemetry, hwmon, SFF compatibility and consumer lookup live in the vendor-
 * neutral optical_frontend subsystem.
 *
 * This header defines the Airoha family state, per-chip operations, EN757x
 * calibration helpers and legacy SFF-8472 units used by the chip algorithms.
 * Each chip embeds struct airoha_lddla as the first member of its private
 * structure.
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
#include <linux/optical_frontend.h>
#include <linux/types.h>

struct dentry;
struct seq_file;
struct airoha_lddla;

/* xPON mode (shared values). */
#define AIROHA_PON_UNKNOWN	(-1)
#define AIROHA_PON_EPON		0
#define AIROHA_PON_GPON		1

/* Calibration NVM: 100 little-endian 32-bit words; 0xffffffff == erased. */
#define AIROHA_LDDLA_FLASH_WORDS	100
#define AIROHA_LDDLA_FLASH_ERASED	0xffffffff
#define AIROHA_LDDLA_BOB_MAGIC_OFFSET		0x094
#define AIROHA_LDDLA_BOB_MAX_SIZE		512

/*
 * Every known Airoha LDD/LA BOB magic has the form 0xPP0507CC:
 *
 *   PP - PON/profile selector (for example 0x07 GPON, 0xe7 EPON)
 *   CC - LDD/LA model/variant byte (0x00 EN7570, 0x01 EN7571, ...)
 *
 * Only the 0x0507 family signature is invariant.  Keep the core open-ended:
 * a newly introduced LDD/LA must not require extending a static chip enum.
 */
#define AIROHA_LDDLA_BOB_MAGIC_COMMON_MASK	GENMASK(23, 8)
#define AIROHA_LDDLA_BOB_MAGIC_COMMON_MIN	0x00050700
#define AIROHA_LDDLA_BOB_MAGIC_PROFILE_MASK	GENMASK(31, 24)
#define AIROHA_LDDLA_BOB_MAGIC_CHIP_MASK	GENMASK(7, 0)

enum airoha_lddla_bob_endian {
	AIROHA_LDDLA_BOB_ENDIAN_UNKNOWN,
	AIROHA_LDDLA_BOB_ENDIAN_LITTLE,
	AIROHA_LDDLA_BOB_ENDIAN_BIG,
};

/* Keep the vendor driver names while using the generic alarm ABI. */
#define AIROHA_ALARM_TX_LOW_POWER	OPTICAL_FRONTEND_ALARM_TX_LOW_POWER
#define AIROHA_ALARM_TX_HIGH_POWER	OPTICAL_FRONTEND_ALARM_TX_HIGH_POWER
#define AIROHA_ALARM_TX_LOW_BIAS	OPTICAL_FRONTEND_ALARM_TX_LOW_BIAS
#define AIROHA_ALARM_TX_HIGH_BIAS	OPTICAL_FRONTEND_ALARM_TX_HIGH_BIAS
#define AIROHA_ALARM_RX_LOW_POWER	OPTICAL_FRONTEND_ALARM_RX_LOW_POWER
#define AIROHA_ALARM_RX_HIGH_POWER	OPTICAL_FRONTEND_ALARM_RX_HIGH_POWER
#define AIROHA_ALARM_LOW_VOLT		OPTICAL_FRONTEND_ALARM_LOW_VOLTAGE
#define AIROHA_ALARM_HIGH_VOLT		OPTICAL_FRONTEND_ALARM_HIGH_VOLTAGE
#define AIROHA_ALARM_LOW_TEMP		OPTICAL_FRONTEND_ALARM_LOW_TEMP
#define AIROHA_ALARM_HIGH_TEMP		OPTICAL_FRONTEND_ALARM_HIGH_TEMP

/* Legacy SFF-8472 alarm/warning thresholds used by EN7572 DDMI. */
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

extern const struct optical_frontend_thresholds
airoha_lddla_default_thresholds;

/**
 * struct airoha_lddla_ops - per-chip callbacks invoked by the shared core.
 * @name: hwmon / i2c driver name.
 * @vendor_name: SFP MSA vendor name; NULL keeps the Airoha default.
 * @vendor_oui: SFP MSA vendor OUI used when @vendor_name is non-NULL.
 * @part_number: SFP MSA vendor part-number string.
 * @serial: SFP MSA vendor serial-number string.
 * @date_code: SFP MSA date code (6 chars).
 * @protocols: BIT(OPTICAL_FRONTEND_PROTO_*) bitmap supported by this chip.
 * @thresholds: normalized alarm thresholds for this chip/firmware family.
 * @bob_size: expected BOB/calibration image size in bytes.
 * @temp_refresh: refresh the temperature DDMI word; return IC temp (m degC).
 * @bosa_temp_refresh: refresh and return BOSA temperature (m degC).
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
	const char *vendor_name;
	u8 vendor_oui[3];
	const char *part_number;
	const char *serial;
	const char *date_code;
	u32 protocols;
	const struct optical_frontend_thresholds *thresholds;
	size_t bob_size;

	s32 (*temp_refresh)(struct airoha_lddla *lddla);
	s32 (*bosa_temp_refresh)(struct airoha_lddla *lddla);
	u16 (*vcc_refresh)(struct airoha_lddla *lddla);
	u16 (*bias_refresh)(struct airoha_lddla *lddla);
	u16 (*tx_power_refresh)(struct airoha_lddla *lddla);
	u16 (*rx_power_refresh)(struct airoha_lddla *lddla);
	void (*diag_show)(struct airoha_lddla *lddla, struct seq_file *s);
	int (*tx_rearm)(struct airoha_lddla *lddla);
};

/**
 * struct airoha_lddla - shared per-device state, embedded first in each chip priv.
 * @client: backing I2C client.
 * @dev: device for logging and firmware/sysfs registration.
 * @ops: per-chip operations table.
 * @lock: serialises every I2C transaction so the multi-step shared-ADC
 *	sequence (channel-select, latch, status-read) stays atomic.
 * @frontend: generic optical frontend provider handle.
 * @frontend_desc: immutable description exported to generic consumers.
 * @debugfs: per-device debugfs directory.
 * @bob_fw_name: calibration/BOB firmware blob name.
 * @bob: canonical little-endian in-memory BOB image.
 * @bob_len: number of valid bytes in @bob.
 * @bob_valid: true when a valid BOB image was loaded.
 * @bob_source_endian: byte order detected in the source blob.
 * @bob_magic: normalized BOB magic read from byte 0x94.
 * @bob_chip_id: model/variant byte dynamically extracted from @bob_magic.
 * @bob_profile: PON/profile byte dynamically extracted from @bob_magic.
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

	struct optical_frontend *frontend;
	struct optical_frontend_desc frontend_desc;
	struct dentry *debugfs;

	const char *bob_fw_name;
	u8 bob[AIROHA_LDDLA_BOB_MAX_SIZE];
	size_t bob_len;
	bool bob_valid;
	enum airoha_lddla_bob_endian bob_source_endian;
	u32 bob_magic;
	u8 bob_chip_id;
	u8 bob_profile;

	int pon_mode;
	u32 alarm;

	u16 ddmi_temperature;
	u16 ddmi_voltage;
	u16 ddmi_current;
	u16 ddmi_tx_power;
	u16 ddmi_rx_power;
};

/* --- EN7570/EN7571 I2C transport (airoha_lddla_core.c) --- */
int lddla_rd(struct airoha_lddla *lddla, u16 addr, u8 *buf, int len);
int lddla_wr(struct airoha_lddla *lddla, u16 addr, const u8 *buf, int len);
int lddla_rd8(struct airoha_lddla *lddla, u16 addr, u8 *val);
int lddla_wr8(struct airoha_lddla *lddla, u16 addr, u8 val);
int lddla_update8(struct airoha_lddla *lddla, u16 addr, u8 mask, u8 set);
int lddla_rd16(struct airoha_lddla *lddla, u16 addr, u16 *val);
int lddla_rd32(struct airoha_lddla *lddla, u16 addr, u32 *val);
int lddla_lock(struct airoha_lddla *lddla);

/* --- shared Airoha BOB/calibration store (airoha_lddla_core.c) --- */
int lddla_bob_load(struct airoha_lddla *lddla);
u32 lddla_flash_read(struct airoha_lddla *lddla, u32 off);
void lddla_flash_defaults(struct airoha_lddla *lddla);

/* --- generic optical frontend bridge + Airoha debugfs --- */
int lddla_frontend_register(struct airoha_lddla *lddla);
void lddla_debugfs_init(struct airoha_lddla *lddla);
void lddla_debugfs_remove(struct airoha_lddla *lddla);

#endif /* _AIROHA_LDDLA_H */
