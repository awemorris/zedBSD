/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The malta board's tick and counter, from coprocessor 0.
 *
 * Count runs at a fixed rate, and Compare raises the timer interrupt on
 * hardware line 5 (IP7) when Count reaches it; writing Compare also takes
 * the interrupt down.  Each tick arms the next boundary one interval
 * ahead.  Count is 32 bits wide, so the 64-bit counter counts its wraps;
 * it is read at least once a tick, far more often than Count wraps.
 */

#include <hal/hal.h>
#include "../asm.h"
#include "../bsp.h"
#include "../clock.h"
#include "../spin.h"
#include "timer.h"

/*
 * The rate of coprocessor 0's Count: the processor's 320 MHz clock
 * divided by the two cycles of CCRes.
 *
 * Values referenced: QEMU v10.0.11 hw/mips/malta.c (cpu-refclk, 320 MHz)
 * and target/mips/cpu-defs.c.inc (24Kf, CCRes 2); target/mips/cpu.c
 * (mips_cp0_period_set, Count at the clock divided by CCRes).
 */
#define TIMER_COUNT_FREQUENCY	160000000U

/*
 * The Count cycles between two ticks.  Set once by
 * mips_board_timer_init() before the timer interrupt is let in.
 */
static uint32_t timer_interval;

/*
 * The Compare value of the next tick.  Written by the tick alone after
 * the first one is armed.
 */
static uint32_t timer_deadline;

/*
 * The 64-bit counter's state: Count when it was last read, and the wraps
 * of Count before that reading.  Both change together with interrupts
 * off, so that a reading never sees one without the other.
 */
static uint32_t timer_last_count;
static uint32_t timer_wraps;

static uint32_t read_count(void);
static void write_compare(uint32_t value);

/*
 * Arms the first tick of a rate of hz and starts the counter, and
 * reports the counter's rate.
 *
 * The caller lets the timer's line in through Status.IM afterwards.
 */
uint32_t
mips_board_timer_init(
	uint32_t hz)
{
	uint32_t now;

	/* The interval of a tick, from Count's rate. */
	timer_interval = TIMER_COUNT_FREQUENCY / hz;
	if (timer_interval == 0)
		timer_interval = 1;

	/* Starts the 64-bit counter from Count as it is now. */
	now = read_count();
	timer_last_count = now;
	timer_wraps = 0;

	/* Arms the first tick one interval from now. */
	timer_deadline = now + timer_interval;
	write_compare(timer_deadline);

	/* Succeeded: Count's rate. */
	return TIMER_COUNT_FREQUENCY;
}

/*
 * Serves the timer interrupt: arms the next tick and counts this one.
 *
 * A boundary already behind Count would only come again after Count
 * wraps, so a late tick moves the deadline one interval past now
 * instead.  Called by the board's interrupt handling with interrupts
 * off.
 */
void
malta_timer_interrupt(
	void)
{
	uint32_t now;
	int32_t ahead;

	/* Keeps the 64-bit counter's wraps counted. */
	(void)mips_board_counter_read();

	/* Moves the deadline on by one interval, or past now when that is already behind. */
	now = read_count();
	timer_deadline += timer_interval;
	ahead = (int32_t)(timer_deadline - now);
	if (ahead <= 0)
		timer_deadline = now + timer_interval;

	/* Arms the next tick, which also takes this one's interrupt down. */
	write_compare(timer_deadline);

	/* The HAL counts the tick and, once it runs, hands it to the kernel. */
	mips_clock_tick();
}

/*
 * Reads the 64-bit counter: Count with its wraps.
 */
uint64_t
mips_board_counter_read(
	void)
{
	uint32_t now;
	uint32_t wraps;
	bool enabled;

	/* Counts a wrap when Count went below its last reading. */
	enabled = mips_irq_save();

	now = read_count();
	if (now < timer_last_count)
		timer_wraps++;
	timer_last_count = now;
	wraps = timer_wraps;

	mips_irq_restore(enabled);

	/* Succeeded: the wraps above Count. */
	return ((uint64_t)wraps << 32) | now;
}

/* Reads Count (coprocessor 0 register 9). */
static uint32_t
read_count(
	void)
{
	uint32_t value;

	/* Moves the register from coprocessor 0. */
	__asm__ volatile("mfc0 %0, $9" : "=r"(value));

	/* Succeeded: the register's value. */
	return value;
}

/* Writes Compare (coprocessor 0 register 11), which also clears the timer interrupt. */
static void
write_compare(
	uint32_t value)
{
	/* The barrier keeps the interrupt from being seen still pending after the write. */
	__asm__ volatile("mtc0 %0, $11\n\tehb" : : "r"(value) : "memory");
}
