// SPDX-License-Identifier: GPL-2.0-only

#include <linux/err.h>
#include <linux/leds.h>

#include "internal.h"

static void xpon_led_set(struct led_classdev *led, enum led_brightness value)
{
	if (led)
		led_set_brightness(led, value);
}

static struct led_classdev *
xpon_led_get_optional(struct xpon_device *xpon, char *name)
{
	struct led_classdev *led;

	led = devm_led_get(xpon->parent, name);
	if (IS_ERR(led) && PTR_ERR(led) == -ENOENT)
		return NULL;

	return led;
}

int xpon_leds_register(struct xpon_device *xpon)
{
	if (!xpon->pon_led) {
		xpon->pon_led = xpon_led_get_optional(xpon, "pon");
		if (IS_ERR(xpon->pon_led))
			return PTR_ERR(xpon->pon_led);
	}

	if (!xpon->los_led) {
		xpon->los_led = xpon_led_get_optional(xpon, "los");
		if (IS_ERR(xpon->los_led))
			return PTR_ERR(xpon->los_led);
	}

	if (!xpon->fiber_led) {
		xpon->fiber_led = xpon_led_get_optional(xpon, "fiber");
		if (IS_ERR(xpon->fiber_led))
			return PTR_ERR(xpon->fiber_led);
	}

	return 0;
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
