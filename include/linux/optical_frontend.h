/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_OPTICAL_FRONTEND_H
#define _LINUX_OPTICAL_FRONTEND_H

#include <linux/bits.h>
#include <linux/errno.h>
#include <linux/err.h>
#include <linux/kconfig.h>
#include <linux/types.h>

struct attribute_group;
struct device;
struct fwnode_handle;
struct optical_frontend;

#define OPTICAL_FRONTEND_CAP_TEMPERATURE	BIT(0)
#define OPTICAL_FRONTEND_CAP_VOLTAGE		BIT(1)
#define OPTICAL_FRONTEND_CAP_BIAS		BIT(2)
#define OPTICAL_FRONTEND_CAP_TX_POWER		BIT(3)
#define OPTICAL_FRONTEND_CAP_RX_POWER		BIT(4)
#define OPTICAL_FRONTEND_CAP_ALARMS		BIT(5)
#define OPTICAL_FRONTEND_CAP_TX_REARM		BIT(6)
#define OPTICAL_FRONTEND_CAP_TX_DISABLE	BIT(7)
#define OPTICAL_FRONTEND_CAP_TX_FAULT		BIT(8)
#define OPTICAL_FRONTEND_CAP_RX_LOS		BIT(9)
#define OPTICAL_FRONTEND_CAP_BURST_TX		BIT(10)
#define OPTICAL_FRONTEND_CAP_APD		BIT(11)

#define OPTICAL_FRONTEND_TELEMETRY_F_TEMPERATURE	BIT(0)
#define OPTICAL_FRONTEND_TELEMETRY_F_VOLTAGE		BIT(1)
#define OPTICAL_FRONTEND_TELEMETRY_F_BIAS		BIT(2)
#define OPTICAL_FRONTEND_TELEMETRY_F_TX_POWER		BIT(3)
#define OPTICAL_FRONTEND_TELEMETRY_F_RX_POWER		BIT(4)
#define OPTICAL_FRONTEND_TELEMETRY_F_ALARMS		BIT(5)

#define OPTICAL_FRONTEND_STATE_F_PRESENT	BIT(0)
#define OPTICAL_FRONTEND_STATE_F_READY		BIT(1)
#define OPTICAL_FRONTEND_STATE_F_RX_LOS	BIT(2)
#define OPTICAL_FRONTEND_STATE_F_TX_FAULT	BIT(3)
#define OPTICAL_FRONTEND_STATE_F_TX_ENABLED	BIT(4)

#define OPTICAL_FRONTEND_ALARM_TX_LOW_POWER	BIT(0)
#define OPTICAL_FRONTEND_ALARM_TX_HIGH_POWER	BIT(1)
#define OPTICAL_FRONTEND_ALARM_TX_LOW_BIAS	BIT(2)
#define OPTICAL_FRONTEND_ALARM_TX_HIGH_BIAS	BIT(3)
#define OPTICAL_FRONTEND_ALARM_RX_LOW_POWER	BIT(4)
#define OPTICAL_FRONTEND_ALARM_RX_HIGH_POWER	BIT(5)
#define OPTICAL_FRONTEND_ALARM_LOW_VOLTAGE	BIT(6)
#define OPTICAL_FRONTEND_ALARM_HIGH_VOLTAGE	BIT(7)
#define OPTICAL_FRONTEND_ALARM_LOW_TEMP	BIT(8)
#define OPTICAL_FRONTEND_ALARM_HIGH_TEMP	BIT(9)

enum optical_frontend_protocol {
	OPTICAL_FRONTEND_PROTO_UNSPEC,
	OPTICAL_FRONTEND_PROTO_ETHERNET,
	OPTICAL_FRONTEND_PROTO_EPON,
	OPTICAL_FRONTEND_PROTO_GPON,
	OPTICAL_FRONTEND_PROTO_XGPON,
	OPTICAL_FRONTEND_PROTO_XGSPON,
	OPTICAL_FRONTEND_PROTO_NGPON2,
};

#define OPTICAL_FRONTEND_MODE_BURST_TX	BIT(0)
#define OPTICAL_FRONTEND_MODE_CONT_TX	BIT(1)
#define OPTICAL_FRONTEND_MODE_BURST_RX	BIT(2)

/**
 * struct optical_frontend_mode - optical line operating mode
 * @protocol: line protocol
 * @tx_rate: nominal transmitter bit rate in bit/s
 * @rx_rate: nominal receiver bit rate in bit/s
 * @flags: OPTICAL_FRONTEND_MODE_* bitmap
 */
struct optical_frontend_mode {
	enum optical_frontend_protocol protocol;
	u64 tx_rate;
	u64 rx_rate;
	u32 flags;
};

/**
 * struct optical_frontend_telemetry - normalized live optical measurements
 * @valid: OPTICAL_FRONTEND_TELEMETRY_F_* bitmap
 * @temperature_mc: frontend/BOSA temperature in milli-degrees Celsius
 * @voltage_uv: supply voltage in microvolts
 * @bias_ua: laser bias current in microamps
 * @tx_power_nw: transmitted optical power in nanowatts
 * @rx_power_nw: received optical power in nanowatts
 * @alarms: OPTICAL_FRONTEND_ALARM_* bitmap
 */
struct optical_frontend_telemetry {
	u32 valid;
	s32 temperature_mc;
	u32 voltage_uv;
	u32 bias_ua;
	u32 tx_power_nw;
	u32 rx_power_nw;
	u32 alarms;
};

/**
 * struct optical_frontend_state - normalized frontend state
 * @valid: OPTICAL_FRONTEND_STATE_F_* bitmap
 * @present: frontend is physically present/responding
 * @ready: frontend is configured and ready for normal operation
 * @rx_los: receiver loss-of-signal indication
 * @tx_fault: transmitter fault indication
 * @tx_enabled: transmitter is enabled
 */
struct optical_frontend_state {
	u32 valid;
	bool present;
	bool ready;
	bool rx_los;
	bool tx_fault;
	bool tx_enabled;
};

#define OPTICAL_FRONTEND_THRESHOLD_F_TEMPERATURE	BIT(0)
#define OPTICAL_FRONTEND_THRESHOLD_F_VOLTAGE		BIT(1)
#define OPTICAL_FRONTEND_THRESHOLD_F_BIAS		BIT(2)
#define OPTICAL_FRONTEND_THRESHOLD_F_TX_POWER		BIT(3)
#define OPTICAL_FRONTEND_THRESHOLD_F_RX_POWER		BIT(4)

/**
 * struct optical_frontend_thresholds - normalized alarm thresholds
 * @valid: OPTICAL_FRONTEND_THRESHOLD_F_* bitmap
 * @temperature_low_mc: low temperature threshold in milli-degrees Celsius
 * @temperature_high_mc: high temperature threshold in milli-degrees Celsius
 * @voltage_low_uv: low supply-voltage threshold in microvolts
 * @voltage_high_uv: high supply-voltage threshold in microvolts
 * @bias_low_ua: low laser-bias threshold in microamps
 * @bias_high_ua: high laser-bias threshold in microamps
 * @tx_power_low_nw: low Tx optical-power threshold in nanowatts
 * @tx_power_high_nw: high Tx optical-power threshold in nanowatts
 * @rx_power_low_nw: low Rx optical-power threshold in nanowatts
 * @rx_power_high_nw: high Rx optical-power threshold in nanowatts
 */
struct optical_frontend_thresholds {
	u32 valid;
	s32 temperature_low_mc;
	s32 temperature_high_mc;
	u32 voltage_low_uv;
	u32 voltage_high_uv;
	u32 bias_low_ua;
	u32 bias_high_ua;
	u32 tx_power_low_nw;
	u32 tx_power_high_nw;
	u32 rx_power_low_nw;
	u32 rx_power_high_nw;
};

/**
 * struct optical_frontend_desc - immutable provider description
 * @name: short driver/model name used by hwmon and diagnostics
 * @type: frontend type string exposed through sysfs (for example "lddla")
 * @vendor_name: vendor string for SFF compatibility
 * @vendor_oui: vendor OUI for SFF compatibility
 * @part_number: part number for SFF compatibility
 * @serial: optional component serial number
 * @date_code: optional SFF-style date code
 * @capabilities: OPTICAL_FRONTEND_CAP_* bitmap
 * @protocols: BIT(OPTICAL_FRONTEND_PROTO_*) bitmap
 * @thresholds: optional normalized alarm thresholds
 * @telemetry_cache_ms: core telemetry cache lifetime; zero disables caching
 * @groups: optional provider-specific sysfs groups on the frontend class device
 */
struct optical_frontend_desc {
	const char *name;
	const char *type;
	const char *vendor_name;
	u8 vendor_oui[3];
	const char *part_number;
	const char *serial;
	const char *date_code;
	u32 capabilities;
	u32 protocols;
	const struct optical_frontend_thresholds *thresholds;
	u32 telemetry_cache_ms;
	const struct attribute_group **groups;
};

/**
 * struct optical_frontend_ops - provider operations
 * @set_mode: configure protocol/rates; optional
 * @tx_enable: prepare/enable or disable the optical transmitter; optional
 * @tx_rearm: rearm a transmitter safety/fault latch; optional
 * @get_state: retrieve normalized frontend state; optional
 * @get_telemetry: retrieve normalized live measurements; optional
 *
 * The core serializes calls into this table. Providers may additionally use
 * their own lock for asynchronous workers which access the same hardware.
 */
struct optical_frontend_ops {
	int (*set_mode)(struct optical_frontend *frontend,
			const struct optical_frontend_mode *mode);
	int (*tx_enable)(struct optical_frontend *frontend, bool enable);
	int (*tx_rearm)(struct optical_frontend *frontend);
	int (*get_state)(struct optical_frontend *frontend,
			 struct optical_frontend_state *state);
	int (*get_telemetry)(struct optical_frontend *frontend,
			     struct optical_frontend_telemetry *telemetry);
};

#if IS_REACHABLE(CONFIG_OPTICAL_FRONTEND)
struct optical_frontend *
devm_optical_frontend_register(struct device *dev,
			       const struct optical_frontend_desc *desc,
			       const struct optical_frontend_ops *ops,
			       void *drvdata);

struct optical_frontend *
devm_optical_frontend_get_optional(struct device *dev, const char *name);
struct optical_frontend *
devm_optical_frontend_get_by_fwnode(struct device *dev,
				    const struct fwnode_handle *fwnode);

void *optical_frontend_get_drvdata(struct optical_frontend *frontend);
struct optical_frontend *optical_frontend_from_dev(struct device *dev);
struct device *optical_frontend_get_device(struct optical_frontend *frontend);
struct device *optical_frontend_get_provider(struct optical_frontend *frontend);
const struct optical_frontend_desc *
optical_frontend_get_desc(struct optical_frontend *frontend);

int optical_frontend_set_mode(struct optical_frontend *frontend,
			      const struct optical_frontend_mode *mode);
int optical_frontend_get_mode(struct optical_frontend *frontend,
			      struct optical_frontend_mode *mode);
int optical_frontend_tx_enable(struct optical_frontend *frontend, bool enable);
int optical_frontend_tx_rearm(struct optical_frontend *frontend);
int optical_frontend_get_state(struct optical_frontend *frontend,
			       struct optical_frontend_state *state);
int optical_frontend_get_telemetry(struct optical_frontend *frontend,
				   struct optical_frontend_telemetry *telemetry);
void optical_frontend_invalidate_telemetry(struct optical_frontend *frontend);
#else
static inline struct optical_frontend *
devm_optical_frontend_register(struct device *dev,
			       const struct optical_frontend_desc *desc,
			       const struct optical_frontend_ops *ops,
			       void *drvdata)
{
	return ERR_PTR(-EOPNOTSUPP);
}

static inline struct optical_frontend *
devm_optical_frontend_get_optional(struct device *dev, const char *name)
{
	return NULL;
}

static inline struct optical_frontend *
devm_optical_frontend_get_by_fwnode(struct device *dev,
				    const struct fwnode_handle *fwnode)
{
	return ERR_PTR(-EOPNOTSUPP);
}

static inline void *optical_frontend_get_drvdata(struct optical_frontend *frontend)
{
	return NULL;
}

static inline struct optical_frontend *
optical_frontend_from_dev(struct device *dev)
{
	return NULL;
}

static inline struct device *
optical_frontend_get_device(struct optical_frontend *frontend)
{
	return NULL;
}

static inline struct device *
optical_frontend_get_provider(struct optical_frontend *frontend)
{
	return NULL;
}

static inline const struct optical_frontend_desc *
optical_frontend_get_desc(struct optical_frontend *frontend)
{
	return NULL;
}

static inline int
optical_frontend_set_mode(struct optical_frontend *frontend,
			  const struct optical_frontend_mode *mode)
{
	return -EOPNOTSUPP;
}

static inline int
optical_frontend_get_mode(struct optical_frontend *frontend,
			  struct optical_frontend_mode *mode)
{
	return -EOPNOTSUPP;
}

static inline int
optical_frontend_tx_enable(struct optical_frontend *frontend, bool enable)
{
	return -EOPNOTSUPP;
}

static inline int optical_frontend_tx_rearm(struct optical_frontend *frontend)
{
	return -EOPNOTSUPP;
}

static inline int
optical_frontend_get_state(struct optical_frontend *frontend,
			   struct optical_frontend_state *state)
{
	return -EOPNOTSUPP;
}

static inline int
optical_frontend_get_telemetry(struct optical_frontend *frontend,
			       struct optical_frontend_telemetry *telemetry)
{
	return -EOPNOTSUPP;
}

static inline void
optical_frontend_invalidate_telemetry(struct optical_frontend *frontend)
{
}

#endif

#if IS_REACHABLE(CONFIG_OPTICAL_FRONTEND_HWMON)
int devm_optical_frontend_hwmon_register(struct optical_frontend *frontend);
#else
static inline int
devm_optical_frontend_hwmon_register(struct optical_frontend *frontend)
{
	return 0;
}
#endif

#endif /* _LINUX_OPTICAL_FRONTEND_H */
