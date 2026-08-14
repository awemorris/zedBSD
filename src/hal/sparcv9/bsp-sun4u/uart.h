/* sun4u 16550 polling UART. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#ifndef ZEDBSD_HAL_SPARCV9_SUN4U_UART_H
#define ZEDBSD_HAL_SPARCV9_SUN4U_UART_H

void sun4u_uart_init(unsigned long long pci_io_base,
	unsigned long serial_offset);
void sun4u_uart_putc(int character);
void sun4u_uart_puts(const char *text);
int sun4u_uart_poll(void);
int sun4u_uart_getc(void);

#endif
