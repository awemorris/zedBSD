/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The console of the Raspberry Pi 4 and the Raspberry Pi 5: /dev/console
 * over the firmware's screen and the debug UART.
 */

#ifndef KERN_DRIVERS_RPI_CONSOLE_H
#define KERN_DRIVERS_RPI_CONSOLE_H

/* The board the console runs on, which names its UART and its screen. */
enum drv_rpi_board {
	DRV_RPI_BOARD_PI4,
	DRV_RPI_BOARD_PI5
};

/* Publishes the console and the screen's /dev/graphics backend; call once, before /dev/console opens. */
void drv_rpi_console_init(enum drv_rpi_board board);

/* Starts reading the serial port into the console; returns an errno. */
int drv_rpi_console_start_input(void);

#endif
