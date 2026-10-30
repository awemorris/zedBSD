/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The console of the Raspberry Pi 4, the Raspberry Pi 5 and the ODROID-C5:
 * /dev/console over the firmware's screen and the debug UART.
 */

#ifndef KERN_DRIVERS_RPI4_CONSOLE_H
#define KERN_DRIVERS_RPI4_CONSOLE_H

/* Publishes the console and the screen's /dev/graphics backend; call once, before /dev/console opens. */
void drv_rpi4_console_init(void);

/* Starts reading the serial port into the console; returns an errno. */
int drv_rpi4_console_start_input(void);

#endif
