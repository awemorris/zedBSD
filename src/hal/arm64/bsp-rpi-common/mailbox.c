/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The firmware's property mailbox on a Raspberry Pi.
 *
 * Where the VideoCore sees the ARM's memory differs by board; board.h
 * gives it as RPI_BOARD_MAILBOX_BUS_ALIAS.
 */

#include <hal/hal.h>
#include "../defs.h"
#include "board.h"
#include "mailbox.h"

/* The mailbox word of a message: its bus address on the property channel. */
#define MBOX_REQUEST(physical) ((uint32_t)((physical) | RPI_BOARD_MAILBOX_BUS_ALIAS) | MBOX_PROPERTY)

/* The mailbox's registers: mailbox 0's read and status, mailbox 1's write. */
#define MBOX_READ 0x00U
#define MBOX_STATUS 0x18U
#define MBOX_WRITE 0x20U

/* The status bits: no room to write, nothing to read. */
#define MBOX_FULL 0x80000000U
#define MBOX_EMPTY 0x40000000U

/* The property channel, in the low four bits of a mailbox word. */
#define MBOX_PROPERTY 8U

/* The firmware's answer code for a request it handled. */
#define MBOX_RESPONSE_SUCCESS 0x80000000U

/* The smallest message: its size, its code and the end tag. */
#define MBOX_MIN_BYTES 12U

/* The memory the VideoCore reaches through the mailbox: the first gigabyte. */
#define MBOX_DMA_LIMIT 0x40000000ULL

static uint64_t now(void);
static uint64_t frequency(void);

/*
 * Sends a property message to the firmware and waits for its answer.
 *
 * The message must be 16-byte aligned in the direct map and lie in the
 * first gigabyte.  The answer replaces the message; it is good when 0 is
 * returned.  The wait is limited to a second.
 */
int
rpi_mailbox_property(
	uintptr_t mailbox_phys,
	uint32_t *message,
	size_t bytes)
{
	volatile uint8_t *base;
	uintptr_t virtual_address;
	uint64_t physical;
	uint64_t deadline;
	uint64_t current;
	uint32_t request;
	uint32_t status;
	uint32_t answer;

	/* Refuses a missing or short message. */
	if (message == NULL)
		return -1;
	if (bytes < MBOX_MIN_BYTES)
		return -1;

	/* Refuses a message the mailbox word cannot name: unaligned, or outside the direct map. */
	virtual_address = (uintptr_t)message;
	if ((virtual_address & 15U) != 0)
		return -1;
	if (virtual_address < ARM64_DIRECT_BASE)
		return -1;

	/* Refuses a message the VideoCore cannot reach. */
	physical = virtual_address - ARM64_DIRECT_BASE;
	if (physical >= MBOX_DMA_LIMIT)
		return -1;

	/* Names the message by its bus address on the property channel. */
	request = MBOX_REQUEST(physical);

	/* Puts the message in memory, where the VideoCore reads it. */
	base = (volatile uint8_t *)(ARM64_DIRECT_BASE + mailbox_phys);
	hal_dcache_clean_range(virtual_address, bytes);
	deadline = now() + frequency();

	/* Waits for room to write. */
	for (;;) {
		status = hal_mmio_read32(base + MBOX_STATUS);
		if ((status & MBOX_FULL) == 0)
			break;

		/* Gives up after the limit. */
		current = now();
		if (current >= deadline)
			return -1;
	}

	/* Sends the request. */
	hal_mmio_write32(base + MBOX_WRITE, request);

	/* Waits for the answer to this request, reading past any other. */
	for (;;) {
		/* Waits for a word to read. */
		for (;;) {
			status = hal_mmio_read32(base + MBOX_STATUS);
			if ((status & MBOX_EMPTY) == 0)
				break;

			/* Gives up after the limit. */
			current = now();
			if (current >= deadline)
				return -1;
		}

		/* The answer names the same message. */
		answer = hal_mmio_read32(base + MBOX_READ);
		if (answer == request)
			break;

		/* Gives up after the limit. */
		current = now();
		if (current >= deadline)
			return -1;
	}

	/* Reads the answer the VideoCore wrote to memory. */
	hal_dcache_invalidate_range(virtual_address, bytes);

	/* Refuses an answer the firmware did not mark handled. */
	if (message[1] != MBOX_RESPONSE_SUCCESS)
		return -1;

	/* Succeeded: the message holds the firmware's answer. */
	return 0;
}

/* Reads the generic timer's count. */
static uint64_t
now(
	void)
{
	uint64_t value;

	/* CNTPCT_EL0 counts at the timer's frequency. */
	__asm__ volatile("mrs %0,cntpct_el0" : "=r"(value));

	/* Succeeded: the count now. */
	return value;
}

/* Reads the generic timer's frequency. */
static uint64_t
frequency(
	void)
{
	uint64_t value;

	/* The firmware sets CNTFRQ_EL0 to the counter's rate in hertz. */
	__asm__ volatile("mrs %0,cntfrq_el0" : "=r"(value));

	/* Succeeded: the counter's ticks per second, the wait's limit. */
	return value;
}
