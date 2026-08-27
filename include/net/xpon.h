/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _NET_XPON_H
#define _NET_XPON_H

#include <linux/bitops.h>
#include <linux/types.h>
#include <uapi/linux/xpon.h>

struct device;
struct led_classdev;
struct net_device;
struct notifier_block;
struct xpon_device;

#define XPON_MODE_CAP(_mode)	BIT(_mode)

#define XPON_STATE_F_SIGNAL	BIT(0)
#define XPON_STATE_F_LOS	BIT(1)

struct xpon_state {
	enum xpon_mode mode;
	enum xpon_registration_state registration;
	u32 valid;
	bool carrier;
	bool signal_detect;
	bool los;
};

enum xpon_event {
	XPON_EVENT_MODE		= BIT(0),
	XPON_EVENT_REGISTRATION	= BIT(1),
	XPON_EVENT_CARRIER	= BIT(2),
	XPON_EVENT_SIGNAL	= BIT(3),
	XPON_EVENT_LOS		= BIT(4),
	XPON_EVENT_OPTICAL	= BIT(5),
	XPON_EVENT_LLID		= BIT(6),
	XPON_EVENT_OMCI		= BIT(7),
	XPON_EVENT_OAM		= BIT(8),
};

struct xpon_notifier_info {
	struct xpon_device *xpon;
	unsigned long changed;
	struct xpon_state old;
	struct xpon_state new;
};

struct xpon_device_ops {
	int (*set_mode)(struct xpon_device *xpon, enum xpon_mode mode);
};

struct xpon_device_desc {
	struct net_device *netdev;
	struct device *optical;
	const struct xpon_device_ops *ops;
	struct led_classdev *pon_led;
	struct led_classdev *los_led;
	struct led_classdev *fiber_led;
	unsigned long modes;
	enum xpon_mode mode;
	void *priv;
};

struct xpon_device *
xpon_device_register(struct device *parent,
		     const struct xpon_device_desc *desc);
void xpon_device_unregister(struct xpon_device *xpon);

struct device *xpon_device_dev(struct xpon_device *xpon);
struct net_device *xpon_device_netdev(struct xpon_device *xpon);
void *xpon_device_priv(struct xpon_device *xpon);
unsigned long xpon_device_modes(struct xpon_device *xpon);
enum xpon_mode xpon_device_mode(struct xpon_device *xpon);

int xpon_device_set_mode(struct xpon_device *xpon, enum xpon_mode mode);
void xpon_device_report_registration(struct xpon_device *xpon,
				     enum xpon_registration_state state);
void xpon_device_report_carrier(struct xpon_device *xpon, bool carrier);
void xpon_device_report_optical(struct xpon_device *xpon,
				bool signal_detect, bool los);
void xpon_device_report_event(struct xpon_device *xpon,
			      unsigned long event);

int xpon_device_register_notifier(struct xpon_device *xpon,
				  struct notifier_block *nb);
int xpon_device_unregister_notifier(struct xpon_device *xpon,
				    struct notifier_block *nb);

#endif /* _NET_XPON_H */
