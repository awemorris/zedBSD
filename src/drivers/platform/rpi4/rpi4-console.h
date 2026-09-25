/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Raspberry Pi 4 console: /dev/console over the PL011 serial port.
 */

#ifndef KERN_DRIVERS_RPI4_CONSOLE_H
#define KERN_DRIVERS_RPI4_CONSOLE_H

/* Publishes the console output; call once, before /dev/console opens. */
void drv_rpi4_console_init(void);

/* Starts reading the serial port into the console; returns an errno. */
int drv_rpi4_console_start_input(void);

#endif
