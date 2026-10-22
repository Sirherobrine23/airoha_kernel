// SPDX-License-Identifier: GPL-2.0-only
/*
 * Control EN751221 and EN7528 profiles; report rates on other EcoNet SoCs.
 */

#include <linux/clk.h>
#include <linux/cpu.h>
#include <linux/cpufreq.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_opp.h>
#include <linux/slab.h>

#define ECONET_CPUFREQ_TRANSITION_DELAY_US	100000

struct econet_cpufreq_data {
	struct clk *clk;
	struct device *cpu_dev;
	struct cpufreq_frequency_table *table;
	struct cpufreq_frequency_table *opp_table;
	struct cpufreq_frequency_table fixed_table[2];
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

static void econet_cpufreq_remove_opps(void *arg)
{
	struct econet_cpufreq_data *data = arg;

	if (data->opp_table)
		dev_pm_opp_free_cpufreq_table(data->cpu_dev, &data->opp_table);
	dev_pm_opp_of_remove_table(data->cpu_dev);
}

static int econet_cpufreq_add_opps(struct device *dev,
				   struct econet_cpufreq_data *data)
{
	struct dev_pm_opp *opp;
	unsigned long freq = 0;
	unsigned int i;
	int count, ret;

	/* Older DTBs and reporting-only SoCs need no programmable OPP table. */
	if (!of_property_present(data->cpu_dev->of_node, "operating-points-v2"))
		return 0;

	ret = dev_pm_opp_of_add_table(data->cpu_dev);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to add CPU OPP table\n");
	ret = devm_add_action_or_reset(dev, econet_cpufreq_remove_opps, data);
	if (ret)
		return ret;

	/* Validate Hz before the OPP helper converts them to CPUFreq kHz. */
	for (;;) {
		opp = dev_pm_opp_find_freq_ceil(data->cpu_dev, &freq);
		if (IS_ERR(opp)) {
			ret = PTR_ERR(opp);
			if (ret != -ERANGE)
				return ret;
			break;
		}
		dev_pm_opp_put(opp);
		/* OPPs restrict provider profiles; they cannot add PLL settings. */
		if (freq % 1000 || clk_round_rate(data->clk, freq) != freq) {
			ret = dev_pm_opp_disable(data->cpu_dev, freq);
			if (ret)
				return ret;
		}
		if (freq == ULONG_MAX)
			break;
		freq++;
	}

	count = dev_pm_opp_get_opp_count(data->cpu_dev);
	if (count < 0)
		return count;
	if (!count)
		return 0;

	ret = dev_pm_opp_init_cpufreq_table(data->cpu_dev, &data->opp_table);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to create CPU frequency table\n");
	data->num_frequencies = count;
	/* Keep overclocking opt-in even if a DT omits turbo-mode. */
	for (i = 0; i < data->num_frequencies; i++)
		if (data->opp_table[i].frequency > 900000)
			data->opp_table[i].flags |= CPUFREQ_BOOST_FREQ;

	return 0;
}

static int econet_cpufreq_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct econet_cpufreq_data *data;
	struct device *cpu_dev;
	unsigned long rate;
	unsigned int i;
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

	data->cpu_dev = cpu_dev;
	data->driver = econet_cpufreq_driver;
	data->driver.driver_data = data;

	ret = econet_cpufreq_add_opps(dev, data);
	if (ret)
		return ret;

	/* A read-only provider can match only the current boot frequency. */
	if (data->num_frequencies > 1 ||
	    (data->num_frequencies == 1 &&
	     data->opp_table[0].frequency != rate / 1000)) {
		data->table = data->opp_table;
		data->scaling = true;
		for (i = 0; i < data->num_frequencies; i++)
			boost |= !!(data->table[i].flags & CPUFREQ_BOOST_FREQ);
		if (boost)
			data->driver.set_boost = cpufreq_boost_set_sw;
		/* Permit governors and rescale the MIPS delay loops on transitions. */
		data->driver.flags &= ~(CPUFREQ_CONST_LOOPS |
					CPUFREQ_NO_AUTO_DYNAMIC_SWITCHING);
	} else {
		data->table = data->fixed_table;
		data->num_frequencies = 1;
		data->table[0].frequency = rate / 1000;
		data->table[1].frequency = CPUFREQ_TABLE_END;
	}

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
