/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The sun4u board's serial port, a 16550 on the PCI I/O space, reached
 * through the physical bypass.  The HAL only writes to it; the kernel's
 * console reads it.
 */

#include <hal/hal.h>
#include "../asi.h"
#include "uart.h"

/*
 * The 16550's registers, by their offset, and the bits the HAL uses.
 *
 * Values referenced: National Semiconductor PC16550D data sheet (the
 * register offsets, the divisor latch access bit of the line control
 * register, the 8N1 format, the FIFO enable and reset bits, and the
 * transmitter-empty bit of the line status register).
 */
#define UART_THR		0U
#define UART_DLL		0U
#define UART_DLM		1U
#define UART_FCR		2U
#define UART_LCR		3U
#define UART_MCR		4U
#define UART_LSR		5U
#define UART_LCR_DLAB		0x80U
#define UART_LCR_8N1		0x03U
#define UART_FCR_ENABLE_RESET	0x07U
#define UART_MCR_DTR_RTS	0x03U
#define UART_LSR_THRE		0x20U

/* How long a write waits for room before the character is dropped. */
#define UART_SPIN_LIMIT		1000000UL

/*
 * The physical address of the port's first register.
 *
 * Set once by sun4u_uart_init() before the first character; zero until
 * then, when nothing is written.
 */
static uint64_t uart_base;

static void uart_write(unsigned offset, uint8_t value);

/*
 * Sets the serial port up for output.
 */
void
sun4u_uart_init(
	uint64_t pci_io_base,
	uint32_t serial_offset)
{
	/* The port lies in the PCI I/O space at the offset the firmware gave. */
	uart_base = pci_io_base + serial_offset;

	/* Sets the fastest divisor, 8N1, the FIFOs on, and the modem lines up. */
	uart_write(UART_LCR, UART_LCR_DLAB);
	uart_write(UART_DLL, 1);
	uart_write(UART_DLM, 0);
	uart_write(UART_LCR, UART_LCR_8N1);
	uart_write(UART_FCR, UART_FCR_ENABLE_RESET);
	uart_write(UART_MCR, UART_MCR_DTR_RTS);
}

/*
 * Writes one character, a newline as a carriage return and a line feed.
 */
void
sun4u_uart_putc(
	int character)
{
	unsigned long spins;
	uint8_t status;

	/* Nothing is written before the port is set up. */
	if (uart_base == 0)
		return;

	/* A newline is preceded by a carriage return. */
	if (character == '\n')
		sun4u_uart_putc('\r');

	/* Waits for room, dropping the character on a port that never drains. */
	for (spins = 0; spins < UART_SPIN_LIMIT; spins++) {
		status = sparcv9_phys_read8(uart_base + UART_LSR);
		if ((status & UART_LSR_THRE) != 0)
			break;
	}
	if (spins == UART_SPIN_LIMIT)
		return;

	/* Hands the character to the transmitter. */
	uart_write(UART_THR, (uint8_t)character);
}

/* Writes one register of the port. */
static void
uart_write(
	unsigned offset,
	uint8_t value)
{
	/* Stores the byte past the caches. */
	sparcv9_phys_write8(uart_base + offset, value);
}
