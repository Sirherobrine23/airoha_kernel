// SPDX-License-Identifier: GPL-2.0-only
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>

#include "airoha_eth.h"

static int airoha_eth_probe(struct platform_device *pdev)
{
	const struct airoha_eth_soc_data *soc;

	soc = of_device_get_match_data(&pdev->dev);
	if (!soc || !soc->eth_ops || !soc->eth_ops->probe)
		return dev_err_probe(&pdev->dev, -EINVAL,
				     "missing Ethernet backend data\n");

	return soc->eth_ops->probe(pdev);
}

static void airoha_eth_remove(struct platform_device *pdev)
{
	const struct airoha_eth_soc_data *soc;

	soc = of_device_get_match_data(&pdev->dev);
	if (soc && soc->eth_ops && soc->eth_ops->remove)
		soc->eth_ops->remove(pdev);
}

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
