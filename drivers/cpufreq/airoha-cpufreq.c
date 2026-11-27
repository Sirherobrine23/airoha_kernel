// SPDX-License-Identifier: GPL-2.0

#include <linux/clk.h>
#include <linux/cpufreq.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>
#include <linux/pm_runtime.h>
#include <linux/slab.h>
#include <linux/soc/airoha/pkgids.h>
#include <dt-bindings/soc/airoha,soc-variant.h>

#include "cpufreq-dt.h"

struct airoha_hw_clocks {
	enum airoha_pkg_ids id;
	unsigned int max_support_hw;
};

struct airoha_hw_data {
	const struct airoha_hw_clocks *clocks;
	size_t clocks_size;
	bool use_pmdomain;
};

struct airoha_cpufreq_priv {
	int opp_token;
	struct dev_pm_domain_list *pd_list;
	struct platform_device *cpufreq_dt;
	unsigned int supported_hw[1];
};

static struct platform_device *cpufreq_pdev;

static const char *airoha_cpufreq_max_freq_str(unsigned int hw)
{
	switch (hw) {
	case AIROHA_CPU_HW_C:
		return "1.0 GHz";
	case AIROHA_CPU_HW_D:
		return "1.2 GHz";
	case AIROHA_CPU_HW_G:
		return "1.3 GHz";
	case AIROHA_CPU_HW_P:
		return "1.4 GHz";
	case AIROHA_CPU_HW_S:
	case AIROHA_CPU_HW_I:
		return "2.0 GHz";
	default:
		return "unknown";
	}
}

/* NOP function to disable OPP from setting clock */
static int airoha_cpufreq_config_clks_nop(struct device *dev,
					  struct opp_table *opp_table,
					  struct dev_pm_opp *opp,
					  void *data, bool scaling_down)
{
	return 0;
}

static const char * const airoha_cpufreq_clk_names[] = { "cpu", NULL };
static const char * const airoha_cpufreq_pd_names[] = { "perf" };

static int airoha_cpufreq_probe(struct platform_device *pdev)
{
	const struct dev_pm_domain_attach_data attach_data = {
		.pd_names = airoha_cpufreq_pd_names,
		.num_pd_names = ARRAY_SIZE(airoha_cpufreq_pd_names),
		.pd_flags = PD_FLAG_DEV_LINK_ON | PD_FLAG_REQUIRED_OPP,
	};
	struct dev_pm_opp_config config = {
		.clk_names = airoha_cpufreq_clk_names,
	};
	struct platform_device *cpufreq_dt;
	struct airoha_cpufreq_priv *priv;
	const struct of_device_id *match;
	const struct airoha_hw_data *data;
	struct device *dev = &pdev->dev;
	struct device *cpu_dev;
	int ret;

	/* CPUs refer to the same OPP table */
	cpu_dev = get_cpu_device(0);
	if (!cpu_dev)
		return -ENODEV;

	match = dev_get_platdata(dev);
	if (!match || !match->data)
		return dev_err_probe(dev, -EINVAL, "cannot get SoC data\n");
	data = match->data;

	if (data->use_pmdomain)
		config.config_clks = airoha_cpufreq_config_clks_nop;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	if (data->clocks_size) {
		enum airoha_pkg_ids pkgid;
		bool pkgid_matched = false;
		struct clk *cpu_clk;
		size_t i;

		/* Wait for the clock provider before reading its cached ID */
		cpu_clk = clk_get(cpu_dev, "cpu");
		if (IS_ERR(cpu_clk))
			return dev_err_probe(dev, PTR_ERR(cpu_clk),
					     "CPU clock is not available\n");
		clk_put(cpu_clk);

		/* Safe fallback: expose only the lowest frequency class */
		priv->supported_hw[0] = AIROHA_CPU_HW_C;
		config.supported_hw = priv->supported_hw;
		config.supported_hw_count = ARRAY_SIZE(priv->supported_hw);

		pkgid = airoha_get_pkg_id(NULL);
		if (pkgid != END_PACKAGE_ID) {
			for (i = 0; i < data->clocks_size; i++) {
				if (data->clocks[i].id != pkgid)
					continue;

				priv->supported_hw[0] =
					data->clocks[i].max_support_hw;
				pkgid_matched = true;
				break;
			}
		}

		if (pkgid_matched)
			dev_info(dev, "package %s: maximum CPU frequency %s\n",
				 airoha_id_name(pkgid),
				 airoha_cpufreq_max_freq_str(priv->supported_hw[0]));
		else
			dev_warn(dev,
				 "unable to identify package, limiting CPU to %s\n",
				 airoha_cpufreq_max_freq_str(AIROHA_CPU_HW_C));
	}

	priv->opp_token = dev_pm_opp_set_config(cpu_dev, &config);
	if (priv->opp_token < 0)
		return dev_err_probe(dev, priv->opp_token,
				     "Failed to set OPP config\n");

	if (data->use_pmdomain) {
		/* Attach PM for OPP */
		ret = dev_pm_domain_attach_list(cpu_dev, &attach_data,
						&priv->pd_list);
		if (ret < 0)
			goto clear_opp_config;
	}

	cpufreq_dt = platform_device_register_simple("cpufreq-dt", -1, NULL, 0);
	ret = PTR_ERR_OR_ZERO(cpufreq_dt);
	if (ret) {
		dev_err(dev, "failed to create cpufreq-dt device: %d\n", ret);
		goto detach_pm;
	}

	priv->cpufreq_dt = cpufreq_dt;
	platform_set_drvdata(pdev, priv);

	return 0;

detach_pm:
	if (priv->pd_list)
		dev_pm_domain_detach_list(priv->pd_list);
clear_opp_config:
	dev_pm_opp_clear_config(priv->opp_token);

	return ret;
}

static void airoha_cpufreq_remove(struct platform_device *pdev)
{
	struct airoha_cpufreq_priv *priv = platform_get_drvdata(pdev);

	platform_device_unregister(priv->cpufreq_dt);

	if (priv->pd_list)
		dev_pm_domain_detach_list(priv->pd_list);

	dev_pm_opp_clear_config(priv->opp_token);
}

static struct platform_driver airoha_cpufreq_driver = {
	.probe = airoha_cpufreq_probe,
	.remove = airoha_cpufreq_remove,
	.driver = {
		.name = "airoha-cpufreq",
	},
};

static const struct airoha_hw_clocks airoha_en7523_hw_clocks[] = {
	{ EN7529CT, AIROHA_CPU_HW_C },
	{ EN7529CTM, AIROHA_CPU_HW_C },
	{ EN7529CU, AIROHA_CPU_HW_C },
	{ EN7562CT, AIROHA_CPU_HW_C },
	{ EN7562CTM, AIROHA_CPU_HW_C },
	{ EN7562CU, AIROHA_CPU_HW_C },
	{ EN7529DU, AIROHA_CPU_HW_D },
	{ EN7529DT, AIROHA_CPU_HW_D },
	{ EN7529DTM, AIROHA_CPU_HW_D },
	{ EN7523DU, AIROHA_CPU_HW_D },
	{ EN7523DT, AIROHA_CPU_HW_D },
	{ EN7523DTM, AIROHA_CPU_HW_D },
	{ EN7562DU, AIROHA_CPU_HW_D },
	{ EN7562DT, AIROHA_CPU_HW_D },
	{ EN7562DTM, AIROHA_CPU_HW_D },
	{ EN7523GU, AIROHA_CPU_HW_G },
	{ EN7529GTH, AIROHA_CPU_HW_G },
	{ EN7529GTS, AIROHA_CPU_HW_G },
	{ EN7562GTH, AIROHA_CPU_HW_G },
	{ EN7562GTS, AIROHA_CPU_HW_G },
	{ EN7523SU, AIROHA_CPU_HW_S },
	{ EN7529IT, AIROHA_CPU_HW_I },
	{ EN7529ITM, AIROHA_CPU_HW_I },
};

static const struct airoha_hw_data airoha_en7523 = {
	.clocks = airoha_en7523_hw_clocks,
	.clocks_size = ARRAY_SIZE(airoha_en7523_hw_clocks),
};

static const struct airoha_hw_data airoha_en7581 = {
	.use_pmdomain = true,
};

static const struct of_device_id airoha_cpufreq_match_list[] __initconst = {
	{ .compatible = "airoha,an7583", .data = &airoha_en7581 },
	{ .compatible = "airoha,en7523", .data = &airoha_en7523 },
	{ .compatible = "airoha,en7581", .data = &airoha_en7581 },
	{},
};
MODULE_DEVICE_TABLE(of, airoha_cpufreq_match_list);

static int __init airoha_cpufreq_init(void)
{
	const struct of_device_id *match;
	int ret;

	match = of_machine_get_match(airoha_cpufreq_match_list);
	if (!match)
		return -ENODEV;

	ret = platform_driver_register(&airoha_cpufreq_driver);
	if (unlikely(ret < 0))
		return ret;

	cpufreq_pdev = platform_device_register_data(NULL, "airoha-cpufreq",
						     -1, match, sizeof(*match));
	ret = PTR_ERR_OR_ZERO(cpufreq_pdev);
	if (ret)
		platform_driver_unregister(&airoha_cpufreq_driver);

	return ret;
}
module_init(airoha_cpufreq_init);

static void __exit airoha_cpufreq_exit(void)
{
	platform_device_unregister(cpufreq_pdev);
	platform_driver_unregister(&airoha_cpufreq_driver);
}
module_exit(airoha_cpufreq_exit);

MODULE_AUTHOR("Christian Marangi <ansuelsmth@gmail.com>");
MODULE_DESCRIPTION("CPUfreq driver for Airoha SoCs");
MODULE_LICENSE("GPL");
