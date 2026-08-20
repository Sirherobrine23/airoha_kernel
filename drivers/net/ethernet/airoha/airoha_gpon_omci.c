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

static int airoha_gpon_omci_start_transport(struct omci_device *odev)
{
	struct airoha_gpon_omci *omci = omci_device_priv(odev);
	int ret;

	ret = airoha_eth_xpon_control_start(omci->gdm_dev);
	if (ret)
		return ret;

	ret = airoha_gpon_omci_hw_start(omci->hw_priv);
	if (ret)
		airoha_eth_xpon_control_stop(omci->gdm_dev);

	return ret;
}

static void airoha_gpon_omci_stop_transport(struct omci_device *odev)
{
	struct airoha_gpon_omci *omci = omci_device_priv(odev);

	airoha_gpon_omci_hw_stop(omci->hw_priv);
	airoha_eth_xpon_control_stop(omci->gdm_dev);
}

static int airoha_gpon_omci_xmit(struct omci_device *odev,
				 struct sk_buff *skb, u16 gem_port_id)
{
	struct airoha_gpon_omci *omci = omci_device_priv(odev);

	return airoha_eth_xmit_xpon_oam(omci->gdm_dev, skb, gem_port_id);
}

static int
airoha_gpon_omci_get_ani_topology(struct omci_device *odev,
				  struct omci_ani_topology *topology)
{
	struct airoha_gpon_omci *omci = omci_device_priv(odev);

	return airoha_gpon_omci_hw_get_ani_topology(omci->hw_priv, topology);
}

static int airoha_gpon_omci_set_tcont(struct omci_device *odev,
				      u16 entity_id, u16 alloc_id,
				       bool valid)
{
	struct airoha_gpon_omci *omci = omci_device_priv(odev);

	return airoha_gpon_omci_hw_set_tcont(omci->hw_priv, entity_id,
					     alloc_id, valid);
}

static int airoha_gpon_omci_set_gem_port(struct omci_device *odev,
					 u16 entity_id,
					 u16 gem_port_id,
					 u16 tcont_entity_id,
					 u8 direction,
					 bool valid,
					 bool encrypted)
{
	struct airoha_gpon_omci *omci = omci_device_priv(odev);

	return airoha_gpon_omci_hw_set_gem_port(omci->hw_priv, entity_id,
						gem_port_id, tcont_entity_id,
						direction, valid, encrypted);
}

static int airoha_gpon_omci_set_uni(struct omci_device *odev,
				    u16 entity_id, bool enable)
{
	struct airoha_gpon_omci *omci = omci_device_priv(odev);

	return airoha_gpon_omci_hw_set_uni(omci->hw_priv, entity_id, enable);
}

static int
airoha_gpon_omci_replace_service(struct omci_device *odev,
				 const struct omci_service_config *service)
{
	struct airoha_gpon_omci *omci = omci_device_priv(odev);

	return airoha_gpon_omci_hw_replace_service(omci->hw_priv, service);
}

static int airoha_gpon_omci_delete_service(struct omci_device *odev,
					   u32 cookie)
{
	struct airoha_gpon_omci *omci = omci_device_priv(odev);

	return airoha_gpon_omci_hw_delete_service(omci->hw_priv, cookie);
}

static int airoha_gpon_omci_get_telemetry(struct omci_device *odev,
					  struct omci_telemetry *telemetry)
{
	struct airoha_gpon_omci *omci = omci_device_priv(odev);

	return airoha_gpon_omci_hw_get_telemetry(omci->hw_priv, telemetry);
}

static int
airoha_gpon_omci_set_olt_profile(struct omci_device *odev,
				 const struct omci_olt_profile_state *state)
{
	struct airoha_gpon_omci *omci = omci_device_priv(odev);

	return airoha_gpon_omci_hw_set_olt_profile(omci->hw_priv, state);
}

static void airoha_gpon_omci_set_operational(struct omci_device *odev,
					     bool operational)
{
	struct airoha_gpon_omci *omci = omci_device_priv(odev);

	airoha_gpon_omci_hw_set_operational(omci->hw_priv, operational);
}

static void
airoha_gpon_omci_config_changed(struct omci_device *odev, u16 key,
				const struct omci_identity *identity)
{
	struct airoha_gpon_omci *omci = omci_device_priv(odev);

	airoha_gpon_omci_hw_config_changed(omci->hw_priv, key, identity);
}

static const struct omci_device_ops airoha_gpon_omci_ops = {
	.start = airoha_gpon_omci_start_transport,
	.stop = airoha_gpon_omci_stop_transport,
	.xmit = airoha_gpon_omci_xmit,
	.get_ani_topology = airoha_gpon_omci_get_ani_topology,
	.set_tcont = airoha_gpon_omci_set_tcont,
	.set_gem_port = airoha_gpon_omci_set_gem_port,
	.set_uni = airoha_gpon_omci_set_uni,
	.replace_service = airoha_gpon_omci_replace_service,
	.delete_service = airoha_gpon_omci_delete_service,
	.get_telemetry = airoha_gpon_omci_get_telemetry,
	.set_olt_profile = airoha_gpon_omci_set_olt_profile,
	.set_operational = airoha_gpon_omci_set_operational,
	.config_changed = airoha_gpon_omci_config_changed,
};

int airoha_gpon_omci_register(struct airoha_gpon_omci *omci,
			      struct device *dev,
			      struct net_device *gdm_dev,
			      void *hw_priv,
			      const struct omci_identity *identity)
{
	int ret;

	omci->gdm_dev = gdm_dev;
	omci->hw_priv = hw_priv;
	omci->odev = omci_device_register(dev, gdm_dev->ifindex,
					  OMCI_CAP_HW_MIC | OMCI_CAP_TELEMETRY,
					  &airoha_gpon_omci_ops, omci);
	if (IS_ERR(omci->odev))
		return PTR_ERR(omci->odev);

	omci_device_set_identity_info(omci->odev, identity);
	ret = omci_device_set_dying_gasp_enabled(omci->odev, true,
						 OMCI_CONFIG_SOURCE_DRIVER);
	if (ret) {
		omci_device_unregister(omci->odev);
		omci->odev = NULL;
	}

	return ret;
}

int airoha_gpon_omci_start(struct airoha_gpon_omci *omci)
{
	return omci_device_start(omci->odev);
}

void airoha_gpon_omci_stop(struct airoha_gpon_omci *omci)
{
	omci_device_stop(omci->odev);
}

void airoha_gpon_omci_unregister(struct airoha_gpon_omci *omci)
{
	omci_device_unregister(omci->odev);
	omci->odev = NULL;
	omci->gdm_dev = NULL;
	omci->hw_priv = NULL;
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

void airoha_gpon_omci_reconcile_services(struct airoha_gpon_omci *omci)
{
	if (omci && omci->odev)
		omci_device_reconcile_services(omci->odev);
}

void airoha_gpon_omci_reset_session(struct airoha_gpon_omci *omci)
{
	omci_device_reset_session(omci->odev);
}

int airoha_gpon_omci_send_dying_gasp(struct airoha_gpon_omci *omci)
{
	return omci_device_send_dying_gasp(omci->odev);
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
