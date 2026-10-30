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

#ifndef KERN_HAL_ARM64_UART_H
#define KERN_HAL_ARM64_UART_H

void arm64_uart_init(void);
void arm64_uart_putc(int c);
int arm64_uart_getc(void);
int arm64_uart_poll(void);
void arm64_uart_enable_rx_irq(void);
void arm64_uart_clear_rx_irq(void);

#endif
