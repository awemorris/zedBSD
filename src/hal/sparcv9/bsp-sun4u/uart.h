/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The sun4u board's serial port, a 16550, polled by the HAL for output.
 */

#ifndef KERN_HAL_SPARCV9_BSP_SUN4U_UART_H
#define KERN_HAL_SPARCV9_BSP_SUN4U_UART_H

#include <hal/types.h>

/*
 * Sets the serial port up for output.
 */
void
sun4u_uart_init(
	uint64_t pci_io_base,
	uint32_t serial_offset);

/*
 * Writes one character, a newline as a carriage return and a line feed.
 */
void
sun4u_uart_putc(
	int character);

#endif
