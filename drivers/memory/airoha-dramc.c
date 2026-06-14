// SPDX-License-Identifier: GPL-2.0-only
/*
 * Airoha DRAM controller telemetry driver
 *
 * The AN75xx DRAMC blocks share the same
 * channel, NAO, and DDRPHY layout.
 */

#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/io.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>

#define AIROHA_MAX_RANKS		2
#define AIROHA_MAX_MODE_REGISTERS	16
#define AIROHA_MAX_FREQ_STEPS		16

#define AIROHA_SCU_XTAL_STATUS		0x258
#define AIROHA_SCU_XTAL_25MHZ		BIT(19)

#define EN7523_SCU_XTAL_STATUS		0x254
#define EN7523_SCU_XTAL_25MHZ		BIT(19)
#define EN7523_DDRPHY_SHU1_PLL1		0xd84
#define EN7523_DDRPHY_PLL_SEL		BIT(0)
#define EN7523_DDRPHY_SHU1_PHYPLL_PCW	0xd94
#define EN7523_DDRPHY_SHU1_CLRPLL_PCW	0xd9c
#define EN7523_DDRPHY_PLL_PCW		GENMASK(30, 0)
#define EN7523_DDRPHY_PLL_PCW_FRAC_BITS	24
#define EN7523_DDRPHY_SHU1_PHYPLL_DIV	0xda0
#define EN7523_DDRPHY_SHU1_CLRPLL_DIV	0xda8
#define EN7523_DDRPHY_PLL_FBDIV2	BIT(31)
#define EN7523_DDRPHY_PLL_PREDIV	GENMASK(19, 18)

#define EN751221_DRAMC_DDR2CTL		0x07c
#define EN751221_DRAMC_DDR2_EN		BIT(1)
#define EN751221_DRAMC_PADCTL4		0x0e4
#define EN751221_DRAMC_DDR3_EN		BIT(7)
#define EN751221_DRAMC_LPDDR2		0x1e0
#define EN751221_DRAMC_LPDDR2_EN	BIT(28)
#define EN751221_DRAMC_RKCFG		0x110
#define EN751221_DRAMC_RKSIZE		GENMASK(26, 24)
#define EN751221_DRAMC_RKMODE		GENMASK(2, 0)

#define EN7523_DRAMC_RKCFG		0x034
#define EN7523_DRAMC_RKSIZE		GENMASK(18, 16)
#define EN7523_DRAMC_RKMODE		GENMASK(7, 4)
#define EN7523_DRAMC_THREE_RANKS	BIT(8)

#define AN75XX_DRAMC_SHU_STATUS		0x0e4
#define AN75XX_DRAMC_SHU_LEVEL		GENMASK(2, 1)
#define AN75XX_DDRPHY_NAO_OFFSET	0x8000
#define AN75XX_DDRPHY_OFFSET		0xa000
#define AN75XX_DDRPHY_PLL_STATUS	0x190
#define AN75XX_DDRPHY_PHYPLL_SELECTED	BIT(31)
#define AN75XX_DDRPHY_SHU_STRIDE	0x900
#define AN75XX_DDRPHY_SHU_PHYPLL_PCW	0x904
#define AN75XX_DDRPHY_SHU_PHYPLL_DIV	0x908
#define AN75XX_DDRPHY_SHU_CLRPLL_PCW	0x924
#define AN75XX_DDRPHY_SHU_CLRPLL_DIV	0x928
#define AN75XX_DDRPHY_PLL_PCW		GENMASK(31, 16)
#define AN75XX_DDRPHY_PLL_PCW_FRAC_BITS	8
#define AN75XX_DDRPHY_PLL_PREDIV	GENMASK(19, 18)
#define AN75XX_DRAMC_DDRCOMMON0		0x000
#define AN75XX_DRAMC_DDR4_EN		BIT(24)
#define AN75XX_DRAMC_DDR3_EN		BIT(23)
#define AN75XX_DRAMC_DDR2_EN		BIT(22)
#define AN75XX_DRAMC_LPDDR5_EN		BIT(20)
#define AN75XX_DRAMC_LPDDR4_EN		BIT(19)
#define AN75XX_DRAMC_LPDDR3_EN		BIT(18)
#define AN75XX_DRAMC_LPDDR2_EN		BIT(17)
#define AN75XX_DRAMC_NAO_OFFSET		0x4000
#define AN75XX_DRAMC_MR4_STATUS		0x090
#define AN75XX_DRAMC_CHANNEL_STRIDE	0x10000
#define AN75XX_DRAMC_CMD_DEC_CTRL0	0x21c
#define AN75XX_DRAMC_RKMODE		GENMASK(10, 8)
#define AN75XX_DRAMC_CLKAR		0x260
#define AN75XX_DRAMC_RKSIZE		GENMASK(22, 20)

enum airoha_dram_type {
	AIROHA_DRAM_UNKNOWN,
	AIROHA_DRAM_DDR2,
	AIROHA_DRAM_DDR3,
	AIROHA_DRAM_DDR4,
	AIROHA_DRAM_LPDDR2,
	AIROHA_DRAM_LPDDR3,
	AIROHA_DRAM_LPDDR4,
	AIROHA_DRAM_LPDDR5,
};

static const char * const airoha_dram_type_names[] = {
	[AIROHA_DRAM_UNKNOWN] = "UNKNOWN",
	[AIROHA_DRAM_DDR2] = "DDR2",
	[AIROHA_DRAM_DDR3] = "DDR3",
	[AIROHA_DRAM_DDR4] = "DDR4",
	[AIROHA_DRAM_LPDDR2] = "LPDDR2",
	[AIROHA_DRAM_LPDDR3] = "LPDDR3",
	[AIROHA_DRAM_LPDDR4] = "LPDDR4",
	[AIROHA_DRAM_LPDDR5] = "LPDDR5",
};

struct airoha_dramc;

struct airoha_dramc_soc_data {
	u8 channel_count;
	u8 xtal_mhz;
	bool has_mr4;
	bool needs_chip_scu;
	bool has_separate_ddrphy;
	u16 rank_config_offset;
	u16 rank_size_offset;
	u32 rank_mode_mask;
	u32 rank_size_mask;
	u32 three_rank_mask;
	int (*data_rate)(struct airoha_dramc *dramc);
	enum airoha_dram_type (*dram_type)(struct airoha_dramc *dramc);
};

struct airoha_dramc {
	void __iomem *base;
	void __iomem *ddrphy;
	struct regmap *chip_scu;
	const struct airoha_dramc_soc_data *soc;
	u64 rank_size[AIROHA_MAX_RANKS];
	u32 mode_register[AIROHA_MAX_MODE_REGISTERS];
	u32 freq_step[AIROHA_MAX_FREQ_STEPS];
	u8 rank_count;
	u8 mode_register_count;
	u8 freq_step_count;
};

/**
 * airoha_dramc_size() - calculate total DRAM capacity
 * @dramc: DRAM controller state
 *
 * With multiple ranks, RKSIZE selects the system address bit used as the rank
 * boundary. With one rank, it selects the highest populated address bit.
 * EN7523-style controllers also expose a separate bit for a third rank.
 *
 * Return: total installed DRAM capacity in bytes.
 */
static u64 airoha_dramc_size(struct airoha_dramc *dramc)
{
	const struct airoha_dramc_soc_data *soc = dramc->soc;
	u32 rank_config, rank_size;
	unsigned int boundary, rank_count = 1;

	rank_config = readl(dramc->base + soc->rank_config_offset);
	if ((rank_config & soc->rank_mode_mask) >>
	    __ffs(soc->rank_mode_mask))
		rank_count = 2;
	if (soc->three_rank_mask && rank_config & soc->three_rank_mask)
		rank_count = 3;

	rank_size = readl(dramc->base + soc->rank_size_offset);
	boundary = 31 - ((rank_size & soc->rank_size_mask) >>
			 __ffs(soc->rank_size_mask));

	if (rank_count == 1)
		return BIT_ULL(boundary + 1);

	return BIT_ULL(boundary) * rank_count;
}

/**
 * en751221_dramc_type() - read the configured EN751221 DRAM protocol
 * @dramc: DRAM controller state
 *
 * Return: configured DRAM type, or AIROHA_DRAM_UNKNOWN if no enable bit is
 * set.
 */
static enum airoha_dram_type en751221_dramc_type(struct airoha_dramc *dramc)
{
	if (readl(dramc->base + EN751221_DRAMC_LPDDR2) &
	    EN751221_DRAMC_LPDDR2_EN)
		return AIROHA_DRAM_LPDDR2;

	if (readl(dramc->base + EN751221_DRAMC_PADCTL4) &
	    EN751221_DRAMC_DDR3_EN)
		return AIROHA_DRAM_DDR3;

	if (readl(dramc->base + EN751221_DRAMC_DDR2CTL) &
	    EN751221_DRAMC_DDR2_EN)
		return AIROHA_DRAM_DDR2;

	return AIROHA_DRAM_UNKNOWN;
}

/**
 * en751221_dramc_data_rate() - return the EN751221 DRAM data rate
 * @dramc: DRAM controller state
 *
 * The EN7521/EN7526 programming guide specifies DDR2 at 800 MT/s and DDR3 at
 * 1066 MT/s. LPDDR2 uses the DDR2 controller operating point.
 *
 * Return: current data rate in MT/s, or -EINVAL for an unknown protocol.
 */
static int en751221_dramc_data_rate(struct airoha_dramc *dramc)
{
	switch (en751221_dramc_type(dramc)) {
	case AIROHA_DRAM_DDR3:
		return 1066;
	case AIROHA_DRAM_DDR2:
	case AIROHA_DRAM_LPDDR2:
		return 800;
	default:
		return -EINVAL;
	}
}

/**
 * en7523_dramc_data_rate() - read the current EN7523 DDR data rate
 * @dramc: DRAM controller state
 *
 * Return: current data rate in MT/s, or a negative error code.
 */
static int en7523_dramc_data_rate(struct airoha_dramc *dramc)
{
	u64 rate;
	u32 div, mux, pcw, xtal_status;
	unsigned int prediv;
	int ret;

	ret = regmap_read(dramc->chip_scu, EN7523_SCU_XTAL_STATUS,
			  &xtal_status);
	if (ret)
		return ret;

	mux = readl(dramc->ddrphy + EN7523_DDRPHY_SHU1_PLL1);
	if (mux & EN7523_DDRPHY_PLL_SEL) {
		pcw = readl(dramc->ddrphy + EN7523_DDRPHY_SHU1_PHYPLL_PCW);
		div = readl(dramc->ddrphy + EN7523_DDRPHY_SHU1_PHYPLL_DIV);
	} else {
		pcw = readl(dramc->ddrphy + EN7523_DDRPHY_SHU1_CLRPLL_PCW);
		div = readl(dramc->ddrphy + EN7523_DDRPHY_SHU1_CLRPLL_DIV);
	}

	prediv = 1U << FIELD_GET(EN7523_DDRPHY_PLL_PREDIV, div);
	rate = (xtal_status & EN7523_SCU_XTAL_25MHZ) ? 25 : 40;
	rate *= FIELD_GET(EN7523_DDRPHY_PLL_PCW, pcw);
	rate = DIV_ROUND_CLOSEST_ULL(rate,
				     BIT_ULL(EN7523_DDRPHY_PLL_PCW_FRAC_BITS));
	rate = DIV_ROUND_CLOSEST_ULL(rate, prediv);
	if (!(div & EN7523_DDRPHY_PLL_FBDIV2))
		rate = DIV_ROUND_CLOSEST_ULL(rate, 2);

	return rate;
}

/**
 * en7523_dramc_type() - return the DRAM type supported by EN7523
 * @dramc: DRAM controller state
 *
 * Return: DDR3 type identifier.
 */
static enum airoha_dram_type en7523_dramc_type(struct airoha_dramc *dramc)
{
	return AIROHA_DRAM_DDR3;
}

/**
 * en7528_dramc_data_rate() - return the EN7528 fixed DDR data rate
 * @dramc: DRAM controller state
 *
 * Return: DDR3-1333 data rate in MT/s.
 */
static int en7528_dramc_data_rate(struct airoha_dramc *dramc)
{
	return 1333;
}

/**
 * en7580_dramc_data_rate() - return the EN7580 fixed DDR data rate
 * @dramc: DRAM controller state
 *
 * Return: DDR3-1866 data rate in MT/s.
 */
static int en7580_dramc_data_rate(struct airoha_dramc *dramc)
{
	return 1866;
}

/**
 * an75xx_dramc_data_rate() - calculate the current AN75xx DDR data rate
 * @dramc: DRAM controller state
 *
 * The active shuffle bank and PLL are selected from live status registers.
 * The PLL PCW has eight fractional bits and produces four data transfers per
 * reference-clock/PCW product.
 *
 * Return: current data rate in MT/s, or a negative error code.
 */
static int an75xx_dramc_data_rate(struct airoha_dramc *dramc)
{
	void __iomem *ddrphy = dramc->base + AN75XX_DDRPHY_OFFSET;
	u32 div, pcw, pll_status, shu, xtal_status;
	unsigned int prediv, shu_offset;
	u64 rate;
	int ret;

	if (dramc->soc->xtal_mhz) {
		xtal_status = 0;
	} else {
		ret = regmap_read(dramc->chip_scu, AIROHA_SCU_XTAL_STATUS,
				  &xtal_status);
		if (ret)
			return ret;
	}

	shu = FIELD_GET(AN75XX_DRAMC_SHU_LEVEL,
			readl(dramc->base + AN75XX_DRAMC_SHU_STATUS));
	if (shu > 1)
		return -EINVAL;

	shu_offset = shu * AN75XX_DDRPHY_SHU_STRIDE;
	pll_status = readl(dramc->base + AN75XX_DDRPHY_NAO_OFFSET +
			   AN75XX_DDRPHY_PLL_STATUS);
	if (pll_status & AN75XX_DDRPHY_PHYPLL_SELECTED) {
		pcw = readl(ddrphy + AN75XX_DDRPHY_SHU_PHYPLL_PCW +
			    shu_offset);
		div = readl(ddrphy + AN75XX_DDRPHY_SHU_PHYPLL_DIV +
			    shu_offset);
	} else {
		pcw = readl(ddrphy + AN75XX_DDRPHY_SHU_CLRPLL_PCW +
			    shu_offset);
		div = readl(ddrphy + AN75XX_DDRPHY_SHU_CLRPLL_DIV +
			    shu_offset);
	}

	prediv = 1U << FIELD_GET(AN75XX_DDRPHY_PLL_PREDIV, div);
	rate = dramc->soc->xtal_mhz ? dramc->soc->xtal_mhz :
	       ((xtal_status & AIROHA_SCU_XTAL_25MHZ) ? 25 : 40);
	rate *= FIELD_GET(AN75XX_DDRPHY_PLL_PCW, pcw) * 4;
	rate = DIV_ROUND_CLOSEST_ULL(rate,
				     BIT_ULL(AN75XX_DDRPHY_PLL_PCW_FRAC_BITS));
	rate = DIV_ROUND_CLOSEST_ULL(rate, prediv);

	return rate;
}

/**
 * an75xx_dramc_type() - read the configured AN75xx DRAM protocol
 * @dramc: DRAM controller state
 *
 * Return: configured DRAM type identifier.
 */
static enum airoha_dram_type an75xx_dramc_type(struct airoha_dramc *dramc)
{
	u32 val = readl(dramc->base + AN75XX_DRAMC_DDRCOMMON0);

	if (val & AN75XX_DRAMC_LPDDR5_EN)
		return AIROHA_DRAM_LPDDR5;
	if (val & AN75XX_DRAMC_LPDDR4_EN)
		return AIROHA_DRAM_LPDDR4;
	if (val & AN75XX_DRAMC_LPDDR3_EN)
		return AIROHA_DRAM_LPDDR3;
	if (val & AN75XX_DRAMC_DDR4_EN)
		return AIROHA_DRAM_DDR4;
	if (val & AN75XX_DRAMC_DDR3_EN)
		return AIROHA_DRAM_DDR3;

	return AIROHA_DRAM_UNKNOWN;
}

/**
 * airoha_dramc_mr4() - read a channel's hardware-captured MR4 value
 * @dramc: DRAM controller state
 * @channel: zero-based channel index
 *
 * Return: MR4 value, or a negative error code.
 */
static int airoha_dramc_mr4(struct airoha_dramc *dramc, unsigned int channel)
{
	void __iomem *nao;

	if (!dramc->soc->has_mr4 || channel >= dramc->soc->channel_count)
		return -EOPNOTSUPP;

	nao = dramc->base + channel * AN75XX_DRAMC_CHANNEL_STRIDE +
	      AN75XX_DRAMC_NAO_OFFSET;
	return readl(nao + AN75XX_DRAMC_MR4_STATUS) & GENMASK(15, 0);
}

/**
 * airoha_dramc_frequency() - convert DDR data rate to PHY clock frequency
 * @data_rate: effective DDR data rate in MT/s
 *
 * DDR transfers data on both clock edges, so the PHY clock is half the
 * effective transfer rate.
 *
 * Return: DDR PHY clock frequency in Hz.
 */
static u64 airoha_dramc_frequency(unsigned int data_rate)
{
	return (u64)data_rate * 500000;
}

/**
 * frequency_show() - show the current DDR PHY clock frequency
 * @dev: memory controller device
 * @attr: sysfs attribute
 * @buf: output buffer
 *
 * Return: number of bytes written, or a negative error code.
 */
static ssize_t frequency_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	struct airoha_dramc *dramc = dev_get_drvdata(dev);
	int rate = dramc->soc->data_rate(dramc);

	if (rate < 0)
		return rate;

	return sysfs_emit(buf, "%llu\n", airoha_dramc_frequency(rate));
}
static DEVICE_ATTR_RO(frequency);

/**
 * dram_data_rate_show() - show the current DDR data rate
 * @dev: memory controller device
 * @attr: sysfs attribute
 * @buf: output buffer
 *
 * Return: number of bytes written, or a negative error code.
 */
static ssize_t dram_data_rate_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct airoha_dramc *dramc = dev_get_drvdata(dev);
	int rate = dramc->soc->data_rate(dramc);

	if (rate < 0)
		return rate;

	return sysfs_emit(buf, "%d\n", rate);
}
static DEVICE_ATTR_RO(dram_data_rate);

/**
 * dram_type_show() - show the configured DRAM protocol name
 * @dev: memory controller device
 * @attr: sysfs attribute
 * @buf: output buffer
 *
 * Return: number of bytes written.
 */
static ssize_t dram_type_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	struct airoha_dramc *dramc = dev_get_drvdata(dev);
	enum airoha_dram_type type = dramc->soc->dram_type(dramc);

	if (type >= ARRAY_SIZE(airoha_dram_type_names))
		type = AIROHA_DRAM_UNKNOWN;

	return sysfs_emit(buf, "%s\n", airoha_dram_type_names[type]);
}
static DEVICE_ATTR_RO(dram_type);

/**
 * channel_count_show() - show the number of DRAM channels
 * @dev: memory controller device
 * @attr: sysfs attribute
 * @buf: output buffer
 *
 * Return: number of bytes written.
 */
static ssize_t channel_count_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct airoha_dramc *dramc = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%u\n", dramc->soc->channel_count);
}
static DEVICE_ATTR_RO(channel_count);

/**
 * dram_size_show() - show total installed DRAM capacity
 * @dev: memory controller device
 * @attr: sysfs attribute
 * @buf: output buffer
 *
 * Return: number of bytes written.
 */
static ssize_t dram_size_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	struct airoha_dramc *dramc = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%llu\n", airoha_dramc_size(dramc));
}
static DEVICE_ATTR_RO(dram_size);

/**
 * rank_count_show() - show the firmware-described rank count
 * @dev: memory controller device
 * @attr: sysfs attribute
 * @buf: output buffer
 *
 * Return: number of bytes written.
 */
static ssize_t rank_count_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct airoha_dramc *dramc = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%u\n", dramc->rank_count);
}
static DEVICE_ATTR_RO(rank_count);

/**
 * rank_sizes_show() - show firmware-described rank sizes
 * @dev: memory controller device
 * @attr: sysfs attribute
 * @buf: output buffer
 *
 * Return: number of bytes written.
 */
static ssize_t rank_sizes_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct airoha_dramc *dramc = dev_get_drvdata(dev);
	ssize_t len = 0;
	unsigned int i;

	for (i = 0; i < dramc->rank_count; i++)
		len += sysfs_emit_at(buf, len, "%s%llu", i ? " " : "",
				     dramc->rank_size[i]);

	return sysfs_emit_at(buf, len, "\n") + len;
}
static DEVICE_ATTR_RO(rank_sizes);

/**
 * mode_registers_show() - show firmware-supplied mode-register values
 * @dev: memory controller device
 * @attr: sysfs attribute
 * @buf: output buffer
 *
 * Return: number of bytes written.
 */
static ssize_t mode_registers_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct airoha_dramc *dramc = dev_get_drvdata(dev);
	ssize_t len = 0;
	unsigned int i;

	for (i = 0; i < dramc->mode_register_count; i++)
		len += sysfs_emit_at(buf, len, "%s%u", i ? " " : "",
				     dramc->mode_register[i]);

	return sysfs_emit_at(buf, len, "\n") + len;
}
static DEVICE_ATTR_RO(mode_registers);

/**
 * available_frequencies_show() - show firmware-supplied DDR frequencies
 * @dev: memory controller device
 * @attr: sysfs attribute
 * @buf: output buffer
 *
 * Return: number of bytes written.
 */
static ssize_t available_frequencies_show(struct device *dev,
					  struct device_attribute *attr,
					  char *buf)
{
	struct airoha_dramc *dramc = dev_get_drvdata(dev);
	ssize_t len = 0;
	unsigned int i;

	for (i = 0; i < dramc->freq_step_count; i++)
		len += sysfs_emit_at(buf, len, "%s%llu", i ? " " : "",
				     airoha_dramc_frequency(dramc->freq_step[i]));

	return sysfs_emit_at(buf, len, "\n") + len;
}
static DEVICE_ATTR_RO(available_frequencies);

#define AIROHA_MR4_SHOW(_channel) \
	static ssize_t mr4_ch##_channel##_show(struct device *dev, \
					      struct device_attribute *attr, \
					      char *buf) \
	{ \
		struct airoha_dramc *dramc = dev_get_drvdata(dev); \
		int mr4 = airoha_dramc_mr4(dramc, _channel); \
		if (mr4 < 0) \
			return mr4; \
		return sysfs_emit(buf, "%u\n", mr4); \
	} \
	static DEVICE_ATTR_RO(mr4_ch##_channel)

AIROHA_MR4_SHOW(0);
AIROHA_MR4_SHOW(1);

static struct attribute *airoha_dramc_attrs[] = {
	&dev_attr_frequency.attr,
	&dev_attr_dram_data_rate.attr,
	&dev_attr_dram_type.attr,
	&dev_attr_channel_count.attr,
	&dev_attr_dram_size.attr,
	&dev_attr_rank_count.attr,
	&dev_attr_rank_sizes.attr,
	&dev_attr_mode_registers.attr,
	&dev_attr_available_frequencies.attr,
	&dev_attr_mr4_ch0.attr,
	&dev_attr_mr4_ch1.attr,
	NULL,
};

/**
 * airoha_dramc_attr_is_visible() - hide unsupported optional attributes
 * @kobj: device kobject
 * @attr: attribute being evaluated
 * @index: attribute index
 *
 * Return: attribute mode, or zero when unsupported.
 */
static umode_t airoha_dramc_attr_is_visible(struct kobject *kobj,
					    struct attribute *attr, int index)
{
	struct device *dev = kobj_to_dev(kobj);
	struct airoha_dramc *dramc = dev_get_drvdata(dev);

	if ((attr == &dev_attr_rank_count.attr ||
	     attr == &dev_attr_rank_sizes.attr) && !dramc->rank_count)
		return 0;
	if (attr == &dev_attr_mode_registers.attr &&
	    !dramc->mode_register_count)
		return 0;
	if (attr == &dev_attr_available_frequencies.attr &&
	    !dramc->freq_step_count)
		return 0;
	if (attr == &dev_attr_mr4_ch0.attr && !dramc->soc->has_mr4)
		return 0;
	if (attr == &dev_attr_mr4_ch1.attr &&
	    (!dramc->soc->has_mr4 || dramc->soc->channel_count < 2))
		return 0;

	return attr->mode;
}

static const struct attribute_group airoha_dramc_group = {
	.attrs = airoha_dramc_attrs,
	.is_visible = airoha_dramc_attr_is_visible,
};

static const struct attribute_group *airoha_dramc_groups[] = {
	&airoha_dramc_group,
	NULL,
};

/**
 * airoha_dramc_probe() - bind an Airoha DRAM telemetry device
 * @pdev: platform device described by the device tree
 *
 * Return: zero on success, or a negative error code.
 */
static int airoha_dramc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct airoha_dramc *dramc;

	dramc = devm_kzalloc(dev, sizeof(*dramc), GFP_KERNEL);
	if (!dramc)
		return -ENOMEM;

	dramc->soc = device_get_match_data(dev);
	if (!dramc->soc)
		return -EINVAL;

	dramc->base = devm_platform_ioremap_resource_byname(pdev, "dramc");
	if (IS_ERR(dramc->base))
		return PTR_ERR(dramc->base);

	if (!dramc->soc->has_separate_ddrphy)
		dramc->ddrphy = dramc->base;
	else {
		dramc->ddrphy =
			devm_platform_ioremap_resource_byname(pdev, "ddrphy");
		if (IS_ERR(dramc->ddrphy))
			return PTR_ERR(dramc->ddrphy);
	}

	if (dramc->soc->needs_chip_scu) {
		dramc->chip_scu =
			syscon_regmap_lookup_by_phandle(np, "airoha,chip-scu");
		if (IS_ERR(dramc->chip_scu))
			return dev_err_probe(dev, PTR_ERR(dramc->chip_scu),
					     "failed to get chip SCU regmap\n");
	}

	platform_set_drvdata(pdev, dramc);
	return 0;
}

static const struct airoha_dramc_soc_data en751221_dramc_data = {
	.channel_count = 1,
	.rank_config_offset = EN751221_DRAMC_RKCFG,
	.rank_size_offset = EN751221_DRAMC_RKCFG,
	.rank_mode_mask = EN751221_DRAMC_RKMODE,
	.rank_size_mask = EN751221_DRAMC_RKSIZE,
	.data_rate = en751221_dramc_data_rate,
	.dram_type = en751221_dramc_type,
};

static const struct airoha_dramc_soc_data en7523_dramc_data = {
	.channel_count = 1,
	.needs_chip_scu = true,
	.has_separate_ddrphy = true,
	.rank_config_offset = EN7523_DRAMC_RKCFG,
	.rank_size_offset = EN7523_DRAMC_RKCFG,
	.rank_mode_mask = EN7523_DRAMC_RKMODE,
	.rank_size_mask = EN7523_DRAMC_RKSIZE,
	.three_rank_mask = EN7523_DRAMC_THREE_RANKS,
	.data_rate = en7523_dramc_data_rate,
	.dram_type = en7523_dramc_type,
};

static const struct airoha_dramc_soc_data en7528_dramc_data = {
	.channel_count = 1,
	.rank_config_offset = EN751221_DRAMC_RKCFG,
	.rank_size_offset = EN751221_DRAMC_RKCFG,
	.rank_mode_mask = EN751221_DRAMC_RKMODE,
	.rank_size_mask = EN751221_DRAMC_RKSIZE,
	.data_rate = en7528_dramc_data_rate,
	.dram_type = en7523_dramc_type,
};

static const struct airoha_dramc_soc_data en7580_dramc_data = {
	.channel_count = 1,
	.rank_config_offset = EN7523_DRAMC_RKCFG,
	.rank_size_offset = EN7523_DRAMC_RKCFG,
	.rank_mode_mask = EN7523_DRAMC_RKMODE,
	.rank_size_mask = EN7523_DRAMC_RKSIZE,
	.three_rank_mask = EN7523_DRAMC_THREE_RANKS,
	.data_rate = en7580_dramc_data_rate,
	.dram_type = en7523_dramc_type,
};

static const struct airoha_dramc_soc_data an7552_an7583_dramc_data = {
	.channel_count = 2,
	.xtal_mhz = 25,
	.has_mr4 = true,
	.rank_config_offset = AN75XX_DRAMC_CMD_DEC_CTRL0,
	.rank_size_offset = AN75XX_DRAMC_CLKAR,
	.rank_mode_mask = AN75XX_DRAMC_RKMODE,
	.rank_size_mask = AN75XX_DRAMC_RKSIZE,
	.data_rate = an75xx_dramc_data_rate,
	.dram_type = an75xx_dramc_type,
};

static const struct airoha_dramc_soc_data an7581_dramc_data = {
	.channel_count = 2,
	.has_mr4 = true,
	.needs_chip_scu = true,
	.rank_config_offset = AN75XX_DRAMC_CMD_DEC_CTRL0,
	.rank_size_offset = AN75XX_DRAMC_CLKAR,
	.rank_mode_mask = AN75XX_DRAMC_RKMODE,
	.rank_size_mask = AN75XX_DRAMC_RKSIZE,
	.data_rate = an75xx_dramc_data_rate,
	.dram_type = an75xx_dramc_type,
};

static const struct of_device_id airoha_dramc_of_match[] = {
	{ .compatible = "econet,en751221-dramc", .data = &en751221_dramc_data },
	{ .compatible = "econet,en7528-dramc", .data = &en7528_dramc_data },
	{ .compatible = "econet,en7580-dramc", .data = &en7580_dramc_data },
	{ .compatible = "airoha,en7523-dramc", .data = &en7523_dramc_data },
	{ .compatible = "airoha,an7552-dramc", .data = &an7552_an7583_dramc_data },
	{ .compatible = "airoha,an7581-dramc", .data = &an7581_dramc_data },
	{ .compatible = "airoha,an7583-dramc", .data = &an7552_an7583_dramc_data },
	{ }
};
MODULE_DEVICE_TABLE(of, airoha_dramc_of_match);

static struct platform_driver airoha_dramc_driver = {
	.probe = airoha_dramc_probe,
	.driver = {
		.name = "airoha-dramc",
		.of_match_table = airoha_dramc_of_match,
		.dev_groups = airoha_dramc_groups,
	},
};
module_platform_driver(airoha_dramc_driver);

MODULE_AUTHOR("Benjamin Larsson <benjamin.larsson@genexis.eu>");
MODULE_DESCRIPTION("Airoha DRAM controller telemetry driver");
MODULE_LICENSE("GPL");
