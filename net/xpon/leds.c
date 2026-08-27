// SPDX-License-Identifier: GPL-2.0-only

#include <linux/leds.h>

#include "internal.h"

static void xpon_led_set(struct led_classdev *led, enum led_brightness value)
{
	if (led)
		led_set_brightness(led, value);
}

void xpon_leds_update(struct xpon_device *xpon,
		      const struct xpon_state *state)
{
	unsigned long delay_on = 250;
	unsigned long delay_off = 250;

	if ((state->valid & XPON_STATE_F_LOS) && state->los) {
		xpon_led_set(xpon->fiber_led, LED_OFF);
		xpon_led_set(xpon->los_led, LED_FULL);
		xpon_led_set(xpon->pon_led, LED_OFF);
		return;
	}

	if (state->valid & XPON_STATE_F_SIGNAL)
		xpon_led_set(xpon->fiber_led,
			     state->signal_detect ? LED_FULL : LED_OFF);
	xpon_led_set(xpon->los_led, LED_OFF);

	switch (state->registration) {
	case XPON_REGISTRATION_OPERATIONAL:
		xpon_led_set(xpon->pon_led, LED_FULL);
		break;
	case XPON_REGISTRATION_DISCOVERY:
	case XPON_REGISTRATION_REGISTERING:
		if (xpon->pon_led)
			led_blink_set(xpon->pon_led, &delay_on, &delay_off);
		break;
	default:
		xpon_led_set(xpon->pon_led, LED_OFF);
		break;
	}
}
