/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The sun4u console: /dev/console over the HAL's screen and serial port.
 */

#ifndef KERN_DRIVERS_SUN4U_CONSOLE_H
#define KERN_DRIVERS_SUN4U_CONSOLE_H

/*
 * Publishes the console output.
 */
void
drv_sun4u_console_init(
	void);

/*
 * Starts reading the serial port into the console.
 */
int
drv_sun4u_console_start_input(
	void);

#endif
