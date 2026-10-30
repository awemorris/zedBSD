/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The ODROID-C5's debug UART: uart_B of the S905X5M, on the board's UART
 * connector, which the bootloader has already started at the console's
 * rate from the 24 MHz crystal.
 */

#include <hal/hal.h>
#include "../defs.h"
#include "board.h"
#include "../bsp-rpi-common/uart.h"

/*
 * The UART's registers: the transmit and receive FIFOs, the control, the
 * status, the interrupt thresholds, and the baud rate.
 *
 * Values referenced: Linux v7.3 drivers/tty/serial/meson_uart.c (the
 * register offsets).
 */
#define UART_WFIFO	0x00U
#define UART_RFIFO	0x04U
#define UART_CONTROL	0x08U
#define UART_STATUS	0x0cU
#define UART_MISC	0x10U
#define UART_REG5	0x14U

/*
 * The control register's bits: the transmitter and the receiver, the
 * resets of both FIFOs and of the error flags, and the receive interrupt.
 *
 * Values referenced: Linux v7.3 drivers/tty/serial/meson_uart.c (the
 * control register's bits).
 */
#define UART_CONTROL_TX_EN	(1U << 12)
#define UART_CONTROL_RX_EN	(1U << 13)
#define UART_CONTROL_TWO_WIRE	(1U << 15)
#define UART_CONTROL_TX_RST	(1U << 22)
#define UART_CONTROL_RX_RST	(1U << 23)
#define UART_CONTROL_CLEAR_ERR	(1U << 24)
#define UART_CONTROL_RX_INT_EN	(1U << 27)
#define UART_CONTROL_TX_INT_EN	(1U << 28)

/*
 * The status register's bits: a parity or framing error, an overrun of
 * the transmit FIFO, an empty receive FIFO and a full transmit FIFO.
 *
 * Values referenced: Linux v7.3 drivers/tty/serial/meson_uart.c (the
 * status register's bits).
 */
#define UART_STATUS_PARITY_ERR	(1U << 16)
#define UART_STATUS_FRAME_ERR	(1U << 17)
#define UART_STATUS_TX_WERR	(1U << 18)
#define UART_STATUS_RX_EMPTY	(1U << 20)
#define UART_STATUS_TX_FULL	(1U << 21)
#define UART_STATUS_ERRORS	(UART_STATUS_PARITY_ERR | UART_STATUS_FRAME_ERR | UART_STATUS_TX_WERR)

/*
 * The baud register: a divisor of the crystal (24 MHz) halved, and the
 * bits that select the crystal, its halving and the divisor.
 *
 * Values referenced: Linux v7.3 drivers/tty/serial/meson_uart.c (the
 * REG5 bits and the divisor's computation for a part with the halved
 * crystal) and Hardkernel Linux odroids7d-6.12.y common_drivers
 * arch/arm64/boot/dts/amlogic/meson-s7d.dtsi (uart_B's xtal_tick_en).
 */
#define UART_REG5_DIVISOR_MASK	0x7fffffU
#define UART_REG5_USE_DIVISOR	(1U << 23)
#define UART_REG5_XTAL		(1U << 24)
#define UART_REG5_XTAL_DIV2	(1U << 27)
#define UART_XTAL_HZ		24000000U
#define UART_BAUD		115200U

/* The receive interrupt fires on every byte: a threshold of one. */
#define UART_MISC_RECV_ONE	1U

/* The debug UART through the direct map; constant. */
static volatile uint8_t *const uart = (volatile uint8_t *)(ARM64_DIRECT_BASE + S905X5M_DEBUG_UART_BASE);

/*
 * Starts the debug UART.
 *
 * The bootloader's line is kept as it is (its rate and framing) with the
 * interrupts masked.  A stopped UART is started at 115200 baud, 8 bits, no
 * parity, two wires.
 */
void
rpi_uart_init(
	void)
{
	uint32_t control;
	uint32_t divisor;

	/* Keeps a line the bootloader started, with interrupts masked. */
	control = hal_mmio_read32(uart + UART_CONTROL);
	if ((control & UART_CONTROL_TX_EN) != 0) {
		control &= ~(UART_CONTROL_RX_INT_EN | UART_CONTROL_TX_INT_EN);
		control |= UART_CONTROL_RX_EN;
		hal_mmio_write32(uart + UART_CONTROL, control);
		return;
	}

	/* Programs the rate from the halved crystal. */
	divisor = ((UART_XTAL_HZ / 2U) + (UART_BAUD / 2U)) / UART_BAUD - 1U;
	hal_mmio_write32(uart + UART_REG5,
			 (divisor & UART_REG5_DIVISOR_MASK) |
			 UART_REG5_USE_DIVISOR |
			 UART_REG5_XTAL |
			 UART_REG5_XTAL_DIV2);

	/* Resets both FIFOs and the error flags, then lets them run. */
	control = UART_CONTROL_TWO_WIRE;
	hal_mmio_write32(uart + UART_CONTROL,
			 control | UART_CONTROL_TX_RST | UART_CONTROL_RX_RST | UART_CONTROL_CLEAR_ERR);
	hal_mmio_write32(uart + UART_CONTROL, control);

	/* Starts the transmitter and the receiver, 8 bits, one stop bit, no parity. */
	hal_mmio_write32(uart + UART_CONTROL, control | UART_CONTROL_TX_EN | UART_CONTROL_RX_EN);
}

/*
 * Writes one byte, a newline as a carriage return and a newline.
 */
void
rpi_uart_putc(
	int c)
{
	uint32_t status;

	/* A terminal needs the carriage return to start the next line at its left. */
	if (c == '\n')
		rpi_uart_putc('\r');

	/* Waits for room in the transmit FIFO. */
	for (;;) {
		status = hal_mmio_read32(uart + UART_STATUS);
		if ((status & UART_STATUS_TX_FULL) == 0)
			break;
	}

	/* Queues the byte. */
	hal_mmio_write32(uart + UART_WFIFO, (uint32_t)c & 0xffU);
}

/*
 * Reports whether a received byte is waiting.
 */
int
rpi_uart_poll(
	void)
{
	uint32_t status;
	uint32_t control;

	/* A parity or framing error stops the receiver until it is cleared. */
	status = hal_mmio_read32(uart + UART_STATUS);
	if ((status & UART_STATUS_ERRORS) != 0) {
		control = hal_mmio_read32(uart + UART_CONTROL);
		hal_mmio_write32(uart + UART_CONTROL, control | UART_CONTROL_CLEAR_ERR);
		hal_mmio_write32(uart + UART_CONTROL, control);
	}

	/* The receive FIFO is empty when its flag is set. */
	if ((status & UART_STATUS_RX_EMPTY) != 0)
		return 0;

	/* Succeeded: a byte is waiting. */
	return 1;
}

/*
 * Reads one received byte, waiting for it.
 */
int
rpi_uart_getc(
	void)
{
	uint32_t data;
	int ready;

	/* Waits until a byte is waiting. */
	for (;;) {
		ready = rpi_uart_poll();
		if (ready)
			break;
	}

	/* Succeeded: the byte. */
	data = hal_mmio_read32(uart + UART_RFIFO);
	return (int)(data & 0xffU);
}

/*
 * Lets the received bytes raise the UART's interrupt.
 */
void
rpi_uart_enable_rx_irq(
	void)
{
	uint32_t control;

	/* One byte in the receive FIFO raises the interrupt; none for the transmitter. */
	hal_mmio_write32(uart + UART_MISC, UART_MISC_RECV_ONE);

	/* Unmasks the receive interrupt. */
	control = hal_mmio_read32(uart + UART_CONTROL);
	control |= UART_CONTROL_RX_INT_EN;
	control &= ~UART_CONTROL_TX_INT_EN;
	hal_mmio_write32(uart + UART_CONTROL, control);
}

/*
 * Ends the UART's receive interrupt.
 *
 * The UART raises its interrupt while the receive FIFO holds at least the
 * threshold; reading the bytes ends it, so there is nothing to clear.
 */
void
rpi_uart_clear_rx_irq(
	void)
{
	/* Succeeded: the interrupt ends with the FIFO drained. */
	return;
}
