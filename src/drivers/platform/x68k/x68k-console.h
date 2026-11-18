/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The X68000 console: /dev/console over the HAL's text screen.
 */

#ifndef KERN_DRIVERS_X68K_CONSOLE_H
#define KERN_DRIVERS_X68K_CONSOLE_H

/*
 * Publishes the console output.
 */
void
drv_x68k_console_init(
	void);

#endif
