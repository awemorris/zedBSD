/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Raspberry Pi 4's debug UART: the PL011 on GPIO 14 and 15.
 */

#include <hal/hal.h>
#include "../defs.h"
#include "board.h"
#include "../uart.h"

/* The GPIO block's function select of pins 10 to 19, and its pull control of pins 0 to 15. */
#define GPIO_GPFSEL1 0x04
#define GPIO_PUP_PDN_CNTRL0 0xe4

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
 * The divisor of 115200 baud from the 48 MHz PL011 clock config.txt fixes:
 * 48000000 / (16 * 115200) is 26 and 3/64.
 */
#define UART_IBRD_115200 26U
#define UART_FBRD_115200 3U

/* The GPIO block and the PL011 through the direct map; constant. */
static volatile uint32_t *const gpio = (volatile uint32_t *)(ARM64_DIRECT_BASE + RPI4_GPIO_BASE);
static volatile uint32_t *const uart = (volatile uint32_t *)(ARM64_DIRECT_BASE + RPI4_PL011_BASE);

/*
 * Starts the PL011 at 115200 baud, 8 bits, no parity, on GPIO 14 and 15.
 */
void
arm64_uart_init(
	void)
{
	uint32_t value;

	/* Stops the UART while it is set up. */
	uart[UART_CR / 4] = 0;

	/* Gives GPIO 14 and 15 their UART function (ALT0, 100). */
	value = gpio[GPIO_GPFSEL1 / 4];
	value &= ~((7U << 12) | (7U << 15));
	value |= (4U << 12) | (4U << 15);
	gpio[GPIO_GPFSEL1 / 4] = value;

	/* Takes the pull-up and pull-down off both pins. */
	value = gpio[GPIO_PUP_PDN_CNTRL0 / 4];
	value &= ~((3U << 28) | (3U << 30));
	gpio[GPIO_PUP_PDN_CNTRL0 / 4] = value;

	/* Clears the interrupts and programs the rate. */
	uart[UART_ICR / 4] = UART_ICR_ALL;
	uart[UART_IBRD / 4] = UART_IBRD_115200;
	uart[UART_FBRD / 4] = UART_FBRD_115200;

	/* The FIFO preserves burst input; the receive timeout handles a lone byte. */
	uart[UART_LCRH / 4] = UART_LCRH_8N1_FIFO;

	/* Starts the UART with every interrupt masked. */
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
