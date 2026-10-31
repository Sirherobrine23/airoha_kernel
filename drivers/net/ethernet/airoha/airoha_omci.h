/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _AIROHA_OMCI_H
#define _AIROHA_OMCI_H

#include <linux/types.h>
#include <net/xpon/omci.h>

#include "airoha_ploam.h"

struct device;
struct omci_device;
struct omci_identity;
struct omci_ani_topology;
struct omci_telemetry;
struct omci_olt_profile_state;
struct omci_service_config;
struct sk_buff;
struct xpon_device;

/* MAC operations are passed in by the provider, keeping the common OMCI
 * module independent of the driver that uses it. start/stop/xmit are required;
 * absent provisioning callbacks remain unsupported by the shared agent.
 */
struct airoha_gpon_omci_hw_ops {
	int (*start)(void *priv);
	void (*stop)(void *priv);
	int (*xmit)(void *priv, struct sk_buff *skb, u16 port_id);
	int (*get_ani_topology)(void *priv, struct omci_ani_topology *topology);
	int (*set_tcont)(void *priv, u16 entity_id, u16 alloc_id, bool valid);
	int (*set_gem_port)(void *priv, u16 entity_id, u16 port_id,
			    u16 tcont_entity_id, u8 direction, bool valid, bool encrypted);
	int (*set_uni)(void *priv, u16 entity_id, bool enable);
	int (*validate_services)(void *priv, const struct omci_service_config *services,
				 size_t count);
	int (*replace_service)(void *priv, const struct omci_service_config *service);
	int (*delete_service)(void *priv, u32 cookie);
	int (*get_telemetry)(void *priv, struct omci_telemetry *telemetry);
	int (*set_olt_profile)(void *priv, const struct omci_olt_profile_state *state);
	int (*set_time_of_day)(void *priv, const struct omci_time_of_day *time);
	void (*set_operational)(void *priv, bool operational);
	void (*config_changed)(void *priv, u16 key, const struct omci_identity *identity);
};

struct airoha_gpon_omci {
	struct omci_device *odev;
	struct omci_device_ops ops;
	const struct airoha_gpon_omci_hw_ops *hw_ops;
	void *hw_priv;
};

int airoha_gpon_omci_register(struct airoha_gpon_omci *omci,
			      struct xpon_device *xpon,
			      const struct airoha_gpon_omci_hw_ops *hw_ops,
			      void *hw_priv,
			      const struct omci_identity *identity, bool has_tod);
int airoha_gpon_omci_start(struct airoha_gpon_omci *omci);
void airoha_gpon_omci_stop(struct airoha_gpon_omci *omci);
void airoha_gpon_omci_unregister(struct airoha_gpon_omci *omci);
void airoha_gpon_omci_set_onu_id(struct airoha_gpon_omci *omci, u16 onu_id);
void airoha_gpon_omci_set_channel(struct airoha_gpon_omci *omci,
				  u16 gem_port_id, bool valid);
void airoha_gpon_omci_set_state(struct airoha_gpon_omci *omci, u8 state);
void airoha_gpon_omci_reconcile_services(struct airoha_gpon_omci *omci);
void airoha_gpon_omci_reset_session(struct airoha_gpon_omci *omci);
int airoha_gpon_omci_send_dying_gasp(struct airoha_gpon_omci *omci);
bool airoha_gpon_omci_receive(void *data, struct sk_buff *skb, u8 channel,
			      u16 gem_port_id, u32 flags);

int airoha_gpon_omci_hw_get_ani_topology(void *hw_priv,
					 struct omci_ani_topology *topology);
int airoha_gpon_omci_hw_start(void *hw_priv);
void airoha_gpon_omci_hw_stop(void *hw_priv);
int airoha_gpon_omci_hw_set_tcont(void *hw_priv, u16 entity_id,
				  u16 alloc_id, bool valid);
int airoha_gpon_omci_hw_set_gem_port(void *hw_priv, u16 entity_id,
				     u16 gem_port_id, u16 tcont_entity_id,
				     u8 direction, bool valid, bool encrypted);
int airoha_gpon_omci_hw_set_uni(void *hw_priv, u16 entity_id, bool enable);
int airoha_gpon_omci_hw_validate_services(void *hw_priv,
					  const struct omci_service_config *services,
					  size_t count);
int airoha_gpon_omci_hw_replace_service(void *hw_priv,
					const struct omci_service_config *service);
int airoha_gpon_omci_hw_delete_service(void *hw_priv, u32 cookie);
int airoha_gpon_omci_hw_get_telemetry(void *hw_priv,
				      struct omci_telemetry *telemetry);
int airoha_gpon_omci_hw_set_olt_profile(void *hw_priv,
					const struct omci_olt_profile_state *state);
void airoha_gpon_omci_hw_set_operational(void *hw_priv, bool operational);
void airoha_gpon_omci_hw_config_changed(void *hw_priv, u16 key,
					const struct omci_identity *identity);

struct airoha_xgspon;

/*
 * OMCI transport callbacks consume skb only on success and handle MIC in both
 * directions. stop() drains incoming callbacks before unregister returns.
 * MAC provisioning is passed the original provider private pointer. Missing
 * provisioning operations remain unsupported in the shared OMCI agent.
 */
struct airoha_xgspon_ops {
	struct airoha_xgspon_ploam_ops ploam;
	int (*start)(void *priv);
	void (*stop)(void *priv);
	int (*omci_xmit)(void *priv, struct sk_buff *skb, u16 port_id);
	int (*get_telemetry)(void *priv, struct omci_telemetry *telemetry);
};

struct airoha_xgspon *
airoha_xgspon_register(struct xpon_device *xpon, const struct airoha_xgspon_ops *ops,
		       void *priv, const struct omci_identity *identity,
		       const u8 reg_id[XGSPON_PLOAM_REG_ID_LEN]);
void airoha_xgspon_unregister(struct airoha_xgspon *xgs);
struct omci_device *airoha_xgspon_omci_device(struct airoha_xgspon *xgs);
int airoha_xgspon_notify_link(struct airoha_xgspon *xgs, bool ready);
/* Discard a damaged FIFO/session while preserving an OLT's O7 denial. */
int airoha_xgspon_recover(struct airoha_xgspon *xgs);
int airoha_xgspon_receive_ploam(struct airoha_xgspon *xgs, const void *body,
				size_t len, bool mic_valid);
int airoha_xgspon_serial_number(struct airoha_xgspon *xgs, u32 random_delay);
int airoha_xgspon_registration(struct airoha_xgspon *xgs, u8 seq_no);
struct airoha_xgspon_ploam_rx {
	u8 body[XGSPON_PLOAM_BODY_LEN];
	bool mic_valid;
	bool registered;
};

/* Read and dispatch under the session lock, preventing an old FIFO packet
 * from crossing a reset. The reader returns -ENODATA when the FIFO is empty.
 */
int airoha_xgspon_poll_ploam(struct airoha_xgspon *xgs,
			     int (*read)(void *priv, struct airoha_xgspon_ploam_rx *rx),
			     void *priv);
/* Always consumes skb, including an invalid MIC, inactive or wrong OMCC. */
void airoha_xgspon_receive_omci(struct airoha_xgspon *xgs, struct sk_buff *skb,
				u16 port_id, u32 flags);

#endif /* _AIROHA_OMCI_H */
