/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The virt board's PL011 UART as the HAL's console: written by polling,
 * and read by polling when the kernel asks for a character.
 *
 * Values referenced: ARM DDI 0183 (PrimeCell UART PL011 Technical
 * Reference Manual), the registers UARTDR at 0x000, UARTFR at 0x018 with
 * TXFF in bit 5 and RXFE in bit 4, UARTCR at 0x030 with UARTEN, TXE and
 * RXE, and UARTIMSC at 0x038.
 */

#include <hal/hal.h>
#include "../pte.h"
#include "../space.h"
#include "virt.h"

/* The registers and their bits. */
#define PL011_DR		0x000U
#define PL011_FR		0x018U
#define PL011_CR		0x030U
#define PL011_IMSC		0x038U
#define PL011_FR_RXFE		0x10U
#define PL011_FR_TXFF		0x20U
#define PL011_CR_UARTEN		0x001U
#define PL011_CR_TXE		0x100U
#define PL011_CR_RXE		0x200U

/* How many times a full transmit FIFO is looked at before a character is dropped. */
#define PL011_TX_POLLS		100000U

/*
 * The UART's registers through the device window, or NULL before
 * virt_uart_init() mapped them.  Set on the boot processor, first at the
 * board's known address and again if the device tree names another.
 */
static volatile uint8_t *uart_registers;

/*
 * Maps the UART's registers and turns its transmitter and receiver on,
 * with its interrupts masked: the console polls it.
 */
int
virt_uart_init(
	uint64_t physical)
{
	void *registers;
	int error;

	/* Maps the registers into the device window. */
	error = armv7_space_map_device_window(physical, VIRT_UART_BYTES, ARMV7_PTE_ATTR_DEVICE, &registers);
	if (error != HAL_OK)
		return error;

	/* Masks its interrupts and enables both directions. */
	hal_mmio_write32((volatile uint8_t *)registers + PL011_IMSC, 0);
	hal_mmio_write32((volatile uint8_t *)registers + PL011_CR, PL011_CR_UARTEN | PL011_CR_TXE | PL011_CR_RXE);

	/* Succeeded: the console writes here from now on. */
	uart_registers = registers;
	return HAL_OK;
}

/*
 * Writes one character, turning a newline into a carriage return and a
 * newline for the terminal on the other end.
 */
void
virt_uart_putc(
	int character)
{
	uint32_t flags;
	uint32_t polls;

	/* Nothing to write to before the UART is mapped. */
	if (uart_registers == NULL)
		return;

	/* A newline goes out after a carriage return. */
	if (character == '\n')
		virt_uart_putc('\r');

	/* Waits for room in the transmit FIFO, a bounded time. */
	for (polls = 0; polls < PL011_TX_POLLS; polls++) {
		flags = hal_mmio_read32(uart_registers + PL011_FR);
		if ((flags & PL011_FR_TXFF) == 0)
			break;
	}

	/* Writes the character, or drops it when the FIFO never drained. */
	if (polls < PL011_TX_POLLS)
		hal_mmio_write32(uart_registers + PL011_DR, (uint32_t)(unsigned char)character);
}

/*
 * Reads one character if one has arrived; reports -1 when none has.
 */
int
virt_uart_getc(
	void)
{
	uint32_t flags;
	uint32_t data;

	/* Nothing to read before the UART is mapped. */
	if (uart_registers == NULL)
		return -1;

	/* An empty receive FIFO has nothing. */
	flags = hal_mmio_read32(uart_registers + PL011_FR);
	if ((flags & PL011_FR_RXFE) != 0)
		return -1;

	/* Succeeded: the character, without its error bits. */
	data = hal_mmio_read32(uart_registers + PL011_DR);
	return (int)(data & 0xffU);
}
