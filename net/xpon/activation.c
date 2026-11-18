// SPDX-License-Identifier: GPL-2.0-only
/* Administrative activation policy, independent of the PON wire protocol. */

#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/netdevice.h>

#include "internal.h"

#define XPON_DEACTIVATION_DELAY_MS 5000

static void xpon_activation_changed(struct xpon_device *xpon)
{
	xpon_device_report_event(xpon, XPON_EVENT_ACTIVATION);
}

static void xpon_activation_revoke(struct xpon_device *xpon)
{
	lockdep_assert_held(&xpon->activation_lock);

	WRITE_ONCE(xpon->activation_allowed, false);
	if (xpon->activation_approved) {
		xpon->activation_approved = false;
		xpon->activation_reset_pending = true;
		mod_delayed_work(system_wq, &xpon->activation_work, 0);
	}
	xpon->activation_cookie++;
	xpon_activation_changed(xpon);
}

static void xpon_activation_work(struct work_struct *work)
{
	struct xpon_device *xpon = container_of(to_delayed_work(work),
					       struct xpon_device, activation_work);

	mutex_lock(&xpon->activation_lock);
	if (!xpon->activation_ops)
		goto out;

	if (!xpon->activation_admin_up && xpon->activation_approved) {
		WRITE_ONCE(xpon->activation_allowed, false);
		xpon->activation_approved = false;
		xpon->activation_cookie++;
		xpon->activation_reset_pending = true;
	}
	if (xpon->activation_reset_pending) {
		xpon->activation_ops->deactivate(xpon);
		xpon->activation_reset_pending = false;
		xpon_activation_changed(xpon);
	}
out:
	mutex_unlock(&xpon->activation_lock);
}

void xpon_activation_init(struct xpon_device *xpon)
{
	mutex_init(&xpon->activation_lock);
	INIT_DELAYED_WORK(&xpon->activation_work, xpon_activation_work);
}

bool xpon_device_activation_allowed(struct xpon_device *xpon)
{
	return xpon && READ_ONCE(xpon->activation_allowed);
}
EXPORT_SYMBOL_GPL(xpon_device_activation_allowed);

/* A new discovery cycle requires a fresh decision from a bound agent. */
void xpon_device_activation_request(struct xpon_device *xpon)
{
	if (!xpon)
		return;

	mutex_lock(&xpon->activation_lock);
	if (xpon->activation_ops && xpon->activation_owner &&
	    xpon->activation_approved) {
		WRITE_ONCE(xpon->activation_allowed, false);
		xpon->activation_approved = false;
		xpon->activation_cookie++;
		xpon_activation_changed(xpon);
	}
	mutex_unlock(&xpon->activation_lock);
}
EXPORT_SYMBOL_GPL(xpon_device_activation_request);

int xpon_device_activation_attach(struct xpon_device *xpon,
				  const struct xpon_activation_ops *ops)
{
	int ret = 0;

	if (!xpon || !ops || !ops->set_admin || !ops->deactivate)
		return -EINVAL;

	mutex_lock(&xpon->activation_lock);
	if (xpon->activation_ops || !xpon->registered || xpon->unregistering) {
		ret = -EBUSY;
		goto out;
	}
	if (xpon->netdev->flags & IFF_UP) {
		ret = ops->set_admin(xpon, true);
		if (ret)
			goto out;
		xpon->activation_admin_up = true;
	}
	xpon->activation_ops = ops;
	xpon->activation_cookie++;
	xpon_activation_changed(xpon);
out:
	mutex_unlock(&xpon->activation_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(xpon_device_activation_attach);

void xpon_device_activation_detach(struct xpon_device *xpon)
{
	if (!xpon)
		return;

	mutex_lock(&xpon->activation_lock);
	WRITE_ONCE(xpon->activation_allowed, false);
	if (xpon->activation_ops && xpon->activation_admin_up)
		xpon->activation_ops->set_admin(xpon, false);
	xpon->activation_ops = NULL;
	xpon->activation_admin_up = false;
	xpon->activation_approved = false;
	xpon->activation_reset_pending = false;
	xpon->activation_owner = 0;
	xpon->activation_cookie++;
	mutex_unlock(&xpon->activation_lock);
	cancel_delayed_work_sync(&xpon->activation_work);
}
EXPORT_SYMBOL_GPL(xpon_device_activation_detach);

void xpon_activation_admin(struct xpon_device *xpon, bool up)
{
	int ret;

	mutex_lock(&xpon->activation_lock);
	if (!xpon->activation_ops || xpon->activation_admin_up == up)
		goto out;

	if (!up)
		WRITE_ONCE(xpon->activation_allowed, false);
	ret = xpon->activation_ops->set_admin(xpon, up);
	if (ret) {
		dev_err(xpon->parent, "failed to change protocol admin state: %d\n", ret);
		goto out;
	}
	xpon->activation_admin_up = up;
	WRITE_ONCE(xpon->activation_allowed, up && xpon->activation_approved);
	xpon->activation_cookie++;
	if (!up && xpon->activation_approved && !xpon->activation_reset_pending)
		mod_delayed_work(system_wq, &xpon->activation_work,
				 msecs_to_jiffies(XPON_DEACTIVATION_DELAY_MS));
	else if (up && !xpon->activation_reset_pending)
		cancel_delayed_work(&xpon->activation_work);
	xpon_activation_changed(xpon);
out:
	mutex_unlock(&xpon->activation_lock);
}

int xpon_activation_bind(struct xpon_device *xpon, u32 owner, bool bind)
{
	int ret = 0;

	mutex_lock(&xpon->activation_lock);
	if (!xpon->activation_ops) {
		ret = -EOPNOTSUPP;
		goto out;
	}
	if (!owner) {
		ret = -EINVAL;
		goto out;
	}
	if (bind) {
		if (xpon->activation_owner == owner)
			goto out;
		if (xpon->activation_owner) {
			ret = -EBUSY;
			goto out;
		}
		xpon->activation_owner = owner;
	} else {
		if (xpon->activation_owner != owner) {
			ret = -EPERM;
			goto out;
		}
		xpon->activation_owner = 0;
	}
	xpon_activation_revoke(xpon);
out:
	mutex_unlock(&xpon->activation_lock);
	return ret;
}

int xpon_activation_set(struct xpon_device *xpon, u32 owner, u32 cookie,
			bool enable)
{
	int ret = 0;

	mutex_lock(&xpon->activation_lock);
	if (!xpon->activation_ops) {
		ret = -EOPNOTSUPP;
		goto out;
	}
	if (owner != xpon->activation_owner) {
		ret = -EPERM;
		goto out;
	}
	if (owner && cookie != xpon->activation_cookie) {
		ret = -ESTALE;
		goto out;
	}
	if (enable) {
		if (!xpon->activation_admin_up) {
			ret = -ENETDOWN;
			goto out;
		}
		if (xpon->activation_reset_pending) {
			ret = -EAGAIN;
			goto out;
		}
		xpon->activation_approved = true;
		WRITE_ONCE(xpon->activation_allowed, true);
		xpon_activation_changed(xpon);
	} else {
		xpon_activation_revoke(xpon);
	}
out:
	mutex_unlock(&xpon->activation_lock);
	return ret;
}
