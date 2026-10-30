/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The console of an S905X5M board: /dev/console over the boot loader's
 * screen and the board's debug UART.
 */

#ifndef KERN_DRIVERS_S905X5M_CONSOLE_H
#define KERN_DRIVERS_S905X5M_CONSOLE_H

#include <stdint.h>

/*
 * Publishes the console over the debug UART at uart_physical and the screen
 * the platform hands over as framebuffer_name, and the screen's
 * /dev/graphics backend; call once, before /dev/console opens.
 */
void drv_s905x5m_console_init(uint64_t uart_physical, const char *framebuffer_name);

/* Starts reading the serial port into the console; returns an errno. */
int drv_s905x5m_console_start_input(void);

#endif
