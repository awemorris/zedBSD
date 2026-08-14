/* sun4u 16550 polling UART over PCI I/O physical bypass. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#include "../asi.h"
#include "uart.h"

#define UART_THR 0U
#define UART_DLL 0U
#define UART_DLM 1U
#define UART_FCR 2U
#define UART_LCR 3U
#define UART_MCR 4U
#define UART_LSR 5U
#define UART_LSR_DR 0x01U
#define UART_LSR_THRE 0x20U

static unsigned long long uart_base;

static void
uart_write(unsigned int port, unsigned char value)
{
	sparcv9_phys_write8(uart_base + port, value);
}

void
sun4u_uart_init(unsigned long long pci_io_base, unsigned long serial_offset)
{
	uart_base = pci_io_base + serial_offset;
	uart_write(UART_LCR, 0x80);
	uart_write(UART_DLL, 1);
	uart_write(UART_DLM, 0);
	uart_write(UART_LCR, 0x03);
	uart_write(UART_FCR, 0x07);
	uart_write(UART_MCR, 0x03);
}

void
sun4u_uart_putc(int character)
{
	unsigned long timeout = 1000000UL;

	while ((sparcv9_phys_read8(uart_base + UART_LSR) & UART_LSR_THRE) == 0)
		if (--timeout == 0)
			return;
	if (character == '\n')
		sun4u_uart_putc('\r');
	uart_write(UART_THR, (unsigned char)character);
}

void
sun4u_uart_puts(const char *text)
{
	while (*text != '\0')
		sun4u_uart_putc(*text++);
}

int
sun4u_uart_poll(void)
{
	return (sparcv9_phys_read8(uart_base + UART_LSR) & UART_LSR_DR) != 0;
}

int
sun4u_uart_getc(void)
{
	while (!sun4u_uart_poll())
		__asm__ volatile("nop");
	return sparcv9_phys_read8(uart_base + UART_THR);
}
