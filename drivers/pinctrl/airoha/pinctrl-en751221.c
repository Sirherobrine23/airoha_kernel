// SPDX-License-Identifier: GPL-2.0-only
/*
 * EcoNet EN7512/EN7521 family pinctrl data for the Airoha pinctrl core.
 *
 * The EN7512, EN7513, EN7521 and EN7526 share the same GPIO controller.
 * DSL and xPON variants differ mostly in which muxed signals are bonded out.
 */

#include "airoha-common.h"

#define REG_IOMUX_CONTROL1			0x0104

#define SIPO_RCLK_MODE_MASK			BIT(23)
#define DMT_TOD_1PPS_MODE_MASK			BIT(22)
#define PCIE_RESET1_MODE_MASK			BIT(21)
#define PCIE_RESET0_MODE_MASK			BIT(20)
#define SPI_QUAD_MODE_MASK			BIT(19)
#define UART2_MODE_MASK			BIT(18)
#define SIPO_MODE_MASK				BIT(17)
#define PON_MODE_MASK				BIT(15)
#define PCM2_MODE_MASK				BIT(14)
#define PCM1_MODE_MASK				BIT(13)
#define PCM_SPI_MODE_MASK			BIT(12)
#define PCM_SPI_INT_MODE_MASK			BIT(11)
#define PCM_SPI_RESET_MODE_MASK		BIT(10)
#define PCM_SPI_CS4_MODE_MASK			BIT(9)
#define PCM_SPI_CS3_MODE_MASK			BIT(8)
#define GSW_PORT4_LED0_MODE_MASK		BIT(7)
#define PHY4_LED0_MODE_MASK			BIT(6)
#define PHY3_LED0_MODE_MASK			BIT(5)
#define PHY2_LED0_MODE_MASK			BIT(4)
#define PHY1_LED0_MODE_MASK			BIT(3)
#define PON_TOD_1PPS_MODE_MASK			BIT(2)
#define GSW_TOD_1PPS_MODE_MASK			BIT(1)
#define PON_I2C_MODE_MASK			BIT(0)

#define GPIO3_MUX_MASK		(SPI_QUAD_MODE_MASK | UART2_MODE_MASK | \
				 PCM_SPI_INT_MODE_MASK | PCM_SPI_CS3_MODE_MASK | \
				 GSW_PORT4_LED0_MODE_MASK)
#define GPIO4_6_MUX_MASK	(PCM2_MODE_MASK | PCM_SPI_MODE_MASK)
#define GPIO7_MUX_MASK		(GPIO4_6_MUX_MASK | PHY4_LED0_MODE_MASK)
#define GPIO8_MUX_MASK		(SIPO_MODE_MASK | PHY3_LED0_MODE_MASK)
#define GPIO9_MUX_MASK		(SIPO_RCLK_MODE_MASK | PCM_SPI_CS4_MODE_MASK | \
				 PHY2_LED0_MODE_MASK)
#define GPIO10_MUX_MASK		(SPI_QUAD_MODE_MASK | UART2_MODE_MASK | \
				 PHY1_LED0_MODE_MASK)
#define GPIO22_MUX_MASK		(DMT_TOD_1PPS_MODE_MASK | \
				 PON_TOD_1PPS_MODE_MASK | \
				 GSW_TOD_1PPS_MODE_MASK)

#define ECONET_MUX_GROUP(_name, _mask, _val)			\
	{							\
		.name = (_name),				\
		.regmap[0] = {					\
			AIROHA_FUNC_MUX,			\
			REG_IOMUX_CONTROL1,			\
			(_mask),				\
			(_val),				\
		},						\
		.regmap_size = 1,				\
	}

#define ECONET_GPIO_MUX(_pin, _mask)				\
	{							\
		.pin = (_pin),					\
		.reg = { REG_IOMUX_CONTROL1, (_mask) },		\
	}

static const struct pinctrl_pin_desc pinctrl_pins[] = {
	PINCTRL_PIN(0, "gpio0"),
	PINCTRL_PIN(1, "gpio1"),
	PINCTRL_PIN(2, "gpio2"),
	PINCTRL_PIN(3, "gpio3"),
	PINCTRL_PIN(4, "gpio4"),
	PINCTRL_PIN(5, "gpio5"),
	PINCTRL_PIN(6, "gpio6"),
	PINCTRL_PIN(7, "gpio7"),
	PINCTRL_PIN(8, "gpio8"),
	PINCTRL_PIN(9, "gpio9"),
	PINCTRL_PIN(10, "gpio10"),
	PINCTRL_PIN(11, "gpio11"),
	PINCTRL_PIN(12, "gpio12"),
	PINCTRL_PIN(13, "gpio13"),
	PINCTRL_PIN(14, "gpio14"),
	PINCTRL_PIN(15, "gpio15"),
	PINCTRL_PIN(16, "gpio16"),
	PINCTRL_PIN(17, "gpio17"),
	PINCTRL_PIN(18, "gpio18"),
	PINCTRL_PIN(19, "gpio19"),
	PINCTRL_PIN(20, "gpio20"),
	PINCTRL_PIN(21, "gpio21"),
	PINCTRL_PIN(22, "gpio22"),
	PINCTRL_PIN(23, "gpio23"),
	PINCTRL_PIN(24, "gpio24"),
	PINCTRL_PIN(25, "gpio25"),
	PINCTRL_PIN(26, "gpio26"),
	PINCTRL_PIN(27, "gpio27"),
	PINCTRL_PIN(28, "gpio28"),
	PINCTRL_PIN(29, "gpio29"),
	PINCTRL_PIN(30, "gpio30"),
	PINCTRL_PIN(31, "gpio31"),
	PINCTRL_PIN(32, "gpio32"),
	PINCTRL_PIN(33, "gpio33"),
	PINCTRL_PIN(34, "gpio34"),
	PINCTRL_PIN(35, "gpio35"),
	PINCTRL_PIN(36, "gpio36"),
	PINCTRL_PIN(37, "gpio37"),
	PINCTRL_PIN(38, "gpio38"),
	PINCTRL_PIN(39, "gpio39"),
	PINCTRL_PIN(40, "gpio40"),
	PINCTRL_PIN(41, "gpio41"),
	PINCTRL_PIN(42, "gpio42"),
	PINCTRL_PIN(43, "gpio43"),
	PINCTRL_PIN(44, "gpio44"),
	PINCTRL_PIN(45, "gpio45"),
	PINCTRL_PIN(46, "gpio46"),
	PINCTRL_PIN(47, "gpio47"),
	PINCTRL_PIN(48, "gpio48"),
	PINCTRL_PIN(49, "gpio49"),
	PINCTRL_PIN(50, "gpio50"),
	PINCTRL_PIN(51, "gpio51"),
	PINCTRL_PIN(52, "gpio52"),
	PINCTRL_PIN(53, "gpio53"),
	PINCTRL_PIN(54, "gpio54"),
	PINCTRL_PIN(55, "gpio55"),
	PINCTRL_PIN(56, "gpio56"),
	PINCTRL_PIN(57, "gpio57"),
	PINCTRL_PIN(58, "gpio58"),
	PINCTRL_PIN(59, "gpio59"),
	PINCTRL_PIN(60, "gpio60"),
	PINCTRL_PIN(61, "gpio61"),
	PINCTRL_PIN(62, "gpio62"),
	PINCTRL_PIN(63, "gpio63"),
};

static const int sipo_pins[] = { 8, 11 };
static const int sipo_rclk_pins[] = { 8, 9, 11 };
static const int dmt_tod_1pps_pins[] = { 22 };
static const int pon_tod_1pps_pins[] = { 22 };
static const int gsw_tod_1pps_pins[] = { 22 };
static const int pcie_reset0_pins[] = { 30 };
static const int pcie_reset1_pins[] = { 31 };
static const int spi_quad_pins[] = { 3, 10 };
static const int uart2_pins[] = { 3, 10 };
static const int pon_pins[] = { 16, 17, 18, 19, 20 };
static const int pcm1_pins[] = { 12, 13, 14, 15 };
static const int pcm2_pins[] = { 4, 5, 6, 7 };
static const int pcm_spi_pins[] = { 4, 5, 6, 7 };
static const int pcm_spi_int_pins[] = { 3 };
static const int pcm_spi_rst_pins[] = { 2 };
static const int pcm_spi_cs3_pins[] = { 3 };
static const int pcm_spi_cs4_pins[] = { 9 };
static const int gpio2_pins[] = { 2 };
static const int gpio3_pins[] = { 3 };
static const int gpio4_pins[] = { 4 };
static const int gpio5_pins[] = { 5 };
static const int gpio6_pins[] = { 6 };
static const int gpio7_pins[] = { 7 };
static const int gpio8_pins[] = { 8 };
static const int gpio9_pins[] = { 9 };
static const int gpio10_pins[] = { 10 };
static const int gpio11_pins[] = { 11 };
static const int gpio12_pins[] = { 12 };
static const int gpio13_pins[] = { 13 };
static const int gpio14_pins[] = { 14 };
static const int gpio15_pins[] = { 15 };
static const int gpio16_pins[] = { 16 };
static const int gpio17_pins[] = { 17 };
static const int gpio18_pins[] = { 18 };
static const int gpio19_pins[] = { 19 };
static const int gpio20_pins[] = { 20 };
static const int gpio22_pins[] = { 22 };
static const int gpio30_pins[] = { 30 };
static const int gpio31_pins[] = { 31 };

static const struct pingroup pinctrl_groups[] = {
	PINCTRL_PIN_GROUP("sipo", sipo),
	PINCTRL_PIN_GROUP("sipo_rclk", sipo_rclk),
	PINCTRL_PIN_GROUP("dmt_tod_1pps", dmt_tod_1pps),
	PINCTRL_PIN_GROUP("pon_tod_1pps", pon_tod_1pps),
	PINCTRL_PIN_GROUP("gsw_tod_1pps", gsw_tod_1pps),
	PINCTRL_PIN_GROUP("pcie_reset0", pcie_reset0),
	PINCTRL_PIN_GROUP("pcie_reset1", pcie_reset1),
	PINCTRL_PIN_GROUP("spi_quad", spi_quad),
	PINCTRL_PIN_GROUP("uart2", uart2),
	PINCTRL_PIN_GROUP("pon", pon),
	PINCTRL_PIN_GROUP("pcm1", pcm1),
	PINCTRL_PIN_GROUP("pcm2", pcm2),
	PINCTRL_PIN_GROUP("pcm_spi", pcm_spi),
	PINCTRL_PIN_GROUP("pcm_spi_int", pcm_spi_int),
	PINCTRL_PIN_GROUP("pcm_spi_rst", pcm_spi_rst),
	PINCTRL_PIN_GROUP("pcm_spi_cs3", pcm_spi_cs3),
	PINCTRL_PIN_GROUP("pcm_spi_cs4", pcm_spi_cs4),
	PINCTRL_PIN_GROUP("gpio2", gpio2),
	PINCTRL_PIN_GROUP("gpio3", gpio3),
	PINCTRL_PIN_GROUP("gpio4", gpio4),
	PINCTRL_PIN_GROUP("gpio5", gpio5),
	PINCTRL_PIN_GROUP("gpio6", gpio6),
	PINCTRL_PIN_GROUP("gpio7", gpio7),
	PINCTRL_PIN_GROUP("gpio8", gpio8),
	PINCTRL_PIN_GROUP("gpio9", gpio9),
	PINCTRL_PIN_GROUP("gpio10", gpio10),
	PINCTRL_PIN_GROUP("gpio11", gpio11),
	PINCTRL_PIN_GROUP("gpio12", gpio12),
	PINCTRL_PIN_GROUP("gpio13", gpio13),
	PINCTRL_PIN_GROUP("gpio14", gpio14),
	PINCTRL_PIN_GROUP("gpio15", gpio15),
	PINCTRL_PIN_GROUP("gpio16", gpio16),
	PINCTRL_PIN_GROUP("gpio17", gpio17),
	PINCTRL_PIN_GROUP("gpio18", gpio18),
	PINCTRL_PIN_GROUP("gpio19", gpio19),
	PINCTRL_PIN_GROUP("gpio20", gpio20),
	PINCTRL_PIN_GROUP("gpio22", gpio22),
	PINCTRL_PIN_GROUP("gpio30", gpio30),
	PINCTRL_PIN_GROUP("gpio31", gpio31),
};

static const char *const sipo_groups[] = { "sipo", "sipo_rclk" };
static const char *const tod_1pps_groups[] = {
	"dmt_tod_1pps", "pon_tod_1pps", "gsw_tod_1pps"
};

static const char *const pcie_reset_groups[] = {
	"pcie_reset0", "pcie_reset1"
};

static const char *const spi_groups[] = { "spi_quad" };
static const char *const uart_groups[] = { "uart2" };
static const char *const pon_groups[] = { "pon" };
static const char *const pcm_groups[] = { "pcm1", "pcm2" };
static const char *const pcm_spi_groups[] = {
	"pcm_spi", "pcm_spi_int", "pcm_spi_rst", "pcm_spi_cs3",
	"pcm_spi_cs4"
};

static const char *const phy1_led0_groups[] = { "gpio10" };
static const char *const phy2_led0_groups[] = { "gpio9" };
static const char *const phy3_led0_groups[] = { "gpio8" };
static const char *const phy4_led0_groups[] = { "gpio7" };
static const char *const gsw_port4_led0_groups[] = { "gpio3" };

static const char *const gpio_groups[] = {
	"gpio2", "gpio3", "gpio4", "gpio5", "gpio6", "gpio7",
	"gpio8", "gpio9", "gpio10", "gpio11", "gpio12", "gpio13",
	"gpio14", "gpio15", "gpio16", "gpio17", "gpio18", "gpio19",
	"gpio20", "gpio22", "gpio30", "gpio31"
};

static const struct airoha_pinctrl_func_group sipo_func_group[] = {
	ECONET_MUX_GROUP("sipo", GPIO8_MUX_MASK | SIPO_RCLK_MODE_MASK,
			 SIPO_MODE_MASK),
	ECONET_MUX_GROUP("sipo_rclk", GPIO8_MUX_MASK | GPIO9_MUX_MASK,
			 SIPO_MODE_MASK | SIPO_RCLK_MODE_MASK),
};

static const struct airoha_pinctrl_func_group tod_1pps_func_group[] = {
	ECONET_MUX_GROUP("dmt_tod_1pps", GPIO22_MUX_MASK,
			 DMT_TOD_1PPS_MODE_MASK),
	ECONET_MUX_GROUP("pon_tod_1pps", GPIO22_MUX_MASK,
			 PON_TOD_1PPS_MODE_MASK),
	ECONET_MUX_GROUP("gsw_tod_1pps", GPIO22_MUX_MASK,
			 GSW_TOD_1PPS_MODE_MASK),
};

static const struct airoha_pinctrl_func_group pcie_reset_func_group[] = {
	ECONET_MUX_GROUP("pcie_reset0", PCIE_RESET0_MODE_MASK,
			 PCIE_RESET0_MODE_MASK),
	ECONET_MUX_GROUP("pcie_reset1", PCIE_RESET1_MODE_MASK,
			 PCIE_RESET1_MODE_MASK),
};

static const struct airoha_pinctrl_func_group spi_func_group[] = {
	ECONET_MUX_GROUP("spi_quad", GPIO3_MUX_MASK | GPIO10_MUX_MASK,
			 SPI_QUAD_MODE_MASK),
};

static const struct airoha_pinctrl_func_group uart_func_group[] = {
	ECONET_MUX_GROUP("uart2", GPIO3_MUX_MASK | GPIO10_MUX_MASK,
			 UART2_MODE_MASK),
};

static const struct airoha_pinctrl_func_group pon_func_group[] = {
	ECONET_MUX_GROUP("pon", PON_MODE_MASK | PON_I2C_MODE_MASK,
			 PON_MODE_MASK | PON_I2C_MODE_MASK),
};

static const struct airoha_pinctrl_func_group pcm_func_group[] = {
	ECONET_MUX_GROUP("pcm1", PCM1_MODE_MASK, PCM1_MODE_MASK),
	ECONET_MUX_GROUP("pcm2", PCM2_MODE_MASK | PHY4_LED0_MODE_MASK,
			 PCM2_MODE_MASK),
};

static const struct airoha_pinctrl_func_group pcm_spi_func_group[] = {
	ECONET_MUX_GROUP("pcm_spi", PCM_SPI_MODE_MASK | PHY4_LED0_MODE_MASK,
			 PCM_SPI_MODE_MASK),
	ECONET_MUX_GROUP("pcm_spi_int", GPIO3_MUX_MASK,
			 PCM_SPI_INT_MODE_MASK),
	ECONET_MUX_GROUP("pcm_spi_rst", PCM_SPI_RESET_MODE_MASK,
			 PCM_SPI_RESET_MODE_MASK),
	ECONET_MUX_GROUP("pcm_spi_cs3", GPIO3_MUX_MASK,
			 PCM_SPI_CS3_MODE_MASK),
	ECONET_MUX_GROUP("pcm_spi_cs4", GPIO9_MUX_MASK,
			 PCM_SPI_CS4_MODE_MASK),
};

static const struct airoha_pinctrl_func_group phy1_led0_func_group[] = {
	ECONET_MUX_GROUP("gpio10", GPIO10_MUX_MASK, PHY1_LED0_MODE_MASK),
};

static const struct airoha_pinctrl_func_group phy2_led0_func_group[] = {
	ECONET_MUX_GROUP("gpio9", GPIO9_MUX_MASK, PHY2_LED0_MODE_MASK),
};

static const struct airoha_pinctrl_func_group phy3_led0_func_group[] = {
	ECONET_MUX_GROUP("gpio8", GPIO8_MUX_MASK, PHY3_LED0_MODE_MASK),
};

static const struct airoha_pinctrl_func_group phy4_led0_func_group[] = {
	ECONET_MUX_GROUP("gpio7", GPIO7_MUX_MASK, PHY4_LED0_MODE_MASK),
};

static const struct airoha_pinctrl_func_group gsw_port4_led0_func_group[] = {
	ECONET_MUX_GROUP("gpio3", GPIO3_MUX_MASK, GSW_PORT4_LED0_MODE_MASK),
};

static const struct airoha_pinctrl_func_group gpio_func_group[] = {
	ECONET_MUX_GROUP("gpio2", PCM_SPI_RESET_MODE_MASK, 0),
	ECONET_MUX_GROUP("gpio3", GPIO3_MUX_MASK, 0),
	ECONET_MUX_GROUP("gpio4", GPIO4_6_MUX_MASK, 0),
	ECONET_MUX_GROUP("gpio5", GPIO4_6_MUX_MASK, 0),
	ECONET_MUX_GROUP("gpio6", GPIO4_6_MUX_MASK, 0),
	ECONET_MUX_GROUP("gpio7", GPIO7_MUX_MASK, 0),
	ECONET_MUX_GROUP("gpio8", GPIO8_MUX_MASK, 0),
	ECONET_MUX_GROUP("gpio9", GPIO9_MUX_MASK, 0),
	ECONET_MUX_GROUP("gpio10", GPIO10_MUX_MASK, 0),
	ECONET_MUX_GROUP("gpio11", SIPO_MODE_MASK, 0),
	ECONET_MUX_GROUP("gpio12", PCM1_MODE_MASK, 0),
	ECONET_MUX_GROUP("gpio13", PCM1_MODE_MASK, 0),
	ECONET_MUX_GROUP("gpio14", PCM1_MODE_MASK, 0),
	ECONET_MUX_GROUP("gpio15", PCM1_MODE_MASK, 0),
	ECONET_MUX_GROUP("gpio16", PON_MODE_MASK, 0),
	ECONET_MUX_GROUP("gpio17", PON_MODE_MASK, 0),
	ECONET_MUX_GROUP("gpio18", PON_MODE_MASK, 0),
	ECONET_MUX_GROUP("gpio19", PON_MODE_MASK, 0),
	ECONET_MUX_GROUP("gpio20", PON_MODE_MASK, 0),
	ECONET_MUX_GROUP("gpio22", GPIO22_MUX_MASK, 0),
	ECONET_MUX_GROUP("gpio30", PCIE_RESET0_MODE_MASK, 0),
	ECONET_MUX_GROUP("gpio31", PCIE_RESET1_MODE_MASK, 0),
};

static const struct airoha_pinctrl_func pinctrl_funcs[] = {
	PINCTRL_FUNC_DESC("sipo", sipo),
	PINCTRL_FUNC_DESC("tod_1pps", tod_1pps),
	PINCTRL_FUNC_DESC("pcie_reset", pcie_reset),
	PINCTRL_FUNC_DESC("spi", spi),
	PINCTRL_FUNC_DESC("uart", uart),
	PINCTRL_FUNC_DESC("pon", pon),
	PINCTRL_FUNC_DESC("pcm", pcm),
	PINCTRL_FUNC_DESC("pcm_spi", pcm_spi),
	PINCTRL_FUNC_DESC("phy1_led0", phy1_led0),
	PINCTRL_FUNC_DESC("phy2_led0", phy2_led0),
	PINCTRL_FUNC_DESC("phy3_led0", phy3_led0),
	PINCTRL_FUNC_DESC("phy4_led0", phy4_led0),
	PINCTRL_FUNC_DESC("gsw_port4_led0", gsw_port4_led0),
	PINCTRL_FUNC_DESC("gpio", gpio),
};

static const struct airoha_pinctrl_gpio_mux pinctrl_gpio_muxes[] = {
	ECONET_GPIO_MUX(2, PCM_SPI_RESET_MODE_MASK),
	ECONET_GPIO_MUX(3, GPIO3_MUX_MASK),
	ECONET_GPIO_MUX(4, GPIO4_6_MUX_MASK),
	ECONET_GPIO_MUX(5, GPIO4_6_MUX_MASK),
	ECONET_GPIO_MUX(6, GPIO4_6_MUX_MASK),
	ECONET_GPIO_MUX(7, GPIO7_MUX_MASK),
	ECONET_GPIO_MUX(8, GPIO8_MUX_MASK),
	ECONET_GPIO_MUX(9, GPIO9_MUX_MASK),
	ECONET_GPIO_MUX(10, GPIO10_MUX_MASK),
	ECONET_GPIO_MUX(11, SIPO_MODE_MASK),
	ECONET_GPIO_MUX(12, PCM1_MODE_MASK),
	ECONET_GPIO_MUX(13, PCM1_MODE_MASK),
	ECONET_GPIO_MUX(14, PCM1_MODE_MASK),
	ECONET_GPIO_MUX(15, PCM1_MODE_MASK),
	ECONET_GPIO_MUX(16, PON_MODE_MASK),
	ECONET_GPIO_MUX(17, PON_MODE_MASK),
	ECONET_GPIO_MUX(18, PON_MODE_MASK),
	ECONET_GPIO_MUX(19, PON_MODE_MASK),
	ECONET_GPIO_MUX(20, PON_MODE_MASK),
	ECONET_GPIO_MUX(22, GPIO22_MUX_MASK),
	ECONET_GPIO_MUX(30, PCIE_RESET0_MODE_MASK),
	ECONET_GPIO_MUX(31, PCIE_RESET1_MODE_MASK),
};

static const struct airoha_pinctrl_match_data pinctrl_match_data = {
	.chip_scu_compatible = "airoha,chip-scu",
	.pinctrl_name = KBUILD_MODNAME,
	.pinctrl_owner = THIS_MODULE,
	.pins = pinctrl_pins,
	.num_pins = ARRAY_SIZE(pinctrl_pins),
	.grps = pinctrl_groups,
	.num_grps = ARRAY_SIZE(pinctrl_groups),
	.funcs = pinctrl_funcs,
	.num_funcs = ARRAY_SIZE(pinctrl_funcs),
	.num_gpio = 64,
	.num_irq = 16,
	.gpio_muxes = pinctrl_gpio_muxes,
	.num_gpio_muxes = ARRAY_SIZE(pinctrl_gpio_muxes),
};

static const struct of_device_id econet_pinctrl_of_match[] = {
	{
		.compatible = "econet,en751221-pinctrl",
		.data = &pinctrl_match_data,
	},
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, econet_pinctrl_of_match);

static struct platform_driver econet_pinctrl_driver = {
	.probe = airoha_pinctrl_probe,
	.driver = {
		.name = "pinctrl-econet-en751221",
		.of_match_table = econet_pinctrl_of_match,
	},
};
module_platform_driver(econet_pinctrl_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Matheus Sampaio Queiroga <srherobrine20@gmail.com>");
MODULE_DESCRIPTION("Pinctrl driver for EcoNet EN7512/EN7521 SoCs");
