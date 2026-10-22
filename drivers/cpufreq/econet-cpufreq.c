// SPDX-License-Identifier: GPL-2.0-only
/*
 * Control EN751221 and EN7528 profiles; report rates on other EcoNet SoCs.
 */

#include <linux/clk.h>
#include <linux/cpu.h>
#include <linux/cpufreq.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

#define ECONET_CPUFREQ_TRANSITION_DELAY_US	100000

static const unsigned int econet_cpu_frequencies[] = {
	600000, 625000, 650000, 675000,
	700000, 725000, 750000, 775000,
	800000, 825000, 850000, 875000,
	900000, 925000, 950000, 975000,
	1000000,
};

struct econet_cpufreq_data {
	struct clk *clk;
	struct cpufreq_frequency_table table[ARRAY_SIZE(econet_cpu_frequencies) + 1];
	struct cpufreq_driver driver;
	unsigned int num_frequencies;
	bool scaling;
};

static int econet_cpufreq_init(struct cpufreq_policy *policy)
{
	struct econet_cpufreq_data *data = cpufreq_get_driver_data();

	policy->clk = data->clk;
	cpufreq_generic_init(policy, data->table,
			     data->scaling ? CPUFREQ_DEFAULT_TRANSITION_LATENCY_NS : 0);
	/* PLL changes stop all VPEs; start with 100 ms governor updates. */
	policy->transition_delay_us = data->scaling ?
		ECONET_CPUFREQ_TRANSITION_DELAY_US : 0;
	/* All cores and VPEs belong to the same CPU clock domain. */
	cpumask_copy(policy->cpus, cpu_possible_mask);

	return 0;
}

static int econet_cpufreq_target_index(struct cpufreq_policy *policy,
				      unsigned int index)
{
	struct econet_cpufreq_data *data = cpufreq_get_driver_data();

	if (data->scaling) {
		unsigned long requested;
		int ret;

		if (index >= data->num_frequencies)
			return -EINVAL;
		if ((data->table[index].flags & CPUFREQ_BOOST_FREQ) &&
		    (!cpufreq_boost_enabled() || !policy->boost_enabled))
			return -EPERM;
		requested = data->table[index].frequency * 1000UL;
		ret = clk_set_rate(policy->clk, requested);
		if (ret)
			return ret;
		/* CCF may ignore a provider's set_rate error; verify live readback. */
		return clk_get_rate(policy->clk) == requested ? 0 : -EIO;
	}

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
	unsigned int i, count = 0;
	bool boost = false;
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

	data->driver = econet_cpufreq_driver;
	data->driver.driver_data = data;

	/* A read-only provider can match at most one distinct candidate rate. */
	for (i = 0; i < ARRAY_SIZE(econet_cpu_frequencies); i++) {
		unsigned long requested = econet_cpu_frequencies[i] * 1000UL;

		if (clk_round_rate(data->clk, requested) != requested)
			continue;
		data->table[count].frequency = econet_cpu_frequencies[i];
		if (econet_cpu_frequencies[i] > 900000) {
			data->table[count].flags = CPUFREQ_BOOST_FREQ;
			boost = true;
		}
		count++;
	}
	if (count > 1) {
		data->scaling = true;
		/* Keep rates above the nominal 900 MHz ceiling opt-in. */
		if (boost)
			data->driver.set_boost = cpufreq_boost_set_sw;
		/* Permit governors and rescale the MIPS delay loops on transitions. */
		data->driver.flags &= ~(CPUFREQ_CONST_LOOPS |
					CPUFREQ_NO_AUTO_DYNAMIC_SWITCHING);
	} else {
		count = 1;
		data->table[0].frequency = rate / 1000;
		data->table[0].flags = 0;
	}
	data->num_frequencies = count;
	data->table[count].frequency = CPUFREQ_TABLE_END;

	ret = cpufreq_register_driver(&data->driver);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to register CPUFreq driver\n");

	ret = devm_add_action_or_reset(dev, econet_cpufreq_unregister,
				       &data->driver);
	if (ret)
		return ret;

	if (data->scaling)
		dev_info(dev, "Initial CPU frequency: %lu MHz (%u profiles%s)\n",
			 rate / 1000000, data->num_frequencies,
			 boost ? "; boost above 900 MHz disabled" : "");
	else
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
