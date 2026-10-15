/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _NET_XPON_INTERNAL_H
#define _NET_XPON_INTERNAL_H

#include <linux/device.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/notifier.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>
#include <linux/kref.h>
#include <linux/of.h>
#include <linux/xarray.h>
#include <net/xpon.h>

struct xpon_uni {
	struct device_node *node;
};

struct xpon_endpoint {
	struct xpon_device *xpon;
	struct net_device *netdev;
	struct xpon_endpoint_config config;
};

struct xpon_endpoint_request {
	struct xpon_endpoint_config config;
	bool remove;
};

struct xpon_device {
	struct list_head list;
	struct kref ref;
	bool unregistering;
	u16 max_links;
	u16 max_endpoints;
	u8 max_queues;
	u8 report_sets;
	u8 onu_type;
	u8 uni_count;
	struct xpon_uni *unis;
	struct xpon_link_info *links;
	struct xarray endpoints;
	struct xarray rx_endpoints;
	struct xarray pending_endpoints;
	/* Serialize desired endpoint updates from protocol callbacks. */
	struct mutex pending_lock;
	struct work_struct endpoint_work;
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
	u8 pon_led_mode;
	u8 los_led_mode;

	void *omci;
	void *oam;
};

extern struct class *xpon_class;

int xpon_sysfs_init(void);
void xpon_sysfs_exit(void);
int xpon_sysfs_register(struct xpon_device *xpon);
void xpon_sysfs_unregister(struct xpon_device *xpon);
void xpon_sysfs_notify(struct xpon_device *xpon, unsigned long changed);

int xpon_leds_register(struct xpon_device *xpon);
void xpon_leds_update(struct xpon_device *xpon, const struct xpon_state *state);

int xpon_genl_init(void);
void xpon_genl_exit(void);
void xpon_genl_notify(struct xpon_device *xpon, unsigned long changed);

struct xpon_device *xpon_device_find_by_ifindex(u32 ifindex);
void xpon_device_put(struct xpon_device *xpon);

int xpon_oam_transmit(struct xpon_device *xpon, u32 id, const void *frame,
		      size_t len);
int xpon_unis_init(struct xpon_device *xpon);
void xpon_unis_cleanup(struct xpon_device *xpon);
struct net_device *xpon_uni_get_netdev(struct xpon_device *xpon, u32 id);
void xpon_endpoint_work_init(struct xpon_device *xpon);
void xpon_endpoint_work_cleanup(struct xpon_device *xpon);
void xpon_endpoints_cleanup(struct xpon_device *xpon);
void xpon_endpoints_update_carrier(struct xpon_device *xpon);
void xpon_genl_oam_receive(struct xpon_device *xpon, u32 id,
			   struct sk_buff *skb);

#endif /* _NET_XPON_INTERNAL_H */
