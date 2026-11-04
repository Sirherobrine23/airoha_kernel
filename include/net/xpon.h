/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _NET_XPON_H
#define _NET_XPON_H

#include <linux/bitops.h>
#include <linux/errno.h>
#include <linux/log2.h>
#include <linux/types.h>
#include <linux/netdevice.h>
#include <uapi/linux/xpon.h>

struct device;
struct led_classdev;
struct net_device;
struct notifier_block;
struct xpon_device;
struct optical_frontend;

#define XPON_MODE_CAP(_mode)	((unsigned long)(_mode))

static inline bool xpon_mode_valid(u32 mode)
{
	return mode >= XPON_MODE_GPON && mode <= XPON_MODE_MAX &&
	       is_power_of_2(mode);
}

#define XPON_STATE_F_SIGNAL	BIT(0)
#define XPON_STATE_F_LOS	BIT(1)

/* XEPON line profiles are protocol settings, not Device Tree properties.
 * Values deliberately do not encode the Airoha SCU WAN_CONF register.
 */
enum xpon_xepon_profile {
	XPON_XEPON_10G_10G = 0,
	XPON_XEPON_10G_1G,
	XPON_XEPON_1G_1G,
};

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

static inline bool
xpon_endpoint_same_binding(const struct xpon_endpoint_config *a,
			   const struct xpon_endpoint_config *b)
{
	return a->id == b->id && a->wire_id == b->wire_id &&
	       a->scheduler_id == b->scheduler_id && a->link_id == b->link_id &&
	       a->type == b->type && a->channel == b->channel &&
	       a->queue == b->queue && a->enabled == b->enabled;
}

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

enum xpon_vlan_tag_mode {
	XPON_VLAN_SINGLE_TAG,
	XPON_VLAN_DOUBLE_TAG,
	XPON_VLAN_UNTAGGED,
};

/**
 * struct xpon_vlan_rule - match existing VLAN tags and modify the first tag
 * @tpid: first tag protocol to match, or zero for any VLAN protocol
 * @new_tpid: replacement first tag protocol, or zero to preserve it
 * @tci: host-order first tag TCI bits to match
 * @tci_mask: significant bits in @tci
 * @new_tci: replacement host-order first tag TCI bits
 * @new_tci_mask: bits to replace; all other bits are preserved
 * @inner_tpid: second tag protocol to match, or zero for any VLAN protocol
 * @inner_tci: host-order second tag TCI bits to match
 * @inner_tci_mask: significant bits in @inner_tci
 * @tag_mode: required tag count; zero defaults to a single tag
 * @drop: discard a matching frame instead of modifying it
 *
 * Modification preserves the tag count. With two tags, the second tag is
 * matched but never changed. Untagged rules can only discard frames.
 */
struct xpon_vlan_rule {
	__be16 tpid;
	__be16 new_tpid;
	u16 tci;
	u16 tci_mask;
	u16 new_tci;
	u16 new_tci_mask;
	__be16 inner_tpid;
	u16 inner_tci;
	u16 inner_tci_mask;
	u8 tag_mode;
	bool drop;
};

/**
 * struct xpon_vlan_config - bidirectional VLAN rewrite for one PON service
 * @cookie: stable rule identifier, independent of hardware table indices
 * @port_id: provider port identifier (normalized UNI entity for OMCI)
 * @wire_id: GEM port or LLID identifying the downstream service
 * @priority: rule precedence, lower values match first
 * @upstream: customer-to-network translation, before service classification
 * @downstream: network-to-customer translation, after wire-ID classification
 * @downstream_valid: apply @downstream; otherwise downstream is transparent
 */
struct xpon_vlan_config {
	u32 cookie;
	u16 port_id;
	u16 wire_id;
	u16 priority;
	struct xpon_vlan_rule upstream;
	struct xpon_vlan_rule downstream;
	bool downstream_valid;
};

/**
 * struct xpon_device_limits - limits for a single PON mode
 * @max_links: maximum number of LLIDs (zero for protocols without LLIDs)
 * @max_endpoints: maximum number of independently exposed endpoint netdevs
 * @max_queues: maximum number of hardware queues per endpoint
 * @report_sets: number of EPON MPCP REPORT sets
 *
 * These are effective per-mode limits, not the maximum across all modes.
 * get_limits() provides the limits before registration; the core caches them
 * and preallocates enough LLID storage for every advertised mode.
 * The callback must be side-effect free. An error for any advertised mode
 * prevents registration, so do not advertise an unimplemented mode.
 */
struct xpon_device_limits {
	u16 max_links;
	u16 max_endpoints;
	u8 max_queues;
	u8 report_sets;
};

/**
 * struct xpon_device_ops - xPON hardware and protocol lifecycle
 *
 * @get_limits: report hardware capacities for each advertised mode;
 *              called before hardware preparation, must be side-effect free,
 *              and may not access a live MAC/PHY.
 * @hw_prepare: initialize the initially selected mode during registration
 * @hw_pre_shutdown: stop protocol services, mask/synchronize MAC IRQs, disable
 *                   the optical TX and quiesce the old PHY/MAC/datapath
 * @set_mode: select WAN mux/optical path and fully start the requested mode;
 *            leave TX disabled until the new protocol is ready
 * @hw_fallback: recover a safe mode after either transition callback fails;
 *               report the mode actually recovered in @recovered
 * @get_mode: read the hardware-selected mode
 * @validate_vlans: optional read-only validation of the complete VLAN rule set
 * @replace_vlan: atomically add or replace a VLAN rewrite by cookie
 * @delete_vlan: remove both directions of a VLAN rewrite by cookie
 *
 * Mode changes run under the core mode_lock with the lower netdev stopped and
 * without logical endpoints. Callers with live netdev/OAM consumers must hold
 * RTNL (both sysfs and Generic Netlink entrypoints do so) to serialize their
 * lifetimes. A provider must keep MAC/PHY/OMCI/OAM teardown
 * and re-registration inside its callbacks. Callbacks must not recursively
 * request another mode change. Returning failure from hw_fallback leaves the
 * device faulted; it cannot be switched again without reinitialization.
 *
 * VLAN callbacks are an optional group: implement all three to opt in. They
 * run in sleepable context, serialized by the provisioning caller, without
 * mode_lock. Providers must synchronize with their datapath and quiesce the
 * provisioning caller before mode teardown. A failed replacement must leave
 * the old rule intact. Deleting an absent cookie may return -ENOENT.
 */
struct xpon_device_ops {
	int (*get_limits)(struct xpon_device *xpon, enum xpon_mode mode,
			  struct xpon_device_limits *limits);
	int (*hw_prepare)(struct xpon_device *xpon, enum xpon_mode mode);
	int (*hw_pre_shutdown)(struct xpon_device *xpon,
			       enum xpon_mode previous, enum xpon_mode next);
	int (*hw_fallback)(struct xpon_device *xpon,
			   enum xpon_mode previous, enum xpon_mode attempted,
			   enum xpon_mode *recovered);
	int (*set_mode)(struct xpon_device *xpon, enum xpon_mode mode);
	int (*get_mode)(struct xpon_device *xpon, enum xpon_mode *mode);
	int (*validate_vlans)(struct xpon_device *xpon,
			      const struct xpon_vlan_config *configs, size_t count);
	int (*replace_vlan)(struct xpon_device *xpon,
			    const struct xpon_vlan_config *config);
	int (*delete_vlan)(struct xpon_device *xpon, u32 cookie);
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

/* Each physical BOB/LDDLA may support a different set of PON modes. */
struct xpon_optical_path {
	struct device *dev;
	struct optical_frontend *frontend;
	unsigned long modes;
	/* Exactly one frontend is active for a shared xPON MAC. */
	bool active;
};

/*
 * Configuration is stored directly in the allocated xpon_device. The provider
 * may keep its hardware-specific settings in xpon_device_priv().
 * Setup may only be changed before registration. Limits are supplied by
 * ops->get_limits() and are committed automatically on mode transitions.
 */
struct xpon_device *xpon_device_alloc(struct device *parent, int priv_size);
void xpon_device_free(struct xpon_device *xpon);
int xpon_device_setup(struct xpon_device *xpon, struct net_device *netdev,
		      const struct xpon_device_ops *ops, void *priv,
		      enum xpon_mode mode, unsigned long modes);
int xpon_device_set_leds(struct xpon_device *xpon,
			 struct led_classdev *pon, struct led_classdev *los,
			 struct led_classdev *fiber);
int xpon_device_register_allocated(struct xpon_device *xpon);
void xpon_device_unregister(struct xpon_device *xpon);

struct device *xpon_device_dev(struct xpon_device *xpon);
struct net_device *xpon_device_netdev(struct xpon_device *xpon);
void *xpon_device_priv(struct xpon_device *xpon);
unsigned long xpon_device_modes(struct xpon_device *xpon);
enum xpon_mode xpon_device_mode(struct xpon_device *xpon);
/* Set before entering XEPON. A live XEPON PHY must be switched out first. */
enum xpon_xepon_profile xpon_device_xepon_profile(struct xpon_device *xpon);
int xpon_device_set_xepon_profile(struct xpon_device *xpon,
				  enum xpon_xepon_profile profile);
int xpon_device_get_limits(struct xpon_device *xpon,
			   struct xpon_device_limits *limits);
int xpon_device_get_hw_mode(struct xpon_device *xpon, enum xpon_mode *mode);
bool xpon_device_has_vlan_ops(struct xpon_device *xpon);
int xpon_device_validate_vlans(struct xpon_device *xpon,
			       const struct xpon_vlan_config *configs, size_t count);
int xpon_device_replace_vlan(struct xpon_device *xpon,
			     const struct xpon_vlan_config *config);
int xpon_device_delete_vlan(struct xpon_device *xpon, u32 cookie);
/* Borrowed active optical frontend, owned by the xPON core. */
struct optical_frontend *xpon_device_optical_frontend(struct xpon_device *xpon);
/* Borrowed candidate frontend; intended for use inside mode callbacks. */
struct optical_frontend *xpon_device_optical_frontend_for_mode(
				struct xpon_device *xpon, enum xpon_mode mode);
bool xpon_device_mode_faulted(struct xpon_device *xpon);
struct device *xpon_device_optical(struct xpon_device *xpon);

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

/* The caller must keep dev alive; the returned lower device is borrowed. */
#if IS_REACHABLE(CONFIG_XPON)
int xpon_endpoint_get_path(struct net_device *dev, struct net_device **lower,
			   struct xpon_endpoint_config *config);
#else
static inline int
xpon_endpoint_get_path(struct net_device *dev, struct net_device **lower,
		       struct xpon_endpoint_config *config)
{
	return -EOPNOTSUPP;
}
#endif

#endif /* _NET_XPON_H */
