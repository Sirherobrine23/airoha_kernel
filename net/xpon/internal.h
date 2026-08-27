/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _NET_XPON_INTERNAL_H
#define _NET_XPON_INTERNAL_H

#include <linux/device.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/notifier.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>
#include <net/xpon.h>

struct xpon_device {
	struct list_head list;
	struct device *parent;
	struct device *class_dev;
	struct net_device *netdev;
	struct device *optical;
	const struct xpon_device_ops *ops;
	void *priv;

	unsigned long modes;
	struct xpon_state state;
	spinlock_t state_lock;
	struct mutex mode_lock;
	bool mode_changing;

	unsigned long pending_events;
	struct xpon_state notify_old;
	struct work_struct notify_work;
	struct blocking_notifier_head notifier;

	struct led_classdev *pon_led;
	struct led_classdev *los_led;
	struct led_classdev *fiber_led;

	void *omci;
	void *oam;
};

extern struct class *xpon_class;

int xpon_sysfs_init(void);
void xpon_sysfs_exit(void);
int xpon_sysfs_register(struct xpon_device *xpon);
void xpon_sysfs_unregister(struct xpon_device *xpon);
void xpon_sysfs_notify(struct xpon_device *xpon, unsigned long changed);

void xpon_leds_update(struct xpon_device *xpon,
		      const struct xpon_state *state);

int xpon_genl_init(void);
void xpon_genl_exit(void);
void xpon_genl_notify(struct xpon_device *xpon, unsigned long changed);

struct xpon_device *xpon_device_find_by_ifindex(u32 ifindex);
void xpon_device_put(struct xpon_device *xpon);

#endif /* _NET_XPON_INTERNAL_H */
