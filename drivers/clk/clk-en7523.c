// SPDX-License-Identifier: GPL-2.0-only

#include <linux/bitfield.h>
#include <linux/bitops.h>
#include <linux/clk-provider.h>
#include <linux/delay.h>
#include <linux/export.h>
#include <linux/io.h>
#include <linux/mfd/syscon.h>
#include <linux/of_address.h>
#include <linux/of_platform.h>
#include <linux/phy.h>
#include <linux/phy/phy.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/reset-controller.h>
#include <linux/soc/airoha/pkgids.h>
#include <dt-bindings/clock/econet,en751221-scu.h>
#include <dt-bindings/clock/en7523-clk.h>
#include <dt-bindings/reset/airoha,an7583-reset.h>
#include <dt-bindings/reset/airoha,en7523-reset.h>
#include <dt-bindings/reset/airoha,en7581-reset.h>
#include <dt-bindings/reset/econet,en751221-scu.h>
#include <dt-bindings/soc/airoha,scu-ssr.h>

#define RST_NR_PER_BANK			32

#define REG_PCI_CONTROL			0x88
#define   REG_PCI_CONTROL_PERSTOUT	BIT(29)
#define   REG_PCI_CONTROL_PERSTOUT1	BIT(26)
#define   REG_PCI_CONTROL_REFCLK_EN0	BIT(23)
#define   REG_PCI_CONTROL_REFCLK_EN1	BIT(22)
#define   REG_PCI_CONTROL_PERSTOUT2	BIT(16)
#define REG_GSW_CLK_DIV_SEL		0x1b4
#define REG_EMI_CLK_DIV_SEL		0x1b8
#define REG_BUS_CLK_DIV_SEL		0x1bc
#define REG_SPI_CLK_DIV_SEL		0x1c4
#define REG_SPI_CLK_FREQ_SEL		0x1c8
#define REG_NPU_CLK_DIV_SEL		0x1fc
#define REG_CRYPTO_CLKSRC		0x200
#define REG_RESET_CONTROL2		0x830
#define   REG_RESET2_CONTROL_PCIE2	BIT(27)
#define REG_RESET_CONTROL1		0x834
#define   REG_RESET_CONTROL_PCIEHB	BIT(29)
#define   REG_RESET_CONTROL_PCIE1	BIT(27)
#define   REG_RESET_CONTROL_PCIE2	BIT(26)
#define REG_TRNG_BUS_CLK_GAT		0x1e4
#define REG_TRNG_PER_CLK_GAT_1		0x1e8
#define REG_TRNG_PER_CLK_GAT_2		0x1ec
#define   REG_TRNG_BUS_EN		BIT(3)
#define   REG_TRNG_PER1_EN		BIT(21)
#define   REG_TRNG_PER2_EN		BIT(0)
/* EN7581 */
#define REG_NP_SCU_PCIC			0x88
#define REG_PCIE_CTRL			GENMASK(7, 0)
#define REG_NP_SCU_SSR3			0x94
#define REG_SSUSB_HSGMII_SEL_MASK	BIT(29)
#define REG_SSUSB_HSGMII_SEL_HSGMII	FIELD_PREP_CONST(REG_SSUSB_HSGMII_SEL_MASK, 0x0)
#define REG_SSUSB_HSGMII_SEL_USB	FIELD_PREP_CONST(REG_SSUSB_HSGMII_SEL_MASK, 0x1)
#define REG_NP_SCU_SSTR			0x9c
#define REG_PCIE_XSI0_SEL_MASK		GENMASK(14, 13)
#define REG_PCIE_XSI0_SEL_PCIE		FIELD_PREP_CONST(REG_PCIE_XSI0_SEL_MASK, 0x0)
#define REG_PCIE_XSI0_SEL_XFI		FIELD_PREP_CONST(REG_PCIE_XSI0_SEL_MASK, 0x1)
#define REG_PCIE_XSI0_SEL_HSGMII	FIELD_PREP_CONST(REG_PCIE_XSI0_SEL_MASK, 0x2)
#define REG_PCIE_XSI1_SEL_MASK		GENMASK(12, 11)
#define REG_PCIE_XSI1_SEL_PCIE		FIELD_PREP_CONST(REG_PCIE_XSI1_SEL_MASK, 0x0)
#define REG_PCIE_XSI1_SEL_XFI		FIELD_PREP_CONST(REG_PCIE_XSI1_SEL_MASK, 0x1)
#define REG_PCIE_XSI1_SEL_HSGMII	FIELD_PREP_CONST(REG_PCIE_XSI1_SEL_MASK, 0x2)
#define REG_USB_PCIE_SEL_MASK		BIT(3)
#define REG_USB_PCIE_SEL_PCIE		FIELD_PREP_CONST(REG_USB_PCIE_SEL_MASK, 0x0)
#define REG_USB_PCIE_SEL_USB		FIELD_PREP_CONST(REG_USB_PCIE_SEL_MASK, 0x1)
#define REG_CRYPTO_CLKSRC2		0x20c
#define REG_TRNG_PER_CLK_GAT_1_AN7581	0x1ec
#define REG_TRNG_PER_CLK_GAT_2_AN7581	0x200
/* EN751221 */
#define EN751221_REG_SPI_DIV		0x0cc
#define EN751221_SPI_BASE		500000000
#define EN751221_SPI_BASE_EN7526C	400000000
#define EN751221_SPI_DIV_DEFAULT	40
#define EN751221_REG_BUS_MASK		GENMASK(21, 12)
#define EN7528_REG_BUS_MASK		GENMASK(19, 10)
#define EN751221_REG_SSR3		0x094
#define EN751221_REG_NP_PER_DOM_CLK_GAT_1	0x0e4
#define   EN751221_XPON_TOD_CLK_EN	BIT(8)
#define EN751221_REG_TOD_DIVIDER_ENABLE	0x0ec
#define   EN751221_XPON_TOD_DIV_EN	BIT(1)

/* EN7516/EN7527 use a different CHIP-SCU register layout */
#define EN751627_REG_SPI_DIV		0x11c
#define EN751627_SPI_BASE		400000000
#define EN751627_SPI_DIV_DEFAULT	40
#define EN751627_REG_NP_PER_DOM_CLK_GAT_1	0x13c
#define EN751627_REG_TOD_DIVIDER_ENABLE	0x144

/* EN7528 CHIP-SCU differs from both EN751627 and EN7523 */
#define EN7528_REG_SPI_DIV		0x0cc
#define EN7528_SPI_BASE			400000000
#define EN7528_SPI_DIV_DEFAULT		10
#define EN7528_REG_NP_PER_DOM_CLK_GAT_1	0x0e4
#define   EN7528_XPON_TOD_CLK_EN	BIT(8)
#define EN7528_REG_TOD_DIVIDER_ENABLE	0x0ec
#define   EN7528_XPON_TOD_DIV_EN	BIT(1)
#define EN7528_REG_SHARED_UNZIPMENT_SEL	0x954
#define EN7528_SHARED_UNZIPMENT_FE	0x3

/* EN7580 MIPS CHIP-SCU layout */
#define EN7580_REG_SPI_DIV		0x1b8
#define EN7580_SPI_BASE			400000000
#define EN7580_SPI_DIV_DEFAULT		40
#define EN7580_REG_NP_PER_DOM_CLK_GAT_1	0x1dc
#define EN7580_REG_TOD_DIVIDER_ENABLE	0x1e4

#define REG_RST_CTRL2			0x830
#define REG_RST_CTRL1			0x834
#define   REG_PCIE_HB_RST		BIT(29)
#define EN751221_REG_RST_DMT		0x84
#define EN751221_REG_RST_USB		0xec

#define EN751221_MAX_CLKS		5

/* SCU package ID */
#define AIROHA_NP_SCU_PDIDR		0x05c
#define AIROHA_NP_SCU_HIR		0x064
#define AIROHA_NP_SCU_SUBMODEL		0x0f8
#define AIROHA_NP_SCU_SCREG_WR1		0x284
#define AIROHA_CHIP_SCU_PACKAGE		0x174
#define AIROHA_CHIP_SCU_CONFIG		0x1ec
#define AIROHA_EFUSE_VERIFY_DATA0	0x014
#define AIROHA_EFUSE_VERIFY_DATA1	0x018

#define AIROHA_NP_SCU_HIR_MASK		GENMASK(31, 16)
#define AIROHA_NP_SCU_PDIDR_MASK	GENMASK(15, 0)
#define AIROHA_NP_SCU_PACKAGE_ID_MASK	GENMASK(3, 0)
#define AIROHA_NP_SCU_PACKAGE_ID_EXT	BIT(7)
#define AIROHA_NP_SCU_SUBMODEL_MASK	GENMASK(1, 0)

/* EN7512/EN7521 and EN7526C/EN7522 eFuse encoding */
#define AIROHA_EFUSE_PKG_MASK		GENMASK(5, 0)
#define AIROHA_EFUSE_PKG_IGNORE_BIT01	GENMASK(5, 2)
#define AIROHA_EFUSE_REMARK_BIT		BIT(6)
#define AIROHA_EFUSE_PKG_REMARK_SHIFT	7

#define AIROHA_EFUSE_DDR3_BIT		BIT(23)
#define AIROHA_EFUSE_DDR3_REMARK_BIT	BIT(24)

/* EN7516/EN7527 eFuse encoding */
#define AIROHA_EFUSE_7516_PKG_MASK	GENMASK(19, 18)
#define AIROHA_EFUSE_7516_REMARK_BIT	BIT(0)
#define AIROHA_EFUSE_7516_REMARK_SHIFT	2

#define AIROHA_EFUSE_EN7527H		0x00000
#define AIROHA_EFUSE_EN7527G		0x00000
#define AIROHA_EFUSE_EN7516G		0x80000
#define AIROHA_EFUSE_EN7561G		0xc0000

/* Additional package selectors used by the legacy vendor code */
#define AIROHA_EN751627_QFP_MASK	BIT(15)
#define AIROHA_EN7526C_FP_MASK		BIT(10)
#define AIROHA_EN7526C_FTC_BIT		BIT(9)
#define AIROHA_EN7526C_FTC_REMARK_BIT	BIT(10)

/* MT7510/MT7520 module-selection bits in EFUSE_VERIFY_DATA0 */
#define AIROHA_MT751020_ENP_SEL		BIT(3)
#define AIROHA_MT751020_ENP_LOW		BIT(1)
#define AIROHA_MT751020_ENP_HIGH	BIT(5)
#define AIROHA_MT751020_ENS_LOW		BIT(2)
#define AIROHA_MT751020_ENS_HIGH	BIT(6)

#define EN_SERDES_PHY_NUM		4

struct en_clk_desc {
	int id;
	const char *name;
	bool np_scu;
	bool read_only;

	u32 base_reg;
	u8 base_bits;
	u8 base_shift;
	union {
		const unsigned int *base_values;
		unsigned int base_value;
	};
	size_t n_base_values;

	u16 div_reg;
	u8 div_bits;
	u8 div_shift;
	u16 div_val0;
	u8 div_step;
	u8 div_offset;
};

struct en_clk_gate {
	struct regmap *map;
	struct clk_hw hw;
};

struct en_clk {
	struct regmap *map;
	const struct en_clk_desc *desc;
	struct clk_hw hw;
};

struct en_rst_data {
	const u16 *bank_ofs;
	const u16 *idx_map;
	struct regmap *map;
	struct reset_controller_dev rcdev;
};

struct en_serdes_phy_instance {
	struct phy *phy;
	unsigned int serdes_port;
};

struct econet_clk_soc_data {
	const struct en_clk_desc *spi_alt_desc;
	u16 xpon_tod_clk_reg;
	u32 xpon_tod_clk_mask;
	u16 xpon_tod_div_reg;
	u32 xpon_tod_div_mask;
	u16 shared_unzip_reg;
	u32 shared_unzip_val;
};

struct airoha_clk_priv;

struct airoha_clk_soc_data {
	const char *old_chip_scu_compatible;
	bool probe_child;
	/* EcoNet bootloader bus clock field and CPU/bus ratio */
	u32 bus_clk_mask;
	u8 cpu_clk_mult;
	u32 num_clocks;
	u32 num_base_clks;
	u32 pcie_clk_id;
	u32 bus_clk_id;
	u32 cpu_clk_id;
	const u16 *rst_map;
	const u16 *rst_ofs;
	int nr_resets;
	const struct en_clk_desc *base_clks;
	const struct econet_clk_soc_data *econet;
	const struct clk_ops pcie_ops;
	int (*hw_init)(struct platform_device *pdev,
		       struct airoha_clk_priv *priv,
		       struct clk_hw_onecell_data *clk_data);
};

struct airoha_clk_priv {
	const struct airoha_clk_soc_data *soc_data;
	enum airoha_id_source soc_source;
	enum airoha_pkg soc_family;
	enum airoha_pkg_ids soc_id;
	struct regmap *scu_chip;
	struct regmap *scu_np;
	void __iomem *np_scu_base;
	void __iomem *chip_scu_base;
	resource_size_t chip_scu_size;
	void __iomem *efuse_base;
	struct en_serdes_phy_instance *serdes_phys[EN_SERDES_PHY_NUM];
};

static const struct of_device_id of_match_clk_en7523[];

static const char *const airoha_pkg_id_names[] = {
	[EN7529DU] = "EN7529DU",
	[EN7529DT] = "EN7529DT",
	[EN7529CU] = "EN7529CU",
	[EN7562DU] = "EN7562DU",
	[EN7562DT] = "EN7562DT",
	[EN7562CU] = "EN7562CU",
	[EN7523GU] = "EN7523GU",
	[EN7523DU] = "EN7523DU",
	[EN7529GTH] = "EN7529GTH",
	[EN7562GTH] = "EN7562GTH",
	[EN7523SU] = "EN7523SU",
	[EN7529GTS] = "EN7529GTS",
	[EN7562GTS] = "EN7562GTS",
	[EN7529IT] = "EN7529IT",
	[EN7529CT] = "EN7529CT",
	[EN7562CT] = "EN7562CT",
	[EN7523DT] = "EN7523DT",
	[EN7529DTM] = "EN7529DTM",
	[EN7562DTM] = "EN7562DTM",
	[EN7529ITM] = "EN7529ITM",
	[EN7529CTM] = "EN7529CTM",
	[EN7562CTM] = "EN7562CTM",
	[EN7523DTM] = "EN7523DTM",
	[EN7528HU] = "EN7528HU",
	[EN7528DU] = "EN7528DU",
	[EN7561DU] = "EN7561DU",
	[EN7526FH_EN7528DU] = "EN7526FH_EN7528DU",
	[EN7561HU] = "EN7561HU",
	[EN7521G_EN7528DU] = "EN7521G_EN7528DU",
	[EN7580GT] = "EN7580GT",
	[EN7580ST] = "EN7580ST",
	[EN7580GAT] = "EN7580GAT",
	[EN7565] = "EN7565",
	[EN7528GT_EN7580] = "EN7528GT_EN7580",
	[EN7580] = "EN7580",
	[EN7516G] = "EN7516G",
	[EN7527H] = "EN7527H",
	[EN7527G] = "EN7527G",
	[EN7561G] = "EN7561G",
	[EN751627] = "EN751627",
	[EN7512] = "EN7512",
	[EN7513] = "EN7513",
	[EN7513G] = "EN7513G",
	[EN7521FCUD] = "EN7521FCUD",
	[EN7521F] = "EN7521F",
	[EN7521S] = "EN7521S",
	[EN7521G] = "EN7521G",
	[EN7526D] = "EN7526D",
	[EN7526F] = "EN7526F",
	[EN7526G] = "EN7526G",
	[EN7526FT] = "EN7526FT",
	[EN7526FP] = "EN7526FP",
	[EN7526FT_C] = "EN7526FT_C",
	[EN7586] = "EN7586",
	[EN751221] = "EN751221",
	[MT7510] = "MT7510",
	[MT7511] = "MT7511",
	[MT7520S] = "MT7520S",
	[MT7520] = "MT7520",
	[MT7520G] = "MT7520G",
	[MT7525] = "MT7525",
	[MT7525G] = "MT7525G",
	[AN7581GT] = "AN7581GT",
	[AN7566GT] = "AN7566GT",
	[AN7581PT] = "AN7581PT",
	[AN7581ST] = "AN7581ST",
	[AN7551PT] = "AN7551PT",
	[AN7581CT] = "AN7581CT",
	[AN7581DT] = "AN7581DT",
	[AN7581FG] = "AN7581FG",
	[AN7581FP] = "AN7581FP",
	[AN7581FD] = "AN7581FD",
	[AN7551GT] = "AN7551GT",
	[AN7566PT] = "AN7566PT",
	[AN7581IT] = "AN7581IT",
	[AN7581SIT] = "AN7581SIT",
	[AN7552CT] = "AN7552CT",
	[AN7552ST] = "AN7552ST",
	[AN7552FT] = "AN7552FT",
	[AN7563CT] = "AN7563CT",
	[AN7563PT] = "AN7563PT",
	[AN7583GT] = "AN7583GT",
	[AN7583GIT] = "AN7583GIT",
	[AN7583CT] = "AN7583CT",
	[AN7583DT] = "AN7583DT",
	[AN7583ST] = "AN7583ST",
	[AN9510GT] = "AN9510GT",
	[AN7553GT] = "AN7553GT",
	[AN7553CT] = "AN7553CT",
	[AN7567GT] = "AN7567GT",
	[AN7567CT] = "AN7567CT",
	[AN7583ET] = "AN7583ET",
	[AN7583EIT] = "AN7583EIT",
	[AN7583FG] = "AN7583FG",
	[AN7583FP] = "AN7583FP",
	[AN7583FD] = "AN7583FD",
	[AN7583FS] = "AN7583FS",
	[AN7583FF] = "AN7583FF",
};

static enum airoha_pkg_ids
airoha_pkg_id_from_range(u32 pkgid, enum airoha_pkg_ids first,
			 enum airoha_pkg_ids last)
{
	u32 id;

	if (pkgid > (u32)(last - first))
		return END_PACKAGE_ID;

	id = first + pkgid;
	if (id >= ARRAY_SIZE(airoha_pkg_id_names) ||
	    !airoha_pkg_id_names[id])
		return END_PACKAGE_ID;

	return id;
}

static enum airoha_pkg airoha_pkg_from_id(u32 id)
{
	switch (id) {
	case AN7583_PKG:
	case 0x7583:
	case 0x9510:
	case 0x7553:
	case 0x7567:
		return AN7583_PKG;
	case AN7552_PKG:
	case 0x7552:
	case 0x7563:
		return AN7552_PKG;
	case EN7581_PKG:
	case 0x7581:
	case 0x7566:
	case 0x7551:
		return EN7581_PKG;
	case EN7523_PKG:
	case 0x7523:
	case 0x7529:
	case 0x7562:
		return EN7523_PKG;
	case EN7528_PKG:
	case 0x7528:
	case 0x7561:
		return EN7528_PKG;
	case EN7580_PKG:
	case 0x7580:
	case 0x7565:
		return EN7580_PKG;
	case EN751627_PKG:
	case 0x7516:
	case 0x7527:
		return EN751627_PKG;
	case EN7526C_PKG:
	case 0x7522:
		return EN7526C_PKG;
	case EN751221_PKG:
	case 0x7512:
	case 0x7513:
	case 0x7521:
	case 0x7526:
		return EN751221_PKG;
	case MT751020_PKG:
	case 0x7510:
	case 0x7520:
	case 0x7525:
		return MT751020_PKG;
	default:
		return UNKNOWN_PKG;
	}
}

static u32 airoha_efuse_pkg_value(u32 efuse_data0)
{
	if (efuse_data0 & AIROHA_EFUSE_REMARK_BIT)
		return (efuse_data0 >> AIROHA_EFUSE_PKG_REMARK_SHIFT) &
		       AIROHA_EFUSE_PKG_MASK;

	return efuse_data0 & AIROHA_EFUSE_PKG_MASK;
}

static u32 airoha_efuse_7516_pkg_value(u32 efuse_data0)
{
	if (efuse_data0 & AIROHA_EFUSE_7516_REMARK_BIT)
		return (efuse_data0 >> AIROHA_EFUSE_7516_REMARK_SHIFT) &
		       AIROHA_EFUSE_7516_PKG_MASK;

	return efuse_data0 & AIROHA_EFUSE_7516_PKG_MASK;
}

static bool airoha_efuse_is_ddr3(u32 efuse_data0)
{
	if (efuse_data0 & AIROHA_EFUSE_REMARK_BIT)
		return !!(efuse_data0 & AIROHA_EFUSE_DDR3_REMARK_BIT);

	return !!(efuse_data0 & AIROHA_EFUSE_DDR3_BIT);
}

static bool airoha_en7526c_is_ft_c(u32 efuse_data0, u32 efuse_data1)
{
	if (efuse_data0 & AIROHA_EFUSE_REMARK_BIT)
		return !!(efuse_data1 & AIROHA_EN7526C_FTC_REMARK_BIT);

	return !!(efuse_data1 & AIROHA_EN7526C_FTC_BIT);
}

static enum airoha_pkg_ids airoha_en751221_variant_id(u32 efuse_data0)
{
	switch (airoha_efuse_pkg_value(efuse_data0)) {
	case EN7526F_EFUSE:
		return EN7526F;
	case EN7526D_EFUSE:
		return EN7526D;
	case EN7526G_EFUSE:
		return EN7526G;
	case EN7512_EFUSE:
		return EN7512;
	case EN7513_EFUSE:
		return EN7513;
	case EN7513G_EFUSE:
		return EN7513G;
	case EN7586_EFUSE:
		return EN7586;
	case EN7521F_EFUSE:
		return EN7521F;
	case EN7526FT_EFUSE:
		return EN7526FT;
	case EN7521G_EFUSE:
		return EN7521G;
	case EN7521S_EFUSE:
		return EN7521S;
	default:
		return EN751221;
	}
}

static enum airoha_pkg_ids
airoha_en7526c_variant_id(u32 efuse_data0, u32 efuse_data1, u32 chip_scu_config)
{
	u32 pkg = airoha_efuse_pkg_value(efuse_data0) &
		  AIROHA_EFUSE_PKG_IGNORE_BIT01;

	/* The vendor macros make these refinements orthogonal to the base ID */
	if (airoha_en7526c_is_ft_c(efuse_data0, efuse_data1))
		return EN7526FT_C;

	switch (pkg) {
	case EN7526F_EFUSE:
		if (chip_scu_config & AIROHA_EN7526C_FP_MASK)
			return EN7526FP;
		return EN7526F;
	case EN7521F_EFUSE:
		if (airoha_efuse_is_ddr3(efuse_data0))
			return EN7521FCUD;
		return EN7521F;
	case EN7521S_EFUSE:
		return EN7521S;
	default:
		return END_PACKAGE_ID;
	}
}

static enum airoha_pkg_ids
airoha_en751627_variant_id(u32 efuse_data0, u32 chip_scu_package)
{
	switch (airoha_efuse_7516_pkg_value(efuse_data0)) {
	case AIROHA_EFUSE_EN7527G:
		if (chip_scu_package & AIROHA_EN751627_QFP_MASK)
			return EN7527H;
		return EN7527G;
	case AIROHA_EFUSE_EN7516G:
		return EN7516G;
	case AIROHA_EFUSE_EN7561G:
		return EN7561G;
	default:
		return EN751627;
	}
}

static bool airoha_mt751020_is_enp(u32 efuse_data0)
{
	if (efuse_data0 & AIROHA_MT751020_ENP_SEL)
		return !!(efuse_data0 & AIROHA_MT751020_ENP_HIGH);

	return !!(efuse_data0 & AIROHA_MT751020_ENP_LOW);
}

static bool airoha_mt751020_is_ens(u32 efuse_data0)
{
	if (efuse_data0 & AIROHA_MT751020_ENP_SEL)
		return !!(efuse_data0 & AIROHA_MT751020_ENS_HIGH);

	return !!(efuse_data0 & AIROHA_MT751020_ENS_LOW);
}

static enum airoha_pkg_ids
airoha_mt751020_variant_id(u32 submodel, u32 efuse_data0)
{
	bool enp = airoha_mt751020_is_enp(efuse_data0);
	bool ens = airoha_mt751020_is_ens(efuse_data0);

	switch (FIELD_GET(AIROHA_NP_SCU_SUBMODEL_MASK, submodel)) {
	case 0:
		return enp ? MT7510 : MT7511;
	case 2:
		if (ens)
			return MT7520S;
		return enp ? MT7520 : MT7525;
	case 3:
		return enp ? MT7520G : MT7525G;
	default:
		return END_PACKAGE_ID;
	}
}

static enum airoha_pkg_ids airoha_pkgid_variant_id(enum airoha_pkg pkg, u32 pkgid)
{
	switch (airoha_pkg_from_id(pkg)) {
	case AN7583_PKG:
		return airoha_pkg_id_from_range(pkgid, AN7583GT, AN7583FF);
	case AN7552_PKG:
		return airoha_pkg_id_from_range(pkgid, AN7552CT, AN7563PT);
	case EN7581_PKG:
		return airoha_pkg_id_from_range(pkgid, AN7581GT, AN7581SIT);
	case EN7523_PKG:
		return airoha_pkg_id_from_range(pkgid, EN7529DU, EN7523DTM);
	case EN7528_PKG:
		switch (pkgid) {
		case 0x0:
			return EN7528HU;
		case 0x1:
			return EN7528DU;
		case 0x2:
			return EN7561DU;
		case 0x3:
			return EN7526FH_EN7528DU;
		case 0x4:
			return EN7561HU;
		case 0x7:
			return EN7521G_EN7528DU;
		default:
			return END_PACKAGE_ID;
		}
	case EN7580_PKG:
		switch (pkgid) {
		case 0x0:
			return EN7580GT;
		case 0x1:
			return EN7580ST;
		case 0x2:
			return EN7580GAT;
		case 0x3:
			return EN7565;
		case 0x4:
			return EN7528GT_EN7580;
		default:
			return END_PACKAGE_ID;
		}
	default:
		return END_PACKAGE_ID;
	}
}

static const u32 gsw_base[] = { 400000000, 500000000 };
static const u32 emi_base[] = { 333000000, 400000000 };
static const u32 bus_base[] = { 500000000, 540000000 };
static const u32 slic_base[] = { 100000000, 3125000 };
static const u32 npu_base[] = { 333000000, 400000000, 500000000 };
/* EN7581 */
static const u32 emi7581_base[] = { 540000000, 480000000, 400000000, 300000000 };
static const u32 bus7581_base[] = { 600000000, 540000000 };
static const u32 npu7581_base[] = { 800000000, 750000000, 720000000, 600000000 };
static const u32 crypto_base[] = { 540000000, 480000000 };
static const u32 emmc7581_base[] = { 200000000, 150000000 };
/* EN751221 */
static const u32 gsw751221_base[] = { 500000000, 250000000, 400000000, 200000000 };
/* AN7583 */
static const u32 gsw7583_base[] = { 540672000, 270336000, 400000000, 200000000 };
static const u32 emi7583_base[] = { 540672000, 480000000, 400000000, 300000000 };
static const u32 bus7583_base[] = { 600000000, 540672000, 480000000, 400000000 };
static const u32 spi7583_base[] = { 400000000, 12500000 };
static const u32 npu7583_base[] = { 666000000, 800000000, 720000000, 600000000 };
static const u32 crypto7583_base[] = { 540672000, 400000000 };
static const u32 emmc7583_base[] = { 150000000, 200000000 };

static const struct en_clk_desc en7523_base_clks[] = {
	{
		.id = EN7523_CLK_GSW,
		.name = "gsw",

		.base_reg = REG_GSW_CLK_DIV_SEL,
		.base_bits = 1,
		.base_shift = 8,
		.base_values = gsw_base,
		.n_base_values = ARRAY_SIZE(gsw_base),

		.div_bits = 3,
		.div_shift = 0,
		.div_step = 1,
		.div_offset = 1,
	}, {
		.id = EN7523_CLK_EMI,
		.name = "emi",

		.base_reg = REG_EMI_CLK_DIV_SEL,
		.base_bits = 1,
		.base_shift = 8,
		.base_values = emi_base,
		.n_base_values = ARRAY_SIZE(emi_base),

		.div_bits = 3,
		.div_shift = 0,
		.div_step = 1,
		.div_offset = 1,
	}, {
		.id = EN7523_CLK_BUS,
		.name = "bus",

		.base_reg = REG_BUS_CLK_DIV_SEL,
		.base_bits = 1,
		.base_shift = 8,
		.base_values = bus_base,
		.n_base_values = ARRAY_SIZE(bus_base),

		.div_bits = 3,
		.div_shift = 0,
		.div_step = 1,
		.div_offset = 1,
	}, {
		.id = EN7523_CLK_SLIC,
		.name = "slic",

		.base_reg = REG_SPI_CLK_FREQ_SEL,
		.base_bits = 1,
		.base_shift = 0,
		.base_values = slic_base,
		.n_base_values = ARRAY_SIZE(slic_base),

		.div_reg = REG_SPI_CLK_DIV_SEL,
		.div_bits = 5,
		.div_shift = 24,
		.div_val0 = 20,
		.div_step = 2,
	}, {
		.id = EN7523_CLK_SPI,
		.name = "spi",

		.base_reg = REG_SPI_CLK_DIV_SEL,

		.base_value = 400000000,

		.div_bits = 5,
		.div_shift = 8,
		.div_val0 = 40,
		.div_step = 2,
	}, {
		.id = EN7523_CLK_NPU,
		.name = "npu",

		.base_reg = REG_NPU_CLK_DIV_SEL,
		.base_bits = 2,
		.base_shift = 8,
		.base_values = npu_base,
		.n_base_values = ARRAY_SIZE(npu_base),

		.div_bits = 3,
		.div_shift = 0,
		.div_step = 1,
		.div_offset = 1,
	}, {
		.id = EN7523_CLK_CRYPTO,
		.name = "crypto",

		.base_reg = REG_CRYPTO_CLKSRC,
		.base_bits = 1,
		.base_shift = 0,
		.base_values = emi_base,
		.n_base_values = ARRAY_SIZE(emi_base),
	}
};

static const struct en_clk_desc en7581_base_clks[] = {
	{
		.id = EN7523_CLK_GSW,
		.name = "gsw",

		.base_reg = REG_GSW_CLK_DIV_SEL,
		.base_bits = 1,
		.base_shift = 8,
		.base_values = gsw_base,
		.n_base_values = ARRAY_SIZE(gsw_base),

		.div_bits = 3,
		.div_shift = 0,
		.div_step = 1,
		.div_offset = 1,
	}, {
		.id = EN7523_CLK_EMI,
		.name = "emi",

		.base_reg = REG_EMI_CLK_DIV_SEL,
		.base_bits = 2,
		.base_shift = 8,
		.base_values = emi7581_base,
		.n_base_values = ARRAY_SIZE(emi7581_base),

		.div_bits = 3,
		.div_shift = 0,
		.div_step = 1,
		.div_offset = 1,
	}, {
		.id = EN7523_CLK_BUS,
		.name = "bus",

		.base_reg = REG_BUS_CLK_DIV_SEL,
		.base_bits = 1,
		.base_shift = 8,
		.base_values = bus7581_base,
		.n_base_values = ARRAY_SIZE(bus7581_base),

		.div_bits = 3,
		.div_shift = 0,
		.div_step = 1,
		.div_offset = 1,
	}, {
		.id = EN7523_CLK_SLIC,
		.name = "slic",

		.base_reg = REG_SPI_CLK_FREQ_SEL,
		.base_bits = 1,
		.base_shift = 0,
		.base_values = slic_base,
		.n_base_values = ARRAY_SIZE(slic_base),

		.div_reg = REG_SPI_CLK_DIV_SEL,
		.div_bits = 5,
		.div_shift = 24,
		.div_val0 = 20,
		.div_step = 2,
	}, {
		.id = EN7523_CLK_SPI,
		.name = "spi",

		.base_reg = REG_SPI_CLK_DIV_SEL,

		.base_value = 400000000,

		.div_bits = 5,
		.div_shift = 8,
		.div_val0 = 40,
		.div_step = 2,
	}, {
		.id = EN7523_CLK_NPU,
		.name = "npu",

		.base_reg = REG_NPU_CLK_DIV_SEL,
		.base_bits = 2,
		.base_shift = 8,
		.base_values = npu7581_base,
		.n_base_values = ARRAY_SIZE(npu7581_base),

		.div_bits = 3,
		.div_shift = 0,
		.div_step = 1,
		.div_offset = 1,
	}, {
		.id = EN7523_CLK_CRYPTO,
		.name = "crypto",

		.base_reg = REG_CRYPTO_CLKSRC2,
		.base_bits = 1,
		.base_shift = 0,
		.base_values = crypto_base,
		.n_base_values = ARRAY_SIZE(crypto_base),
	}, {
		.id = EN7581_CLK_EMMC,
		.name = "emmc",

		.base_reg = REG_CRYPTO_CLKSRC2,
		.base_bits = 1,
		.base_shift = 12,
		.base_values = emmc7581_base,
		.n_base_values = ARRAY_SIZE(emmc7581_base),
	}
};

static const struct en_clk_desc an7583_base_clks[] = {
	{
		.id = EN7523_CLK_GSW,
		.name = "gsw",

		.base_reg = REG_GSW_CLK_DIV_SEL,
		.base_bits = 2,
		.base_shift = 8,
		.base_values = gsw7583_base,
		.n_base_values = ARRAY_SIZE(gsw7583_base),

		.div_bits = 3,
		.div_shift = 0,
		.div_step = 1,
		.div_offset = 1,
	}, {
		.id = EN7523_CLK_EMI,
		.name = "emi",

		.base_reg = REG_EMI_CLK_DIV_SEL,
		.base_bits = 2,
		.base_shift = 8,
		.base_values = emi7583_base,
		.n_base_values = ARRAY_SIZE(emi7583_base),

		.div_bits = 3,
		.div_shift = 0,
		.div_step = 1,
		.div_offset = 1,
	}, {
		.id = EN7523_CLK_BUS,
		.name = "bus",

		.base_reg = REG_BUS_CLK_DIV_SEL,
		.base_bits = 2,
		.base_shift = 8,
		.base_values = bus7583_base,
		.n_base_values = ARRAY_SIZE(bus7583_base),

		.div_bits = 3,
		.div_shift = 0,
		.div_step = 1,
		.div_offset = 1,
	}, {
		.id = EN7523_CLK_SLIC,
		.name = "slic",

		.base_reg = REG_SPI_CLK_FREQ_SEL,
		.base_bits = 1,
		.base_shift = 1,
		.base_values = slic_base,
		.n_base_values = ARRAY_SIZE(slic_base),

		.div_reg = REG_SPI_CLK_DIV_SEL,
		.div_bits = 5,
		.div_shift = 24,
		.div_val0 = 20,
		.div_step = 2,
	}, {
		.id = EN7523_CLK_SPI,
		.name = "spi",

		.base_reg = REG_SPI_CLK_FREQ_SEL,
		.base_bits = 1,
		.base_shift = 0,
		.base_values = spi7583_base,
		.n_base_values = ARRAY_SIZE(spi7583_base),

		.div_reg = REG_SPI_CLK_DIV_SEL,
		.div_bits = 5,
		.div_shift = 8,
		.div_val0 = 40,
		.div_step = 2,
	}, {
		.id = EN7523_CLK_NPU,
		.name = "npu",

		.base_reg = REG_NPU_CLK_DIV_SEL,
		.base_bits = 2,
		.base_shift = 9,
		.base_values = npu7583_base,
		.n_base_values = ARRAY_SIZE(npu7583_base),

		.div_bits = 3,
		.div_shift = 0,
		.div_step = 1,
		.div_offset = 1,
	}, {
		.id = EN7523_CLK_CRYPTO,
		.name = "crypto",

		.base_reg = REG_CRYPTO_CLKSRC2,
		.base_bits = 1,
		.base_shift = 0,
		.base_values = crypto7583_base,
		.n_base_values = ARRAY_SIZE(crypto7583_base),
	}, {
		.id = EN7581_CLK_EMMC,
		.name = "emmc",

		.base_reg = REG_CRYPTO_CLKSRC2,
		.base_bits = 1,
		.base_shift = 13,
		.base_values = emmc7583_base,
		.n_base_values = ARRAY_SIZE(emmc7583_base),
	}, {
		.id = AN7583_CLK_MDIO0,
		.name = "mdio0",

		.base_reg = REG_CRYPTO_CLKSRC2,

		.base_value = 25000000,

		.div_bits = 4,
		.div_shift = 15,
		.div_step = 1,
		.div_offset = 1,
	}, {
		.id = AN7583_CLK_MDIO1,
		.name = "mdio1",

		.base_reg = REG_CRYPTO_CLKSRC2,

		.base_value = 25000000,

		.div_bits = 4,
		.div_shift = 19,
		.div_step = 1,
		.div_offset = 1,
	}
};

static const struct en_clk_desc en751221_base_clks[] = {
	{
		.id = EN751221_CLK_SPI,
		.name = "spi",
		.read_only = true,
		.base_reg = EN751221_REG_SPI_DIV,
		.base_value = EN751221_SPI_BASE,
		.div_bits = 8,
		.div_shift = 8,
		.div_val0 = EN751221_SPI_DIV_DEFAULT,
		.div_step = 2,
	}, {
		.id = EN751221_CLK_GSW,
		.name = "gsw",
		.np_scu = true,
		.read_only = true,
		.base_reg = EN751221_REG_SSR3,
		.base_bits = 2,
		.base_shift = 8,
		.base_values = gsw751221_base,
		.n_base_values = ARRAY_SIZE(gsw751221_base),
	}
};

static const struct en_clk_desc en751627_base_clks[] = {
	{
		.id = EN7523_CLK_SPI,
		.name = "spi",
		.read_only = true,
		.base_reg = EN751627_REG_SPI_DIV,
		.base_value = EN751627_SPI_BASE,
		.div_bits = 8,
		.div_shift = 8,
		.div_val0 = EN751627_SPI_DIV_DEFAULT,
		.div_step = 2,
	}, {
		.id = EN7523_CLK_GSW,
		.name = "gsw",
		.np_scu = true,
		.read_only = true,
		.base_reg = EN751221_REG_SSR3,
		.base_bits = 2,
		.base_shift = 8,
		.base_values = gsw751221_base,
		.n_base_values = ARRAY_SIZE(gsw751221_base),
	}
};

static const struct en_clk_desc en7528_base_clks[] = {
	{
		.id = EN7523_CLK_SPI,
		.name = "spi",
		.read_only = true,
		.base_reg = EN7528_REG_SPI_DIV,
		.base_value = EN7528_SPI_BASE,
		.div_bits = 8,
		.div_shift = 8,
		.div_val0 = EN7528_SPI_DIV_DEFAULT,
		.div_step = 2,
	}, {
		.id = EN7523_CLK_GSW,
		.name = "gsw",
		.np_scu = true,
		.read_only = true,
		.base_reg = EN751221_REG_SSR3,
		.base_bits = 2,
		.base_shift = 8,
		.base_values = gsw751221_base,
		.n_base_values = ARRAY_SIZE(gsw751221_base),
	}
};

static const struct en_clk_desc en7580_base_clks[] = {
	{
		.id = EN7523_CLK_SPI,
		.name = "spi",
		.read_only = true,
		.base_reg = EN7580_REG_SPI_DIV,
		.base_value = EN7580_SPI_BASE,
		.div_bits = 8,
		.div_shift = 8,
		.div_val0 = EN7580_SPI_DIV_DEFAULT,
		.div_step = 2,
	}, {
		.id = EN7523_CLK_GSW,
		.name = "gsw",
		.np_scu = true,
		.read_only = true,
		.base_reg = EN751221_REG_SSR3,
		.base_bits = 2,
		.base_shift = 8,
		.base_values = gsw751221_base,
		.n_base_values = ARRAY_SIZE(gsw751221_base),
	}
};

static const struct en_clk_desc en7526c_spi_clk = {
	.id = EN751221_CLK_SPI,
	.name = "spi",
	.read_only = true,
	.base_reg = EN751221_REG_SPI_DIV,
	.base_value = EN751221_SPI_BASE_EN7526C,
	.div_bits = 8,
	.div_shift = 8,
	.div_val0 = EN751221_SPI_DIV_DEFAULT,
	.div_step = 2,
};

static const u16 en7581_rst_ofs[] = {
	REG_RST_CTRL2,
	REG_RST_CTRL1,
	REG_NP_SCU_PCIC,
};

static const u16 en751221_rst_ofs[] = {
	REG_RST_CTRL2,
	REG_RST_CTRL1,
	EN751221_REG_RST_DMT,
	EN751221_REG_RST_USB,
};

static const u16 en7523_rst_map[] = {
	/* RST_CTRL2 */
	[EN7523_XPON_PHY_RST]		= 0,
	[EN7523_XSI_MAC_RST]		= 7,
	[EN7523_XSI_PHY_RST]		= 8,
	[EN7523_NPU_RST]		= 9,
	[EN7523_I2S_RST]		= 10,
	[EN7523_TRNG_RST]		= 11,
	[EN7523_TRNG_MSTART_RST]	= 12,
	[EN7523_DUAL_HSI0_RST]		= 13,
	[EN7523_DUAL_HSI1_RST]		= 14,
	[EN7523_HSI_RST]		= 15,
	[EN7523_DUAL_HSI0_MAC_RST]	= 16,
	[EN7523_DUAL_HSI1_MAC_RST]	= 17,
	[EN7523_HSI_MAC_RST]		= 18,
	[EN7523_WDMA_RST]		= 19,
	[EN7523_WOE0_RST]		= 20,
	[EN7523_WOE1_RST]		= 21,
	[EN7523_HSDMA_RST]		= 22,
	[EN7523_I2C2RBUS_RST]		= 23,
	[EN7523_TDMA_RST]		= 24,
	/* RST_CTRL1 */
	[EN7523_PCM1_ZSI_ISI_RST]	= RST_NR_PER_BANK + 0,
	[EN7523_FE_PDMA_RST]		= RST_NR_PER_BANK + 1,
	[EN7523_FE_QDMA_RST]		= RST_NR_PER_BANK + 2,
	[EN7523_PCM_SPIWP_RST]		= RST_NR_PER_BANK + 4,
	[EN7523_CRYPTO_RST]		= RST_NR_PER_BANK + 6,
	[EN7523_TIMER_RST]		= RST_NR_PER_BANK + 8,
	[EN7523_PCM1_RST]		= RST_NR_PER_BANK + 11,
	[EN7523_UART_RST]		= RST_NR_PER_BANK + 12,
	[EN7523_GPIO_RST]		= RST_NR_PER_BANK + 13,
	[EN7523_GDMA_RST]		= RST_NR_PER_BANK + 14,
	[EN7523_I2C_MASTER_RST]		= RST_NR_PER_BANK + 16,
	[EN7523_PCM2_ZSI_ISI_RST]	= RST_NR_PER_BANK + 17,
	[EN7523_SFC_RST]		= RST_NR_PER_BANK + 18,
	[EN7523_UART2_RST]		= RST_NR_PER_BANK + 19,
	[EN7523_GDMP_RST]		= RST_NR_PER_BANK + 20,
	[EN7523_FE_RST]			= RST_NR_PER_BANK + 21,
	[EN7523_USB_HOST_P0_RST]	= RST_NR_PER_BANK + 22,
	[EN7523_GSW_RST]		= RST_NR_PER_BANK + 23,
	[EN7523_SFC2_PCM_RST]		= RST_NR_PER_BANK + 25,
	[EN7523_PCIE0_RST]		= RST_NR_PER_BANK + 26,
	[EN7523_PCIE1_RST]		= RST_NR_PER_BANK + 27,
	[EN7523_PCIE_HB_RST]		= RST_NR_PER_BANK + 29,
	[EN7523_XPON_MAC_RST]		= RST_NR_PER_BANK + 31,
};

static const u16 en7581_rst_map[] = {
	/* RST_CTRL2 */
	[EN7581_XPON_PHY_RST]		= 0,
	[EN7581_CPU_TIMER2_RST]		= 2,
	[EN7581_HSUART_RST]		= 3,
	[EN7581_UART4_RST]		= 4,
	[EN7581_UART5_RST]		= 5,
	[EN7581_I2C2_RST]		= 6,
	[EN7581_XSI_MAC_RST]		= 7,
	[EN7581_XSI_PHY_RST]		= 8,
	[EN7581_NPU_RST]		= 9,
	[EN7581_I2S_RST]		= 10,
	[EN7581_TRNG_RST]		= 11,
	[EN7581_TRNG_MSTART_RST]	= 12,
	[EN7581_DUAL_HSI0_RST]		= 13,
	[EN7581_DUAL_HSI1_RST]		= 14,
	[EN7581_HSI_RST]		= 15,
	[EN7581_DUAL_HSI0_MAC_RST]	= 16,
	[EN7581_DUAL_HSI1_MAC_RST]	= 17,
	[EN7581_HSI_MAC_RST]		= 18,
	[EN7581_WDMA_RST]		= 19,
	[EN7581_WOE0_RST]		= 20,
	[EN7581_WOE1_RST]		= 21,
	[EN7581_HSDMA_RST]		= 22,
	[EN7581_TDMA_RST]		= 24,
	[EN7581_EMMC_RST]		= 25,
	[EN7581_SOE_RST]		= 26,
	[EN7581_PCIE2_RST]		= 27,
	[EN7581_XFP_MAC_RST]		= 28,
	[EN7581_USB_HOST_P1_RST]	= 29,
	[EN7581_USB_HOST_P1_U3_PHY_RST]	= 30,
	/* RST_CTRL1 */
	[EN7581_PCM1_ZSI_ISI_RST]	= RST_NR_PER_BANK + 0,
	[EN7581_FE_PDMA_RST]		= RST_NR_PER_BANK + 1,
	[EN7581_FE_QDMA_RST]		= RST_NR_PER_BANK + 2,
	[EN7581_PCM_SPIWP_RST]		= RST_NR_PER_BANK + 4,
	[EN7581_CRYPTO_RST]		= RST_NR_PER_BANK + 6,
	[EN7581_TIMER_RST]		= RST_NR_PER_BANK + 8,
	[EN7581_PCM1_RST]		= RST_NR_PER_BANK + 11,
	[EN7581_UART_RST]		= RST_NR_PER_BANK + 12,
	[EN7581_GPIO_RST]		= RST_NR_PER_BANK + 13,
	[EN7581_GDMA_RST]		= RST_NR_PER_BANK + 14,
	[EN7581_I2C_MASTER_RST]		= RST_NR_PER_BANK + 16,
	[EN7581_PCM2_ZSI_ISI_RST]	= RST_NR_PER_BANK + 17,
	[EN7581_SFC_RST]		= RST_NR_PER_BANK + 18,
	[EN7581_UART2_RST]		= RST_NR_PER_BANK + 19,
	[EN7581_GDMP_RST]		= RST_NR_PER_BANK + 20,
	[EN7581_FE_RST]			= RST_NR_PER_BANK + 21,
	[EN7581_USB_HOST_P0_RST]	= RST_NR_PER_BANK + 22,
	[EN7581_GSW_RST]		= RST_NR_PER_BANK + 23,
	[EN7581_SFC2_PCM_RST]		= RST_NR_PER_BANK + 25,
	[EN7581_PCIE0_RST]		= RST_NR_PER_BANK + 26,
	[EN7581_PCIE1_RST]		= RST_NR_PER_BANK + 27,
	[EN7581_CPU_TIMER_RST]		= RST_NR_PER_BANK + 28,
	[EN7581_PCIE_HB_RST]		= RST_NR_PER_BANK + 29,
	[EN7581_XPON_MAC_RST]		= RST_NR_PER_BANK + 31,

	/* RST_PCIC */
	[EN7581_PCIC_PERSTOUT0_RST]	= 2 * RST_NR_PER_BANK + 29,
	[EN7581_PCIC_PERSTOUT1_RST]	= 2 * RST_NR_PER_BANK + 26,
	[EN7581_PCIC_PERSTOUT2_RST]	= 2 * RST_NR_PER_BANK + 16,
};

static const u16 an7583_rst_map[] = {
	/* RST_CTRL2 */
	[AN7583_XPON_PHY_RST]		= 0,
	[AN7583_GPON_OLT_RST]		= 1,
	[AN7583_CPU_TIMER2_RST]		= 2,
	[AN7583_HSUART_RST]		= 3,
	[AN7583_UART4_RST]		= 4,
	[AN7583_UART5_RST]		= 5,
	[AN7583_I2C2_RST]		= 6,
	[AN7583_XSI_MAC_RST]		= 7,
	[AN7583_XSI_PHY_RST]		= 8,
	[AN7583_NPU_RST]		= 9,
	[AN7583_TRNG_MSTART_RST]	= 12,
	[AN7583_DUAL_HSI0_RST]		= 13,
	[AN7583_DUAL_HSI1_RST]		= 14,
	[AN7583_DUAL_HSI0_MAC_RST]	= 16,
	[AN7583_DUAL_HSI1_MAC_RST]	= 17,
	[AN7583_XPON_XFI_RST]		= 18,
	[AN7583_WDMA_RST]		= 19,
	[AN7583_WOE0_RST]		= 20,
	[AN7583_HSDMA_RST]		= 22,
	[AN7583_TDMA_RST]		= 24,
	[AN7583_EMMC_RST]		= 25,
	[AN7583_SOE_RST]		= 26,
	[AN7583_XFP_MAC_RST]		= 28,
	[AN7583_MDIO0]			= 30,
	[AN7583_MDIO1]			= 31,
	/* RST_CTRL1 */
	[AN7583_PCM1_ZSI_ISI_RST]	= RST_NR_PER_BANK + 0,
	[AN7583_FE_PDMA_RST]		= RST_NR_PER_BANK + 1,
	[AN7583_FE_QDMA_RST]		= RST_NR_PER_BANK + 2,
	[AN7583_PCM_SPIWP_RST]		= RST_NR_PER_BANK + 4,
	[AN7583_CRYPTO_RST]		= RST_NR_PER_BANK + 6,
	[AN7583_TIMER_RST]		= RST_NR_PER_BANK + 8,
	[AN7583_PCM1_RST]		= RST_NR_PER_BANK + 11,
	[AN7583_UART_RST]		= RST_NR_PER_BANK + 12,
	[AN7583_GPIO_RST]		= RST_NR_PER_BANK + 13,
	[AN7583_GDMA_RST]		= RST_NR_PER_BANK + 14,
	[AN7583_I2C_MASTER_RST]		= RST_NR_PER_BANK + 16,
	[AN7583_PCM2_ZSI_ISI_RST]	= RST_NR_PER_BANK + 17,
	[AN7583_SFC_RST]		= RST_NR_PER_BANK + 18,
	[AN7583_UART2_RST]		= RST_NR_PER_BANK + 19,
	[AN7583_GDMP_RST]		= RST_NR_PER_BANK + 20,
	[AN7583_FE_RST]			= RST_NR_PER_BANK + 21,
	[AN7583_USB_HOST_P0_RST]	= RST_NR_PER_BANK + 22,
	[AN7583_GSW_RST]		= RST_NR_PER_BANK + 23,
	[AN7583_SFC2_PCM_RST]		= RST_NR_PER_BANK + 25,
	[AN7583_PCIE0_RST]		= RST_NR_PER_BANK + 26,
	[AN7583_PCIE1_RST]		= RST_NR_PER_BANK + 27,
	[AN7583_CPU_TIMER_RST]		= RST_NR_PER_BANK + 28,
	[AN7583_PCIE_HB_RST]		= RST_NR_PER_BANK + 29,
	[AN7583_XPON_MAC_RST]		= RST_NR_PER_BANK + 31,
};

static const u16 en751221_rst_map[] = {
	/* RST_CTRL2 */
	[EN751221_XPON_PHY_RST]		= 0,
	[EN751221_GFAST_RST]		= 1,
	[EN751221_CPU_TIMER2_RST]	= 2,
	[EN751221_UART3_RST]		= 3,
	[EN751221_UART4_RST]		= 4,
	[EN751221_UART5_RST]		= 5,
	[EN751221_I2C2_RST]		= 6,
	[EN751221_XSI_MAC_RST]		= 7,
	[EN751221_XSI_PHY_RST]		= 8,

	/* RST_CTRL1 */
	[EN751221_PCM1_ZSI_ISI_RST]	= RST_NR_PER_BANK + 0,
	[EN751221_FE_QDMA1_RST]		= RST_NR_PER_BANK + 1,
	[EN751221_FE_QDMA2_RST]		= RST_NR_PER_BANK + 2,
	[EN751221_FE_UNZIP_RST]		= RST_NR_PER_BANK + 3,
	[EN751221_PCM2_RST]		= RST_NR_PER_BANK + 4,
	[EN751221_PTM_MAC_RST]		= RST_NR_PER_BANK + 5,
	[EN751221_CRYPTO_RST]		= RST_NR_PER_BANK + 6,
	[EN751221_SAR_RST]		= RST_NR_PER_BANK + 7,
	[EN751221_TIMER_RST]		= RST_NR_PER_BANK + 8,
	[EN751221_INTC_RST]		= RST_NR_PER_BANK + 9,
	[EN751221_BONDING_RST]		= RST_NR_PER_BANK + 10,
	[EN751221_PCM1_RST]		= RST_NR_PER_BANK + 11,
	[EN751221_UART_RST]		= RST_NR_PER_BANK + 12,
	[EN751221_GPIO_RST]		= RST_NR_PER_BANK + 13,
	[EN751221_GDMA_RST]		= RST_NR_PER_BANK + 14,
	[EN751221_I2C_MASTER_RST]	= RST_NR_PER_BANK + 16,
	[EN751221_PCM2_ZSI_ISI_RST]	= RST_NR_PER_BANK + 17,
	[EN751221_SFC_RST]		= RST_NR_PER_BANK + 18,
	[EN751221_UART2_RST]		= RST_NR_PER_BANK + 19,
	[EN751221_GDMP_RST]		= RST_NR_PER_BANK + 20,
	[EN751221_FE_RST]		= RST_NR_PER_BANK + 21,
	[EN751221_USB_HOST_P0_RST]	= RST_NR_PER_BANK + 22,
	[EN751221_GSW_RST]		= RST_NR_PER_BANK + 23,
	[EN751221_SFC2_PCM_RST]		= RST_NR_PER_BANK + 25,
	[EN751221_PCIE0_RST]		= RST_NR_PER_BANK + 26,
	[EN751221_PCIE1_RST]		= RST_NR_PER_BANK + 27,
	[EN751221_CPU_TIMER_RST]	= RST_NR_PER_BANK + 28,
	[EN751221_PCIE_HB_RST]		= RST_NR_PER_BANK + 29,
	[EN751221_SIMIF_RST]		= RST_NR_PER_BANK + 30,
	[EN751221_XPON_MAC_RST]		= RST_NR_PER_BANK + 31,

	/* RST_DMT */
	[EN751221_DMT_RST]		= 2 * RST_NR_PER_BANK + 0,

	/* RST_USB */
	[EN751221_USB_PHY_P0_RST]	= 3 * RST_NR_PER_BANK + 6,
	[EN751221_USB_PHY_P1_RST]	= 3 * RST_NR_PER_BANK + 7,
};

static int register_resets(struct device *dev, struct regmap *map,
			   const struct airoha_clk_soc_data *soc_data);

static u32 en7523_get_base_rate(const struct en_clk_desc *desc, u32 val)
{
	if (!desc->base_bits)
		return desc->base_value;

	val >>= desc->base_shift;
	val &= (1 << desc->base_bits) - 1;

	if (val >= desc->n_base_values)
		return 0;

	return desc->base_values[val];
}

static u32 en7523_get_div(const struct en_clk_desc *desc, u32 val)
{
	if (!desc->div_bits)
		return 1;

	val >>= desc->div_shift;
	val &= (1 << desc->div_bits) - 1;

	if (!val && desc->div_val0)
		return desc->div_val0;

	return (val + desc->div_offset) * desc->div_step;
}

static int en7523_pci_is_enabled(struct clk_hw *hw)
{
	struct en_clk_gate *cg = container_of(hw, struct en_clk_gate, hw);
	u32 val;

	regmap_read(cg->map, REG_PCI_CONTROL, &val);
	return !!(val & REG_PCI_CONTROL_REFCLK_EN1);
}

static int en7523_pci_prepare(struct clk_hw *hw)
{
	struct en_clk_gate *cg = container_of(hw, struct en_clk_gate, hw);
	struct regmap *map = cg->map;
	u32 mask;

	/* Need to pull device low before reset */
	regmap_clear_bits(map, REG_PCI_CONTROL,
			  REG_PCI_CONTROL_PERSTOUT1 | REG_PCI_CONTROL_PERSTOUT);
	usleep_range(1000, 2000);

	/* Enable PCIe port 1 */
	regmap_set_bits(map, REG_PCI_CONTROL, REG_PCI_CONTROL_REFCLK_EN1);
	usleep_range(1000, 2000);

	/* Reset to default */
	mask = REG_RESET_CONTROL_PCIE1 | REG_RESET_CONTROL_PCIE2 |
	       REG_RESET_CONTROL_PCIEHB;
	regmap_clear_bits(map, REG_RESET_CONTROL1, mask);
	usleep_range(1000, 2000);
	regmap_set_bits(map, REG_RESET_CONTROL1, mask);
	msleep(100);
	regmap_clear_bits(map, REG_RESET_CONTROL1, mask);
	usleep_range(5000, 10000);

	/* Release device */
	mask = REG_PCI_CONTROL_PERSTOUT1 | REG_PCI_CONTROL_PERSTOUT;
	regmap_clear_bits(map, REG_PCI_CONTROL, mask);
	usleep_range(1000, 2000);
	regmap_set_bits(map, REG_PCI_CONTROL, mask);
	msleep(250);

	return 0;
}

static void en7523_pci_unprepare(struct clk_hw *hw)
{
	struct en_clk_gate *cg = container_of(hw, struct en_clk_gate, hw);

	regmap_clear_bits(cg->map, REG_PCI_CONTROL, REG_PCI_CONTROL_REFCLK_EN1);
}

static struct clk_hw *en7523_register_pcie_clk(struct device *dev,
					       struct regmap *clk_map)
{
	const struct airoha_clk_soc_data *soc_data = device_get_match_data(dev);
	struct clk_init_data init = {
		.name = "pcie",
		.ops = &soc_data->pcie_ops,
	};
	struct en_clk_gate *cg;
	int err;

	cg = devm_kzalloc(dev, sizeof(*cg), GFP_KERNEL);
	if (!cg)
		return ERR_PTR(-ENOMEM);

	cg->map = clk_map;
	cg->hw.init = &init;

	if (init.ops->unprepare)
		init.ops->unprepare(&cg->hw);

	err = devm_clk_hw_register(dev, &cg->hw);
	if (err)
		return ERR_PTR(err);

	return &cg->hw;
}

static unsigned long en75xx_recalc_rate(struct clk_hw *hw,
					unsigned long parent_rate)
{
	struct en_clk *c = container_of(hw, struct en_clk, hw);
	const struct en_clk_desc *desc = c->desc;
	u32 val, rate, div;
	int err;

	err = regmap_read(c->map, desc->base_reg, &val);
	if (err) {
		pr_err("Failed reading clock %s: %d\n", desc->name, err);
		return 0;
	}

	rate = en7523_get_base_rate(desc, val);
	if (!desc->div_bits)
		return rate;

	if (desc->div_reg && desc->div_reg != desc->base_reg) {
		err = regmap_read(c->map, desc->div_reg, &val);
		if (err) {
			pr_err("Failed reading divider for clock %s: %d\n",
			       desc->name, err);
			return 0;
		}
	}

	div = en7523_get_div(desc, val);
	if (!div)
		return 0;

	return rate / div;
}

static int en75xx_get_base_val_for_rate(const struct en_clk_desc *desc,
					int div, unsigned long rate)
{
	int i;

	if (!div)
		return -EINVAL;

	/* Single base rate */
	if (!desc->base_bits) {
		if (rate != desc->base_value / div)
			return -EINVAL;

		return 0;
	}

	/* Check every base rate with provided divisor */
	for (i = 0; i < desc->n_base_values; i++)
		if (rate == desc->base_values[i] / div)
			return i;

	return -EINVAL;
}

static int en75xx_get_vals_for_rate(const struct en_clk_desc *desc,
				    unsigned long rate,
				    u32 *base_val, u32 *div_val)
{
	int tmp_base_val = 0;
	int tmp_div_val = 0;

	if (!desc->base_bits && !desc->div_bits)
		return -EINVAL;

	/* Divisor not supported, just search in base rate */
	if (!desc->div_bits) {
		tmp_base_val = en75xx_get_base_val_for_rate(desc, 1, rate);
		if (tmp_base_val < 0) {
			pr_err("Invalid rate for clock %s\n", desc->name);
			return -EINVAL;
		}

		goto exit;
	}

	/* Check if div0 satisfies the request */
	if (desc->div_val0) {
		tmp_base_val = en75xx_get_base_val_for_rate(desc,
							    desc->div_val0,
							    rate);
		if (tmp_base_val >= 0)
			goto exit;

		/* Skip checking first divisor val */
		tmp_div_val = 1;
	}

	/* Simulate rate with every divisor supported */
	for (; tmp_div_val < BIT(desc->div_bits); tmp_div_val++) {
		int div = (tmp_div_val + desc->div_offset) * desc->div_step;

		tmp_base_val = en75xx_get_base_val_for_rate(desc, div, rate);
		if (tmp_base_val >= 0)
			goto exit;
	}

	pr_err("Invalid rate for clock %s\n", desc->name);
	return -EINVAL;

exit:
	*base_val = tmp_base_val;
	*div_val = tmp_div_val;

	return 0;
}

static int en75xx_determine_rate(struct clk_hw *hw,
				 struct clk_rate_request *req)
{
	struct en_clk *en_clk = container_of(hw, struct en_clk, hw);
	u32 div_val, base_val;

	return en75xx_get_vals_for_rate(en_clk->desc, req->rate,
					&base_val, &div_val);
}

static int en75xx_set_rate(struct clk_hw *hw, unsigned long rate,
			   unsigned long parent_rate)
{
	struct en_clk *en_clk = container_of(hw, struct en_clk, hw);
	const struct en_clk_desc *desc = en_clk->desc;
	struct regmap *map = en_clk->map;
	u32 base_val, div_val;
	u32 reg, val, mask;
	int err;

	err = en75xx_get_vals_for_rate(desc, rate, &base_val, &div_val);
	if (err)
		return err;

	if (desc->div_bits) {
		reg = desc->div_reg ? desc->div_reg : desc->base_reg;
		mask = (BIT(desc->div_bits) - 1) << desc->div_shift;
		val = div_val << desc->div_shift;

		err = regmap_update_bits(map, reg, mask, val);
		if (err) {
			pr_err("Failed to update div reg for clock %s\n",
			       desc->name);
			return -EINVAL;
		}
	}

	if (desc->base_bits) {
		mask = (BIT(desc->base_bits) - 1) << desc->base_shift;
		val = base_val << desc->base_shift;

		err = regmap_update_bits(map, desc->base_reg, mask, val);
		if (err) {
			pr_err("Failed to update reg for clock %s\n",
			       desc->name);
			return -EINVAL;
		}
	}

	return 0;
}

static const struct clk_ops en75xx_clk_ops = {
	.recalc_rate = en75xx_recalc_rate,
	.determine_rate = en75xx_determine_rate,
	.set_rate = en75xx_set_rate,
};

static const struct clk_ops en75xx_clk_ro_ops = {
	.recalc_rate = en75xx_recalc_rate,
};

static int en75xx_register_clocks(struct device *dev,
				  struct airoha_clk_priv *priv,
				  struct clk_hw_onecell_data *clk_data)
{
	const struct airoha_clk_soc_data *soc_data = priv->soc_data;
	struct clk_hw *hw;
	int i;

	for (i = 0; i < soc_data->num_base_clks; i++) {
		const struct en_clk_desc *desc = &soc_data->base_clks[i];
		struct clk_init_data init = {
			.ops = &en75xx_clk_ops,
		};
		struct en_clk *en_clk;
		int err;

		if (soc_data->econet && soc_data->econet->spi_alt_desc &&
		    desc->id == soc_data->econet->spi_alt_desc->id &&
		    priv->soc_family == EN7526C_PKG)
			desc = soc_data->econet->spi_alt_desc;

		if (WARN_ON_ONCE(desc->id < 0 || desc->id >= clk_data->num))
			return -EINVAL;

		if (desc->read_only) {
			init.ops = &en75xx_clk_ro_ops;
			init.flags = CLK_GET_RATE_NOCACHE;
		}

		en_clk = devm_kzalloc(dev, sizeof(*en_clk), GFP_KERNEL);
		if (!en_clk)
			return -ENOMEM;

		init.name = desc->name;

		en_clk->map = desc->np_scu ? priv->scu_np : priv->scu_chip;
		en_clk->desc = desc;
		en_clk->hw.init = &init;

		err = devm_clk_hw_register(dev, &en_clk->hw);
		if (err) {
			pr_err("Failed to register clk %s: %d\n",
			       desc->name, err);
			return err;
		}

		clk_data->hws[desc->id] = &en_clk->hw;
	}

	hw = en7523_register_pcie_clk(dev, priv->scu_np);
	if (IS_ERR(hw))
		return PTR_ERR(hw);

	clk_data->hws[soc_data->pcie_clk_id] = hw;

	return 0;
}

static int en7581_pci_is_enabled(struct clk_hw *hw)
{
	struct en_clk_gate *cg = container_of(hw, struct en_clk_gate, hw);
	u32 val, mask;

	mask = REG_PCI_CONTROL_REFCLK_EN0 | REG_PCI_CONTROL_REFCLK_EN1;
	regmap_read(cg->map, REG_PCI_CONTROL, &val);
	return (val & mask) == mask;
}

static int en7581_pci_enable(struct clk_hw *hw)
{
	struct en_clk_gate *cg = container_of(hw, struct en_clk_gate, hw);
	u32 mask;

	mask = REG_PCI_CONTROL_REFCLK_EN0 | REG_PCI_CONTROL_REFCLK_EN1;
	regmap_set_bits(cg->map, REG_PCI_CONTROL, mask);

	return 0;
}

static void en7581_pci_disable(struct clk_hw *hw)
{
	struct en_clk_gate *cg = container_of(hw, struct en_clk_gate, hw);
	u32 mask;

	mask = REG_PCI_CONTROL_REFCLK_EN0 | REG_PCI_CONTROL_REFCLK_EN1;
	regmap_clear_bits(cg->map, REG_PCI_CONTROL, mask);
	usleep_range(1000, 2000);
}

static int en7523_clk_hw_init(struct platform_device *pdev,
			      struct airoha_clk_priv *priv,
			      struct clk_hw_onecell_data *clk_data)
{
	const struct airoha_clk_soc_data *soc_data = priv->soc_data;
	struct device *dev = &pdev->dev;
	int err;

	err = en75xx_register_clocks(dev, priv, clk_data);
	if (err)
		return err;

	regmap_set_bits(priv->scu_chip, REG_TRNG_BUS_CLK_GAT, REG_TRNG_BUS_EN);
	regmap_set_bits(priv->scu_chip, REG_TRNG_PER_CLK_GAT_1, REG_TRNG_PER1_EN);
	regmap_set_bits(priv->scu_chip, REG_TRNG_PER_CLK_GAT_2, REG_TRNG_PER2_EN);

	return register_resets(dev, priv->scu_np, soc_data);
}

static int en7523_reset_update(struct reset_controller_dev *rcdev,
			       unsigned long id, bool assert)
{
	struct en_rst_data *rst_data = container_of(rcdev, struct en_rst_data, rcdev);
	u32 addr = rst_data->bank_ofs[id / RST_NR_PER_BANK];
	u32 mask = BIT(id % RST_NR_PER_BANK);
	u32 val;

	/* For PCIC reset logic is inverted, 0:assert 1:deassert */
	if (addr == REG_NP_SCU_PCIC)
		val = assert ? 0 : mask;
	else
		val = assert ? mask : 0;

	return regmap_update_bits(rst_data->map, addr, mask, val);
}

static int en7523_reset_assert(struct reset_controller_dev *rcdev,
			       unsigned long id)
{
	return en7523_reset_update(rcdev, id, true);
}

static int en7523_reset_deassert(struct reset_controller_dev *rcdev,
				 unsigned long id)
{
	return en7523_reset_update(rcdev, id, false);
}

static bool en7523_reset_needs_long_pulse(unsigned long id)
{
	return id == RST_NR_PER_BANK + 0 ||
	       id == RST_NR_PER_BANK + 4 ||
	       id == RST_NR_PER_BANK + 17;
}

static int en7523_reset_reset(struct reset_controller_dev *rcdev,
			      unsigned long id)
{
	int err;

	err = en7523_reset_assert(rcdev, id);
	if (err)
		return err;

	if (en7523_reset_needs_long_pulse(id))
		usleep_range(5000, 6000);
	else
		usleep_range(10, 50);

	return en7523_reset_deassert(rcdev, id);
}

static int en7523_reset_status(struct reset_controller_dev *rcdev,
			       unsigned long id)
{
	struct en_rst_data *rst_data = container_of(rcdev, struct en_rst_data, rcdev);
	u32 addr = rst_data->bank_ofs[id / RST_NR_PER_BANK];
	bool inverted = false;
	u32 val;

	/* For PCIC reset logic is inverted, 0:assert 1:deassert */
	if (addr == REG_NP_SCU_PCIC)
		inverted = true;

	regmap_read(rst_data->map, addr, &val);
	val &= BIT(id % RST_NR_PER_BANK);
	return inverted ? !val : !!val;
}

static int en7523_reset_xlate(struct reset_controller_dev *rcdev,
			      const struct of_phandle_args *reset_spec)
{
	struct en_rst_data *rst_data = container_of(rcdev, struct en_rst_data, rcdev);

	if (reset_spec->args[0] >= rcdev->nr_resets)
		return -EINVAL;

	return rst_data->idx_map[reset_spec->args[0]];
}

static const struct reset_control_ops en7581_reset_ops = {
	.assert = en7523_reset_assert,
	.deassert = en7523_reset_deassert,
	.reset = en7523_reset_reset,
	.status = en7523_reset_status,
};

static int register_resets(struct device *dev, struct regmap *map,
			   const struct airoha_clk_soc_data *soc_data)
{
	struct en_rst_data *rst_data;

	rst_data = devm_kzalloc(dev, sizeof(*rst_data), GFP_KERNEL);
	if (!rst_data)
		return -ENOMEM;

	rst_data->bank_ofs = soc_data->rst_ofs;
	rst_data->idx_map = soc_data->rst_map;
	rst_data->map = map;

	rst_data->rcdev.nr_resets = soc_data->nr_resets;
	rst_data->rcdev.of_xlate = en7523_reset_xlate;
	rst_data->rcdev.ops = &en7581_reset_ops;
	rst_data->rcdev.of_node = dev->of_node;
	rst_data->rcdev.of_reset_n_cells = 1;
	rst_data->rcdev.owner = THIS_MODULE;
	rst_data->rcdev.dev = dev;

	return devm_reset_controller_register(dev, &rst_data->rcdev);
}

static int en7581_serdes_phy_set_mode(struct phy *phy, enum phy_mode mode,
				      int submode)
{
	struct en_serdes_phy_instance *instance = phy_get_drvdata(phy);
	struct airoha_clk_priv *priv = dev_get_drvdata(phy->dev.parent);
	u32 reg, mask, sel;

	switch (instance->serdes_port) {
	case AIROHA_SCU_SERDES_PCIE1:
		reg = REG_NP_SCU_SSTR;
		mask = REG_PCIE_XSI0_SEL_MASK;

		if (mode != PHY_MODE_ETHERNET && mode != PHY_MODE_PCIE)
			return -EINVAL;

		if (mode == PHY_MODE_ETHERNET) {
			switch (submode) {
			case PHY_INTERFACE_MODE_USXGMII:
			case PHY_INTERFACE_MODE_10GBASER:
				sel = REG_PCIE_XSI0_SEL_XFI;
				break;
			case PHY_INTERFACE_MODE_SGMII:
			case PHY_INTERFACE_MODE_1000BASEX:
			case PHY_INTERFACE_MODE_2500BASEX:
				sel = REG_PCIE_XSI0_SEL_HSGMII;
				break;
			default:
				return -EINVAL;
			}
		} else {
			sel = REG_PCIE_XSI0_SEL_PCIE;
		}

		break;
	case AIROHA_SCU_SERDES_PCIE2:
		reg = REG_NP_SCU_SSTR;
		mask = REG_PCIE_XSI1_SEL_MASK;

		if (mode != PHY_MODE_ETHERNET && mode != PHY_MODE_PCIE)
			return -EINVAL;

		if (mode == PHY_MODE_ETHERNET) {
			switch (submode) {
			case PHY_INTERFACE_MODE_USXGMII:
			case PHY_INTERFACE_MODE_10GBASER:
				sel = REG_PCIE_XSI1_SEL_XFI;
				break;
			case PHY_INTERFACE_MODE_SGMII:
			case PHY_INTERFACE_MODE_1000BASEX:
			case PHY_INTERFACE_MODE_2500BASEX:
				sel = REG_PCIE_XSI1_SEL_HSGMII;
				break;
			default:
				return -EINVAL;
			}
		} else {
			sel = REG_PCIE_XSI1_SEL_PCIE;
		}

		break;
	case AIROHA_SCU_SERDES_USB1:
		reg = REG_NP_SCU_SSR3;
		mask = REG_SSUSB_HSGMII_SEL_MASK;

		if (mode != PHY_MODE_ETHERNET && mode != PHY_MODE_USB_DEVICE &&
		    mode != PHY_MODE_USB_DEVICE_SS)
			return -EINVAL;

		if (mode == PHY_MODE_ETHERNET)
			sel = REG_SSUSB_HSGMII_SEL_HSGMII;
		else
			sel = REG_SSUSB_HSGMII_SEL_USB;

		break;
	case AIROHA_SCU_SERDES_USB2:
		reg = REG_NP_SCU_SSTR;
		mask = REG_USB_PCIE_SEL_MASK;

		if (mode != PHY_MODE_PCIE && mode != PHY_MODE_USB_DEVICE &&
		    mode != PHY_MODE_USB_DEVICE_SS)
			return -EINVAL;

		if (mode == PHY_MODE_PCIE)
			sel = REG_USB_PCIE_SEL_PCIE;
		else
			sel = REG_USB_PCIE_SEL_USB;

		break;
	default:
		return -EINVAL;
	}

	return regmap_update_bits(priv->scu_np, reg, mask, sel);
}

static const struct phy_ops en7581_serdes_phy_ops = {
	.set_mode	= en7581_serdes_phy_set_mode,
	.owner		= THIS_MODULE,
};

static struct phy *en7581_serdes_phy_xlate(struct device *dev,
					   const struct of_phandle_args *args)
{
	struct airoha_clk_priv *priv = dev_get_drvdata(dev);
	struct en_serdes_phy_instance *instance;
	unsigned int serdes_port;

	if (args->args_count != 1) {
		dev_err(dev, "invalid number of cells in 'phy' property\n");
		return ERR_PTR(-EINVAL);
	}

	serdes_port = args->args[0];
	if (serdes_port >= EN_SERDES_PHY_NUM) {
		dev_err(dev, "invalid serdes port: %d\n", serdes_port);
		return ERR_PTR(-EINVAL);
	}

	instance = priv->serdes_phys[serdes_port];
	if (!instance) {
		dev_err(dev, "failed to find appropriate serdes phy\n");
		return ERR_PTR(-EINVAL);
	}

	return instance->phy;
}

static int en7581_serdes_phy_register(struct device *dev,
				      struct airoha_clk_priv *priv)
{
	struct phy_provider *phy_provider;
	int i;

	for (i = 0; i < EN_SERDES_PHY_NUM; i++) {
		struct en_serdes_phy_instance *instance;

		instance = devm_kzalloc(dev, sizeof(*instance), GFP_KERNEL);
		if (!instance)
			return -ENOMEM;

		instance->phy = devm_phy_create(dev, NULL,
						&en7581_serdes_phy_ops);
		if (IS_ERR(instance->phy))
			return dev_err_probe(dev, PTR_ERR(instance->phy),
					     "failed to create phy\n");

		instance->serdes_port = i;
		priv->serdes_phys[i] = instance;

		phy_set_drvdata(instance->phy, instance);
	}

	phy_provider = devm_of_phy_provider_register(dev,
						     en7581_serdes_phy_xlate);

	return PTR_ERR_OR_ZERO(phy_provider);
}

static int en7581_clk_hw_init(struct platform_device *pdev,
			      struct airoha_clk_priv *priv,
			      struct clk_hw_onecell_data *clk_data)
{
	const struct airoha_clk_soc_data *soc_data = priv->soc_data;
	struct device *dev = &pdev->dev;
	struct regmap *clk_map = priv->scu_np;
	int err;

	err = en75xx_register_clocks(dev, priv, clk_data);
	if (err)
		return err;

	regmap_clear_bits(clk_map, REG_NP_SCU_SSTR,
			  REG_PCIE_XSI0_SEL_MASK | REG_PCIE_XSI1_SEL_MASK);
	regmap_update_bits(clk_map, REG_NP_SCU_PCIC, REG_PCIE_CTRL,
			   FIELD_PREP(REG_PCIE_CTRL, 3));

	regmap_set_bits(priv->scu_chip, REG_TRNG_BUS_CLK_GAT, REG_TRNG_BUS_EN);
	regmap_set_bits(priv->scu_chip, REG_TRNG_PER_CLK_GAT_1_AN7581,
			REG_TRNG_PER1_EN);
	regmap_set_bits(priv->scu_chip, REG_TRNG_PER_CLK_GAT_2_AN7581,
			REG_TRNG_PER2_EN);

	/* Reset PCIE_HB */
	regmap_set_bits(clk_map, REG_RST_CTRL1, REG_PCIE_HB_RST);
	regmap_clear_bits(clk_map, REG_RST_CTRL1, REG_PCIE_HB_RST);

	err = register_resets(dev, clk_map, soc_data);
	if (err)
		return err;

	return en7581_serdes_phy_register(dev, priv);
}

static int an7583_clk_hw_init(struct platform_device *pdev,
			      struct airoha_clk_priv *priv,
			      struct clk_hw_onecell_data *clk_data)
{
	const struct airoha_clk_soc_data *soc_data = priv->soc_data;
	struct device *dev = &pdev->dev;
	struct regmap *clk_map = priv->scu_np;
	int err;

	err = en75xx_register_clocks(dev, priv, clk_data);
	if (err)
		return err;

	regmap_clear_bits(clk_map, REG_NP_SCU_SSTR,
			  REG_PCIE_XSI0_SEL_MASK | REG_PCIE_XSI1_SEL_MASK);
	regmap_update_bits(clk_map, REG_NP_SCU_PCIC, REG_PCIE_CTRL,
			   FIELD_PREP(REG_PCIE_CTRL, 3));

	regmap_set_bits(priv->scu_chip, REG_TRNG_BUS_CLK_GAT, REG_TRNG_BUS_EN);
	regmap_set_bits(priv->scu_chip, REG_TRNG_PER_CLK_GAT_1_AN7581,
			REG_TRNG_PER1_EN);
	regmap_set_bits(priv->scu_chip, REG_TRNG_PER_CLK_GAT_2_AN7581,
			REG_TRNG_PER2_EN);

	return register_resets(dev, clk_map, soc_data);
}

static int econet_register_fixed_rate(struct device *dev, int key,
				      struct clk_hw_onecell_data *clk_data,
				      const char *name, u32 rate)
{
	struct clk_hw *hw;

	if (WARN_ON_ONCE(key >= clk_data->num))
		return -EINVAL;

	hw = devm_clk_hw_register_fixed_rate(dev, name, NULL, 0, rate);
	if (IS_ERR(hw))
		return dev_err_probe(dev, PTR_ERR(hw),
				     "Failed to register clk %s\n", name);

	clk_data->hws[key] = hw;

	return 0;
}

static int econet_register_clocks(struct device *dev,
				  struct airoha_clk_priv *priv,
				  struct clk_hw_onecell_data *clk_data)
{
	const struct airoha_clk_soc_data *soc_data = priv->soc_data;
	u32 rate, val;
	int err;

	err = en75xx_register_clocks(dev, priv, clk_data);
	if (err)
		return err;

	/* BUS */
	err = regmap_read(priv->scu_np, AIROHA_NP_SCU_SCREG_WR1, &val);
	if (err)
		return dev_err_probe(dev, err,
				     "Failed reading fixed clk rate bus\n");

	/* SYS_GLOBAL_PARM is a bootloader bitfield, not a clock selector */
	rate = ((val & soc_data->bus_clk_mask) >>
		__ffs(soc_data->bus_clk_mask)) * 1000000;
	if (!rate)
		return dev_err_probe(dev, -EINVAL,
				     "Bootloader did not save the bus clock rate\n");

	err = econet_register_fixed_rate(dev, soc_data->bus_clk_id, clk_data,
					 "bus", rate);
	if (err)
		return err;

	/* CPU */
	return econet_register_fixed_rate(dev, soc_data->cpu_clk_id, clk_data,
					  "cpu", rate * soc_data->cpu_clk_mult);
}

static int econet_clk_hw_init(struct platform_device *pdev,
			      struct airoha_clk_priv *priv,
			      struct clk_hw_onecell_data *clk_data)
{
	const struct airoha_clk_soc_data *soc_data = priv->soc_data;
	const struct econet_clk_soc_data *data = soc_data->econet;
	struct device *dev = &pdev->dev;
	struct regmap *map = priv->scu_chip, *clk_map = priv->scu_np;
	int err;

	err = econet_register_clocks(dev, priv, clk_data);
	if (err)
		return err;

	if (data->shared_unzip_reg) {
		err = regmap_write(clk_map, data->shared_unzip_reg,
				   data->shared_unzip_val);
		if (err)
			return dev_err_probe(dev, err,
					     "failed to configure shared SRAM\n");
	}

	if (data->xpon_tod_clk_mask) {
		err = regmap_set_bits(map, data->xpon_tod_clk_reg,
				      data->xpon_tod_clk_mask);
		if (err)
			return dev_err_probe(dev, err,
					     "failed to enable xPON ToD clock\n");
	}

	if (data->xpon_tod_div_mask) {
		err = regmap_set_bits(map, data->xpon_tod_div_reg,
				      data->xpon_tod_div_mask);
		if (err)
			return dev_err_probe(dev, err,
					     "failed to enable xPON ToD divider\n");
	}

	return register_resets(dev, clk_map, soc_data);
}

static void airoha_clk_iounmap(void *base)
{
	iounmap(base);
}

static void __iomem *airoha_clk_iomap(struct device *dev,
				      struct device_node *np)
{
	void __iomem *base;
	int err;

	base = of_iomap(np, 0);
	if (!base)
		return ERR_PTR(-ENOMEM);

	err = devm_add_action_or_reset(dev, airoha_clk_iounmap, base);
	if (err)
		return ERR_PTR(err);

	return base;
}

static const struct regmap_config airoha_clk_regmap_config = {
	.reg_bits = 32,
	.val_bits = 32,
	.reg_stride = 4,
};

static int airoha_clk_map_resources(struct platform_device *pdev,
				    struct airoha_clk_priv *priv)
{
	const struct airoha_clk_soc_data *soc_data = priv->soc_data;
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node, *chip_np, *efuse_np;
	struct resource *res_mem;
	struct resource res;
	int np_index = 0;
	int err = 0;

	if (of_property_present(np, "airoha,chip-scu")) {
		chip_np = of_parse_phandle(np, "airoha,chip-scu", 0);
		if (!chip_np)
			return -ENODEV;
	} else {
		chip_np = of_find_compatible_node(NULL, NULL,
						  "airoha,chip-scu");
		if (!chip_np && soc_data->old_chip_scu_compatible)
			chip_np = of_find_compatible_node(NULL, NULL,
							  soc_data->old_chip_scu_compatible);
	}

	if (chip_np) {
		err = of_address_to_resource(chip_np, 0, &res);
		if (err) {
			of_node_put(chip_np);
			return err;
		}
		priv->chip_scu_size = resource_size(&res);

		priv->scu_chip = syscon_node_to_regmap(chip_np);
		if (IS_ERR(priv->scu_chip)) {
			of_node_put(chip_np);
			return PTR_ERR(priv->scu_chip);
		}

		priv->chip_scu_base = airoha_clk_iomap(dev, chip_np);
		of_node_put(chip_np);
		if (IS_ERR(priv->chip_scu_base))
			return PTR_ERR(priv->chip_scu_base);
	} else if (of_device_is_compatible(np, "airoha,en7523-scu") &&
		   !of_property_present(np, "reg-names")) {
		/* Legacy EN7523 binding: reg[0] = CHIP-SCU, reg[1] = NP-SCU */
		priv->chip_scu_base = devm_platform_get_and_ioremap_resource(pdev, 0,
									     &res_mem);
		if (IS_ERR(priv->chip_scu_base))
			return PTR_ERR(priv->chip_scu_base);

		priv->chip_scu_size = resource_size(res_mem);
		priv->scu_chip = devm_regmap_init_mmio(dev, priv->chip_scu_base,
						       &airoha_clk_regmap_config);
		if (IS_ERR(priv->scu_chip))
			return PTR_ERR(priv->scu_chip);

		np_index = 1;
	} else {
		return -ENODEV;
	}

	if (of_property_present(np, "reg-names")) {
		np_index = of_property_match_string(np, "reg-names", "np-scu");
		if (np_index < 0)
			np_index = 0;
	}

	priv->np_scu_base = devm_platform_ioremap_resource(pdev, np_index);
	if (IS_ERR(priv->np_scu_base))
		return PTR_ERR(priv->np_scu_base);

	priv->scu_np = devm_regmap_init_mmio(dev, priv->np_scu_base,
					     &airoha_clk_regmap_config);
	if (IS_ERR(priv->scu_np))
		return PTR_ERR(priv->scu_np);

	if (of_property_present(np, "airoha,scu-efuse")) {
		efuse_np = of_parse_phandle(np, "airoha,scu-efuse", 0);
		if (!efuse_np)
			return -ENODEV;

		err = of_address_to_resource(efuse_np, 0, &res);
		if (!err &&
		    resource_size(&res) < AIROHA_EFUSE_VERIFY_DATA1 + sizeof(u32))
			err = -EINVAL;
		if (!err) {
			priv->efuse_base = airoha_clk_iomap(dev, efuse_np);
			if (IS_ERR(priv->efuse_base))
				err = PTR_ERR(priv->efuse_base);
		}
		of_node_put(efuse_np);
	}

	return err;
}

static void airoha_clk_detect_soc(struct airoha_clk_priv *priv)
{
	void __iomem *np = priv->np_scu_base, *chip = priv->chip_scu_base;
	u32 efuse0, efuse1, value, pkgid;
	enum airoha_pkg pkg;

	priv->soc_id = END_PACKAGE_ID;
	priv->soc_source = ID_SOURCE_UNKNOWN;
	value = readl(np + AIROHA_NP_SCU_HIR);
	pkg = airoha_pkg_from_id(FIELD_GET(AIROHA_NP_SCU_HIR_MASK, value));
	if (pkg == UNKNOWN_PKG) {
		value = readl(np + AIROHA_NP_SCU_PDIDR);
		pkg = airoha_pkg_from_id(FIELD_GET(AIROHA_NP_SCU_PDIDR_MASK, value));
	}

	priv->soc_family = pkg;

	switch (pkg) {
	case EN751221_PKG:
	case EN7526C_PKG:
	case EN751627_PKG:
	case MT751020_PKG:
		/* Never decode absent eFuse data as a valid zero-valued ID */
		if (!priv->efuse_base)
			return;

		efuse0 = readl(priv->efuse_base + AIROHA_EFUSE_VERIFY_DATA0);
		priv->soc_source = FROM_EFUSE;
		break;
	default:
		value = readl(np + AIROHA_NP_SCU_SCREG_WR1);
		pkgid = FIELD_GET(AIROHA_NP_SCU_PACKAGE_ID_MASK, value);
		if (value & AIROHA_NP_SCU_PACKAGE_ID_EXT)
			pkgid |= BIT(4);
		priv->soc_id = airoha_pkgid_variant_id(pkg, pkgid);
		priv->soc_source = FROM_SCU;
		goto out;
	}

	switch (pkg) {
	case EN751221_PKG:
		priv->soc_id = airoha_en751221_variant_id(efuse0);
		break;
	case EN7526C_PKG:
		efuse1 = readl(priv->efuse_base + AIROHA_EFUSE_VERIFY_DATA1);
		value = readl(chip + AIROHA_CHIP_SCU_CONFIG);
		priv->soc_id = airoha_en7526c_variant_id(efuse0, efuse1, value);
		break;
	case EN751627_PKG:
		value = readl(chip + AIROHA_CHIP_SCU_PACKAGE);
		priv->soc_id = airoha_en751627_variant_id(efuse0, value);
		break;
	case MT751020_PKG:
		value = readl(np + AIROHA_NP_SCU_SUBMODEL);
		priv->soc_id = airoha_mt751020_variant_id(value, efuse0);
		break;
	default:
		break;
	}
out:
	if (priv->soc_id == END_PACKAGE_ID)
		priv->soc_source = ID_SOURCE_UNKNOWN;
}

static enum airoha_pkg_ids
airoha_clk_get_soc_id(struct device_node *np, enum airoha_id_source *source)
{
	enum airoha_pkg_ids id = END_PACKAGE_ID;
	struct platform_device *pdev;
	struct airoha_clk_priv *priv;

	if (source)
		*source = ID_SOURCE_UNKNOWN;

	if (np) {
		if (!of_match_node(of_match_clk_en7523, np))
			return id;
		of_node_get(np);
	} else {
		for_each_matching_node(np, of_match_clk_en7523) {
			if (of_device_is_available(np))
				break;
		}
	}
	if (!np)
		return id;

	if (!of_device_is_available(np)) {
		of_node_put(np);
		return id;
	}

	pdev = of_find_device_by_node(np);
	of_node_put(np);
	if (!pdev)
		return id;

	/* Copy the immutable result while probe and teardown are excluded */
	device_lock(&pdev->dev);
	priv = platform_get_drvdata(pdev);
	if (priv) {
		id = priv->soc_id;
		if (source)
			*source = priv->soc_source;
	}
	device_unlock(&pdev->dev);
	put_device(&pdev->dev);

	return id;
}

enum airoha_pkg_ids airoha_get_pkg_id(struct device_node *np)
{
	return airoha_clk_get_soc_id(np, NULL);
}
EXPORT_SYMBOL_GPL(airoha_get_pkg_id);

enum airoha_id_source airoha_pkg_source(struct device_node *np)
{
	enum airoha_id_source source;

	airoha_clk_get_soc_id(np, &source);

	return source;
}
EXPORT_SYMBOL_GPL(airoha_pkg_source);

const char *airoha_id_name(enum airoha_pkg_ids id)
{
	if (id >= ARRAY_SIZE(airoha_pkg_id_names) ||
	    !airoha_pkg_id_names[id])
		return "unknown";

	return airoha_pkg_id_names[id];
}
EXPORT_SYMBOL_GPL(airoha_id_name);

static int en7523_clk_probe(struct platform_device *pdev)
{
	const struct airoha_clk_soc_data *soc_data;
	struct clk_hw_onecell_data *clk_data;
	struct device *dev = &pdev->dev;
	struct airoha_clk_priv *priv;
	int err;

	soc_data = device_get_match_data(dev);
	if (!soc_data)
		return -EINVAL;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->soc_data = soc_data;
	err = airoha_clk_map_resources(pdev, priv);
	if (err)
		return dev_err_probe(dev, err, "Failed to map SCU resources\n");

	airoha_clk_detect_soc(priv);
	platform_set_drvdata(pdev, priv);

	clk_data = devm_kzalloc(dev,
				struct_size(clk_data, hws,
					    soc_data->num_clocks),
				GFP_KERNEL);
	if (!clk_data)
		return -ENOMEM;

	clk_data->num = soc_data->num_clocks;
	err = soc_data->hw_init(pdev, priv, clk_data);
	if (err)
		return err;

	err = devm_of_clk_add_hw_provider(dev, of_clk_hw_onecell_get, clk_data);
	if (err)
		return err;

	if (soc_data->probe_child)
		return of_platform_populate(dev->of_node, NULL, NULL, dev);

	return 0;
}

static const struct airoha_clk_soc_data en7523_data = {
	.old_chip_scu_compatible = "airoha,en7523-chip-scu",
	.base_clks = en7523_base_clks,
	.num_base_clks = ARRAY_SIZE(en7523_base_clks),
	/* Increment num_clocks by 1 to account for additional PCIe clock */
	.num_clocks = ARRAY_SIZE(en7523_base_clks) + 1,
	.pcie_clk_id = EN7523_CLK_PCIE,
	.rst_map = en7523_rst_map,
	.rst_ofs = en7581_rst_ofs,
	.nr_resets = ARRAY_SIZE(en7523_rst_map),
	.pcie_ops = {
		.is_enabled = en7523_pci_is_enabled,
		.prepare = en7523_pci_prepare,
		.unprepare = en7523_pci_unprepare,
	},
	.hw_init = en7523_clk_hw_init,
};

static const struct airoha_clk_soc_data en7581_data = {
	.old_chip_scu_compatible = "airoha,en7581-chip-scu",
	.base_clks = en7581_base_clks,
	.num_base_clks = ARRAY_SIZE(en7581_base_clks),
	/* Increment num_clocks by 1 to account for additional PCIe clock */
	.num_clocks = ARRAY_SIZE(en7581_base_clks) + 1,
	.pcie_clk_id = EN7523_CLK_PCIE,
	.rst_map = en7581_rst_map,
	.rst_ofs = en7581_rst_ofs,
	.nr_resets = ARRAY_SIZE(en7581_rst_map),
	.pcie_ops = {
		.is_enabled = en7581_pci_is_enabled,
		.enable = en7581_pci_enable,
		.disable = en7581_pci_disable,
	},
	.hw_init = en7581_clk_hw_init,
};

static const struct airoha_clk_soc_data an7583_data = {
	.old_chip_scu_compatible = "airoha,an7583-chip-scu",
	.probe_child = true,
	.base_clks = an7583_base_clks,
	.num_base_clks = ARRAY_SIZE(an7583_base_clks),
	/* Increment num_clocks by 1 to account for additional PCIe clock */
	.num_clocks = ARRAY_SIZE(an7583_base_clks) + 1,
	.pcie_clk_id = EN7523_CLK_PCIE,
	.rst_map = an7583_rst_map,
	.rst_ofs = en7581_rst_ofs,
	.nr_resets = ARRAY_SIZE(an7583_rst_map),
	.pcie_ops = {
		.is_enabled = en7581_pci_is_enabled,
		.enable = en7581_pci_enable,
		.disable = en7581_pci_disable,
	},
	.hw_init = an7583_clk_hw_init,
};

static const struct econet_clk_soc_data en751221_econet_data = {
	.spi_alt_desc = &en7526c_spi_clk,
	.xpon_tod_clk_reg = EN751221_REG_NP_PER_DOM_CLK_GAT_1,
	.xpon_tod_clk_mask = EN751221_XPON_TOD_CLK_EN,
	.xpon_tod_div_reg = EN751221_REG_TOD_DIVIDER_ENABLE,
	.xpon_tod_div_mask = EN751221_XPON_TOD_DIV_EN,
};

static const struct econet_clk_soc_data en751627_econet_data = {
	.xpon_tod_clk_reg = EN751627_REG_NP_PER_DOM_CLK_GAT_1,
	.xpon_tod_clk_mask = EN751221_XPON_TOD_CLK_EN,
	.xpon_tod_div_reg = EN751627_REG_TOD_DIVIDER_ENABLE,
	.xpon_tod_div_mask = EN751221_XPON_TOD_DIV_EN,
};

static const struct econet_clk_soc_data en7528_econet_data = {
	.xpon_tod_clk_reg = EN7528_REG_NP_PER_DOM_CLK_GAT_1,
	.xpon_tod_clk_mask = EN7528_XPON_TOD_CLK_EN,
	.xpon_tod_div_reg = EN7528_REG_TOD_DIVIDER_ENABLE,
	.xpon_tod_div_mask = EN7528_XPON_TOD_DIV_EN,
	.shared_unzip_reg = EN7528_REG_SHARED_UNZIPMENT_SEL,
	.shared_unzip_val = EN7528_SHARED_UNZIPMENT_FE,
};

static const struct econet_clk_soc_data en7580_econet_data = {
	.xpon_tod_clk_reg = EN7580_REG_NP_PER_DOM_CLK_GAT_1,
	.xpon_tod_clk_mask = EN751221_XPON_TOD_CLK_EN,
	.xpon_tod_div_reg = EN7580_REG_TOD_DIVIDER_ENABLE,
	.xpon_tod_div_mask = EN751221_XPON_TOD_DIV_EN,
};

static const struct airoha_clk_soc_data en751221_data = {
	.old_chip_scu_compatible = "econet,en751221-chip-scu",
	.base_clks = en751221_base_clks,
	.num_base_clks = ARRAY_SIZE(en751221_base_clks),
	.bus_clk_mask = EN751221_REG_BUS_MASK,
	.cpu_clk_mult = 4,
	.num_clocks = EN751221_MAX_CLKS,
	.pcie_clk_id = EN751221_CLK_PCIE,
	.bus_clk_id = EN751221_CLK_BUS,
	.cpu_clk_id = EN751221_CLK_CPU,
	.rst_map = en751221_rst_map,
	.rst_ofs = en751221_rst_ofs,
	.nr_resets = ARRAY_SIZE(en751221_rst_map),
	.econet = &en751221_econet_data,
	.pcie_ops = {
		.is_enabled = en7523_pci_is_enabled,
		.prepare = en7523_pci_prepare,
		.unprepare = en7523_pci_unprepare,
	},
	.hw_init = econet_clk_hw_init,
};

static const struct airoha_clk_soc_data en751627_data = {
	.old_chip_scu_compatible = "econet,en751627-chip-scu",
	.base_clks = en751627_base_clks,
	.num_base_clks = ARRAY_SIZE(en751627_base_clks),
	.bus_clk_mask = EN751221_REG_BUS_MASK,
	.cpu_clk_mult = 4,
	/* EN751627 keeps the EN751221 NP-SCU reset layout */
	.num_clocks = EN7523_CLK_CPU + 1,
	.pcie_clk_id = EN7523_CLK_PCIE,
	.bus_clk_id = EN7523_CLK_BUS,
	.cpu_clk_id = EN7523_CLK_CPU,
	.rst_map = en751221_rst_map,
	.rst_ofs = en751221_rst_ofs,
	.nr_resets = ARRAY_SIZE(en751221_rst_map),
	.econet = &en751627_econet_data,
	.pcie_ops = {
		.is_enabled = en7523_pci_is_enabled,
		.prepare = en7523_pci_prepare,
		.unprepare = en7523_pci_unprepare,
	},
	.hw_init = econet_clk_hw_init,
};

static const struct airoha_clk_soc_data en7528_data = {
	.old_chip_scu_compatible = "econet,en7528-chip-scu",
	.base_clks = en7528_base_clks,
	.num_base_clks = ARRAY_SIZE(en7528_base_clks),
	.bus_clk_mask = EN7528_REG_BUS_MASK,
	.cpu_clk_mult = 4,
	.num_clocks = EN7523_CLK_CPU + 1,
	.pcie_clk_id = EN7523_CLK_PCIE,
	.bus_clk_id = EN7523_CLK_BUS,
	.cpu_clk_id = EN7523_CLK_CPU,
	/* EN7528 keeps the EN751221 NP-SCU reset layout */
	.rst_map = en751221_rst_map,
	.rst_ofs = en751221_rst_ofs,
	.nr_resets = ARRAY_SIZE(en751221_rst_map),
	.econet = &en7528_econet_data,
	.pcie_ops = {
		.is_enabled = en7523_pci_is_enabled,
		.prepare = en7523_pci_prepare,
		.unprepare = en7523_pci_unprepare,
	},
	.hw_init = econet_clk_hw_init,
};

static const struct airoha_clk_soc_data en7580_data = {
	.old_chip_scu_compatible = "econet,en7580-chip-scu",
	.base_clks = en7580_base_clks,
	.num_base_clks = ARRAY_SIZE(en7580_base_clks),
	.bus_clk_mask = EN7528_REG_BUS_MASK,
	.cpu_clk_mult = 5,
	.num_clocks = EN7523_CLK_CPU + 1,
	.pcie_clk_id = EN7523_CLK_PCIE,
	.bus_clk_id = EN7523_CLK_BUS,
	.cpu_clk_id = EN7523_CLK_CPU,
	/* The MIPS EN7580 retains the legacy NP-SCU reset banks */
	.rst_map = en751221_rst_map,
	.rst_ofs = en751221_rst_ofs,
	.nr_resets = ARRAY_SIZE(en751221_rst_map),
	.econet = &en7580_econet_data,
	.pcie_ops = {
		.is_enabled = en7523_pci_is_enabled,
		.prepare = en7523_pci_prepare,
		.unprepare = en7523_pci_unprepare,
	},
	.hw_init = econet_clk_hw_init,
};

static const struct of_device_id of_match_clk_en7523[] = {
	{ .compatible = "airoha,en7523-scu", .data = &en7523_data },
	{ .compatible = "airoha,en7581-scu", .data = &en7581_data },
	{ .compatible = "airoha,an7583-scu", .data = &an7583_data },
	{ .compatible = "econet,en751221-scu", .data = &en751221_data },
	{ .compatible = "econet,en751627-scu", .data = &en751627_data },
	{ .compatible = "econet,en7528-scu", .data = &en7528_data },
	{ .compatible = "econet,en7580-scu", .data = &en7580_data },
	{ /* sentinel */ }
};

static struct platform_driver clk_en7523_drv = {
	.probe = en7523_clk_probe,
	.driver = {
		.name = "clk-en7523",
		.of_match_table = of_match_clk_en7523,
		.suppress_bind_attrs = true,
	},
};

static int __init clk_en7523_init(void)
{
	return platform_driver_register(&clk_en7523_drv);
}

arch_initcall(clk_en7523_init);
