// SPDX-License-Identifier: GPL-2.0-only

#include <linux/err.h>
#include <linux/slab.h>
#include <linux/skbuff.h>
#include <net/xpon.h>
#include <net/xpon/oam.h>

#include "../internal.h"
#include "internal.h"

struct xpon_oam *xpon_oam_register(struct xpon_device *xpon,
				   const struct xpon_oam_ops *ops,
				   void *priv)
{
	struct xpon_oam *oam;
	int ret;

	if (!xpon || !ops || !ops->xmit)
		return ERR_PTR(-EINVAL);
	if (xpon->oam)
		return ERR_PTR(-EBUSY);

	oam = kzalloc(sizeof(*oam), GFP_KERNEL);
	if (!oam)
		return ERR_PTR(-ENOMEM);

	mutex_init(&oam->lock);
	oam->xpon = xpon;
	oam->ops = ops;
	oam->priv = priv;

	ret = xpon_oam_sysfs_register(oam);
	if (ret) {
		kfree(oam);
		return ERR_PTR(ret);
	}

	xpon->oam = oam;
	xpon_device_report_event(xpon, XPON_EVENT_OAM);
	return oam;
}
EXPORT_SYMBOL_GPL(xpon_oam_register);

void xpon_oam_unregister(struct xpon_oam *oam)
{
	if (!oam)
		return;

	if (oam->xpon && oam->xpon->oam == oam)
		oam->xpon->oam = NULL;
	xpon_oam_sysfs_unregister(oam);
	kfree(oam);
}
EXPORT_SYMBOL_GPL(xpon_oam_unregister);

void *xpon_oam_priv(struct xpon_oam *oam)
{
	return oam ? oam->priv : NULL;
}
EXPORT_SYMBOL_GPL(xpon_oam_priv);

void xpon_oam_llid_registered(struct xpon_oam *oam, u8 index, u16 llid)
{
	if (!oam || index >= XPON_OAM_MAX_LLID)
		return;

	mutex_lock(&oam->lock);
	oam->llids[index].llid = llid;
	oam->llids[index].registered = true;
	memset(&oam->llids[index].remote, 0,
	       sizeof(oam->llids[index].remote));
	mutex_unlock(&oam->lock);
	xpon_device_report_event(oam->xpon, XPON_EVENT_LLID | XPON_EVENT_OAM);
}
EXPORT_SYMBOL_GPL(xpon_oam_llid_registered);

void xpon_oam_llid_unregistered(struct xpon_oam *oam, u8 index)
{
	if (!oam || index >= XPON_OAM_MAX_LLID)
		return;

	mutex_lock(&oam->lock);
	memset(&oam->llids[index], 0, sizeof(oam->llids[index]));
	mutex_unlock(&oam->lock);
	xpon_device_report_event(oam->xpon, XPON_EVENT_LLID | XPON_EVENT_OAM);
}
EXPORT_SYMBOL_GPL(xpon_oam_llid_unregistered);

void xpon_oam_receive(struct xpon_oam *oam, struct sk_buff *skb,
		      u8 llid_index)
{
	if (!oam || !skb || llid_index >= XPON_OAM_MAX_LLID) {
		kfree_skb(skb);
		return;
	}

	if (!oam->llids[llid_index].registered) {
		kfree_skb(skb);
		return;
	}

	oam->llids[llid_index].rx_oampdu++;
	if (xpon_oam_wire_receive(oam, skb, llid_index))
		kfree_skb(skb);
}
EXPORT_SYMBOL_GPL(xpon_oam_receive);
