/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The ZYBO's UART1, a Cadence UART, as the HAL's console: written and
 * read by polling.
 *
 * On the board U-Boot SPL has set the UART up for 115200 baud from its
 * 100 MHz reference, and the HAL keeps that.  The emulator starts it with
 * the transmitter disabled, where a write goes nowhere, so a UART whose
 * transmitter is off is set up here with the values SPL uses.
 *
 * Values referenced: Linux v6.12 drivers/tty/serial/xilinx_uartps.c, the
 * register offsets CDNS_UART_CR, MR, IDR, BAUDGEN, SR, FIFO and BAUDDIV,
 * the control bits TXRST, RXRST, RX_EN, TX_EN, TX_DIS and STOPBRK, the
 * mode for 8 data bits, no parity, 1 stop bit and the normal channel
 * (MR_PARITY_NONE alone), and the status bits RXEMPTY and TXFULL; the
 * divisors BAUDGEN 0xae and BAUDDIV 4, which the board's SPL left
 * (plan/ws286/design.md section 4.8).
 */

#include <hal/hal.h>
#include "../defs.h"
#include "../pte.h"
#include "../space.h"
#include "zybo.h"

/* The registers. */
#define UART_CR			0x00U
#define UART_MR			0x04U
#define UART_IDR		0x0cU
#define UART_BAUDGEN		0x18U
#define UART_SR			0x2cU
#define UART_FIFO		0x30U
#define UART_BAUDDIV		0x34U

/* The control register's bits. */
#define UART_CR_RXRST		0x001U
#define UART_CR_TXRST		0x002U
#define UART_CR_RX_EN		0x004U
#define UART_CR_TX_EN		0x010U
#define UART_CR_TX_DIS		0x020U
#define UART_CR_STOPBRK		0x100U

/* The mode: 8 data bits, no parity, one stop bit, the normal channel. */
#define UART_MR_8N1		0x020U

/* Every interrupt the UART has, for the disable register. */
#define UART_IXR_ALL		0x3fffU

/* The status register's bits. */
#define UART_SR_RXEMPTY		0x002U
#define UART_SR_TXFULL		0x010U

/* 115200 baud from 100 MHz: 100 MHz / (0xae * (4 + 1)). */
#define UART_BAUDGEN_115200	0xaeU
#define UART_BAUDDIV_115200	4U

/* How many times a busy register is looked at before the UART is given up on. */
#define UART_POLLS		100000U

/*
 * UART1's registers in the device window, or NULL before zybo_uart_init().
 * Set once on the boot processor.
 */
static volatile uint8_t *uart_registers;

static void start_transmitter(volatile uint8_t *registers);

/*
 * Maps UART1 and sets it up for the console if its transmitter is off.
 */
int
zybo_uart_init(
	void)
{
	void *mapped;
	uint32_t control;
	int error;

	/* Maps the registers. */
	error = armv7_space_map_device_window(ZYBO_UART1_PHYS, ZYBO_UART_BYTES, ARMV7_PTE_ATTR_DEVICE, &mapped);
	if (error != HAL_OK)
		return error;

	/* A transmitter the loader left off is set up as SPL sets it. */
	control = hal_mmio_read32((volatile uint8_t *)mapped + UART_CR);
	if ((control & UART_CR_TX_EN) == 0 || (control & UART_CR_TX_DIS) != 0)
		start_transmitter(mapped);

	/* Succeeded: the console writes here from now on. */
	uart_registers = mapped;
	return HAL_OK;
}

/*
 * Writes one character, turning a newline into a carriage return and a
 * newline for the terminal on the other end.
 */
void
zybo_uart_putc(
	int character)
{
	uint32_t status;
	uint32_t polls;

	/* Nothing to write to before the UART is mapped. */
	if (uart_registers == NULL)
		return;

	/* A newline goes out after a carriage return. */
	if (character == '\n')
		zybo_uart_putc('\r');

	/* Waits for room in the transmit FIFO, a bounded time. */
	for (polls = 0; polls < UART_POLLS; polls++) {
		status = hal_mmio_read32(uart_registers + UART_SR);
		if ((status & UART_SR_TXFULL) == 0)
			break;
	}

	/* Writes the character, or drops it when the FIFO never drained. */
	if (polls < UART_POLLS)
		hal_mmio_write32(uart_registers + UART_FIFO, (uint32_t)(unsigned char)character);
}

/*
 * Reads one character if one has arrived; reports -1 when none has.
 */
int
zybo_uart_getc(
	void)
{
	uint32_t status;
	uint32_t data;

	/* Nothing to read before the UART is mapped. */
	if (uart_registers == NULL)
		return -1;

	/* An empty receive FIFO has nothing. */
	status = hal_mmio_read32(uart_registers + UART_SR);
	if ((status & UART_SR_RXEMPTY) != 0)
		return -1;

	/* Succeeded: the character. */
	data = hal_mmio_read32(uart_registers + UART_FIFO);
	return (int)(data & 0xffU);
}

/*
 * Sets the UART up for 115200 baud, 8N1, with its interrupts off, and
 * turns both directions on.
 */
static void
start_transmitter(
	volatile uint8_t *registers)
{
	uint32_t control;
	uint32_t polls;

	/* No interrupts: the console polls. */
	hal_mmio_write32(registers + UART_IDR, UART_IXR_ALL);

	/* The line's format and speed. */
	hal_mmio_write32(registers + UART_MR, UART_MR_8N1);
	hal_mmio_write32(registers + UART_BAUDGEN, UART_BAUDGEN_115200);
	hal_mmio_write32(registers + UART_BAUDDIV, UART_BAUDDIV_115200);

	/* Resets both directions and waits, a bounded time, for the reset to end. */
	hal_mmio_write32(registers + UART_CR, UART_CR_TXRST | UART_CR_RXRST);
	for (polls = 0; polls < UART_POLLS; polls++) {
		control = hal_mmio_read32(registers + UART_CR);
		if ((control & (UART_CR_TXRST | UART_CR_RXRST)) == 0)
			break;
	}

	/* Turns both directions on, with no break being sent. */
	hal_mmio_write32(registers + UART_CR, UART_CR_TX_EN | UART_CR_RX_EN | UART_CR_STOPBRK);
}
