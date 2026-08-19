// SPDX-License-Identifier: GPL-2.0-only
/* Generic hwmon bridge for optical frontend telemetry. */

#include <linux/device.h>
#include <linux/hwmon.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/optical_frontend.h>

static bool optical_frontend_hwmon_has_threshold(
	const struct optical_frontend_desc *desc,
	enum hwmon_sensor_types type, int channel)
{
	const struct optical_frontend_thresholds *t = desc->thresholds;
	u32 flag;

	if (!t)
		return false;

	switch (type) {
	case hwmon_temp:
		flag = OPTICAL_FRONTEND_THRESHOLD_F_TEMPERATURE;
		break;
	case hwmon_in:
		flag = OPTICAL_FRONTEND_THRESHOLD_F_VOLTAGE;
		break;
	case hwmon_curr:
		flag = OPTICAL_FRONTEND_THRESHOLD_F_BIAS;
		break;
	case hwmon_power:
		flag = channel ? OPTICAL_FRONTEND_THRESHOLD_F_RX_POWER :
				 OPTICAL_FRONTEND_THRESHOLD_F_TX_POWER;
		break;
	default:
		return false;
	}

	return t->valid & flag;
}

static u32 optical_frontend_hwmon_cap(enum hwmon_sensor_types type, int channel)
{
	switch (type) {
	case hwmon_temp:
		return OPTICAL_FRONTEND_CAP_TEMPERATURE;
	case hwmon_in:
		return OPTICAL_FRONTEND_CAP_VOLTAGE;
	case hwmon_curr:
		return OPTICAL_FRONTEND_CAP_BIAS;
	case hwmon_power:
		return channel ? OPTICAL_FRONTEND_CAP_RX_POWER :
				 OPTICAL_FRONTEND_CAP_TX_POWER;
	default:
		return 0;
	}
}

static bool optical_frontend_hwmon_is_input(enum hwmon_sensor_types type,
					    u32 attr)
{
	return (type == hwmon_temp && attr == hwmon_temp_input) ||
	       (type == hwmon_in && attr == hwmon_in_input) ||
	       (type == hwmon_curr && attr == hwmon_curr_input) ||
	       (type == hwmon_power && attr == hwmon_power_input);
}

static bool optical_frontend_hwmon_is_threshold(enum hwmon_sensor_types type,
						u32 attr)
{
	return (type == hwmon_temp &&
		(attr == hwmon_temp_min || attr == hwmon_temp_max)) ||
	       (type == hwmon_in &&
		(attr == hwmon_in_min || attr == hwmon_in_max)) ||
	       (type == hwmon_curr &&
		(attr == hwmon_curr_min || attr == hwmon_curr_max)) ||
	       (type == hwmon_power &&
		(attr == hwmon_power_min || attr == hwmon_power_max));
}

static bool optical_frontend_hwmon_is_alarm(enum hwmon_sensor_types type,
					    u32 attr)
{
	return (type == hwmon_temp &&
		(attr == hwmon_temp_min_alarm || attr == hwmon_temp_max_alarm)) ||
	       (type == hwmon_in &&
		(attr == hwmon_in_min_alarm || attr == hwmon_in_max_alarm)) ||
	       (type == hwmon_curr &&
		(attr == hwmon_curr_min_alarm || attr == hwmon_curr_max_alarm)) ||
	       (type == hwmon_power &&
		(attr == hwmon_power_min_alarm || attr == hwmon_power_max_alarm));
}

static umode_t optical_frontend_hwmon_is_visible(const void *data,
						enum hwmon_sensor_types type,
						u32 attr, int channel)
{
	struct optical_frontend *frontend = (void *)data;
	const struct optical_frontend_desc *desc =
		optical_frontend_get_desc(frontend);
	u32 cap;

	if (!desc)
		return 0;

	cap = optical_frontend_hwmon_cap(type, channel);
	if (!cap || !(desc->capabilities & cap))
		return 0;

	if (optical_frontend_hwmon_is_input(type, attr))
		return 0444;

	if (optical_frontend_hwmon_is_threshold(type, attr) &&
	    optical_frontend_hwmon_has_threshold(desc, type, channel))
		return 0444;

	if (optical_frontend_hwmon_is_alarm(type, attr) &&
	    (desc->capabilities & OPTICAL_FRONTEND_CAP_ALARMS))
		return 0444;

	return 0;
}

static int optical_frontend_hwmon_read_threshold(
	const struct optical_frontend_desc *desc,
	enum hwmon_sensor_types type, u32 attr, int channel, long *val)
{
	const struct optical_frontend_thresholds *t = desc->thresholds;

	if (!optical_frontend_hwmon_has_threshold(desc, type, channel))
		return -ENODATA;

	switch (type) {
	case hwmon_temp:
		*val = attr == hwmon_temp_min ? t->temperature_low_mc :
						 t->temperature_high_mc;
		return 0;
	case hwmon_in:
		*val = DIV_ROUND_CLOSEST(attr == hwmon_in_min ? t->voltage_low_uv :
							 t->voltage_high_uv, 1000);
		return 0;
	case hwmon_curr:
		*val = DIV_ROUND_CLOSEST(attr == hwmon_curr_min ? t->bias_low_ua :
							   t->bias_high_ua, 1000);
		return 0;
	case hwmon_power:
		if (channel)
			*val = DIV_ROUND_CLOSEST(attr == hwmon_power_min ?
						 t->rx_power_low_nw :
						 t->rx_power_high_nw, 1000);
		else
			*val = DIV_ROUND_CLOSEST(attr == hwmon_power_min ?
						 t->tx_power_low_nw :
						 t->tx_power_high_nw, 1000);
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

static int optical_frontend_hwmon_read_alarm(
	const struct optical_frontend_telemetry *t,
	enum hwmon_sensor_types type, u32 attr, int channel, long *val)
{
	u32 alarm;

	if (!(t->valid & OPTICAL_FRONTEND_TELEMETRY_F_ALARMS))
		return -ENODATA;

	switch (type) {
	case hwmon_temp:
		alarm = attr == hwmon_temp_min_alarm ?
			OPTICAL_FRONTEND_ALARM_LOW_TEMP :
			OPTICAL_FRONTEND_ALARM_HIGH_TEMP;
		break;
	case hwmon_in:
		alarm = attr == hwmon_in_min_alarm ?
			OPTICAL_FRONTEND_ALARM_LOW_VOLTAGE :
			OPTICAL_FRONTEND_ALARM_HIGH_VOLTAGE;
		break;
	case hwmon_curr:
		alarm = attr == hwmon_curr_min_alarm ?
			OPTICAL_FRONTEND_ALARM_TX_LOW_BIAS :
			OPTICAL_FRONTEND_ALARM_TX_HIGH_BIAS;
		break;
	case hwmon_power:
		if (channel)
			alarm = attr == hwmon_power_min_alarm ?
				OPTICAL_FRONTEND_ALARM_RX_LOW_POWER :
				OPTICAL_FRONTEND_ALARM_RX_HIGH_POWER;
		else
			alarm = attr == hwmon_power_min_alarm ?
				OPTICAL_FRONTEND_ALARM_TX_LOW_POWER :
				OPTICAL_FRONTEND_ALARM_TX_HIGH_POWER;
		break;
	default:
		return -EOPNOTSUPP;
	}

	*val = !!(t->alarms & alarm);
	return 0;
}

static int optical_frontend_hwmon_read(struct device *dev,
				       enum hwmon_sensor_types type,
				       u32 attr, int channel, long *val)
{
	struct optical_frontend *frontend = dev_get_drvdata(dev);
	const struct optical_frontend_desc *desc =
		optical_frontend_get_desc(frontend);
	struct optical_frontend_telemetry telemetry = {};
	int ret;

	if (!desc)
		return -ENODEV;

	if (optical_frontend_hwmon_is_threshold(type, attr))
		return optical_frontend_hwmon_read_threshold(desc, type, attr,
						     channel, val);

	ret = optical_frontend_get_telemetry(frontend, &telemetry);
	if (ret)
		return ret;

	if (optical_frontend_hwmon_is_alarm(type, attr))
		return optical_frontend_hwmon_read_alarm(&telemetry, type, attr,
						 channel, val);

	switch (type) {
	case hwmon_temp:
		if (!(telemetry.valid & OPTICAL_FRONTEND_TELEMETRY_F_TEMPERATURE))
			return -ENODATA;
		*val = telemetry.temperature_mc;
		return 0;
	case hwmon_in:
		if (!(telemetry.valid & OPTICAL_FRONTEND_TELEMETRY_F_VOLTAGE))
			return -ENODATA;
		*val = DIV_ROUND_CLOSEST(telemetry.voltage_uv, 1000);
		return 0;
	case hwmon_curr:
		if (!(telemetry.valid & OPTICAL_FRONTEND_TELEMETRY_F_BIAS))
			return -ENODATA;
		*val = DIV_ROUND_CLOSEST(telemetry.bias_ua, 1000);
		return 0;
	case hwmon_power:
		if (channel) {
			if (!(telemetry.valid & OPTICAL_FRONTEND_TELEMETRY_F_RX_POWER))
				return -ENODATA;
			*val = DIV_ROUND_CLOSEST(telemetry.rx_power_nw, 1000);
		} else {
			if (!(telemetry.valid & OPTICAL_FRONTEND_TELEMETRY_F_TX_POWER))
				return -ENODATA;
			*val = DIV_ROUND_CLOSEST(telemetry.tx_power_nw, 1000);
		}
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

static const struct hwmon_channel_info * const optical_frontend_hwmon_info[] = {
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

static const struct hwmon_ops optical_frontend_hwmon_ops = {
	.is_visible = optical_frontend_hwmon_is_visible,
	.read = optical_frontend_hwmon_read,
};

static const struct hwmon_chip_info optical_frontend_hwmon_chip_info = {
	.ops = &optical_frontend_hwmon_ops,
	.info = optical_frontend_hwmon_info,
};

int devm_optical_frontend_hwmon_register(struct optical_frontend *frontend)
{
	const struct optical_frontend_desc *desc;
	struct device *provider;
	struct device *hwmon;

	if (!frontend)
		return -EINVAL;

	desc = optical_frontend_get_desc(frontend);
	provider = optical_frontend_get_provider(frontend);
	if (!desc || !provider)
		return -EINVAL;

	hwmon = devm_hwmon_device_register_with_info(provider, desc->name,
						     frontend,
						     &optical_frontend_hwmon_chip_info,
						     NULL);
	return PTR_ERR_OR_ZERO(hwmon);
}
EXPORT_SYMBOL_GPL(devm_optical_frontend_hwmon_register);

MODULE_DESCRIPTION("Generic optical frontend hwmon bridge");
MODULE_LICENSE("GPL");
