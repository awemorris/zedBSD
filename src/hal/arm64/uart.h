/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The debug UART (a PL011) the early console writes to.
 *
 * Each board has its own uart.c: the UART is at a different address and
 * is set up differently on each.
 */

#ifndef KERN_HAL_ARM64_BSP_RPI_COMMON_UART_H
#define KERN_HAL_ARM64_BSP_RPI_COMMON_UART_H

void rpi_uart_init(void);
void rpi_uart_putc(int c);
int rpi_uart_getc(void);
int rpi_uart_poll(void);
void rpi_uart_enable_rx_irq(void);
void rpi_uart_clear_rx_irq(void);

#endif
