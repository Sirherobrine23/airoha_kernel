// SPDX-License-Identifier: GPL-2.0-only
/*
 * Generic xPON core
 *
 * GPON, EPON and XGS-PON use different registration and management wire
 * protocols, but share one datapath object, optical state and set of system
 * consumers. Protocol providers translate their native state into this core.
 */

#include <linux/err.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/slab.h>
#include <net/xpon.h>

#include "internal.h"

static LIST_HEAD(xpon_devices);
static DEFINE_MUTEX(xpon_devices_lock);

static void xpon_notify_work(struct work_struct *work)
{
	struct xpon_device *xpon = container_of(work, struct xpon_device,
					       notify_work);
	struct xpon_notifier_info info;
	unsigned long flags, changed;

	spin_lock_irqsave(&xpon->state_lock, flags);
	changed = xpon->pending_events;
	if (!changed) {
		spin_unlock_irqrestore(&xpon->state_lock, flags);
		return;
	}

	info.xpon = xpon;
	info.changed = changed;
	info.old = xpon->notify_old;
	info.new = xpon->state;
	xpon->notify_old = xpon->state;
	xpon->pending_events = 0;
	spin_unlock_irqrestore(&xpon->state_lock, flags);

	xpon_leds_update(xpon, &info.new);
	xpon_sysfs_notify(xpon, changed);
	xpon_genl_notify(xpon, changed);
	blocking_notifier_call_chain(&xpon->notifier, changed, &info);
}

static void xpon_queue_event(struct xpon_device *xpon, unsigned long changed)
{
	unsigned long flags;

	if (!xpon)
		return;

	spin_lock_irqsave(&xpon->state_lock, flags);
	xpon->pending_events |= changed;
	spin_unlock_irqrestore(&xpon->state_lock, flags);
	schedule_work(&xpon->notify_work);
}

struct xpon_device *
xpon_device_register(struct device *parent,
		     const struct xpon_device_desc *desc)
{
	struct xpon_device *xpon;
	int ret;

	if (!parent || !desc || !desc->netdev ||
	    desc->mode >= __XPON_MODE_MAX ||
	    !(desc->modes & XPON_MODE_CAP(desc->mode)))
		return ERR_PTR(-EINVAL);

	xpon = kzalloc(sizeof(*xpon), GFP_KERNEL);
	if (!xpon)
		return ERR_PTR(-ENOMEM);

	INIT_LIST_HEAD(&xpon->list);
	spin_lock_init(&xpon->state_lock);
	mutex_init(&xpon->mode_lock);
	INIT_WORK(&xpon->notify_work, xpon_notify_work);
	BLOCKING_INIT_NOTIFIER_HEAD(&xpon->notifier);

	xpon->parent = parent;
	xpon->netdev = desc->netdev;
	xpon->optical = desc->optical;
	xpon->ops = desc->ops;
	xpon->priv = desc->priv;
	xpon->modes = desc->modes;
	xpon->pon_led = desc->pon_led;
	xpon->los_led = desc->los_led;
	xpon->fiber_led = desc->fiber_led;
	xpon->state.mode = desc->mode;
	xpon->state.registration = XPON_REGISTRATION_DOWN;
	xpon->state.valid = 0;
	xpon->notify_old = xpon->state;

	ret = xpon_leds_register(xpon);
	if (ret) {
		kfree(xpon);
		return ERR_PTR(ret);
	}
	
	ret = xpon_sysfs_register(xpon);
	if (ret) {
		kfree(xpon);
		return ERR_PTR(ret);
	}

	mutex_lock(&xpon_devices_lock);
	list_add_tail(&xpon->list, &xpon_devices);
	mutex_unlock(&xpon_devices_lock);

	xpon_leds_update(xpon, &xpon->state);
	dev_info(parent, "registered xPON device for %s\n", desc->netdev->name);
	return xpon;
}
EXPORT_SYMBOL_GPL(xpon_device_register);

void xpon_device_unregister(struct xpon_device *xpon)
{
	if (!xpon)
		return;

	mutex_lock(&xpon_devices_lock);
	list_del_init(&xpon->list);
	mutex_unlock(&xpon_devices_lock);
	cancel_work_sync(&xpon->notify_work);
	xpon_sysfs_unregister(xpon);
	kfree(xpon);
}
EXPORT_SYMBOL_GPL(xpon_device_unregister);

struct device *xpon_device_dev(struct xpon_device *xpon)
{
	return xpon ? xpon->class_dev : NULL;
}
EXPORT_SYMBOL_GPL(xpon_device_dev);

struct net_device *xpon_device_netdev(struct xpon_device *xpon)
{
	return xpon ? xpon->netdev : NULL;
}
EXPORT_SYMBOL_GPL(xpon_device_netdev);

void *xpon_device_priv(struct xpon_device *xpon)
{
	return xpon ? xpon->priv : NULL;
}
EXPORT_SYMBOL_GPL(xpon_device_priv);

unsigned long xpon_device_modes(struct xpon_device *xpon)
{
	return xpon ? xpon->modes : 0;
}
EXPORT_SYMBOL_GPL(xpon_device_modes);

enum xpon_mode xpon_device_mode(struct xpon_device *xpon)
{
	return xpon ? READ_ONCE(xpon->state.mode) : XPON_MODE_GPON;
}
EXPORT_SYMBOL_GPL(xpon_device_mode);

int xpon_device_set_mode(struct xpon_device *xpon, enum xpon_mode mode)
{
	enum xpon_mode old_mode;
	int ret = 0;

	if (!xpon || mode >= __XPON_MODE_MAX)
		return -EINVAL;
	if (!(xpon->modes & XPON_MODE_CAP(mode)))
		return -EOPNOTSUPP;

	mutex_lock(&xpon->mode_lock);
	if (xpon->mode_changing) {
		ret = -EBUSY;
		goto out;
	}
	if (netif_running(xpon->netdev)) {
		ret = -EBUSY;
		goto out;
	}

	old_mode = xpon->state.mode;
	if (old_mode == mode)
		goto out;

	xpon->mode_changing = true;
	xpon_device_report_carrier(xpon, false);
	xpon_device_report_registration(xpon, XPON_REGISTRATION_DOWN);

	if (xpon->ops && xpon->ops->set_mode) {
		ret = xpon->ops->set_mode(xpon, mode);
		if (ret)
			goto out_changing;
	}

	WRITE_ONCE(xpon->state.mode, mode);
	xpon_queue_event(xpon, XPON_EVENT_MODE);

out_changing:
	xpon->mode_changing = false;
out:
	mutex_unlock(&xpon->mode_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(xpon_device_set_mode);

void xpon_device_report_registration(struct xpon_device *xpon,
				     enum xpon_registration_state state)
{
	unsigned long flags;

	if (!xpon)
		return;

	spin_lock_irqsave(&xpon->state_lock, flags);
	if (xpon->state.registration == state) {
		spin_unlock_irqrestore(&xpon->state_lock, flags);
		return;
	}
	xpon->state.registration = state;
	xpon->pending_events |= XPON_EVENT_REGISTRATION;
	spin_unlock_irqrestore(&xpon->state_lock, flags);
	schedule_work(&xpon->notify_work);
}
EXPORT_SYMBOL_GPL(xpon_device_report_registration);

void xpon_device_report_carrier(struct xpon_device *xpon, bool carrier)
{
	unsigned long flags;

	if (!xpon)
		return;

	spin_lock_irqsave(&xpon->state_lock, flags);
	if (xpon->state.carrier == carrier) {
		spin_unlock_irqrestore(&xpon->state_lock, flags);
		return;
	}
	xpon->state.carrier = carrier;
	xpon->pending_events |= XPON_EVENT_CARRIER;
	spin_unlock_irqrestore(&xpon->state_lock, flags);
	schedule_work(&xpon->notify_work);
}
EXPORT_SYMBOL_GPL(xpon_device_report_carrier);

void xpon_device_report_optical(struct xpon_device *xpon,
				bool signal_detect, bool los)
{
	unsigned long flags;
	unsigned long changed = 0;

	if (!xpon)
		return;

	spin_lock_irqsave(&xpon->state_lock, flags);
	if (!(xpon->state.valid & XPON_STATE_F_SIGNAL) ||
	    xpon->state.signal_detect != signal_detect)
		changed |= XPON_EVENT_SIGNAL;
	if (!(xpon->state.valid & XPON_STATE_F_LOS) || xpon->state.los != los)
		changed |= XPON_EVENT_LOS;
	xpon->state.valid |= XPON_STATE_F_SIGNAL | XPON_STATE_F_LOS;
	xpon->state.signal_detect = signal_detect;
	xpon->state.los = los;
	xpon->pending_events |= changed;
	spin_unlock_irqrestore(&xpon->state_lock, flags);

	if (changed)
		schedule_work(&xpon->notify_work);
}
EXPORT_SYMBOL_GPL(xpon_device_report_optical);

void xpon_device_report_event(struct xpon_device *xpon, unsigned long event)
{
	xpon_queue_event(xpon, event);
}
EXPORT_SYMBOL_GPL(xpon_device_report_event);

int xpon_device_register_notifier(struct xpon_device *xpon,
				  struct notifier_block *nb)
{
	if (!xpon || !nb)
		return -EINVAL;
	return blocking_notifier_chain_register(&xpon->notifier, nb);
}
EXPORT_SYMBOL_GPL(xpon_device_register_notifier);

int xpon_device_unregister_notifier(struct xpon_device *xpon,
				    struct notifier_block *nb)
{
	if (!xpon || !nb)
		return -EINVAL;
	return blocking_notifier_chain_unregister(&xpon->notifier, nb);
}
EXPORT_SYMBOL_GPL(xpon_device_unregister_notifier);

struct xpon_device *xpon_device_find_by_ifindex(u32 ifindex)
{
	struct xpon_device *xpon;

	mutex_lock(&xpon_devices_lock);
	list_for_each_entry(xpon, &xpon_devices, list) {
		if (xpon->netdev->ifindex == ifindex) {
			get_device(xpon->class_dev);
			mutex_unlock(&xpon_devices_lock);
			return xpon;
		}
	}
	mutex_unlock(&xpon_devices_lock);
	return NULL;
}

void xpon_device_put(struct xpon_device *xpon)
{
	if (xpon && xpon->class_dev)
		put_device(xpon->class_dev);
}

static int __init xpon_init(void)
{
	int ret;

	ret = xpon_sysfs_init();
	if (ret)
		return ret;

	ret = xpon_genl_init();
	if (ret) {
		xpon_sysfs_exit();
		return ret;
	}

	return 0;
}

static void __exit xpon_exit(void)
{
	xpon_genl_exit();
	xpon_sysfs_exit();
}

module_init(xpon_init);
module_exit(xpon_exit);

MODULE_DESCRIPTION("Generic xPON subsystem");
MODULE_LICENSE("GPL");
