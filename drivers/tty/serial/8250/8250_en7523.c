// SPDX-License-Identifier: GPL-2.0+
/*
 * Airoha EN7523 driver.
 *
 * Copyright (c) 2022 Genexis Sweden AB
 * Author: Benjamin Larsson <benjamin.larsson@genexis.eu>
 */
#include <linux/clk.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of_irq.h>
#include <linux/of_platform.h>
#include <linux/pinctrl/consumer.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/serial_8250.h>
#include <linux/serial_reg.h>
#include <linux/console.h>
#include <linux/dma-mapping.h>
#include <linux/tty.h>
#include <linux/tty_flip.h>

#include "8250.h"


/* The Airoha UART is 16550-compatible except for the baud rate calculation.
 *
 * crystal_clock = 20 MHz
 * xindiv_clock = crystal_clock / clock_div
 * (x/y) = XYD, 32 bit register with 16 bits of x and and then 16 bits of y
 * clock_div = XINCLK_DIVCNT (default set to 10 (0x4)),
 *           - 3 bit register [ 1, 2, 4, 8, 10, 12, 16, 20 ]
 *
 * baud_rate = ((xindiv_clock) * (x/y)) / ([BRDH,BRDL] * 16)
 *
 * XYD_y seems to need to be larger then XYD_x for things to work.
 * Setting [BRDH,BRDL] to [0,1] and XYD_y to 65000 give even values
 * for usual baud rates.
 *
 * Selecting divider needs to fulfill
 * 1.8432 MHz <= xindiv_clk <= APB clock / 2
 * The clocks are unknown but a divider of value 1 did not work.
 *
 * Optimally the XYD, BRD and XINCLK_DIVCNT registers could be searched to
 * find values that gives the least error for every baud rate. But searching
 * the space takes time and in practise only a few rates are of interest.
 * With some value combinations not working a tested subset is used giving
 * a usable range from 110 to 460800 baud.
 */

#define CLOCK_DIV_TAB_ELEMS 3
#define XYD_Y 65000
#define XINDIV_CLOCK 20000000
#define UART_BRDL_20M 0x01
#define UART_BRDH_20M 0x00

static int clock_div_tab[] = { 10, 4, 2};
static int clock_div_reg[] = {  4, 2, 1};


int en7523_set_uart_baud_rate (struct uart_port *port, unsigned int baud)
{
	struct uart_8250_port *up = up_to_u8250p(port);
	unsigned int xyd_x, nom, denom;
	int i;

	/* set DLAB to access the baud rate divider registers (BRDH, BRDL) */
	serial_port_out(port, UART_LCR, up->lcr | UART_LCR_DLAB);

	/* set baud rate calculation defaults */

	/* set BRDIV ([BRDH,BRDL]) to 1 */
	serial_port_out(port, UART_BRDL, UART_BRDL_20M);
	serial_port_out(port, UART_BRDH, UART_BRDH_20M);

	/* calculate XYD_x and XINCLKDR register */

	for (i = 0 ; i < CLOCK_DIV_TAB_ELEMS ; i++) {
		denom = (XINDIV_CLOCK/40) / clock_div_tab[i];
		nom = (baud * (XYD_Y/40));
		xyd_x = ((nom/denom) << 4);
		if (xyd_x < XYD_Y) break;
	}
	if (i >= CLOCK_DIV_TAB_ELEMS)
		i = CLOCK_DIV_TAB_ELEMS - 1;

	serial_port_out(port, UART_XINCLKDR, clock_div_reg[i]);
	serial_port_out(port, UART_XYD, (xyd_x<<16) | XYD_Y);

	/* unset DLAB */
	serial_port_out(port, UART_LCR, up->lcr);

	return 0;
}

EXPORT_SYMBOL_GPL(en7523_set_uart_baud_rate);
