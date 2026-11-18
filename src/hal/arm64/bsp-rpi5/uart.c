/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Raspberry Pi 5's debug UART: the PL011 of the three-pin connector
 * between the HDMI ports (uart10).
 */

#include <hal/hal.h>
#include "../defs.h"
#include "board.h"
#include "../uart.h"

/* The PL011's registers. */
#define UART_DR 0x00
#define UART_FR 0x18
#define UART_IBRD 0x24
#define UART_FBRD 0x28
#define UART_LCRH 0x2c
#define UART_CR 0x30
#define UART_IMSC 0x38
#define UART_ICR 0x44

/* The flag register's receive-empty and transmit-full bits. */
#define UART_FR_RXFE (1U << 4)
#define UART_FR_TXFF (1U << 5)

/* The receive and receive-timeout interrupts. */
#define UART_IRQ_RX  (1U << 4)
#define UART_IRQ_RT  (1U << 6)

/* The control register's bits: the UART, its transmitter and its receiver. */
#define UART_CR_UARTEN (1U << 0)
#define UART_CR_TXE (1U << 8)
#define UART_CR_RXE (1U << 9)

/* The line control: 8 bits (WLEN 3) with the FIFOs on. */
#define UART_LCRH_8N1_FIFO ((3U << 5) | (1U << 4))

/* Every interrupt's clear bit. */
#define UART_ICR_ALL 0x7ffU

/*
 * The divisor of 115200 baud from the debug UART's 9.216 MHz clock:
 * 9216000 / (16 * 115200) is 5 exactly, with no fraction.
 *
 * Value referenced: Raspberry Pi Linux rpi-6.12.y
 * arch/arm64/boot/dts/broadcom/bcm2712.dtsi (clk_uart's clock-frequency).
 */
#define UART_IBRD_115200 5U
#define UART_FBRD_115200 0U

/* The PL011 of the debug connector through the direct map; constant. */
static volatile uint32_t *const uart = (volatile uint32_t *)(ARM64_DIRECT_BASE + RPI5_DEBUG_UART_BASE);

/*
 * Starts the debug UART at 115200 baud, 8 bits, no parity.
 *
 * The connector has pins of its own, so no GPIO function is chosen.  A
 * UART the firmware already runs keeps the firmware's rate and framing;
 * only its interrupts are masked.
 */
void
arm64_uart_init(
	void)
{
	uint32_t control;

	/* Keeps a line the firmware started, with interrupts masked. */
	control = uart[UART_CR / 4];
	if ((control & UART_CR_UARTEN) != 0) {
		uart[UART_IMSC / 4] = 0;
		uart[UART_ICR / 4] = UART_ICR_ALL;
		uart[UART_CR / 4] = control | UART_CR_TXE | UART_CR_RXE;
		return;
	}

	/* Programs a stopped UART: the rate first, then the framing, then the start. */
	uart[UART_ICR / 4] = UART_ICR_ALL;
	uart[UART_IBRD / 4] = UART_IBRD_115200;
	uart[UART_FBRD / 4] = UART_FBRD_115200;
	uart[UART_LCRH / 4] = UART_LCRH_8N1_FIFO;
	uart[UART_IMSC / 4] = 0;
	uart[UART_CR / 4] = UART_CR_RXE | UART_CR_TXE | UART_CR_UARTEN;
}

/*
 * Writes one byte, a newline as a carriage return and a newline.
 */
void
arm64_uart_putc(
	int c)
{
	uint32_t flags;

	/* A terminal needs the carriage return to start the next line at its left. */
	if (c == '\n')
		arm64_uart_putc('\r');

	/* Waits for room in the transmit FIFO. */
	for (;;) {
		flags = uart[UART_FR / 4];
		if ((flags & UART_FR_TXFF) == 0)
			break;
	}

	/* Queues the byte. */
	uart[UART_DR / 4] = (uint32_t)c;
}

/*
 * Reports whether a received byte is waiting.
 */
int
arm64_uart_poll(
	void)
{
	uint32_t flags;

	/* The receive FIFO is empty when its flag is set. */
	flags = uart[UART_FR / 4];
	if ((flags & UART_FR_RXFE) != 0)
		return 0;

	/* Succeeded: a byte is waiting. */
	return 1;
}

/*
 * Reads one received byte, waiting for it.
 */
int
arm64_uart_getc(
	void)
{
	uint32_t data;
	int ready;

	/* Waits until a byte is waiting. */
	for (;;) {
		ready = arm64_uart_poll();
		if (ready)
			break;
	}

	/* Succeeded: the byte, without the error flags above it. */
	data = uart[UART_DR / 4];
	return (int)(data & 0xff);
}

/*
 * Lets the received bytes raise the UART's interrupt.
 */
void
arm64_uart_enable_rx_irq(
	void)
{
	/* Drops what was pending, then unmasks receive and receive timeout. */
	uart[UART_ICR / 4] = UART_IRQ_RX | UART_IRQ_RT;
	uart[UART_IMSC / 4] = UART_IRQ_RX | UART_IRQ_RT;
}

/*
 * Ends the UART's receive interrupt.
 */
void
arm64_uart_clear_rx_irq(
	void)
{
	/* Clears receive and receive timeout. */
	uart[UART_ICR / 4] = UART_IRQ_RX | UART_IRQ_RT;
}
