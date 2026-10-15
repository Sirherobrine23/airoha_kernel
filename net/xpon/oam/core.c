// SPDX-License-Identifier: GPL-2.0-only
/* Per-provider OAM attachment. Wire policy is implemented in userspace. */

#include <linux/etherdevice.h>
#include <linux/err.h>
#include <linux/slab.h>
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

	if (!xpon || !ops || !ops->xmit || !xpon->max_links)
		return ERR_PTR(-EINVAL);
	if (xpon->oam)
		return ERR_PTR(-EBUSY);
	oam = kzalloc(sizeof(*oam), GFP_KERNEL);
	if (!oam)
		return ERR_PTR(-ENOMEM);
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
	/* The provider has already detached and drained its RX handler. */
	if (oam->xpon->oam == oam)
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
	if (oam)
		xpon_link_report(oam->xpon, index + 1, llid, XPON_LINK_REGISTERED, true);
}
EXPORT_SYMBOL_GPL(xpon_oam_llid_registered);

void xpon_oam_llid_unregistered(struct xpon_oam *oam, u8 index)
{
	if (oam)
		xpon_link_report(oam->xpon, index + 1, 0, XPON_LINK_REGISTERING, true);
}
EXPORT_SYMBOL_GPL(xpon_oam_llid_unregistered);

void xpon_oam_receive(struct xpon_oam *oam, struct sk_buff *skb, u8 index)
{
	struct xpon_device *xpon;
	bool registered;

	if (!oam || !skb)
		goto free;
	xpon = oam->xpon;
	if (index >= xpon->max_links || skb->len > XPON_OAM_FRAME_MAX ||
	    !pskb_may_pull(skb, ETH_HLEN + 4) ||
	    ((struct ethhdr *)skb->data)->h_proto != htons(ETH_P_SLOW) ||
	    skb->data[ETH_HLEN] != XPON_OAM_SUBTYPE)
		goto free;
	spin_lock_bh(&xpon->state_lock);
	registered = xpon->links[index].state == XPON_LINK_REGISTERED;
	if (registered)
		xpon->links[index].rx_oam++;
	spin_unlock_bh(&xpon->state_lock);
	if (registered)
		xpon_genl_oam_receive(xpon, index + 1, skb);
free:
	dev_kfree_skb_any(skb);
}
EXPORT_SYMBOL_GPL(xpon_oam_receive);
