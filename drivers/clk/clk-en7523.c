// SPDX-License-Identifier: GPL-2.0-only

#include <linux/bitfield.h>
#include <linux/clk-provider.h>
#include <linux/delay.h>
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
#include <dt-bindings/clock/econet,en751221-scu.h>
#include <dt-bindings/clock/en7523-clk.h>
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
#define REG_HIR				0x064
#define   REG_HIR_MASK			GENMASK(31, 16)
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
/* EN751221 */
#define EN751221_REG_SPI_DIV		0x0cc
#define EN751221_SPI_BASE		500000000
#define EN751221_SPI_BASE_EN7526C	400000000
#define EN751221_SPI_DIV_DEFAULT	40
#define EN751221_REG_BUS		0x284
#define EN751221_REG_BUS_MASK		GENMASK(21, 12)
#define EN751221_REG_SSR3		0x094

#define REG_RST_CTRL2			0x830
#define REG_RST_CTRL1			0x834
#define   REG_PCIE_HB_RST		BIT(29)
#define EN751221_REG_RST_DMT		0x84
#define EN751221_REG_RST_USB		0xec

#define EN751221_MAX_CLKS		5

enum en_hir {
	HIR_UNKNOWN	= -1,
	HIR_TC3169	= 0,
	HIR_TC3182	= 1,
	HIR_RT65168	= 2,
	HIR_RT63165	= 3,
	HIR_RT63365	= 4,
	HIR_MT751020	= 5,
	HIR_MT7505	= 6,
	HIR_EN751221	= 7,
	HIR_EN7526C	= 8,
	HIR_EN751627	= 9,
	HIR_EN7580	= 10,
	HIR_EN7528	= 11,
	HIR_EN7523	= 12,
	HIR_EN7581	= 13,
	HIR_MAX		= 14,
};

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

struct airoha_clk_priv;

struct airoha_clk_soc_data {
	const char *old_chip_scu_compatible;
	bool probe_child;
	u32 num_clocks;
	u32 num_base_clks;
	u32 pcie_clk_id;
	const u16 *rst_map;
	const u16 *rst_ofs;
	int nr_resets;
	const struct en_clk_desc *base_clks;
	const struct clk_ops pcie_ops;
	int (*hw_init)(struct platform_device *pdev,
		       struct airoha_clk_priv *priv,
		       struct clk_hw_onecell_data *clk_data);
};

struct airoha_clk_priv {
	const struct airoha_clk_soc_data *soc_data;
	struct regmap *scu_chip;
	struct regmap *scu_np;
	void __iomem *np_scu_base;
	void __iomem *chip_scu_base;
	resource_size_t chip_scu_size;
	struct en_serdes_phy_instance *serdes_phys[EN_SERDES_PHY_NUM];
};

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

static enum en_hir get_hw_id(struct regmap *np_map)
{
	u32 val;

	if (regmap_read(np_map, REG_HIR, &val))
		return HIR_UNKNOWN;

	val = FIELD_GET(REG_HIR_MASK, val);
	if (val < HIR_MAX)
		return (enum en_hir)val;

	pr_warn("Unable to determine EcoNet SoC\n");

	return HIR_UNKNOWN;
}

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

		if (desc->id == EN751221_CLK_SPI &&
		    desc->base_reg == EN751221_REG_SPI_DIV &&
		    get_hw_id(priv->scu_np) == HIR_EN7526C)
			desc = &en7526c_spi_clk;

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

	/* Reset PCIE_HB */
	regmap_set_bits(clk_map, REG_RST_CTRL1, REG_PCIE_HB_RST);
	regmap_clear_bits(clk_map, REG_RST_CTRL1, REG_PCIE_HB_RST);

	err = register_resets(dev, clk_map, soc_data);
	if (err)
		return err;

	return en7581_serdes_phy_register(dev, priv);
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

static int en751221_clk_hw_init(struct platform_device *pdev,
				struct airoha_clk_priv *priv,
				struct clk_hw_onecell_data *clk_data)
{
	const struct airoha_clk_soc_data *soc_data = priv->soc_data;
	struct device *dev = &pdev->dev;
	u32 rate, val;
	int err;

	err = en75xx_register_clocks(dev, priv, clk_data);
	if (err)
		return err;

	/* BUS */
	err = regmap_read(priv->scu_np, EN751221_REG_BUS, &val);
	if (err)
		return dev_err_probe(dev, err,
				     "Failed reading fixed clk rate bus\n");

	rate = FIELD_GET(EN751221_REG_BUS_MASK, val) * 1000000;
	err = econet_register_fixed_rate(dev, EN751221_CLK_BUS, clk_data,
					 "bus", rate);
	if (err)
		return err;

	/* CPU */
	err = econet_register_fixed_rate(dev, EN751221_CLK_CPU, clk_data,
					 "cpu", rate * 4);
	if (err)
		return err;

	return register_resets(dev, priv->scu_np, soc_data);
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
	struct device_node *np = dev->of_node, *chip_np;
	struct resource *res_mem;
	struct resource res;
	int np_index = 0;
	int err;

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

	return 0;
}

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
	platform_set_drvdata(pdev, priv);

	err = airoha_clk_map_resources(pdev, priv);
	if (err)
		return dev_err_probe(dev, err, "Failed to map SCU resources\n");

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

static const struct airoha_clk_soc_data en751221_data = {
	.old_chip_scu_compatible = "econet,en751221-chip-scu",
	.base_clks = en751221_base_clks,
	.num_base_clks = ARRAY_SIZE(en751221_base_clks),
	.num_clocks = EN751221_MAX_CLKS,
	.pcie_clk_id = EN751221_CLK_PCIE,
	.rst_map = en751221_rst_map,
	.rst_ofs = en751221_rst_ofs,
	.nr_resets = ARRAY_SIZE(en751221_rst_map),
	.pcie_ops = {
		.is_enabled = en7523_pci_is_enabled,
		.prepare = en7523_pci_prepare,
		.unprepare = en7523_pci_unprepare,
	},
	.hw_init = en751221_clk_hw_init,
};

static const struct of_device_id of_match_clk_en7523[] = {
	{ .compatible = "airoha,en7523-scu", .data = &en7523_data },
	{ .compatible = "airoha,en7581-scu", .data = &en7581_data },
	{ .compatible = "econet,en751221-scu", .data = &en751221_data },
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
