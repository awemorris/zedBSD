/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The boot stages' LED of the ODROID-C5: none.  The board's LED is driven
 * by the system's own pin logic, so the HAL counts its stages on the
 * debug UART alone.
 */

#include <hal/hal.h>
#include "../bsp-rpi-common/led.h"

/*
 * Sets up the boot stages' LED: there is none to set up.
 */
void
rpi_led_init(
	void)
{
	/* Succeeded: nothing to drive. */
	return;
}

/*
 * Turns the boot stages' LED on or off: there is none.
 */
void
rpi_led_set(
	int on)
{
	/* No LED shows the level. */
	(void)on;
}
