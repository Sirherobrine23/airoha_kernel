// SPDX-License-Identifier: GPL-2.0-only
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/rtnetlink.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <net/net_namespace.h>

#include "airoha_eth.h"

static int airoha_eth_probe(struct platform_device *pdev)
{
	const struct airoha_eth_soc_data *soc;
	struct airoha_eth *eth;
	int err;

	soc = of_device_get_match_data(&pdev->dev);
	if (!soc || !soc->eth_ops || !soc->eth_ops->probe)
		return dev_err_probe(&pdev->dev, -EINVAL,
				     "missing Ethernet backend data\n");

	eth = devm_kzalloc(&pdev->dev, sizeof(*eth), GFP_KERNEL);
	if (!eth)
		return -ENOMEM;

	eth->dev = &pdev->dev;
	eth->soc = soc;
	platform_set_drvdata(pdev, eth);

	err = soc->eth_ops->probe(pdev, eth);
	if (err)
		platform_set_drvdata(pdev, NULL);

	return err;
}

static void airoha_eth_remove(struct platform_device *pdev)
{
	struct airoha_eth *eth = platform_get_drvdata(pdev);

	if (!eth || !eth->soc || !eth->soc->eth_ops ||
	    !eth->soc->eth_ops->remove)
		return;

	eth->soc->eth_ops->remove(pdev, eth);
	platform_set_drvdata(pdev, NULL);
}

static const struct airoha_eth_xpon_ops *
airoha_eth_get_xpon_ops(struct net_device *netdev,
			struct airoha_gdm_common **gdm_out)
{
	struct airoha_gdm_common *gdm;
	struct airoha_eth *eth;

	if (!netdev)
		return NULL;

	gdm = airoha_gdm_common_from_netdev(netdev);
	if (!gdm || gdm->id != 2 || !gdm->eth || !gdm->eth->soc)
		return NULL;

	eth = gdm->eth;
	if (!eth->soc->xpon_ops)
		return NULL;

	if (gdm_out)
		*gdm_out = gdm;

	return eth->soc->xpon_ops;
}

struct net_device *airoha_eth_get_xpon_netdev(void)
{
	struct net_device *netdev, *found = NULL;

	rtnl_lock();
	for_each_netdev(&init_net, netdev) {
		if (!airoha_eth_get_xpon_ops(netdev, NULL))
			continue;

		dev_hold(netdev);
		found = netdev;
		break;
	}
	rtnl_unlock();

	return found;
}
EXPORT_SYMBOL_GPL(airoha_eth_get_xpon_netdev);

int airoha_eth_set_xpon_mode(struct net_device *netdev,
			     enum airoha_xpon_mode mode)
{
	const struct airoha_eth_xpon_ops *ops = airoha_eth_get_xpon_ops(netdev, NULL);

	return ops && ops->set_mode ? ops->set_mode(netdev, mode) : -EOPNOTSUPP;
}
EXPORT_SYMBOL_GPL(airoha_eth_set_xpon_mode);

int airoha_eth_set_xpon_datapath(struct net_device *netdev,
				 enum airoha_xpon_mode mode, bool enable)
{
	const struct airoha_eth_xpon_ops *ops = airoha_eth_get_xpon_ops(netdev, NULL);

	return ops && ops->set_datapath ?
		ops->set_datapath(netdev, mode, enable) : -EOPNOTSUPP;
}
EXPORT_SYMBOL_GPL(airoha_eth_set_xpon_datapath);

int airoha_eth_set_xpon_tcont_channel(struct net_device *netdev,
				      unsigned int channel, bool enable)
{
	const struct airoha_eth_xpon_ops *ops = airoha_eth_get_xpon_ops(netdev, NULL);

	return ops && ops->set_tcont_channel ?
		ops->set_tcont_channel(netdev, channel, enable) : -EOPNOTSUPP;
}
EXPORT_SYMBOL_GPL(airoha_eth_set_xpon_tcont_channel);

int airoha_eth_register_xpon(struct net_device *netdev,
			     enum airoha_xpon_mode mode,
			     const struct airoha_xpon_link_ops *link_ops,
			     void *priv)
{
	const struct airoha_eth_xpon_ops *ops = airoha_eth_get_xpon_ops(netdev, NULL);

	return ops && ops->register_link ?
		ops->register_link(netdev, mode, link_ops, priv) : -EOPNOTSUPP;
}
EXPORT_SYMBOL_GPL(airoha_eth_register_xpon);

void airoha_eth_unregister_xpon(struct net_device *netdev,
				const struct airoha_xpon_link_ops *link_ops,
				void *priv)
{
	const struct airoha_eth_xpon_ops *ops = airoha_eth_get_xpon_ops(netdev, NULL);

	if (ops && ops->unregister_link)
		ops->unregister_link(netdev, link_ops, priv);
}
EXPORT_SYMBOL_GPL(airoha_eth_unregister_xpon);

void airoha_eth_xpon_update_link(struct net_device *netdev,
				 const struct airoha_xpon_link_state *state)
{
	const struct airoha_eth_xpon_ops *ops = airoha_eth_get_xpon_ops(netdev, NULL);

	if (ops && ops->update_link)
		ops->update_link(netdev, state);
}
EXPORT_SYMBOL_GPL(airoha_eth_xpon_update_link);

int airoha_eth_xpon_control_start(struct net_device *netdev)
{
	const struct airoha_eth_xpon_ops *ops = airoha_eth_get_xpon_ops(netdev, NULL);

	return ops && ops->control_start ? ops->control_start(netdev) : -EOPNOTSUPP;
}
EXPORT_SYMBOL_GPL(airoha_eth_xpon_control_start);

void airoha_eth_xpon_control_stop(struct net_device *netdev)
{
	const struct airoha_eth_xpon_ops *ops = airoha_eth_get_xpon_ops(netdev, NULL);

	if (ops && ops->control_stop)
		ops->control_stop(netdev);
}
EXPORT_SYMBOL_GPL(airoha_eth_xpon_control_stop);

void airoha_eth_xpon_dump_oam_rx_state(struct net_device *netdev)
{
	const struct airoha_eth_xpon_ops *ops = airoha_eth_get_xpon_ops(netdev, NULL);

	if (ops && ops->dump_oam_rx_state)
		ops->dump_oam_rx_state(netdev);
}
EXPORT_SYMBOL_GPL(airoha_eth_xpon_dump_oam_rx_state);

int airoha_eth_register_xpon_oam(struct net_device *netdev,
				 struct airoha_xpon_oam_handler *handler)
{
	const struct airoha_eth_xpon_ops *ops = airoha_eth_get_xpon_ops(netdev, NULL);

	return ops && ops->register_oam ?
		ops->register_oam(netdev, handler) : -EOPNOTSUPP;
}
EXPORT_SYMBOL_GPL(airoha_eth_register_xpon_oam);

void airoha_eth_unregister_xpon_oam(struct net_device *netdev,
				    struct airoha_xpon_oam_handler *handler)
{
	const struct airoha_eth_xpon_ops *ops = airoha_eth_get_xpon_ops(netdev, NULL);

	if (ops && ops->unregister_oam)
		ops->unregister_oam(netdev, handler);
}
EXPORT_SYMBOL_GPL(airoha_eth_unregister_xpon_oam);

int airoha_eth_xmit_xpon_oam(struct net_device *netdev, struct sk_buff *skb,
			     u16 gem_port_id)
{
	const struct airoha_eth_xpon_ops *ops = airoha_eth_get_xpon_ops(netdev, NULL);

	return ops && ops->xmit_oam ?
		ops->xmit_oam(netdev, skb, gem_port_id) : -EOPNOTSUPP;
}
EXPORT_SYMBOL_GPL(airoha_eth_xmit_xpon_oam);

int airoha_eth_xpon_add_service(struct net_device *netdev,
				const struct airoha_xpon_service_cfg *cfg)
{
	const struct airoha_eth_xpon_ops *ops = airoha_eth_get_xpon_ops(netdev, NULL);

	return ops && ops->add_service ?
		ops->add_service(netdev, cfg) : -EOPNOTSUPP;
}
EXPORT_SYMBOL_GPL(airoha_eth_xpon_add_service);

int airoha_eth_xpon_get_tx_info(struct net_device *netdev, bool vlan_valid,
				u16 vlan_id, bool pcp_valid, u8 pcp,
				struct airoha_xpon_tx_info *info)
{
	const struct airoha_eth_xpon_ops *ops = airoha_eth_get_xpon_ops(netdev, NULL);

	return ops && ops->get_tx_info ?
		ops->get_tx_info(netdev, vlan_valid, vlan_id, pcp_valid, pcp, info) :
		-EOPNOTSUPP;
}
EXPORT_SYMBOL_GPL(airoha_eth_xpon_get_tx_info);

bool airoha_eth_xpon_del_service(struct net_device *netdev, u32 cookie,
				 u16 *gem_port_id)
{
	const struct airoha_eth_xpon_ops *ops = airoha_eth_get_xpon_ops(netdev, NULL);

	return ops && ops->del_service ?
		ops->del_service(netdev, cookie, gem_port_id) : false;
}
EXPORT_SYMBOL_GPL(airoha_eth_xpon_del_service);

bool airoha_eth_xpon_has_gem_service(struct net_device *netdev, u16 gem_port_id)
{
	const struct airoha_eth_xpon_ops *ops = airoha_eth_get_xpon_ops(netdev, NULL);

	return ops && ops->has_gem_service ?
		ops->has_gem_service(netdev, gem_port_id) : false;
}
EXPORT_SYMBOL_GPL(airoha_eth_xpon_has_gem_service);

void airoha_eth_xpon_flush_services(struct net_device *netdev)
{
	const struct airoha_eth_xpon_ops *ops = airoha_eth_get_xpon_ops(netdev, NULL);

	if (ops && ops->flush_services)
		ops->flush_services(netdev);
}
EXPORT_SYMBOL_GPL(airoha_eth_xpon_flush_services);

static const struct of_device_id airoha_eth_of_match[] = {
	{ .compatible = "econet,en751221-eth", .data = &econet_en751221_soc_data },
	{ .compatible = "econet,en7528-eth", .data = &econet_en7528_soc_data },
	{ .compatible = "airoha,en7523-eth", .data = &airoha_en7523_soc_data },
	{ .compatible = "airoha,en7581-eth", .data = &airoha_en7581_soc_data },
	{ .compatible = "airoha,an7583-eth", .data = &airoha_an7583_soc_data },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, airoha_eth_of_match);

static struct platform_driver airoha_eth_driver = {
	.probe = airoha_eth_probe,
	.remove = airoha_eth_remove,
	.driver = {
		.name = "airoha-eth",
		.of_match_table = airoha_eth_of_match,
	},
};
module_platform_driver(airoha_eth_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Lorenzo Bianconi <lorenzo@kernel.org>");
MODULE_AUTHOR("Caleb James DeLisle <cjd@cjdns.fr>");
MODULE_AUTHOR("Matheus Sampaio Queiroga <srherobrine20@gmail.com>");
MODULE_DESCRIPTION("Airoha and EcoNet frame-engine Ethernet driver");
