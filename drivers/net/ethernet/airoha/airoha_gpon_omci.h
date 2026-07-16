/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _AIROHA_GPON_OMCI_H
#define _AIROHA_GPON_OMCI_H

#include <linux/types.h>

struct device;
struct net_device;
struct omci_device;
struct sk_buff;

struct airoha_gpon_omci {
	struct omci_device *odev;
	struct net_device *gdm_dev;
};

int airoha_gpon_omci_register(struct airoha_gpon_omci *omci,
			      struct device *dev,
			      struct net_device *pon_dev,
			      struct net_device *gdm_dev);
void airoha_gpon_omci_unregister(struct airoha_gpon_omci *omci);
void airoha_gpon_omci_set_onu_id(struct airoha_gpon_omci *omci, u16 onu_id);
void airoha_gpon_omci_set_channel(struct airoha_gpon_omci *omci,
				  u16 gem_port_id, bool valid);
void airoha_gpon_omci_set_state(struct airoha_gpon_omci *omci, u8 state);
bool airoha_gpon_omci_receive(void *data, struct sk_buff *skb,
			      u16 gem_port_id, u32 flags);

#endif /* _AIROHA_GPON_OMCI_H */
