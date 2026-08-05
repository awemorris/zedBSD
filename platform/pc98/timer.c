/*
 * Boots PC-98 polled interval timer
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */

#include "platform/pc98/timer.h"

/*
 * Stage 2 runs with IF clear and no protected-mode IDT, and the compatible
 * BIOS leaves the system timer unprogrammed when no BIOS timer event is
 * active.  A tick interrupt is therefore unavailable; instead channel 0 of
 * the system i8253 (ports 0x71/0x77) is programmed once as a free-running
 * rate generator with the full 65536 reload, and elapsed time accumulates
 * from latched down-counter deltas.  The counter input is 2.4576MHz on
 * 5/10MHz systems and 1.9968MHz on 8MHz systems; the BIOS system-clock
 * flag at 0000:0501h bit 7 selects between them.  Channels 1 (beep) and
 * 2 (RS-232C) are untouched, and the channel is left running on exit:
 * every boot path reprograms the timer for itself.
 */

#define PIT_COUNTER0 0x71U
#define PIT_CONTROL 0x77U
/* Channel 0, low/high byte access, mode 2, binary. */
#define PIT_PROGRAM 0x34U
#define PIT_LATCH0 0x00U

#define TICKS_PER_5MS_2457600HZ 12288U
#define TICKS_PER_5MS_1996800HZ 9984U

static struct {
	int initialized;
	uint16_t last_count;
	uint64_t ticks;
	uint32_t ticks_per_5ms;
} pit;

static uint8_t
port_in8(uint16_t port)
{
	uint8_t value;

	__asm__ volatile ("inb %w1,%0" : "=a"(value) : "Nd"(port));
	return value;
}

static void
port_out8(uint16_t port, uint8_t value)
{
	__asm__ volatile ("outb %0,%w1" : : "a"(value), "Nd"(port));
}

static uint8_t
read_low_byte(uint32_t address)
{
	uint8_t value;

	__asm__ volatile ("movb (%1),%0" : "=q"(value) : "r"(address));
	return value;
}

static uint16_t
latch_counter(void)
{
	uint8_t low;
	uint8_t high;

	port_out8(PIT_CONTROL, PIT_LATCH0);
	low = port_in8(PIT_COUNTER0);
	high = port_in8(PIT_COUNTER0);
	return (uint16_t)((uint16_t)high << 8 | low);
}

uint64_t
boots_pc98_timer_milliseconds(void *context)
{
	uint16_t count;

	(void)context;
	if (!pit.initialized) {
		pit.ticks_per_5ms = (read_low_byte(0x501U) & 0x80U) != 0 ?
			TICKS_PER_5MS_1996800HZ : TICKS_PER_5MS_2457600HZ;
		port_out8(PIT_CONTROL, PIT_PROGRAM);
		port_out8(PIT_COUNTER0, 0x00U);
		port_out8(PIT_COUNTER0, 0x00U);
		/* The full reload makes the first delta start from zero. */
		pit.last_count = 0;
		pit.ticks = 0;
		pit.initialized = 1;
		return 0;
	}
	count = latch_counter();
	pit.ticks += (uint16_t)(pit.last_count - count);
	pit.last_count = count;
	return pit.ticks * 5U / pit.ticks_per_5ms;
}
