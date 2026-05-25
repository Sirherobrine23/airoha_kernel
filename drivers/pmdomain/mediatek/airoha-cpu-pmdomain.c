// SPDX-License-Identifier: GPL-2.0

#include <linux/arm-smccc.h>
#include <linux/bitfield.h>
#include <linux/clk-provider.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>
#include <linux/slab.h>

/* ATF SMC interface for CPU frequency control */
#define AIROHA_SIP_AVS_HANDLE		0x82000301
#define AIROHA_AVS_OP_BASE		0xddddddd0
#define AIROHA_AVS_OP_MASK		GENMASK(1, 0)
#define AIROHA_AVS_OP_FREQ_DYN_ADJ	(AIROHA_AVS_OP_BASE | \
					 FIELD_PREP(AIROHA_AVS_OP_MASK, 0x1))
#define AIROHA_AVS_OP_GET_FREQ		(AIROHA_AVS_OP_BASE | \
					 FIELD_PREP(AIROHA_AVS_OP_MASK, 0x2))

/* CPU PLL register offsets (chip-SCU region) */
#define SCU_XTAL_SELECT				0x254
#define CPUPLL_CLK_MUX				0x1e0
#define PLLRG_PROTECT_AN7581			0x268
#define PLLRG_PROTECT_EN7523			0x264
#define PLLRG_PROTECT_MASK			GENMASK(7, 0)
#define PLLRG_PROTECT_KEY_AN7581		0x12
#define PLLRG_PROTECT_KEY_AN7583		0x80 /* Same for en7523 */

/* EN7523 Offsets */
#define CPUPLL_PCW_INT_MASK_EN7523		GENMASK(30, 24)
#define SYSPLL_DISABLE_EN7523			0x2b0
#define SYSPLL_PCW_25M_EN7523			0x2a8
#define SYSPLL_PCW_20M_EN7523			0x2ac
#define SYSPLL_CHG_BIT_EN7523			BIT(3)

/* AN7581 Offsets */
#define CPUPLL_SDM_PCW_AN7581			0x2b4
#define CPUPLL_SDM_PCW_CHG_AN7581		0x2b8
#define CPUPLL_PCW_INT_MASK_AN7581		GENMASK(31, 24)
#define CPUPLL_CHG_POSDIV_MASK_AN7581		GENMASK(6, 4)
#define CPUPLL_CHG_BIT				BIT(0)

/* AN7583 Offsets */
#define CPUPLL_SDM_PCW_AN7583			0x2ac
#define CPUPLL_SDM_PCW_CHG_AN7583		0x2b0
#define CPUPLL_SDM_SSC_PRD_AN7583		0x2b8
#define CPUPLL_SSC_PRD_POSDIV_MASK_AN7583	GENMASK(20, 18)

/* MCUCFG clock switch register offsets */
#define MCUCFG_CK_SWITCH_UNLOCK			0x640
#define MCUCFG_CK_SOURCE_SEL			0x7c0

#define MCUCFG_CK_UNLOCK_KEY			0x12
#define MCUCFG_CK_SEL_MASK			GENMASK(10, 9)
#define MCUCFG_CK_SEL_ARMPLL			1
#define MCUCFG_CK_SEL_PLL2			3

enum airoha_soc {
	SOC_EN7523,
	SOC_AN7581,
	SOC_AN7583,
};

struct airoha_cpu_pmdomain_scu {
	enum airoha_soc soc;
	int pllrg_protect;
	int pllrg_protect_key;
	int cpupll_pcw_chg;
	int cpupll_ssc_prd;
	u32 chg_bit;
};

struct airoha_cpu_pmdomain_priv {
	struct clk_hw hw;
	struct generic_pm_domain pd;
	const struct airoha_cpu_pmdomain_scu *scu_data;
	bool use_smc;
	void __iomem *chip_scu;
	void __iomem *mcucfg;
};

static void airoha_cpu_pmdomain_iounmap(void *base)
{
	iounmap(base);
}

static void __iomem *
airoha_cpu_pmdomain_map_chip_scu(struct device *dev)
{
	struct device_node *np;
	void __iomem *base;
	int ret;

	np = of_parse_phandle(dev->of_node, "airoha,chip-scu", 0);
	if (!np)
		return ERR_PTR(-ENODEV);

	base = of_iomap(np, 0);
	of_node_put(np);
	if (!base)
		return ERR_PTR(-ENOMEM);

	ret = devm_add_action_or_reset(dev, airoha_cpu_pmdomain_iounmap, base);
	if (ret)
		return ERR_PTR(ret);

	return base;
}

/* Get SoC XTAL clock */
static u32 airoha_xtal_clock(struct airoha_cpu_pmdomain_priv *priv)
{
	u32 val;
	switch (priv->scu_data->soc) {
	case SOC_EN7523:
		val = readl(priv->chip_scu + SCU_XTAL_SELECT);
		if (val & BIT(19))
			return 25; /* 25 MHz */
		return 20; /* 20 MHz */
	case SOC_AN7581:
	case SOC_AN7583:
		return 50; /* 50 MHz */
	default:
		return 0;
	}
}

static u32 airoha_cpupll_pcw_reg(struct airoha_cpu_pmdomain_priv *priv)
{
	switch (priv->scu_data->soc) {
	case SOC_EN7523:
		if (airoha_xtal_clock(priv) == 25)
			return SYSPLL_PCW_25M_EN7523;
		return SYSPLL_PCW_20M_EN7523;
	case SOC_AN7581:
		return CPUPLL_SDM_PCW_AN7581;
	case SOC_AN7583:
		return CPUPLL_SDM_PCW_AN7583;
	default:
		return 0;
	}
}

static int airoha_cpu_pmdomain_clk_determine_rate(struct clk_hw *hw,
						  struct clk_rate_request *req)
{
	return 0;
}

/* Read current CPU frequency via SMC */
static unsigned long airoha_cpu_smc_clk_get(void)
{
	struct arm_smccc_res res;

	arm_smccc_1_1_invoke(AIROHA_SIP_AVS_HANDLE, AIROHA_AVS_OP_GET_FREQ,
			     0, 0, 0, 0, 0, 0, &res);

	return (unsigned long)(res.a0 * 1000 * 1000);
}

/* Read current CPU frequency from PLL registers */
static unsigned long airoha_cpu_pll_clk_get(struct airoha_cpu_pmdomain_priv *priv)
{
	u32 pcw, chg, ssc;
	unsigned int pcw_int, posdiv = 0;

	pcw = readl(priv->chip_scu + airoha_cpupll_pcw_reg(priv));
	pcw_int = priv->scu_data->soc == SOC_EN7523 ?
		FIELD_GET(CPUPLL_PCW_INT_MASK_EN7523, pcw) :
		FIELD_GET(CPUPLL_PCW_INT_MASK_AN7581, pcw);

	switch (priv->scu_data->soc) {
	case SOC_EN7523:
		/* direct frequency pcw * xtal */
		return ((unsigned long)pcw_int * airoha_xtal_clock(priv) * 1000000UL) / 2;
	case SOC_AN7581:
		/* read POSDIV from PCW_CHG */
		chg = readl(priv->chip_scu + priv->scu_data->cpupll_pcw_chg);
		posdiv = FIELD_GET(CPUPLL_CHG_POSDIV_MASK_AN7581, chg);
		break;
	case SOC_AN7583:
		/* read POSDIV from SSC_PRD */
		ssc = readl(priv->chip_scu + priv->scu_data->cpupll_ssc_prd);
		posdiv = FIELD_GET(CPUPLL_SSC_PRD_POSDIV_MASK_AN7583, ssc);
		break;
	}
	return (unsigned long)pcw_int * airoha_xtal_clock(priv) * 1000000UL >> posdiv;
}

static unsigned long airoha_cpu_pmdomain_clk_get(struct clk_hw *hw,
						 unsigned long parent_rate)
{
	struct airoha_cpu_pmdomain_priv *priv =
		container_of(hw, struct airoha_cpu_pmdomain_priv, hw);

	if (priv->use_smc)
		return airoha_cpu_smc_clk_get();

	return airoha_cpu_pll_clk_get(priv);
}

static int airoha_cpu_pmdomain_clk_is_enabled(struct clk_hw *hw)
{
	return true;
}

static const struct clk_ops airoha_cpu_pmdomain_clk_ops = {
	.recalc_rate = airoha_cpu_pmdomain_clk_get,
	.is_enabled = airoha_cpu_pmdomain_clk_is_enabled,
	.determine_rate = airoha_cpu_pmdomain_clk_determine_rate,
};

/* Set CPU frequency via SMC */
static int airoha_cpu_smc_set_freq(unsigned int state)
{
	struct arm_smccc_res res;

	arm_smccc_1_1_invoke(AIROHA_SIP_AVS_HANDLE, AIROHA_AVS_OP_FREQ_DYN_ADJ,
			     0, state, 0, 0, 0, 0, &res);

	return res.a0 & BIT(0) ? -EINVAL : 0;
}

/*
 * Switch CPU clock source via CLK_MUX and MCUCFG registers.
 * Matches ATF BL31 clock_switch() sequence.
 */
static void airoha_cpu_clock_switch(void __iomem *pll_base,
				    void __iomem *mcucfg, unsigned int sel)
{
	u32 val;

	if (sel != 0) {
		val = readl(pll_base + CPUPLL_CLK_MUX);
		writel(val | BIT(sel - 1), pll_base + CPUPLL_CLK_MUX);
	}

	val = readl(mcucfg + MCUCFG_CK_SWITCH_UNLOCK);
	writel((val & ~0x1f) | MCUCFG_CK_UNLOCK_KEY,
	       mcucfg + MCUCFG_CK_SWITCH_UNLOCK);

	/* Wait for sync */
	udelay(1);

	val = readl(mcucfg + MCUCFG_CK_SOURCE_SEL);
	val &= ~MCUCFG_CK_SEL_MASK;
	val |= FIELD_PREP(MCUCFG_CK_SEL_MASK, sel);
	writel(val, mcucfg + MCUCFG_CK_SOURCE_SEL);
}

/*
 * Set CPU PLL frequency directly via register programming.
 * Used as fallback when ATF SMC is not available.
 */
static int airoha_cpu_pll_set_freq(struct airoha_cpu_pmdomain_priv *priv,
				   unsigned int state)
{
	void __iomem *base = priv->chip_scu;
	unsigned int freq_mhz = 500 + state * 50;
	unsigned int posdiv = 0, pcw_int;
	unsigned long flags;
	u32 val, old_chg;

	if (priv->scu_data->soc == SOC_EN7523) {
		pcw_int = (freq_mhz * 2) / airoha_xtal_clock(priv);
	} else {
		if (freq_mhz < 1000) {
			posdiv = 1;
			pcw_int = freq_mhz / 25;
		} else {
			posdiv = 0;
			pcw_int = freq_mhz / 50;
		}
	}

	local_irq_save(flags);

	/* Switch CPU to PLL2 (400 MHz backup) */
	airoha_cpu_clock_switch(base, priv->mcucfg, MCUCFG_CK_SEL_PLL2);

	/* Unlock PLL registers */
	val = readl(base + priv->scu_data->pllrg_protect);
	writel((val & ~PLLRG_PROTECT_MASK) | priv->scu_data->pllrg_protect_key,
	       base + priv->scu_data->pllrg_protect);

	/* Write new PCW integer value */
	val = readl(base + airoha_cpupll_pcw_reg(priv));
	if (priv->scu_data->soc == SOC_EN7523) {
		val &= ~CPUPLL_PCW_INT_MASK_EN7523;
		val |= FIELD_PREP(CPUPLL_PCW_INT_MASK_EN7523, pcw_int);
	} else {
		val &= ~CPUPLL_PCW_INT_MASK_AN7581;
		val |= FIELD_PREP(CPUPLL_PCW_INT_MASK_AN7581, pcw_int);
	}
	writel(val, base + airoha_cpupll_pcw_reg(priv));

	/* Apply POSDIV to the appropriate register using constants */
	switch (priv->scu_data->soc) {
	case SOC_AN7581:
		val = readl(base + priv->scu_data->cpupll_pcw_chg);
		val &= ~CPUPLL_CHG_POSDIV_MASK_AN7581;
		val |= FIELD_PREP(CPUPLL_CHG_POSDIV_MASK_AN7581, posdiv);
		writel(val, base + priv->scu_data->cpupll_pcw_chg);
		break;
	case SOC_AN7583:
		val = readl(base + priv->scu_data->cpupll_ssc_prd);
		val &= ~CPUPLL_SSC_PRD_POSDIV_MASK_AN7583;
		val |= FIELD_PREP(CPUPLL_SSC_PRD_POSDIV_MASK_AN7583, posdiv);
		writel(val, base + priv->scu_data->cpupll_ssc_prd);
		break;
	default:
		break;
	}

	/* apply changes */
	old_chg = readl(base + priv->scu_data->cpupll_pcw_chg);
	if (old_chg & priv->scu_data->chg_bit)
		writel(old_chg & ~priv->scu_data->chg_bit, base + priv->scu_data->cpupll_pcw_chg);
	else
		writel(old_chg | priv->scu_data->chg_bit, base + priv->scu_data->cpupll_pcw_chg);

	/* Wait for PLL to lock */
	udelay(20);

	/* Switch CPU back to ARM PLL */
	airoha_cpu_clock_switch(base, priv->mcucfg, MCUCFG_CK_SEL_ARMPLL);

	/* Clear PLL2 path bit in CLK_MUX */
	val = readl(base + CPUPLL_CLK_MUX);
	writel(val & ~BIT(2), base + CPUPLL_CLK_MUX);

	/* Re-lock PLL registers */
	val = readl(base + priv->scu_data->pllrg_protect);
	writel(val & ~PLLRG_PROTECT_MASK, base + priv->scu_data->pllrg_protect);

	local_irq_restore(flags);

	return 0;
}

static int airoha_cpu_pmdomain_set_performance_state(
		struct generic_pm_domain *domain, unsigned int state)
{
	struct airoha_cpu_pmdomain_priv *priv =
		container_of(domain, struct airoha_cpu_pmdomain_priv, pd);

	if (priv->use_smc)
		return airoha_cpu_smc_set_freq(state);

	return airoha_cpu_pll_set_freq(priv, state);
}

static bool airoha_cpu_smc_available(void)
{
	struct arm_smccc_res res;

	arm_smccc_1_1_invoke(AIROHA_SIP_AVS_HANDLE, AIROHA_AVS_OP_GET_FREQ,
			     0, 0, 0, 0, 0, 0, &res);

	/* SMC returns frequency in MHz; 0 or very large = not supported */
	return res.a0 > 0 && res.a0 < 2000;
}

static int airoha_cpu_pmdomain_probe(struct platform_device *pdev)
{
	struct airoha_cpu_pmdomain_priv *priv;
	struct device *dev = &pdev->dev;
	const struct clk_init_data init = {
		.name = "cpu",
		.ops = &airoha_cpu_pmdomain_clk_ops,
		.flags = CLK_GET_RATE_NOCACHE,
	};
	struct generic_pm_domain *pd;
	unsigned long freq_hz;
	int ret;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	/* Get SOC info to pll SCU controller */
	priv->scu_data = device_get_match_data(dev);
	if (!priv->scu_data)
		return dev_err_probe(dev, -EINVAL, "cannot get SoC SCU Data for pll controller");

	/* Try ATF SMC first */
	priv->use_smc = airoha_cpu_smc_available();
	if (!priv->use_smc) {
		dev_info(dev, "ATF SMC not available, using direct PLL programming");
		priv->mcucfg = devm_platform_ioremap_resource_byname(pdev, "mcucfg");
		if (IS_ERR(priv->mcucfg))
			return dev_err_probe(dev, PTR_ERR(priv->mcucfg),
					     "ATF SMC not available and no mcucfg reg in DT");
	
		priv->chip_scu = airoha_cpu_pmdomain_map_chip_scu(dev);
		if (IS_ERR(priv->chip_scu))
			return dev_err_probe(dev, PTR_ERR(priv->chip_scu),
					     "ATF SMC not available and no chip-scu reg in DT");
	}

	/* Read and verify current frequency */
	priv->hw.init = &init;
	freq_hz = airoha_cpu_pmdomain_clk_get(&priv->hw, 0);
	if (!freq_hz || freq_hz > 2000000000UL)
		return dev_err_probe(dev, -ENODEV,
				     "invalid CPU frequency: %lu Hz", freq_hz);

	if (priv->use_smc)
		dev_info(dev, "CPU frequency: %lu MHz", freq_hz / 1000000);
	else
		dev_info(dev, "CPU frequency: %lu MHz, XTAL Clock %d MHz",
			 freq_hz / 1000000, airoha_xtal_clock(priv));
	ret = devm_clk_hw_register(dev, &priv->hw);
	if (ret)
		return ret;

	ret = devm_of_clk_add_hw_provider(dev, of_clk_hw_simple_get,
					  &priv->hw);
	if (ret)
		return ret;

	/* Init and register a PD for CPU */
	pd = &priv->pd;
	pd->name = "cpu_pd";
	pd->flags = GENPD_FLAG_ALWAYS_ON;
	pd->set_performance_state = airoha_cpu_pmdomain_set_performance_state;

	ret = pm_genpd_init(pd, NULL, false);
	if (ret)
		return ret;

	ret = of_genpd_add_provider_simple(dev->of_node, pd);
	if (ret)
		goto err_add_provider;

	platform_set_drvdata(pdev, priv);
	return 0;

err_add_provider:
	pm_genpd_remove(pd);
	return ret;
}

static void airoha_cpu_pmdomain_remove(struct platform_device *pdev)
{
	struct airoha_cpu_pmdomain_priv *priv = platform_get_drvdata(pdev);

	of_genpd_del_provider(pdev->dev.of_node);
	pm_genpd_remove(&priv->pd);
}

static const struct airoha_cpu_pmdomain_scu airoha_an7581 = {
	.soc = SOC_AN7581,
	.pllrg_protect = PLLRG_PROTECT_AN7581,
	.pllrg_protect_key = PLLRG_PROTECT_KEY_AN7581,
	.cpupll_pcw_chg = CPUPLL_SDM_PCW_CHG_AN7581,
	.chg_bit = CPUPLL_CHG_BIT,
};

static const struct airoha_cpu_pmdomain_scu airoha_an7583 = {
	.soc = SOC_AN7583,
	.pllrg_protect = PLLRG_PROTECT_AN7581,
	.pllrg_protect_key = PLLRG_PROTECT_KEY_AN7583,
	.cpupll_pcw_chg = CPUPLL_SDM_PCW_CHG_AN7583,
	.cpupll_ssc_prd = CPUPLL_SDM_SSC_PRD_AN7583,
	.chg_bit = CPUPLL_CHG_BIT,
};

static const struct of_device_id airoha_cpu_pmdomain_of_match[] = {
	{ .compatible = "airoha,an7581-cpufreq", .data = &airoha_an7581 },
	{ .compatible = "airoha,an7583-cpufreq", .data = &airoha_an7583 },
	{ },
};
MODULE_DEVICE_TABLE(of, airoha_cpu_pmdomain_of_match);

static struct platform_driver airoha_cpu_pmdomain_driver = {
	.probe = airoha_cpu_pmdomain_probe,
	.remove = airoha_cpu_pmdomain_remove,
	.driver = {
		.name = "airoha-cpu-pmdomain",
		.of_match_table = airoha_cpu_pmdomain_of_match,
	},
};
module_platform_driver(airoha_cpu_pmdomain_driver);

MODULE_AUTHOR("Christian Marangi <ansuelsmth@gmail.com>");
MODULE_DESCRIPTION("CPU PM domain driver for Airoha SoCs");
MODULE_LICENSE("GPL");
