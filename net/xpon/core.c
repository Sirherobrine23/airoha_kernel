// SPDX-License-Identifier: GPL-2.0-only
/*
 * Generic xPON core
 *
 * GPON, EPON and XGS-PON use different registration and management wire
 * protocols, but share one datapath object, optical state and set of system
 * consumers. Protocol providers translate their native state into this core.
 */

#include <linux/err.h>
#include <linux/etherdevice.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/optical_frontend.h>
#include <linux/slab.h>
#include <linux/rtnetlink.h>
#include <linux/optical_frontend.h>
#include <linux/property.h>
#include <linux/string.h>
#include <net/net_namespace.h>
#include <net/xpon.h>

#include "internal.h"

static LIST_HEAD(xpon_devices);
static DEFINE_MUTEX(xpon_devices_lock);

static void xpon_opticals_cleanup(struct xpon_device *xpon)
{
	u16 i;

	for (i = 0; i < xpon->num_optical; i++) {
		struct xpon_optical_path *path = xpon->opticals[i];

		put_device(path->dev);
		kfree(path);
	}
	kfree(xpon->opticals);
	xpon->opticals = NULL;
	xpon->num_optical = 0;
}

static void xpon_release(struct kref *ref)
{
	struct xpon_device *xpon = container_of(ref, struct xpon_device, ref);

	xa_destroy(&xpon->endpoints);
	xa_destroy(&xpon->rx_endpoints);
	xa_destroy(&xpon->pending_endpoints);
	xpon_opticals_cleanup(xpon);
	kfree(xpon->links);
	kfree(xpon->owned_priv);
	kfree(xpon);
}

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

	rtnl_lock();
	if (!xpon->unregistering)
		xpon_endpoints_update_carrier(xpon);
	rtnl_unlock();
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

/**
 * xpon_device_alloc() - allocate a generic xPON instance and private area
 * @parent: hardware device owning the instance
 * @priv_size: size of the driver-private area, or zero for external ownership
 *
 * The private area is freed only after the last endpoint reference goes away.
 * The caller must call xpon_device_free() if registration never succeeds,
 * otherwise xpon_device_unregister() after quiescing the hardware.
 */
struct xpon_device *xpon_device_alloc(struct device *parent, int priv_size)
{
	struct xpon_device *xpon;

	if (!parent || priv_size < 0)
		return ERR_PTR(-EINVAL);
	xpon = kzalloc(sizeof(*xpon), GFP_KERNEL);
	if (!xpon)
		return ERR_PTR(-ENOMEM);
	if (priv_size) {
		xpon->owned_priv = kzalloc(priv_size, GFP_KERNEL);
		if (!xpon->owned_priv) {
			kfree(xpon);
			return ERR_PTR(-ENOMEM);
		}
		xpon->priv = xpon->owned_priv;
	}
	INIT_LIST_HEAD(&xpon->list);
	kref_init(&xpon->ref);
	xa_init(&xpon->endpoints);
	xa_init(&xpon->rx_endpoints);
	xpon_endpoint_work_init(xpon);
	spin_lock_init(&xpon->state_lock);
	mutex_init(&xpon->mode_lock);
	INIT_WORK(&xpon->notify_work, xpon_notify_work);
	BLOCKING_INIT_NOTIFIER_HEAD(&xpon->notifier);
	xpon->parent = parent;
	return xpon;
}
EXPORT_SYMBOL_GPL(xpon_device_alloc);

void xpon_device_free(struct xpon_device *xpon)
{
	if (!xpon || WARN_ON_ONCE(xpon->registered))
		return;
	xpon_device_put(xpon);
}
EXPORT_SYMBOL_GPL(xpon_device_free);

/* Describe the board's optical paths in optical-frontend-names. */
static unsigned long xpon_optical_named_modes(const char *name,
					      unsigned long all_modes)
{
	if (!name || !strcmp(name, "pon"))
		return all_modes;
	if (!strcmp(name, "gpon"))
		return XPON_MODE_GPON | XPON_MODE_EPON;
	if (!strcmp(name, "xgspon"))
		return XPON_MODE_XGPON | XPON_MODE_XGSPON |
		       XPON_MODE_XEPON | XPON_MODE_NGPON2;
	if (!strcmp(name, "epon"))
		return XPON_MODE_EPON;
	if (!strcmp(name, "xgpon"))
		return XPON_MODE_XGPON;
	if (!strcmp(name, "xepon"))
		return XPON_MODE_XEPON;
	if (!strcmp(name, "ngpon2"))
		return XPON_MODE_NGPON2;

	return 0;
}

static struct xpon_optical_path *
xpon_optical_for_mode(struct xpon_device *xpon, enum xpon_mode mode)
{
	u16 i;

	for (i = 0; i < xpon->num_optical; i++) {
		struct xpon_optical_path *path = xpon->opticals[i];

		if (path->modes & XPON_MODE_CAP(mode))
			return path;
	}
	return NULL;
}

/* Update while holding mode_lock. No provider is reconfigured here. */
static void xpon_opticals_set_active(struct xpon_device *xpon,
				     struct xpon_optical_path *selected)
{
	u16 i;

	for (i = 0; i < xpon->num_optical; i++)
		WRITE_ONCE(xpon->opticals[i]->active,
			   xpon->opticals[i] == selected);
}

static struct xpon_optical_path *xpon_optical_current(struct xpon_device *xpon)
{
	u16 i;

	for (i = 0; i < xpon->num_optical; i++) {
		struct xpon_optical_path *path = xpon->opticals[i];

		if (READ_ONCE(path->active))
			return path;
	}
	return NULL;
}

/* The parent owns devm frontend consumer references; the xPON instance
 * additionally pins each class device until its final kref is released.
 * All optical selection, including a single legacy "pon" provider, is here.
 */
static int xpon_opticals_init(struct xpon_device *xpon)
{
	struct device *dev = xpon->parent;
	struct xpon_optical_path *path;
	struct optical_frontend *frontend;
	const char *name;
	unsigned long modes;
	int count, ret, i, j;

	count = device_property_count_u32(dev, "optical-frontend-names");
	if (count < 0 && count != -EINVAL && count != -ENOENT &&
	    count != -ENODATA)
		return count;
	/* Without named paths, retain the DT's first unnamed provider. */
	if (count <= 0)
		count = 1;

	xpon->opticals = kcalloc(count, sizeof(*xpon->opticals), GFP_KERNEL);
	if (!xpon->opticals)
		return -ENOMEM;

	for (i = 0; i < count; i++) {
		ret = of_property_read_string_index(dev->of_node,
					"optical-frontend-names", i, &name);
		if (ret) {
			if (i || (ret != -EINVAL && ret != -ENOENT &&
				  ret != -ENODATA))
				return ret;
			name = NULL;
		}

		modes = xpon_optical_named_modes(name, xpon->modes);
		if (!modes)
			return dev_err_probe(dev, -EINVAL,
				"unsupported optical frontend name %s\n",
				name ?: "<none>");
		/* A protocol must never silently select the first of two BOBs. */
		for (j = 0; j < xpon->num_optical; j++) {
			if (xpon->opticals[j]->modes & modes)
				return dev_err_probe(dev, -EINVAL,
					"optical path %s overlaps another path\n",
					name ?: "<none>");
		}

		frontend = devm_optical_frontend_get_optional(dev, name);
		if (IS_ERR(frontend))
			return PTR_ERR(frontend);
		if (!frontend) {
			if (name)
				return dev_err_probe(dev, -EINVAL,
					"optical frontend %s is not referenced\n", name);
			continue;
		}

		path = kzalloc(sizeof(*path), GFP_KERNEL);
		if (!path)
			return -ENOMEM;
		path->frontend = frontend;
		path->dev = get_device(optical_frontend_get_device(frontend));
		if (!path->dev) {
			kfree(path);
			return -ENODEV;
		}
		path->modes = modes;
		xpon->opticals[xpon->num_optical++] = path;
	}

	if (xpon->num_optical &&
	    !xpon_optical_for_mode(xpon, xpon->state.mode))
		return dev_err_probe(dev, -EINVAL,
				"no optical path supports initial PON mode\n");

	xpon_opticals_set_active(xpon,
			xpon_optical_for_mode(xpon, xpon->state.mode));
	return 0;
}

/**
 * xpon_device_setup() - configure a device allocated with xpon_device_alloc()
 *
 * @priv may point to externally managed driver state or be NULL, in which case
 * xpon_device_priv() exposes the private bytes allocated with the device.
 */
int xpon_device_setup(struct xpon_device *xpon, struct net_device *netdev,
		      const struct xpon_device_ops *ops, void *priv,
		      enum xpon_mode mode, unsigned long modes)
{
	if (!xpon || !netdev || !ops || !xpon_mode_valid(mode) ||
	    (modes & ~((unsigned long)XPON_MODE_MASK)) ||
	    !(modes & XPON_MODE_CAP(mode)) || xpon->registered ||
	    (xpon->owned_priv && priv && priv != xpon->owned_priv))
		return -EINVAL;

	xpon->netdev = netdev;
	xpon->ops = ops;
	if (priv)
		xpon->priv = priv;
	xpon->modes = modes;
	xpon->state.mode = mode;
	xpon->state.registration = XPON_REGISTRATION_DOWN;
	xpon->notify_old = xpon->state;
	xpon->configured = true;
	return 0;
}
EXPORT_SYMBOL_GPL(xpon_device_setup);

/* Build a stable capability table before touching hardware. Preallocating the
 * largest LLID table avoids kfree/realloc while switching between GPON (no
 * LLIDs) and EPON. A provider may omit get_limits only if it uses no links,
 * endpoints or queues. A failing advertised mode makes registration fail.
 */
static int xpon_mode_limits_init(struct xpon_device *xpon)
{
	struct xpon_device_limits limits;
	unsigned long modes = xpon->modes;
	unsigned int bit;
	int ret;

	xpon->link_capacity = 0;
	for_each_set_bit(bit, &modes, BITS_PER_LONG) {
		memset(&limits, 0, sizeof(limits));
		if (xpon->ops->get_limits) {
			ret = xpon->ops->get_limits(xpon, BIT(bit), &limits);
			if (ret)
				return ret;
		}
		if (limits.max_endpoints && !limits.max_queues)
			return -EINVAL;
		xpon->mode_limits[bit] = limits;
		xpon->link_capacity = max(xpon->link_capacity,
					  limits.max_links);
	}
	xpon->limits = xpon->mode_limits[__ffs(xpon->state.mode)];
	return 0;
}

int xpon_device_set_leds(struct xpon_device *xpon,
			 struct led_classdev *pon, struct led_classdev *los,
			 struct led_classdev *fiber)
{
	if (!xpon || xpon->registered)
		return -EINVAL;
	xpon->pon_led = pon;
	xpon->los_led = los;
	xpon->fiber_led = fiber;
	return 0;
}
EXPORT_SYMBOL_GPL(xpon_device_set_leds);

/**
 * xpon_device_register_allocated() - publish a configured xPON device
 *
 * On failure, the object remains caller-owned and must be freed. The driver
 * owns unwinding of hardware initialized by hw_prepare on later errors.
 */
int xpon_device_register_allocated(struct xpon_device *xpon)
{
	int ret;

	if (!xpon || xpon->registered || !xpon->configured ||
	    !xpon->netdev || !xpon->ops)
		return -EINVAL;

	ret = xpon_mode_limits_init(xpon);
	if (ret)
		return ret;
	ret = xpon_opticals_init(xpon);
	if (ret)
		goto err_optical;
	if (xpon->link_capacity) {
		xpon->links = kcalloc(xpon->link_capacity, sizeof(*xpon->links),
					 GFP_KERNEL);
		if (!xpon->links) {
			ret = -ENOMEM;
			goto err_optical;
		}
	}
	if (xpon->ops->hw_prepare) {
		ret = xpon->ops->hw_prepare(xpon, xpon->state.mode);
		if (ret)
			goto err_links;
	}
	ret = xpon_unis_init(xpon);
	if (ret)
		goto err_links;
	ret = xpon_leds_register(xpon);
	if (ret)
		goto err_uni;
	ret = xpon_sysfs_register(xpon);
	if (ret)
		goto err_uni;

	mutex_lock(&xpon_devices_lock);
	list_add_tail(&xpon->list, &xpon_devices);
	xpon->registered = true;
	mutex_unlock(&xpon_devices_lock);
	xpon_leds_update(xpon, &xpon->state);
	dev_info(xpon->parent, "registered xPON device for %s\n",
		 xpon->netdev->name);
	return 0;

err_uni:
	xpon_unis_cleanup(xpon);
err_links:
	kfree(xpon->links);
	xpon->links = NULL;
err_optical:
	xpon_opticals_cleanup(xpon);
	return ret;
}
EXPORT_SYMBOL_GPL(xpon_device_register_allocated);

void xpon_device_unregister(struct xpon_device *xpon)
{
	if (!xpon)
		return;

	rtnl_lock();
	if (WARN_ON_ONCE(!xpon->registered)) {
		rtnl_unlock();
		return;
	}
	WRITE_ONCE(xpon->unregistering, true);
	mutex_lock(&xpon_devices_lock);
	list_del_init(&xpon->list);
	xpon->registered = false;
	mutex_unlock(&xpon_devices_lock);
	xpon_endpoints_cleanup(xpon);
	rtnl_unlock();
	cancel_work_sync(&xpon->notify_work);
	xpon_endpoint_work_cleanup(xpon);
	xpon_sysfs_unregister(xpon);
	xpon_unis_cleanup(xpon);
	xpon_device_put(xpon);
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

/* Profile changes are staged for the next XEPON entry. This avoids
 * reprogramming PHY PLLs while an XEPON session is powered.
 */
enum xpon_xepon_profile xpon_device_xepon_profile(struct xpon_device *xpon)
{
	return xpon ? READ_ONCE(xpon->xepon_profile) : XPON_XEPON_10G_10G;
}
EXPORT_SYMBOL_GPL(xpon_device_xepon_profile);

int xpon_device_set_xepon_profile(struct xpon_device *xpon,
				  enum xpon_xepon_profile profile)
{
	int ret = 0;

	if (!xpon || profile > XPON_XEPON_1G_1G)
		return -EINVAL;
	mutex_lock(&xpon->mode_lock);
	if (!xpon->registered || xpon->unregistering ||
	    xpon->mode_changing || xpon->mode_faulted ||
	    READ_ONCE(xpon->state.mode) == XPON_MODE_XEPON ||
	    netif_running(xpon->netdev))
		ret = -EBUSY;
	else
		WRITE_ONCE(xpon->xepon_profile, profile);
	mutex_unlock(&xpon->mode_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(xpon_device_set_xepon_profile);

int xpon_device_get_limits(struct xpon_device *xpon,
			   struct xpon_device_limits *limits)
{
	unsigned long flags;

	if (!xpon || !limits || !xpon->registered)
		return -EINVAL;
	spin_lock_irqsave(&xpon->state_lock, flags);
	*limits = xpon->limits;
	spin_unlock_irqrestore(&xpon->state_lock, flags);
	return 0;
}
EXPORT_SYMBOL_GPL(xpon_device_get_limits);

int xpon_device_get_hw_mode(struct xpon_device *xpon, enum xpon_mode *mode)
{
	int ret;

	if (!xpon || !mode)
		return -EINVAL;
	if (!xpon->ops || !xpon->ops->get_mode)
		return -EOPNOTSUPP;
	mutex_lock(&xpon->mode_lock);
	ret = xpon->ops->get_mode(xpon, mode);
	if (!ret && !xpon_mode_valid(*mode))
		ret = -ERANGE;
	mutex_unlock(&xpon->mode_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(xpon_device_get_hw_mode);

struct optical_frontend *xpon_device_optical_frontend(struct xpon_device *xpon)
{
	struct xpon_optical_path *path;

	if (!xpon)
		return NULL;
	path = xpon_optical_current(xpon);
	return path ? path->frontend : NULL;
}
EXPORT_SYMBOL_GPL(xpon_device_optical_frontend);

struct device *xpon_device_optical(struct xpon_device *xpon)
{
	struct xpon_optical_path *path;

	if (!xpon)
		return NULL;
	path = xpon_optical_current(xpon);
	return path ? path->dev : NULL;
}
EXPORT_SYMBOL_GPL(xpon_device_optical);

struct optical_frontend *xpon_device_optical_frontend_for_mode(
				struct xpon_device *xpon, enum xpon_mode mode)
{
	struct xpon_optical_path *path;

	if (!xpon || !xpon_mode_valid(mode))
		return NULL;
	path = xpon_optical_for_mode(xpon, mode);
	return path ? path->frontend : NULL;
}
EXPORT_SYMBOL_GPL(xpon_device_optical_frontend_for_mode);

bool xpon_device_mode_faulted(struct xpon_device *xpon)
{
	return !xpon || READ_ONCE(xpon->mode_faulted);
}
EXPORT_SYMBOL_GPL(xpon_device_mode_faulted);

static void xpon_commit_mode(struct xpon_device *xpon, enum xpon_mode mode)
{
	struct xpon_optical_path *prev = xpon_optical_current(xpon);
	struct xpon_optical_path *next = xpon_optical_for_mode(xpon, mode);
	enum xpon_mode old = READ_ONCE(xpon->state.mode);
	unsigned long flags;
	bool had_links;

	/* hw_pre_shutdown has drained the old provider. Still serialize against
	 * outstanding LLID reporters and readers of the state/limits snapshot.
	 */
	spin_lock_irqsave(&xpon->state_lock, flags);
	had_links = xpon->limits.max_links ||
		    xpon->mode_limits[__ffs(mode)].max_links;
	if (xpon->links)
		memset(xpon->links, 0,
		       xpon->link_capacity * sizeof(*xpon->links));
	xpon->limits = xpon->mode_limits[__ffs(mode)];
	WRITE_ONCE(xpon->state.mode, mode);
	spin_unlock_irqrestore(&xpon->state_lock, flags);

	xpon_opticals_set_active(xpon, next);
	if (prev != next)
		xpon_sysfs_optical_changed(xpon, prev ? prev->dev : NULL);
	if (old != mode)
		xpon_queue_event(xpon, XPON_EVENT_MODE);
	if (had_links)
		xpon_queue_event(xpon, XPON_EVENT_LLID);
}

int xpon_device_set_mode(struct xpon_device *xpon, enum xpon_mode mode)
{
	enum xpon_mode old_mode, recovered;
	int ret = 0, fallback_ret;

	if (!xpon || !xpon_mode_valid(mode))
		return -EINVAL;
	mutex_lock(&xpon->mode_lock);
	if (xpon->unregistering || !xpon->registered ||
	    xpon->mode_changing || xpon->mode_faulted) {
		ret = -EBUSY;
		goto out;
	}
	if (!(xpon->modes & XPON_MODE_CAP(mode)) ||
	    (xpon->num_optical && !xpon_optical_for_mode(xpon, mode))) {
		ret = -EOPNOTSUPP;
		goto out;
	}
	if (netif_running(xpon->netdev) || !xa_empty(&xpon->endpoints) ||
	    !xa_empty(&xpon->pending_endpoints)) {
		ret = -EBUSY;
		goto out;
	}
	old_mode = READ_ONCE(xpon->state.mode);
	if (old_mode == mode)
		goto out;

	/* Legacy set_mode implementations alone cannot safely change live
	 * protocol services; require an explicit teardown and recovery contract.
	 */
	if (!xpon->ops || !xpon->ops->hw_pre_shutdown ||
	    !xpon->ops->set_mode || !xpon->ops->hw_fallback) {
		ret = -EOPNOTSUPP;
		goto out;
	}

	xpon->transition_target = mode;
	xpon->mode_changing = true;
	xpon_device_report_carrier(xpon, false);
	xpon_device_report_registration(xpon, XPON_REGISTRATION_DOWN);
	ret = xpon->ops->hw_pre_shutdown(xpon, old_mode, mode);
	if (!ret)
		ret = xpon->ops->set_mode(xpon, mode);
	if (!ret) {
		xpon_commit_mode(xpon, mode);
		goto out_changing;
	}

	/* A failed teardown may already have modified the old hardware. The
	 * driver must either rebuild a safe mode or report unrecoverable failure.
	 */
	recovered = old_mode;
	fallback_ret = xpon->ops->hw_fallback(xpon, old_mode, mode,
					    &recovered);
	if (fallback_ret || !xpon_mode_valid(recovered) ||
	    !(xpon->modes & XPON_MODE_CAP(recovered)) ||
	    (xpon->num_optical && !xpon_optical_for_mode(xpon, recovered))) {
		WRITE_ONCE(xpon->mode_faulted, true);
		dev_err(xpon->parent,
			"xPON mode transition failed (%d), recovery failed (%d)\n",
			ret, fallback_ret ?: -EINVAL);
	} else {
		xpon_commit_mode(xpon, recovered);
		dev_warn(xpon->parent,
			 "xPON mode transition failed (%d), recovered mode %u\n",
			 ret, recovered);
	}

out_changing:
	xpon->mode_changing = false;
	xpon->transition_target = READ_ONCE(xpon->state.mode);
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
		if (dev_net(xpon->netdev) == &init_net &&
		    xpon->netdev->ifindex == ifindex) {
			kref_get(&xpon->ref);
			mutex_unlock(&xpon_devices_lock);
			return xpon;
		}
	}
	mutex_unlock(&xpon_devices_lock);
	return NULL;
}

void xpon_device_put(struct xpon_device *xpon)
{
	if (xpon)
		kref_put(&xpon->ref, xpon_release);
}

int xpon_oam_transmit(struct xpon_device *xpon, u32 id, const void *frame,
		      size_t len)
{
	const struct ethhdr *eth = frame;
	struct sk_buff *skb;
	bool registered;
	int ret;

	ASSERT_RTNL();
	if (!xpon->oam || !xpon->ops || !xpon->ops->oam_xmit)
		return -EOPNOTSUPP;
	if (!id || id > xpon->limits.max_links || len < ETH_HLEN + 4 ||
	    len > XPON_OAM_FRAME_MAX || eth->h_proto != htons(ETH_P_SLOW) ||
	    ((const u8 *)frame)[ETH_HLEN] != 3)
		return -EINVAL;
	spin_lock_bh(&xpon->state_lock);
	registered = xpon->links[id - 1].state == XPON_LINK_REGISTERED;
	spin_unlock_bh(&xpon->state_lock);
	if (!registered)
		return -ENOLINK;
	skb = alloc_skb(max_t(size_t, len, ETH_ZLEN) + NET_SKB_PAD, GFP_KERNEL);
	if (!skb)
		return -ENOMEM;
	skb_reserve(skb, NET_SKB_PAD);
	skb_put_data(skb, frame, len);
	if (len < ETH_ZLEN)
		skb_put_zero(skb, ETH_ZLEN - len);
	skb->dev = xpon->netdev;
	skb_reset_mac_header(skb);
	skb->protocol = htons(ETH_P_SLOW);
	ret = xpon->ops->oam_xmit(xpon, skb, id);
	if (ret) {
		kfree_skb(skb);
	} else {
		spin_lock_bh(&xpon->state_lock);
		xpon->links[id - 1].tx_oam++;
		spin_unlock_bh(&xpon->state_lock);
	}
	return ret;
}

void xpon_link_report(struct xpon_device *xpon, u32 id, u16 llid, u8 state,
		      bool enabled)
{
	unsigned long flags;

	if (!xpon || !id)
		return;
	spin_lock_irqsave(&xpon->state_lock, flags);
	if (id > xpon->limits.max_links) {
		spin_unlock_irqrestore(&xpon->state_lock, flags);
		return;
	}
	xpon->links[id - 1].llid = llid;
	xpon->links[id - 1].state = state;
	xpon->links[id - 1].enabled = enabled;
	xpon->pending_events |= XPON_EVENT_LLID;
	spin_unlock_irqrestore(&xpon->state_lock, flags);
	schedule_work(&xpon->notify_work);
}
EXPORT_SYMBOL_GPL(xpon_link_report);

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
