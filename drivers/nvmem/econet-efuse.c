// SPDX-License-Identifier: GPL-2.0-only
/* Read-only access to the EcoNet EN7580 eFuse shadow. */

#include <linux/io.h>
#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/nvmem-provider.h>
#include <linux/platform_device.h>

static int econet_efuse_read(void *context, unsigned int offset,
			     void *val, size_t bytes)
{
	void __iomem *base = context;
	u8 *buf = val;
	size_t i;

	/* Match the byte addressing used by the vendor package-info parser. */
	for (i = 0; i < bytes; i++)
		buf[i] = readb(base + offset + i);

	return 0;
}

static int econet_efuse_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct nvmem_config config = {
		.dev = dev,
		.name = "en7580-efuse",
		.id = NVMEM_DEVID_AUTO,
		.type = NVMEM_TYPE_OTP,
		.read_only = true,
		.root_only = true,
		.stride = 1,
		.word_size = 1,
		.reg_read = econet_efuse_read,
	};
	struct nvmem_device *nvmem;
	struct resource *res;
	void __iomem *base;

	base = devm_platform_get_and_ioremap_resource(pdev, 0, &res);
	if (IS_ERR(base))
		return PTR_ERR(base);

	config.priv = base;
	config.size = resource_size(res);
	nvmem = devm_nvmem_register(dev, &config);

	return PTR_ERR_OR_ZERO(nvmem);
}

static const struct of_device_id econet_efuse_of_match[] = {
	{ .compatible = "econet,en7580-efuse" },
	{ }
};
MODULE_DEVICE_TABLE(of, econet_efuse_of_match);

static struct platform_driver econet_efuse_driver = {
	.probe = econet_efuse_probe,
	.driver = {
		.name = "econet-efuse",
		.of_match_table = econet_efuse_of_match,
	},
};
module_platform_driver(econet_efuse_driver);

MODULE_DESCRIPTION("EcoNet EN7580 eFuse shadow driver");
MODULE_LICENSE("GPL");
