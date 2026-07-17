/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_PHY_AIROHA_LDDLA_H
#define _LINUX_PHY_AIROHA_LDDLA_H

#include <linux/bits.h>
#include <linux/errno.h>
#include <linux/kconfig.h>
#include <linux/types.h>

struct device;

#define AIROHA_LDDLA_TELEMETRY_F_TEMPERATURE	BIT(0)
#define AIROHA_LDDLA_TELEMETRY_F_VOLTAGE	BIT(1)
#define AIROHA_LDDLA_TELEMETRY_F_BIAS		BIT(2)
#define AIROHA_LDDLA_TELEMETRY_F_TX_POWER	BIT(3)
#define AIROHA_LDDLA_TELEMETRY_F_RX_POWER	BIT(4)
#define AIROHA_LDDLA_TELEMETRY_F_ALARMS	BIT(5)

/**
 * struct airoha_lddla_telemetry - live optical frontend measurements
 * @valid: AIROHA_LDDLA_TELEMETRY_F_* bitmap
 * @bosa_temperature_mc: BOSA temperature in milli-degrees Celsius
 * @voltage_uv: supply voltage in microvolts
 * @bias_ua: laser bias current in microamps
 * @tx_power_nw: transmitted optical power in nanowatts
 * @rx_power_nw: received optical power in nanowatts
 * @alarms: AIROHA_ALARM_* bitmap from the LDDLA driver
 */
struct airoha_lddla_telemetry {
	u32 valid;
	s32 bosa_temperature_mc;
	u32 voltage_uv;
	u32 bias_ua;
	u32 tx_power_nw;
	u32 rx_power_nw;
	u32 alarms;
};

#if IS_REACHABLE(CONFIG_AIROHA_LDDLA_PHY)
int airoha_lddla_get_telemetry(struct device *dev,
			       struct airoha_lddla_telemetry *telemetry);
int airoha_lddla_tx_rearm(struct device *dev);
#else
static inline int
airoha_lddla_get_telemetry(struct device *dev,
			   struct airoha_lddla_telemetry *telemetry)
{
	return -EOPNOTSUPP;
}

static inline int airoha_lddla_tx_rearm(struct device *dev)
{
	return -EOPNOTSUPP;
}
#endif

#endif /* _LINUX_PHY_AIROHA_LDDLA_H */
