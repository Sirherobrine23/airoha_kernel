// SPDX-License-Identifier: GPL-2.0-only
/*
 * Airoha GPON and XGS-PON OMCI integration
 */

#include <linux/err.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/netdevice.h>
#include <linux/skbuff.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include <net/xpon.h>
#include <net/xpon/omci.h>

#include "airoha_eth.h"
#include "airoha_omci.h"

static int airoha_gpon_omci_start_transport(struct omci_device *odev)
{
	struct airoha_gpon_omci *omci = omci_device_priv(odev);

	return omci->hw_ops->start(omci->hw_priv);
}

static void airoha_gpon_omci_stop_transport(struct omci_device *odev)
{
	struct airoha_gpon_omci *omci = omci_device_priv(odev);

	omci->hw_ops->stop(omci->hw_priv);
}

static int airoha_gpon_omci_xmit(struct omci_device *odev,
				 struct sk_buff *skb, u16 gem_port_id)
{
	struct airoha_gpon_omci *omci = omci_device_priv(odev);

	return omci->hw_ops->xmit(omci->hw_priv, skb, gem_port_id);
}

static int
airoha_gpon_omci_get_ani_topology(struct omci_device *odev,
				  struct omci_ani_topology *topology)
{
	struct airoha_gpon_omci *omci = omci_device_priv(odev);

	return omci->hw_ops->get_ani_topology(omci->hw_priv, topology);
}

static int airoha_gpon_omci_set_tcont(struct omci_device *odev,
				      u16 entity_id, u16 alloc_id,
				       bool valid)
{
	struct airoha_gpon_omci *omci = omci_device_priv(odev);

	return omci->hw_ops->set_tcont(omci->hw_priv, entity_id,
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

	return omci->hw_ops->set_gem_port(omci->hw_priv, entity_id,
						gem_port_id, tcont_entity_id,
						direction, valid, encrypted);
}

static int airoha_gpon_omci_set_uni(struct omci_device *odev,
				    u16 entity_id, bool enable)
{
	struct airoha_gpon_omci *omci = omci_device_priv(odev);

	return omci->hw_ops->set_uni(omci->hw_priv, entity_id, enable);
}

static int
airoha_gpon_omci_validate_services(struct omci_device *odev,
				   const struct omci_service_config *services,
				   size_t count)
{
	struct airoha_gpon_omci *omci = omci_device_priv(odev);

	return omci->hw_ops->validate_services(omci->hw_priv, services, count);
}

static int
airoha_gpon_omci_replace_service(struct omci_device *odev,
				 const struct omci_service_config *service)
{
	struct airoha_gpon_omci *omci = omci_device_priv(odev);

	return omci->hw_ops->replace_service(omci->hw_priv, service);
}

static int airoha_gpon_omci_delete_service(struct omci_device *odev,
					   u32 cookie)
{
	struct airoha_gpon_omci *omci = omci_device_priv(odev);

	return omci->hw_ops->delete_service(omci->hw_priv, cookie);
}

static int airoha_gpon_omci_get_telemetry(struct omci_device *odev,
					  struct omci_telemetry *telemetry)
{
	struct airoha_gpon_omci *omci = omci_device_priv(odev);

	return omci->hw_ops->get_telemetry(omci->hw_priv, telemetry);
}

static int
airoha_gpon_omci_set_olt_profile(struct omci_device *odev,
				 const struct omci_olt_profile_state *state)
{
	struct airoha_gpon_omci *omci = omci_device_priv(odev);

	return omci->hw_ops->set_olt_profile(omci->hw_priv, state);
}

static void airoha_gpon_omci_set_operational(struct omci_device *odev,
					     bool operational)
{
	struct airoha_gpon_omci *omci = omci_device_priv(odev);

	omci->hw_ops->set_operational(omci->hw_priv, operational);
}

static void
airoha_gpon_omci_config_changed(struct omci_device *odev, u16 key,
				const struct omci_identity *identity)
{
	struct airoha_gpon_omci *omci = omci_device_priv(odev);

	omci->hw_ops->config_changed(omci->hw_priv, key, identity);
}

static int
airoha_gpon_omci_set_time_of_day(struct omci_device *odev,
				 const struct omci_time_of_day *time)
{
	struct airoha_gpon_omci *omci = omci_device_priv(odev);

	return omci->hw_ops->set_time_of_day(omci->hw_priv, time);
}

int airoha_gpon_omci_register(struct airoha_gpon_omci *omci,
			      struct xpon_device *xpon,
			      const struct airoha_gpon_omci_hw_ops *hw_ops,
			      void *hw_priv,
			      const struct omci_identity *identity, bool has_tod)
{
	u32 capabilities = OMCI_CAP_HW_MIC;
	int ret;

	if (!hw_ops || !hw_ops->start || !hw_ops->stop || !hw_ops->xmit)
		return -EINVAL;
	omci->hw_ops = hw_ops;
	omci->hw_priv = hw_priv;
	omci->ops = (struct omci_device_ops) {
		.start = airoha_gpon_omci_start_transport,
		.stop = airoha_gpon_omci_stop_transport,
		.xmit = airoha_gpon_omci_xmit,
		.get_ani_topology = hw_ops->get_ani_topology ?
			airoha_gpon_omci_get_ani_topology : NULL,
		.set_tcont = hw_ops->set_tcont ?
			airoha_gpon_omci_set_tcont : NULL,
		.set_gem_port = hw_ops->set_gem_port ?
			airoha_gpon_omci_set_gem_port : NULL,
		.set_uni = hw_ops->set_uni ?
			airoha_gpon_omci_set_uni : NULL,
		.validate_services = hw_ops->validate_services ?
			airoha_gpon_omci_validate_services : NULL,
		.replace_service = hw_ops->replace_service ?
			airoha_gpon_omci_replace_service : NULL,
		.delete_service = hw_ops->delete_service ?
			airoha_gpon_omci_delete_service : NULL,
		.get_telemetry = hw_ops->get_telemetry ?
			airoha_gpon_omci_get_telemetry : NULL,
		.set_olt_profile = hw_ops->set_olt_profile ?
			airoha_gpon_omci_set_olt_profile : NULL,
		.set_operational = hw_ops->set_operational ?
			airoha_gpon_omci_set_operational : NULL,
		.config_changed = hw_ops->config_changed ?
			airoha_gpon_omci_config_changed : NULL,
	};
	if (has_tod && hw_ops->set_time_of_day)
		omci->ops.set_time_of_day = airoha_gpon_omci_set_time_of_day;
	if (hw_ops->get_telemetry)
		capabilities |= OMCI_CAP_TELEMETRY;
	omci->odev = omci_device_register(xpon,
					  capabilities,
					  &omci->ops, omci);
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
	omci->hw_ops = NULL;
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

bool airoha_gpon_omci_receive(void *data, struct sk_buff *skb, u8 channel,
			      u16 gem_port_id, u32 flags)
{
	struct airoha_gpon_omci *omci = data;
	u32 omci_flags = 0;

	(void)channel;

	if (flags & AIROHA_XPON_OAM_RX_F_MIC_PRESENT)
		omci_flags |= OMCI_F_MIC_PRESENT;
	if (flags & AIROHA_XPON_OAM_RX_F_MIC_VALID)
		omci_flags |= OMCI_F_MIC_VALID;
	if (flags & AIROHA_XPON_OAM_RX_F_CRC_ERROR)
		omci_flags |= OMCI_F_CRC_ERROR;

	omci_device_receive(omci->odev, skb, gem_port_id, omci_flags);
	return true;
}

/* XGS-PON activation and OMCC integration. */

struct airoha_xgspon {
	const struct airoha_xgspon_ops *ops;
	void *priv;
	struct xpon_device *xpon;
	struct omci_device *omci;
	struct airoha_xgspon_ploam *ploam;
	struct airoha_xgspon_ploam_ops ploam_ops;
	/* Serializes activation callbacks and timer deadlines. */
	struct mutex lock;
	struct delayed_work timer;
	unsigned long deadline;
	bool timer_armed;
	bool started;
	bool phy_ready;
	/* RX/TX must not acquire lock: an OMCI session reset drains RX work. */
	bool channel_up;
	u16 port_id;
};

static int airoha_xgspon_reset(void *priv)
{
	struct airoha_xgspon *xgs = priv;

	return xgs->ops->ploam.reset(xgs->priv);
}

static int airoha_xgspon_set_state(void *priv, enum airoha_xgspon_state state)
{
	struct airoha_xgspon *xgs = priv;

	return xgs->ops->ploam.set_state(xgs->priv, state);
}

static int airoha_xgspon_set_profile(void *priv,
				     const struct xgspon_ploam_burst_profile *profile)
{
	struct airoha_xgspon *xgs = priv;

	return xgs->ops->ploam.set_profile(xgs->priv, profile);
}

static int airoha_xgspon_set_onu_id(void *priv, u16 onu_id)
{
	struct airoha_xgspon *xgs = priv;

	return xgs->ops->ploam.set_onu_id(xgs->priv, onu_id);
}

static int airoha_xgspon_set_eqd(void *priv, u32 eqd)
{
	struct airoha_xgspon *xgs = priv;

	return xgs->ops->ploam.set_eqd(xgs->priv, eqd);
}

static int airoha_xgspon_set_alloc_id(void *priv, u16 alloc_id, bool valid)
{
	struct airoha_xgspon *xgs = priv;

	return xgs->ops->ploam.set_alloc_id(xgs->priv, alloc_id, valid);
}

static int airoha_xgspon_set_omcc(void *priv, u16 port_id, bool valid)
{
	struct airoha_xgspon *xgs = priv;

	return xgs->ops->ploam.set_omcc(xgs->priv, port_id, valid);
}

static int airoha_xgspon_send_upstream(void *priv,
				       const u8 body[XGSPON_PLOAM_BODY_LEN],
				     bool registered)
{
	struct airoha_xgspon *xgs = priv;

	return xgs->ops->ploam.send_upstream(xgs->priv, body, registered);
}

static int airoha_xgspon_key_report(void *priv,
				    const struct xgspon_ploam_key_control *request,
				  struct xgspon_ploam_key_report *report)
{
	struct airoha_xgspon *xgs = priv;

	if (!xgs->ops->ploam.key_report)
		return -EOPNOTSUPP;
	return xgs->ops->ploam.key_report(xgs->priv, request, report);
}

static int airoha_xgspon_reboot_request(void *priv,
					const struct xgspon_ploam_reboot *request)
{
	struct airoha_xgspon *xgs = priv;

	if (!xgs->ops->ploam.reboot)
		return -EOPNOTSUPP;
	return xgs->ops->ploam.reboot(xgs->priv, request);
}

static void airoha_xgspon_state_changed(void *priv,
					enum airoha_xgspon_state state, u16 onu_id)
{
	struct airoha_xgspon *xgs = priv;
	bool up = state == AIROHA_XGSPON_O5;
	bool was_up = READ_ONCE(xgs->channel_up);

	/* Reject new data before draining requests accepted in the old session. */
	if (!up)
		WRITE_ONCE(xgs->channel_up, false);
	if (!xgspon_ploam_is_assignable(onu_id))
		omci_device_reset_session(xgs->omci);
	else if (was_up && !up)
		omci_device_set_channel(xgs->omci, onu_id, false);
	omci_device_set_onu_id(xgs->omci, onu_id);
	omci_device_set_state(xgs->omci, state);
	WRITE_ONCE(xgs->port_id, onu_id);
	if (up && !was_up) {
		omci_device_set_channel(xgs->omci, onu_id, true);
		/* Publish the Port-ID before RX/TX observe the open channel. */
		smp_store_release(&xgs->channel_up, true);
	}
	xpon_device_report_registration(xgs->xpon,
					up ? XPON_REGISTRATION_OPERATIONAL :
		state == AIROHA_XGSPON_O4 ? XPON_REGISTRATION_REGISTERING :
		state == AIROHA_XGSPON_O2_3 ? XPON_REGISTRATION_DISCOVERY :
		XPON_REGISTRATION_DOWN);
	if (xgs->ops->ploam.state_changed)
		xgs->ops->ploam.state_changed(xgs->priv, state, onu_id);
}

static void airoha_xgspon_timer_changed(void *priv, unsigned int timeout_ms)
{
	struct airoha_xgspon *xgs = priv;

	xgs->timer_armed = timeout_ms != 0;
	if (timeout_ms) {
		xgs->deadline = jiffies + msecs_to_jiffies(timeout_ms);
		mod_delayed_work(system_wq, &xgs->timer, msecs_to_jiffies(timeout_ms));
	} else {
		cancel_delayed_work(&xgs->timer);
	}
}

static void airoha_xgspon_timeout_work(struct work_struct *work)
{
	struct airoha_xgspon *xgs = container_of(to_delayed_work(work),
					      struct airoha_xgspon, timer);
	unsigned long now;

	mutex_lock(&xgs->lock);
	now = jiffies;
	/* A worker queued for an old state may have waited on lock while a
	 * new O4/O6 deadline was armed. Never expire the new timer early.
	 */
	if (xgs->started && xgs->timer_armed) {
		if (time_before(now, xgs->deadline)) {
			mod_delayed_work(system_wq, &xgs->timer, xgs->deadline - now);
		} else {
			xgs->timer_armed = false;
			airoha_xgspon_ploam_timeout(xgs->ploam);
		}
	}
	mutex_unlock(&xgs->lock);
}

static int airoha_xgspon_omci_start(struct omci_device *odev)
{
	struct airoha_xgspon *xgs = omci_device_priv(odev);
	int ret;

	/* An O1/status-only provider must not pretend to start an OMCC. */
	if (!xgs->ploam)
		return -EOPNOTSUPP;
	ret = xgs->ops->start(xgs->priv);
	if (ret)
		return ret;
	mutex_lock(&xgs->lock);
	ret = airoha_xgspon_ploam_start(xgs->ploam);
	if (!ret) {
		xgs->started = true;
		ret = airoha_xgspon_ploam_link(xgs->ploam, xgs->phy_ready);
	}
	if (ret) {
		xgs->started = false;
		airoha_xgspon_ploam_stop(xgs->ploam);
	}
	mutex_unlock(&xgs->lock);
	if (ret)
		xgs->ops->stop(xgs->priv);
	return ret;
}

static void airoha_xgspon_omci_stop(struct omci_device *odev)
{
	struct airoha_xgspon *xgs = omci_device_priv(odev);

	mutex_lock(&xgs->lock);
	xgs->started = false;
	WRITE_ONCE(xgs->channel_up, false);
	if (xgs->ploam)
		airoha_xgspon_ploam_stop(xgs->ploam);
	mutex_unlock(&xgs->lock);
	cancel_delayed_work_sync(&xgs->timer);
	/* Drain IRQ/work producers outside lock: they may be awaiting it. */
	if (xgs->ploam)
		xgs->ops->stop(xgs->priv);
}

static int airoha_xgspon_omci_xmit(struct omci_device *odev,
				   struct sk_buff *skb, u16 port_id)
{
	struct airoha_xgspon *xgs = omci_device_priv(odev);

	if (!xgs->ploam)
		return -EOPNOTSUPP;
	/* Pair with channel publication before reading the Port-ID. */
	if (!smp_load_acquire(&xgs->channel_up) || port_id != READ_ONCE(xgs->port_id))
		return -ENOLINK;
	return xgs->ops->omci_xmit(xgs->priv, skb, port_id);
}

static int airoha_xgspon_omci_telemetry(struct omci_device *odev,
					struct omci_telemetry *telemetry)
{
	struct airoha_xgspon *xgs = omci_device_priv(odev);

	if (!xgs->ops->get_telemetry)
		return -EOPNOTSUPP;
	return xgs->ops->get_telemetry(xgs->priv, telemetry);
}

static const struct omci_device_ops airoha_xgspon_omci_ops = {
	.start = airoha_xgspon_omci_start,
	.stop = airoha_xgspon_omci_stop,
	.xmit = airoha_xgspon_omci_xmit,
	.get_telemetry = airoha_xgspon_omci_telemetry,
};

struct airoha_xgspon *
airoha_xgspon_register(struct xpon_device *xpon, const struct airoha_xgspon_ops *ops,
		       void *priv, const struct omci_identity *identity,
		       const u8 reg_id[XGSPON_PLOAM_REG_ID_LEN])
{
	struct airoha_xgspon *xgs;
	u32 capabilities = 0;
	int ret;

	if (!xpon || !ops || !identity)
		return ERR_PTR(-EINVAL);
	if (xpon_device_mode(xpon) != XPON_MODE_XGSPON)
		return ERR_PTR(-EOPNOTSUPP);
	xgs = kzalloc(sizeof(*xgs), GFP_KERNEL);
	if (!xgs)
		return ERR_PTR(-ENOMEM);
	xgs->xpon = xpon;
	xgs->ops = ops;
	xgs->priv = priv;
	xgs->port_id = XGSPON_PLOAM_ONU_ID_UNASSIGNED;
	mutex_init(&xgs->lock);
	INIT_DELAYED_WORK(&xgs->timer, airoha_xgspon_timeout_work);
	/* An absent transport is allowed only for the existing O1 diagnostics.
	 * A partial transport is a programming error, never a reduced success.
	 */
	if (ops->start || ops->stop || ops->omci_xmit || ops->ploam.reset ||
	    ops->ploam.set_state || ops->ploam.set_profile || ops->ploam.set_onu_id ||
	    ops->ploam.set_eqd || ops->ploam.set_alloc_id || ops->ploam.set_omcc ||
	    ops->ploam.send_upstream || ops->ploam.key_report || ops->ploam.reboot) {
		if (!ops->start || !ops->stop || !ops->omci_xmit ||
		    !ops->ploam.reset || !ops->ploam.set_state ||
		    !ops->ploam.set_profile || !ops->ploam.set_onu_id ||
		    !ops->ploam.set_eqd || !ops->ploam.set_alloc_id ||
		    !ops->ploam.set_omcc || !ops->ploam.send_upstream) {
			ret = -EINVAL;
			goto free;
		}
		xgs->ploam_ops = (struct airoha_xgspon_ploam_ops) {
			.reset = airoha_xgspon_reset,
			.set_state = airoha_xgspon_set_state,
			.set_profile = airoha_xgspon_set_profile,
			.set_onu_id = airoha_xgspon_set_onu_id,
			.set_eqd = airoha_xgspon_set_eqd,
			.set_alloc_id = airoha_xgspon_set_alloc_id,
			.set_omcc = airoha_xgspon_set_omcc,
			.send_upstream = airoha_xgspon_send_upstream,
			.key_report = airoha_xgspon_key_report,
			.reboot = airoha_xgspon_reboot_request,
			.state_changed = airoha_xgspon_state_changed,
			.timer_changed = airoha_xgspon_timer_changed,
		};
		xgs->ploam = airoha_xgspon_ploam_alloc(&xgs->ploam_ops, xgs,
						       identity->serial_number, reg_id);
		if (IS_ERR(xgs->ploam)) {
			ret = PTR_ERR(xgs->ploam);
			xgs->ploam = NULL;
			goto free;
		}
		capabilities |= OMCI_CAP_HW_MIC;
	}
	if (ops->get_telemetry)
		capabilities |= OMCI_CAP_TELEMETRY;
	xgs->omci = omci_device_register(xpon, capabilities, &airoha_xgspon_omci_ops, xgs);
	if (IS_ERR(xgs->omci)) {
		ret = PTR_ERR(xgs->omci);
		goto free;
	}
	omci_device_set_identity_info(xgs->omci, identity);
	omci_device_set_onu_id(xgs->omci, XGSPON_PLOAM_ONU_ID_UNASSIGNED);
	omci_device_set_state(xgs->omci, AIROHA_XGSPON_O1);
	return xgs;
free:
	airoha_xgspon_ploam_free(xgs->ploam);
	kfree(xgs);
	return ERR_PTR(ret);
}

void airoha_xgspon_unregister(struct airoha_xgspon *xgs)
{
	if (!xgs)
		return;
	omci_device_unregister(xgs->omci);
	cancel_delayed_work_sync(&xgs->timer);
	airoha_xgspon_ploam_free(xgs->ploam);
	kfree(xgs);
}

struct omci_device *airoha_xgspon_omci_device(struct airoha_xgspon *xgs)
{
	return xgs->omci;
}

int airoha_xgspon_notify_link(struct airoha_xgspon *xgs, bool ready)
{
	int ret;

	mutex_lock(&xgs->lock);
	xgs->phy_ready = ready;
	if (xgs->started)
		ret = airoha_xgspon_ploam_link(xgs->ploam, ready);
	else
		ret = -ENETDOWN;
	mutex_unlock(&xgs->lock);
	return ret;
}

int airoha_xgspon_recover(struct airoha_xgspon *xgs)
{
	int ret = -ENETDOWN;

	mutex_lock(&xgs->lock);
	if (xgs->started)
		ret = airoha_xgspon_ploam_reset(xgs->ploam);
	mutex_unlock(&xgs->lock);
	return ret;
}

int airoha_xgspon_receive_ploam(struct airoha_xgspon *xgs, const void *body,
				size_t len, bool mic_valid)
{
	int ret = -ENETDOWN;

	mutex_lock(&xgs->lock);
	if (xgs->started)
		ret = airoha_xgspon_ploam_receive(xgs->ploam, body, len, mic_valid);
	mutex_unlock(&xgs->lock);
	return ret;
}

int airoha_xgspon_serial_number(struct airoha_xgspon *xgs, u32 random_delay)
{
	int ret = -ENETDOWN;

	mutex_lock(&xgs->lock);
	if (xgs->started)
		ret = airoha_xgspon_ploam_serial_number(xgs->ploam, random_delay);
	mutex_unlock(&xgs->lock);
	return ret;
}

int airoha_xgspon_registration(struct airoha_xgspon *xgs, u8 seq_no)
{
	int ret = -ENETDOWN;

	mutex_lock(&xgs->lock);
	if (xgs->started)
		ret = airoha_xgspon_ploam_registration(xgs->ploam, seq_no);
	mutex_unlock(&xgs->lock);
	return ret;
}

int airoha_xgspon_poll_ploam(struct airoha_xgspon *xgs,
			     int (*read)(void *priv,
					 struct airoha_xgspon_ploam_rx *rx),
			     void *priv)
{
	struct airoha_xgspon_ploam_rx rx = {};
	int ret = -ENETDOWN;

	mutex_lock(&xgs->lock);
	if (!xgs->started)
		goto out;
	ret = read(priv, &rx);
	if (ret)
		goto out;
	if (rx.mic_valid && rx.registered)
		airoha_xgspon_ploam_registration_sent(xgs->ploam);
	ret = airoha_xgspon_ploam_receive(xgs->ploam, rx.body,
					  XGSPON_PLOAM_BODY_LEN, rx.mic_valid);
out:
	mutex_unlock(&xgs->lock);
	return ret;
}

void airoha_xgspon_receive_omci(struct airoha_xgspon *xgs, struct sk_buff *skb,
				u16 port_id, u32 flags)
{
	if (!skb)
		return;
	/* Pair with channel publication before reading the Port-ID. */
	if (!smp_load_acquire(&xgs->channel_up) || port_id != READ_ONCE(xgs->port_id) ||
	    !(flags & OMCI_F_MIC_VALID) || flags & OMCI_F_CRC_ERROR) {
		dev_kfree_skb_any(skb);
		return;
	}
	omci_device_receive(xgs->omci, skb, port_id, flags);
}

MODULE_DESCRIPTION("Airoha GPON and XGS-PON OMCI integration");
MODULE_LICENSE("GPL");
