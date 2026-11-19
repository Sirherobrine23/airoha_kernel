// SPDX-License-Identifier: GPL-2.0+
/*
 * Airoha/EcoNet 16550-compatible UART driver.
 *
 * Copyright (c) 2022 Genexis Sweden AB
 * Author: Benjamin Larsson <benjamin.larsson@genexis.eu>
 * Author: Matheus Sampaio Queiroga <srherobrine20@gmail.com>
 */

#include <linux/clk.h>
#include <linux/console.h>
#include <linux/io.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/property.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/reset.h>
#include <linux/serial_8250.h>
#include <linux/serial_core.h>
#include <linux/serial_reg.h>
#include <linux/slab.h>

#include "8250.h"

#define AIROHA_UART_XTAL_HZ		20000000U
#define AIROHA_UART_XYD_Y		65000U
#define AIROHA_UART_BRD			1U
#define AIROHA_UART_MIN_BAUD		110U
#define AIROHA_UART_MAX_BAUD		460800U
#define AIROHA_HSUART_MAX_BAUD		921600U
#define AIROHA_HSUART_XYD_Y		25000U
#define AIROHA_HSUART_XYD_Y_FAST	12500U
#define AIROHA_HSUART_FAST_BAUD		115200U
#define AIROHA_HSUART_SLOW_BAUD		9600U
#define AIROHA_HSUART_SLOW_BRD		100U
#define AIROHA_HSUART_CLK_DIV		2U
#define AIROHA_HSUART_CLK_DIV_REG	1U
#define AIROHA_HSUART_OVERSAMPLING	8U
#define AIROHA_UART_MISCC		9U
#define AIROHA_UART_MISCC_RTSHWFC	BIT(2)
#define AIROHA_UART_MISCC_CTSHWFC	BIT(3)
#define AIROHA_UART_MISCC_HWFC		(AIROHA_UART_MISCC_RTSHWFC | \
					 AIROHA_UART_MISCC_CTSHWFC)

static const unsigned int clock_div[] = { 10, 4, 2 };
static const unsigned int clock_div_reg[] = { 4, 2, 1 };

enum airoha_uart_access {
	AIROHA_UART_32BIT,
	AIROHA_UART_BYTE_LANE3,
	AIROHA_UART_BYTE_LANE3_XYD32,
};

struct airoha_uart_variant {
	unsigned int max_baud;
	unsigned int fifo_size;
	enum airoha_uart_access access;
	bool high_speed;
	bool fixed_baud;
};

/* EN751221/EN751627 UART1: byte registers in lane +3, boot baud fixed. */
static const struct airoha_uart_variant en75xx_legacy_uart1 = {
	.max_baud = 115200,
	.fifo_size = 1,
	.access = AIROHA_UART_BYTE_LANE3,
	.fixed_baud = true,
};

/* EN751221 UART2: 8-bit 8250 registers, but 32-bit fractional XYD. */
static const struct airoha_uart_variant en751221_uart2 = {
	.max_baud = 115200,
	.fifo_size = 1,
	.access = AIROHA_UART_BYTE_LANE3_XYD32,
};

static const struct airoha_uart_variant en7523_uart = {
	.max_baud = AIROHA_UART_MAX_BAUD,
	.fifo_size = 8,
};

static const struct airoha_uart_variant en7581_hsuart = {
	.max_baud = AIROHA_HSUART_MAX_BAUD,
	.fifo_size = 128,
	.high_speed = true,
};

struct airoha_uart_data {
	const struct airoha_uart_variant *variant;
	struct reset_control *rst;
	struct clk *clk;
	struct clk *bus_clk;
	int line;
};

/*
 * The UART baud rate is XTAL / XINCLKDR * XYD_X / XYD_Y / (samples * BRD).
 * The normal UART samples at 16x and the EN7581 HSUART samples at 8x.
 * The standard 8250 divisor must not overwrite the fractional divisor.
 * The register accesses and pin muxing are described by the device tree.
 */
static void airoha_uart_set_baud(struct uart_port *port, unsigned int baud,
				 unsigned int lcr,
				 const struct airoha_uart_variant *variant)
{
	unsigned int i, xyd_x = 0, xyd_y = AIROHA_UART_XYD_Y, clk_div_reg;
	unsigned int brd = AIROHA_UART_BRD;

	if (!baud)
		return;

	if (variant->high_speed) {
		/* Avoid 50-baud steps at low rates while keeping XYD_X below XYD_Y. */
		if (baud <= AIROHA_HSUART_SLOW_BAUD)
			brd = AIROHA_HSUART_SLOW_BRD;
		xyd_y = baud > AIROHA_HSUART_FAST_BAUD ?
			AIROHA_HSUART_XYD_Y_FAST : AIROHA_HSUART_XYD_Y;
		xyd_x = DIV_ROUND_CLOSEST_ULL((u64)baud * xyd_y * brd *
					    AIROHA_HSUART_OVERSAMPLING *
					    AIROHA_HSUART_CLK_DIV, AIROHA_UART_XTAL_HZ);
		clk_div_reg = AIROHA_HSUART_CLK_DIV_REG;
	} else {
		for (i = 0; i < ARRAY_SIZE(clock_div); i++) {
			xyd_x = DIV_ROUND_CLOSEST_ULL((u64)baud * xyd_y *
						    16 * clock_div[i], AIROHA_UART_XTAL_HZ);
			if (xyd_x < xyd_y)
				break;
		}

		if (i == ARRAY_SIZE(clock_div))
			i = ARRAY_SIZE(clock_div) - 1;
		clk_div_reg = clock_div_reg[i];
	}
	xyd_x = min(xyd_x, xyd_y - 1);

	serial_port_out(port, UART_LCR, lcr | UART_LCR_DLAB);
	serial_port_out(port, UART_DLL, brd & 0xff);
	serial_port_out(port, UART_DLM, brd >> 8);
	serial_port_out(port, UART_XINCLKDR, clk_div_reg);
	serial_port_out(port, UART_XYD, (xyd_x << 16) | xyd_y);
	serial_port_out(port, UART_LCR, lcr);
}

static void airoha_uart_set_divisor(struct uart_port *port, unsigned int baud,
				    unsigned int quot, unsigned int quot_frac)
{
	struct uart_8250_port *up = up_to_u8250p(port);
	struct airoha_uart_data *data = port->private_data;

	/* UART1 retains the bootloader's 115200-baud divider. */
	if (data->variant->fixed_baud)
		return;

	airoha_uart_set_baud(port, baud, up->lcr, data->variant);
}

static void airoha_uart_serial_out(struct uart_port *port, unsigned int offset,
				   u32 value)
{
	void __iomem *reg = port->membase + (offset << port->regshift);
	unsigned int miscc;

	/* Translate every MCR write, including console restore and startup. */
	if (offset == UART_MCR) {
		miscc = serial_port_in(port, AIROHA_UART_MISCC);
		miscc &= ~AIROHA_UART_MISCC_HWFC;
		if (value & UART_MCR_AFE)
			miscc |= AIROHA_UART_MISCC_HWFC;
		serial_port_out(port, AIROHA_UART_MISCC, miscc);
		value &= ~UART_MCR_AFE;
	}

	if (port->iotype == UPIO_MEM32BE)
		iowrite32be(value, reg);
	else
		writel(value, reg);
}

/* MIPS UART1 and EN751221 UART2 use the last byte lane of each slot. */
static u32 airoha_uart_byte_in(struct uart_port *port, unsigned int offset)
{
	return readb(port->membase + (offset << port->regshift) + 3);
}

static void airoha_uart_byte_out(struct uart_port *port,
				 unsigned int offset, u32 value)
{
	writeb(value, port->membase + (offset << port->regshift) + 3);
}

/* EN751221 UART2 mixes byte-wide 8250 registers with a 32-bit XYD. */
static void airoha_uart_mixed_out(struct uart_port *port,
				  unsigned int offset, u32 value)
{
	void __iomem *reg = port->membase + (offset << port->regshift);

	if (offset == UART_XYD) {
		if (port->iotype == UPIO_MEM32BE)
			iowrite32be(value, reg);
		else
			writel(value, reg);
		return;
	}

	airoha_uart_byte_out(port, offset, value);
}

static unsigned int airoha_uart_get_mctrl(struct uart_port *port)
{
	struct uart_8250_port *up = up_to_u8250p(port);
	unsigned int mctrl = TIOCM_CTS | TIOCM_DSR | TIOCM_CAR;

	/* The SDK exposes no modem status; CTS gating is handled in MISCC. */
	if (up->gpios)
		mctrl_gpio_get(up->gpios, &mctrl);

	return mctrl;
}

static void airoha_uart_set_termios(struct uart_port *port,
				    struct ktermios *termios,
				    const struct ktermios *old)
{
	struct uart_8250_port *up = up_to_u8250p(port);
	struct airoha_uart_data *data = port->private_data;
	unsigned int baud;

	/* The SDK uses the one-byte RX watermark on the 128-byte HSUART FIFO. */
	if (data->variant->high_speed)
		up->fcr &= ~UART_FCR_TRIGGER_MASK;

	if (!(up->capabilities & UART_CAP_AFE))
		termios->c_cflag &= ~CRTSCTS;

	port->status &= ~(UPSTAT_AUTOCTS | UPSTAT_AUTORTS);
	if (termios->c_cflag & CRTSCTS)
		port->status |= UPSTAT_AUTOCTS | UPSTAT_AUTORTS;

	if (data->variant->fixed_baud) {
		/* Keep tty settings consistent with the fixed 115200 baud hardware. */
		baud = 115200;
		termios->c_cflag |= CREAD;
		tty_termios_encode_baud_rate(termios, baud, baud);
	} else {
		baud = uart_get_baud_rate(port, termios, old,
					  AIROHA_UART_MIN_BAUD,
					  data->variant->max_baud);
		if (tty_termios_baud_rate(termios))
			tty_termios_encode_baud_rate(termios, baud, baud);
	}

	serial8250_do_set_termios(port, termios, old);
}

static int airoha_uart_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct airoha_uart_data *data;
	struct uart_8250_port up = {};
	struct resource res;
	int ret;

	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;
	data->variant = device_get_match_data(dev);

	ret = of_address_to_resource(dev->of_node, 0, &res);
	if (ret)
		return dev_err_probe(dev, ret, "failed to get UART registers\n");

	pm_runtime_enable(dev);
	ret = pm_runtime_resume_and_get(dev);
	if (ret < 0)
		goto err_pm_disable;

	up.port.dev = dev;
	up.port.private_data = data;
	up.port.mapbase = res.start;
	up.port.mapsize = resource_size(&res);
	up.port.flags = UPF_BOOT_AUTOCONF | UPF_FIXED_PORT |
			UPF_FIXED_TYPE | UPF_IOREMAP;
	up.port.type = PORT_AIROHA;
	spin_lock_init(&up.port.lock);

	ret = uart_read_and_validate_port_properties(&up.port);
	if (ret)
		goto err_pm_put;

	if (up.port.regshift != 2 ||
	    (up.port.iotype != UPIO_MEM32 && up.port.iotype != UPIO_MEM32BE) ||
	    up.port.mapsize < 0x30) {
		ret = dev_err_probe(dev, -EINVAL, "invalid UART register layout\n");
		goto err_pm_put;
	}

	data->bus_clk = devm_clk_get_optional_enabled(dev, "bus");
	if (IS_ERR(data->bus_clk)) {
		ret = dev_err_probe(dev, PTR_ERR(data->bus_clk),
				    "failed to enable UART bus clock\n");
		goto err_pm_put;
	}

	data->clk = devm_clk_get_optional_enabled(dev, data->bus_clk ? "core" : NULL);
	if (IS_ERR(data->clk)) {
		ret = dev_err_probe(dev, PTR_ERR(data->clk),
				    "failed to enable UART clock\n");
		goto err_pm_put;
	}

	/*
	 * XYD is sourced by the fixed 20 MHz crystal. Advertise an
	 * equivalent 16x clock so 8250 accepts the entire fractional baud range,
	 * including device trees with a legacy, pre-divided clock-frequency.
	 */
	up.port.uartclk = data->variant->fixed_baud ? 115200 * 16 :
			 data->variant->high_speed ? AIROHA_UART_XTAL_HZ :
			 AIROHA_UART_XTAL_HZ / 2;

	data->rst = devm_reset_control_get_optional_shared(dev, NULL);
	if (IS_ERR(data->rst)) {
		ret = dev_err_probe(dev, PTR_ERR(data->rst),
				    "failed to get UART reset\n");
		goto err_pm_put;
	}

	ret = reset_control_deassert(data->rst);
	if (ret)
		goto err_pm_put;

	up.port.set_divisor = airoha_uart_set_divisor;
	up.port.set_termios = airoha_uart_set_termios;
	switch (data->variant->access) {
	case AIROHA_UART_BYTE_LANE3:
		up.port.serial_in = airoha_uart_byte_in;
		up.port.serial_out = airoha_uart_byte_out;
		break;
	case AIROHA_UART_BYTE_LANE3_XYD32:
		up.port.serial_in = airoha_uart_byte_in;
		up.port.serial_out = airoha_uart_mixed_out;
		break;
	case AIROHA_UART_32BIT:
		up.port.serial_out = airoha_uart_serial_out;
		break;
	}
	up.port.get_mctrl = airoha_uart_get_mctrl;
	/* The SDK polls THRE before each byte, even on FIFO-equipped ports. */
	up.tx_loadsz = 1;
	up.bugs = UART_BUG_NOMSR;
	up.capabilities = UART_CAP_FIFO;
	if (!up.port.fifosize)
		up.port.fifosize = data->variant->fifo_size;
	if (device_property_read_bool(dev, "auto-flow-control"))
		up.capabilities |= UART_CAP_AFE;

	ret = serial8250_register_8250_port(&up);
	if (ret < 0)
		goto err_reset;

	data->line = ret;
	platform_set_drvdata(pdev, data);

	return 0;

err_reset:
	reset_control_assert(data->rst);
err_pm_put:
	pm_runtime_put_sync(dev);
err_pm_disable:
	pm_runtime_disable(dev);
	return ret;
}

static void airoha_uart_remove(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct airoha_uart_data *data = platform_get_drvdata(pdev);

	serial8250_unregister_port(data->line);
	reset_control_assert(data->rst);
	pm_runtime_put_sync(dev);
	pm_runtime_disable(dev);
}

#ifdef CONFIG_PM_SLEEP
static int airoha_uart_suspend(struct device *dev)
{
	struct airoha_uart_data *data = dev_get_drvdata(dev);

	serial8250_suspend_port(data->line);
	return 0;
}

static int airoha_uart_resume(struct device *dev)
{
	struct airoha_uart_data *data = dev_get_drvdata(dev);

	serial8250_resume_port(data->line);
	return 0;
}
#endif

static SIMPLE_DEV_PM_OPS(airoha_uart_pm_ops, airoha_uart_suspend,
			 airoha_uart_resume);

static const struct of_device_id airoha_uart_of_match[] = {
	{ .compatible = "airoha,en751221-uart", .data = &en75xx_legacy_uart1 },
	{ .compatible = "airoha,en751221-uart2", .data = &en751221_uart2 },
	{ .compatible = "airoha,en751627-uart", .data = &en75xx_legacy_uart1 },
	{ .compatible = "airoha,en7523-uart", .data = &en7523_uart },
	{ .compatible = "airoha,en7581-hsuart", .data = &en7581_hsuart },
	{ }
};
MODULE_DEVICE_TABLE(of, airoha_uart_of_match);

static struct platform_driver airoha_uart_driver = {
	.probe = airoha_uart_probe,
	.remove = airoha_uart_remove,
	.driver = {
		.name = "airoha-uart",
		.of_match_table = airoha_uart_of_match,
		.pm = &airoha_uart_pm_ops,
	},
};
module_platform_driver(airoha_uart_driver);

#if defined(CONFIG_SERIAL_8250_CONSOLE) && !defined(MODULE)
/* Early 8250's serial_putc() bypasses uart_port.serial_in/out callbacks.
 * Use our own console writer so the MIPS byte lane and THRE-only polling
 * are honoured. Keep earlycon non-destructive for the boot UART1.
 */
static u32 airoha_uart_early_in(struct uart_port *port, unsigned int offset)
{
	void __iomem *reg = port->membase + (offset << port->regshift);
	const struct airoha_uart_variant *variant = port->private_data;

	if (variant->access != AIROHA_UART_32BIT)
		return readb(reg + 3);
	if (port->iotype == UPIO_MEM32BE)
		return ioread32be(reg);
	return readl(reg);
}

static void airoha_uart_early_out(struct uart_port *port,
				  unsigned int offset, u32 value)
{
	void __iomem *reg = port->membase + (offset << port->regshift);
	const struct airoha_uart_variant *variant = port->private_data;

	if (variant->access != AIROHA_UART_32BIT &&
	    !(variant->access == AIROHA_UART_BYTE_LANE3_XYD32 &&
	      offset == UART_XYD)) {
		writeb(value, reg + 3);
		return;
	}
	if (port->iotype == UPIO_MEM32BE)
		iowrite32be(value, reg);
	else
		writel(value, reg);
}

static void airoha_uart_early_putc(struct uart_port *port, unsigned char ch)
{
	unsigned int timeout = 1000000;

	while (timeout--) {
		if (airoha_uart_early_in(port, UART_LSR) & UART_LSR_THRE) {
			airoha_uart_early_out(port, UART_TX, ch);
			return;
		}
		cpu_relax();
	}
}

static void airoha_uart_early_write(struct console *con,
				     const char *s, unsigned int count)
{
	struct earlycon_device *device = con->data;

	uart_console_write(&device->port, s, count, airoha_uart_early_putc);
}

static int __init
airoha_uart_early_setup_variant(struct earlycon_device *device,
				const char *options,
				const struct airoha_uart_variant *variant)
{
	struct uart_port *port = &device->port;

	if (!port->membase || port->regshift != 2 ||
	    (port->iotype != UPIO_MEM32 && port->iotype != UPIO_MEM32BE))
		return -EINVAL;

	if (device->baud && (device->baud < AIROHA_UART_MIN_BAUD ||
			     device->baud > variant->max_baud))
		return -EINVAL;

	port->private_data = (void *)variant;
	device->con->write = airoha_uart_early_write;

	/* The bootloader initialized UART1 already. Avoid touching its baud,
	 * IER, FCR and MCR when installing earlycon.
	 * The remaining variants are initialized by their normal 8250 probe.
	 */
	return 0;
}

static int __init airoha_uart1_early_setup(struct earlycon_device *device,
					   const char *options)
{
	return airoha_uart_early_setup_variant(device, options,
					      &en75xx_legacy_uart1);
}

static int __init airoha_uart2_early_setup(struct earlycon_device *device,
					   const char *options)
{
	return airoha_uart_early_setup_variant(device, options,
					      &en751221_uart2);
}

static int __init airoha_uart_early_setup(struct earlycon_device *device,
					  const char *options)
{
	return airoha_uart_early_setup_variant(device, options, &en7523_uart);
}

static int __init airoha_hsuart_early_setup(struct earlycon_device *device,
					    const char *options)
{
	return airoha_uart_early_setup_variant(device, options, &en7581_hsuart);
}

OF_EARLYCON_DECLARE(airoha_751221, "airoha,en751221-uart",
			    airoha_uart1_early_setup);
OF_EARLYCON_DECLARE(airoha_7512u2, "airoha,en751221-uart2",
			    airoha_uart2_early_setup);
OF_EARLYCON_DECLARE(airoha_751627, "airoha,en751627-uart",
			    airoha_uart1_early_setup);
OF_EARLYCON_DECLARE(airoha, "airoha,en7523-uart", airoha_uart_early_setup);
OF_EARLYCON_DECLARE(airoha_hsuart, "airoha,en7581-hsuart", airoha_hsuart_early_setup);
#endif

MODULE_IMPORT_NS("SERIAL_8250");
MODULE_AUTHOR("Benjamin Larsson <benjamin.larsson@genexis.eu>");
MODULE_AUTHOR("Matheus Sampaio Queiroga <srherobrine20@gmail.com>");
MODULE_DESCRIPTION("Airoha fractional baud rate 8250 UART driver");
MODULE_LICENSE("GPL");
