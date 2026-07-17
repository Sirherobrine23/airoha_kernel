/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _NET_OMCI_H
#define _NET_OMCI_H

#include <linux/types.h>
#include <uapi/linux/omci.h>

struct device;
struct omci_device;
struct sk_buff;

/**
 * struct omci_device_ops - hardware transport and provisioning operations
 * @xmit: transmit an OMCI PDU; consumes @skb only on success
 * @set_tcont: configure a T-CONT mapping
 * @set_gem_port: configure a GEM port
 * @set_uni: enable or disable a UNI
 */
struct omci_device_ops {
	int (*xmit)(struct omci_device *odev, struct sk_buff *skb,
		    u16 gem_port_id);
	int (*set_tcont)(struct omci_device *odev, u16 entity_id,
			 u16 alloc_id, bool valid);
	int (*set_gem_port)(struct omci_device *odev, u16 entity_id,
			    u16 gem_port_id, u16 tcont_entity_id,
			    bool valid, bool encrypted);
	int (*set_uni)(struct omci_device *odev, u16 entity_id, bool enable);
};

struct omci_device *
omci_device_register(struct device *parent, u32 ifindex, u32 capabilities,
		     const struct omci_device_ops *ops, void *priv);
void omci_device_unregister(struct omci_device *odev);

void *omci_device_priv(const struct omci_device *odev);
u32 omci_device_id(const struct omci_device *odev);

void omci_device_set_identity(struct omci_device *odev,
			      const u8 serial_number[8],
			      const u8 password[10]);
void omci_device_set_onu_id(struct omci_device *odev, u16 onu_id);
void omci_device_set_channel(struct omci_device *odev, u16 gem_port_id,
			     bool valid);
void omci_device_set_state(struct omci_device *odev, u8 state);
void omci_device_receive(struct omci_device *odev, struct sk_buff *skb,
			 u16 gem_port_id, u32 flags);

#endif /* _NET_OMCI_H */
