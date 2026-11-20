// SPDX-License-Identifier: GPL-2.0-only
/* GPON activation follows the administrative state of the PON netdevice. */

#include <linux/module.h>
#include <linux/netdevice.h>

#include "internal.h"

static void xpon_activation_changed(struct xpon_device *xpon)
{
	xpon_device_report_event(xpon, XPON_EVENT_ACTIVATION);
}

void xpon_activation_init(struct xpon_device *xpon)
{
	mutex_init(&xpon->activation_lock);
}

bool xpon_device_activation_allowed(struct xpon_device *xpon)
{
	return xpon && READ_ONCE(xpon->activation_admin_up) &&
	       netif_running(xpon->netdev);
}
EXPORT_SYMBOL_GPL(xpon_device_activation_allowed);

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

	if (netif_running(xpon->netdev)) {
		ret = ops->set_admin(xpon, true);
		if (ret)
			goto out;
		xpon->activation_admin_up = true;
	}
	xpon->activation_ops = ops;
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
	if (xpon->activation_ops) {
		WRITE_ONCE(xpon->activation_admin_up, false);
		xpon->activation_ops->set_admin(xpon, false);
	}
	xpon->activation_ops = NULL;
	xpon_activation_changed(xpon);
	mutex_unlock(&xpon->activation_lock);
}
EXPORT_SYMBOL_GPL(xpon_device_activation_detach);

void xpon_activation_admin(struct xpon_device *xpon, bool up)
{
	int ret;

	mutex_lock(&xpon->activation_lock);
	if (!xpon->activation_ops || xpon->activation_admin_up == up)
		goto out;

	if (!up)
		WRITE_ONCE(xpon->activation_admin_up, false);

	ret = xpon->activation_ops->set_admin(xpon, up);
	if (ret)
		dev_err(xpon->parent,
			"failed to change PON administrative state: %d\n", ret);

	if (up) {
		if (!ret)
			WRITE_ONCE(xpon->activation_admin_up, true);
	} else {
		/* The provider must queue teardown, not wait for its FSM. */
		xpon->activation_ops->deactivate(xpon);
	}

	xpon_activation_changed(xpon);
out:
	mutex_unlock(&xpon->activation_lock);
}
