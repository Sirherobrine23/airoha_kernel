// SPDX-License-Identifier: GPL-2.0-only

#include <linux/err.h>
#include <linux/leds.h>

#include "internal.h"

enum {
	XPON_LED_OFF,
	XPON_LED_ON,
	XPON_LED_BLINK,
};

static void xpon_led_set(struct led_classdev *led, enum led_brightness value)
{
	if (led)
		led_set_brightness(led, value);
}

/*
 * While a software blink runs, led_set_brightness() only changes the
 * brightness of the "on" phase, so a steady state requested afterwards
 * never shows.  Stop the blink first and wait for that to take effect.
 * Re-requesting the same blink would also restart its phase on every event.
 */
static void xpon_led_set_mode(struct led_classdev *led, u8 *cur, u8 mode,
			      unsigned long period_ms)
{
	unsigned long delay_on = period_ms / 2;
	unsigned long delay_off = period_ms / 2;

	if (!led || *cur == mode)
		return;

	if (*cur == XPON_LED_BLINK) {
		led_set_brightness(led, LED_OFF);
		flush_work(&led->set_brightness_work);
	}

	switch (mode) {
	case XPON_LED_ON:
		led_set_brightness(led, LED_FULL);
		break;
	case XPON_LED_BLINK:
		led_blink_set(led, &delay_on, &delay_off);
		break;
	default:
		led_set_brightness(led, LED_OFF);
		break;
	}
	*cur = mode;
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
	if (!IS_REACHABLE(CONFIG_LEDS_CLASS))
		return 0;

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
	if (!IS_REACHABLE(CONFIG_LEDS_CLASS))
		return;

	/*
	 * Some receivers leave LOS deasserted with no fibre at all and only
	 * drop signal detect.  Without a usable optical signal show LOS, or
	 * the PON and LOS LEDs alternate with the raw LOS bit.
	 */
	if (((state->valid & XPON_STATE_F_LOS) && state->los) ||
	    ((state->valid & XPON_STATE_F_SIGNAL) && !state->signal_detect)) {
		xpon_led_set(xpon->fiber_led, LED_OFF);
		/* Slow blink, like the vendor firmware of these ONUs. */
		xpon_led_set_mode(xpon->los_led, &xpon->los_led_mode,
				  XPON_LED_BLINK, 1000);
		xpon_led_set_mode(xpon->pon_led, &xpon->pon_led_mode,
				  XPON_LED_OFF, 0);
		return;
	}

	if (state->valid & XPON_STATE_F_SIGNAL)
		xpon_led_set(xpon->fiber_led,
			     state->signal_detect ? LED_FULL : LED_OFF);
	xpon_led_set_mode(xpon->los_led, &xpon->los_led_mode, XPON_LED_OFF, 0);

	switch (state->registration) {
	case XPON_REGISTRATION_OPERATIONAL:
		xpon_led_set_mode(xpon->pon_led, &xpon->pon_led_mode,
				  XPON_LED_ON, 0);
		break;
	case XPON_REGISTRATION_DISCOVERY:
	case XPON_REGISTRATION_REGISTERING:
		xpon_led_set_mode(xpon->pon_led, &xpon->pon_led_mode,
				  XPON_LED_BLINK, 500);
		break;
	default:
		xpon_led_set_mode(xpon->pon_led, &xpon->pon_led_mode,
				  XPON_LED_OFF, 0);
		break;
	}
}
