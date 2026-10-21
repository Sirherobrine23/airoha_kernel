// SPDX-License-Identifier: GPL-2.0

#include <linux/clk.h>
#include <linux/cpufreq.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>
#include <linux/pm_runtime.h>
#include <linux/slab.h>
#include <dt-bindings/soc/airoha,soc-variant.h>
#include <linux/soc/airoha/pkgids.h>

#include "cpufreq-dt.h"

static const char * const clocks_values[] = {
	[AIROHA_CPU_HW_C] = "1.0 GHz",
	[AIROHA_CPU_HW_D] = "1.2 GHz",
	[AIROHA_CPU_HW_G] = "1.3 GHz",
	[AIROHA_CPU_HW_P] = "1.4 GHz",
	[AIROHA_CPU_HW_S] = "2.0 GHz",
	[AIROHA_CPU_HW_I] = "2.0 GHz",
};

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
	struct device **virt_devs;
	int num_virt_devs;
	struct platform_device *cpufreq_dt;
	unsigned int supported_hw[1];
};

static struct platform_device *cpufreq_pdev;

static int airoha_cpufreq_config_clks_nop(struct device *dev,
					  struct opp_table *opp_table,
					  struct dev_pm_opp *old_opp,
					  struct dev_pm_opp *new_opp,
					  void *data, bool scaling_down)
{
	return 0;
}

static const char * const airoha_cpufreq_clk_names[] = { "cpu", NULL };
static const char * const airoha_cpufreq_pd_names[] = { "perf", NULL };

static int airoha_cpufreq_probe(struct platform_device *pdev)
{
	struct platform_device *cpufreq_dt;
	struct airoha_cpufreq_priv *priv;
	const struct of_device_id *match;
	const struct airoha_hw_data *data;
	enum airoha_pkg_ids pkgid = END_PACKAGE_ID;
	struct device *dev = &pdev->dev;
	struct device *cpu_dev;
	struct clk *cpu_clk;
	struct dev_pm_opp_config config = {
		.clk_names = airoha_cpufreq_clk_names,
	};
	bool pkgid_matched = false;
	int ret, i, num_devs;

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
		/* Wait for the clock provider before reading its cached ID. */
		cpu_clk = clk_get(cpu_dev, "cpu");
		if (IS_ERR(cpu_clk))
			return dev_err_probe(dev, PTR_ERR(cpu_clk),
					     "CPU clock is not available\n");
		clk_put(cpu_clk);

		/* Safe fallback: expose only the lowest frequency class. */
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
				 clocks_values[priv->supported_hw[0]]);
		else
			dev_warn(dev,
				 "unable to identify package, limiting CPU to %s\n",
				 clocks_values[AIROHA_CPU_HW_C]);
	}

	priv->opp_token = dev_pm_opp_set_config(cpu_dev, &config);
	if (priv->opp_token < 0)
		return dev_err_probe(dev, priv->opp_token,
				     "Failed to set OPP config\n");

	num_devs = 0;
	if (data->use_pmdomain)
		while (airoha_cpufreq_pd_names[num_devs])
			num_devs++;

	if (num_devs > 0) {
		struct device **virt_devs;

		virt_devs = devm_kcalloc(dev, num_devs, sizeof(*virt_devs),
					 GFP_KERNEL);
		if (!virt_devs) {
			ret = -ENOMEM;
			goto err_clear_opp_config;
		}

		for (i = 0; i < num_devs; i++) {
			virt_devs[i] = dev_pm_domain_attach_by_name(
				cpu_dev, airoha_cpufreq_pd_names[i]);
			if (IS_ERR_OR_NULL(virt_devs[i])) {
				ret = PTR_ERR(virt_devs[i]) ? : -ENODEV;
				dev_err(dev, "failed to attach %s: %d\n",
					airoha_cpufreq_pd_names[i], ret);
				while (--i >= 0)
					dev_pm_domain_detach(virt_devs[i], false);
				goto err_clear_opp_config;
			}
		}

		for (i = 0; i < num_devs; i++) {
			ret = pm_runtime_resume_and_get(virt_devs[i]);
			if (ret) {
				dev_err(dev, "failed to resume %s: %d\n",
					airoha_cpufreq_pd_names[i], ret);
				while (--i >= 0)
					pm_runtime_put(virt_devs[i]);
				for (i = 0; i < num_devs; i++)
					dev_pm_domain_detach(virt_devs[i], false);
				goto err_clear_opp_config;
			}
		}

		priv->virt_devs = virt_devs;
		priv->num_virt_devs = num_devs;
	}

	cpufreq_dt = platform_device_register_simple("cpufreq-dt", -1, NULL, 0);
	ret = PTR_ERR_OR_ZERO(cpufreq_dt);
	if (ret) {
		dev_err(dev, "failed to create cpufreq-dt device: %d\n", ret);
		goto err_register_cpufreq;
	}

	priv->cpufreq_dt = cpufreq_dt;
	platform_set_drvdata(pdev, priv);

	return 0;

err_register_cpufreq:
	if (priv->virt_devs) {
		for (i = 0; i < priv->num_virt_devs; i++) {
			pm_runtime_put(priv->virt_devs[i]);
			dev_pm_domain_detach(priv->virt_devs[i], false);
		}
	}
err_clear_opp_config:
	dev_pm_opp_clear_config(priv->opp_token);
	return ret;
}

static void airoha_cpufreq_remove(struct platform_device *pdev)
{
	struct airoha_cpufreq_priv *priv = platform_get_drvdata(pdev);
	int i;

	platform_device_unregister(priv->cpufreq_dt);
	dev_pm_opp_clear_config(priv->opp_token);

	if (priv->virt_devs) {
		for (i = 0; i < priv->num_virt_devs; i++) {
			pm_runtime_put(priv->virt_devs[i]);
			dev_pm_domain_detach(priv->virt_devs[i], false);
		}
	}
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

static const struct airoha_hw_data airoha_an7581 = {
	.use_pmdomain = true,
};

static const struct of_device_id airoha_cpufreq_match_list[] __initconst = {
	{ .compatible = "airoha,an7581", .data = &airoha_an7581 },
	{ .compatible = "airoha,an7583", .data = &airoha_an7581 },
	{ .compatible = "airoha,en7523", .data = &airoha_en7523 },
	{},
};
MODULE_DEVICE_TABLE(of, airoha_cpufreq_match_list);

static int __init airoha_cpufreq_init(void)
{
	struct device_node *np = of_find_node_by_path("/");
	const struct of_device_id *match;
	int ret;

	if (!np)
		return -ENODEV;

	match = of_match_node(airoha_cpufreq_match_list, np);
	of_node_put(np);
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
