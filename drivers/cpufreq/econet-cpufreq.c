// SPDX-License-Identifier: GPL-2.0-only
/*
 * Report the CPU frequency supplied by the EcoNet MIPS clock controller.
 *
 * CPU and bus clocks are coupled. Until runtime PLL switching is supported,
 * expose the rate supplied by the clock controller and never change it.
 */

#include <linux/clk.h>
#include <linux/cpu.h>
#include <linux/cpufreq.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

struct econet_cpufreq_data {
	struct clk *clk;
	struct cpufreq_frequency_table table[2];
	struct cpufreq_driver driver;
};

static int econet_cpufreq_init(struct cpufreq_policy *policy)
{
	struct econet_cpufreq_data *data = cpufreq_get_driver_data();

	policy->clk = data->clk;
	cpufreq_generic_init(policy, data->table, 0);
	/* All cores and VPEs belong to the same CPU clock domain. */
	cpumask_copy(policy->cpus, cpu_possible_mask);

	return 0;
}

static int econet_cpufreq_target_index(struct cpufreq_policy *policy,
				      unsigned int index)
{
	struct econet_cpufreq_data *data = cpufreq_get_driver_data();

	if (index)
		return -EINVAL;

	/* A reporting-only driver cannot restore a rate changed out of band. */
	return clk_get_rate(policy->clk) / 1000 == data->table[0].frequency ?
		0 : -EOPNOTSUPP;
}

static const struct cpufreq_driver econet_cpufreq_driver = {
	.name = "econet",
	.flags = CPUFREQ_CONST_LOOPS | CPUFREQ_NO_AUTO_DYNAMIC_SWITCHING,
	.init = econet_cpufreq_init,
	.verify = cpufreq_generic_frequency_table_verify,
	.get = cpufreq_generic_get,
	.target_index = econet_cpufreq_target_index,
};

static void econet_cpufreq_clk_put(void *clk)
{
	clk_put(clk);
}

static void econet_cpufreq_unregister(void *driver)
{
	cpufreq_unregister_driver(driver);
}

static int econet_cpufreq_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct econet_cpufreq_data *data;
	struct device *cpu_dev;
	unsigned long rate;
	int ret;

	cpu_dev = get_cpu_device(0);
	if (!cpu_dev)
		return -EPROBE_DEFER;

	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	/* Use CPU0's DT clock even for VPEs without a separate CPU DT node. */
	data->clk = clk_get(cpu_dev, NULL);
	if (IS_ERR(data->clk))
		return dev_err_probe(dev, PTR_ERR(data->clk),
				     "Failed to get CPU clock\n");

	ret = devm_add_action_or_reset(dev, econet_cpufreq_clk_put, data->clk);
	if (ret)
		return ret;

	rate = clk_get_rate(data->clk);
	if (!rate || rate % 1000)
		return dev_err_probe(dev, -EINVAL,
				     "Invalid initial CPU clock rate %lu\n", rate);

	data->table[0].frequency = rate / 1000;
	data->table[1].frequency = CPUFREQ_TABLE_END;
	data->driver = econet_cpufreq_driver;
	data->driver.driver_data = data;

	ret = cpufreq_register_driver(&data->driver);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to register CPUFreq driver\n");

	ret = devm_add_action_or_reset(dev, econet_cpufreq_unregister,
				       &data->driver);
	if (ret)
		return ret;

	dev_info(dev, "Initial CPU frequency: %lu MHz (reporting only)\n",
		 rate / 1000000);

	return 0;
}

static struct platform_driver econet_cpufreq_platdrv = {
	.probe = econet_cpufreq_probe,
	.driver = {
		.name = "econet-cpufreq",
	},
};
module_platform_driver(econet_cpufreq_platdrv);

MODULE_DESCRIPTION("EcoNet MIPS CPU frequency reporting");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:econet-cpufreq");
