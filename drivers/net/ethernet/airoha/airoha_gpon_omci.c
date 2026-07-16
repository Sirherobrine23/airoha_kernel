// SPDX-License-Identifier: GPL-2.0-only
/*
 * Airoha EN7523 GPON OMCI transport backend
 */

#include <linux/err.h>
#include <linux/netdevice.h>
#include <linux/skbuff.h>
#include <net/omci.h>

#include "airoha_eth.h"
#include "airoha_gpon_omci.h"

static int airoha_gpon_omci_xmit(struct omci_device *odev,
				 struct sk_buff *skb, u16 gem_port_id)
{
	struct airoha_gpon_omci *omci = omci_device_priv(odev);

	return airoha_eth_xmit_xpon_oam(omci->gdm_dev, skb, gem_port_id);
}

static const struct omci_device_ops airoha_gpon_omci_ops = {
	.xmit = airoha_gpon_omci_xmit,
};

int airoha_gpon_omci_register(struct airoha_gpon_omci *omci,
			      struct device *dev,
			      struct net_device *pon_dev,
			      struct net_device *gdm_dev)
{
	omci->gdm_dev = gdm_dev;
	omci->odev = omci_device_register(dev, pon_dev->ifindex,
					  OMCI_CAP_HW_MIC,
					  &airoha_gpon_omci_ops, omci);
	if (IS_ERR(omci->odev))
		return PTR_ERR(omci->odev);

	return 0;
}

void airoha_gpon_omci_unregister(struct airoha_gpon_omci *omci)
{
	omci_device_unregister(omci->odev);
	omci->odev = NULL;
	omci->gdm_dev = NULL;
}

void airoha_gpon_omci_set_onu_id(struct airoha_gpon_omci *omci, u16 onu_id)
{
	omci_device_set_onu_id(omci->odev, onu_id);
}

void airoha_gpon_omci_set_channel(struct airoha_gpon_omci *omci,
				  u16 gem_port_id, bool valid)
{
	omci_device_set_channel(omci->odev, gem_port_id, valid);
}

void airoha_gpon_omci_set_state(struct airoha_gpon_omci *omci, u8 state)
{
	omci_device_set_state(omci->odev, state);
}

bool airoha_gpon_omci_receive(void *data, struct sk_buff *skb,
			      u16 gem_port_id, u32 flags)
{
	struct airoha_gpon_omci *omci = data;
	u32 omci_flags = 0;

	if (flags & AIROHA_XPON_OAM_RX_F_MIC_PRESENT)
		omci_flags |= OMCI_F_MIC_PRESENT;
	if (flags & AIROHA_XPON_OAM_RX_F_MIC_VALID)
		omci_flags |= OMCI_F_MIC_VALID;
	if (flags & AIROHA_XPON_OAM_RX_F_CRC_ERROR)
		omci_flags |= OMCI_F_CRC_ERROR;

	omci_device_receive(omci->odev, skb, gem_port_id, omci_flags);
	return true;
}
