/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _NET_XPON_H
#define _NET_XPON_H

#include <linux/bitops.h>
#include <linux/log2.h>
#include <linux/types.h>
#include <linux/netdevice.h>
#include <uapi/linux/xpon.h>

struct device;
struct led_classdev;
struct net_device;
struct notifier_block;
struct xpon_device;

#define XPON_MODE_CAP(_mode)	((unsigned long)(_mode))

static inline bool xpon_mode_valid(u32 mode)
{
	return mode >= XPON_MODE_GPON && mode <= XPON_MODE_MAX &&
	       is_power_of_2(mode);
}

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
	XPON_EVENT_ENDPOINT	= BIT(9),
};

struct xpon_notifier_info {
	struct xpon_device *xpon;
	unsigned long changed;
	struct xpon_state old;
	struct xpon_state new;
};

/* Local object IDs never encode a hardware channel or an OMCI entity ID. */
struct xpon_endpoint_config {
	u32 id;
	u16 wire_id;
	u16 scheduler_id;
	u32 link_id;
	u8 type;
	u8 channel;
	u8 queue;
	bool enabled;
};

struct xpon_link_info {
	u16 llid;
	u8 state;
	bool enabled;
	u64 rx_oam;
	u64 tx_oam;
};

/* Thresholds are transmission time in 16 ns MPCP time quanta. */
struct xpon_report_config {
	u32 threshold_tq;
	u8 sets;
	u8 set;
	u8 queue;
};

struct xpon_device_ops {
	int (*set_mode)(struct xpon_device *xpon, enum xpon_mode mode);
	/* NETDEV_TX_BUSY leaves the skb with the caller. */
	netdev_tx_t (*xmit)(struct xpon_device *xpon, struct sk_buff *skb,
			    const struct xpon_endpoint_config *config);
	int (*oam_xmit)(struct xpon_device *xpon, struct sk_buff *skb, u32 id);
	int (*resolve_endpoint)(struct xpon_device *xpon,
				struct xpon_endpoint_config *config);
	int (*set_link)(struct xpon_device *xpon, u32 id, bool enable);
	int (*get_report)(struct xpon_device *xpon, u32 id,
			  struct xpon_report_config *config);
	int (*set_report)(struct xpon_device *xpon, u32 id,
			  const struct xpon_report_config *config);
};

struct xpon_device_desc {
	struct net_device *netdev;
	struct device *optical;
	const struct xpon_device_ops *ops;
	struct led_classdev *pon_led;
	struct led_classdev *los_led;
	struct led_classdev *fiber_led;
	unsigned long modes;
	u16 max_links;
	u16 max_endpoints;
	u8 max_queues;
	u8 report_sets;
	enum xpon_mode mode;
	void *priv;
};

struct xpon_device *xpon_device_register(struct device *parent,
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
void xpon_device_report_optical(struct xpon_device *xpon, bool signal_detect,
				bool los);
void xpon_device_report_event(struct xpon_device *xpon, unsigned long event);

int xpon_device_register_notifier(struct xpon_device *xpon,
				  struct notifier_block *nb);
int xpon_device_unregister_notifier(struct xpon_device *xpon,
				    struct notifier_block *nb);

unsigned int xpon_device_uni_count(struct xpon_device *xpon);
enum xpon_onu_type xpon_device_onu_type(struct xpon_device *xpon);
int xpon_uni_set_admin(struct xpon_device *xpon, u32 id, bool enabled);

/* Providers must quiesce RX before unregistering the xPON device. */
void xpon_link_report(struct xpon_device *xpon, u32 id, u16 llid, u8 state,
		      bool enabled);
/* Both helpers require RTNL. Updating a binding preserves the netdev. */
int xpon_endpoint_set(struct xpon_device *xpon,
		      const struct xpon_endpoint_config *config);
int xpon_endpoint_delete(struct xpon_device *xpon, u32 id);
int xpon_endpoint_get_config(struct xpon_device *xpon, u32 id,
			     struct xpon_endpoint_config *config);
/* Coalesces protocol-driven updates without taking RTNL in the producer. */
int xpon_endpoint_queue(struct xpon_device *xpon,
			const struct xpon_endpoint_config *config, bool remove);
/* Consumes a complete Ethernet frame only when a matching endpoint exists. */
bool xpon_endpoint_receive(struct xpon_device *xpon, u8 type, u16 wire_id,
			   struct sk_buff *skb);

#endif /* _NET_XPON_H */
