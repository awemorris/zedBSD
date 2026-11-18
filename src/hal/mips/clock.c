/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The MIPS scheduling tick and counter.
 *
 * The board owns the timer: it arms a tick of CLOCK_TICK_HZ, takes
 * its interrupt down and calls mips_clock_tick() for each, and reads a
 * free-running 64-bit counter at a rate it reports.  The malta board
 * uses coprocessor 0's Count and Compare; a processor without them uses
 * a timer of its SoC.  This file counts the ticks and hands them to the
 * kernel once it runs.
 */

#include <hal/hal.h>
#include "asm.h"
#include "bsp.h"
#include "clock.h"
#include "irq.h"

/* The tick's rate, in hertz, which never changes. */
#define CLOCK_TICK_HZ	1000U

/*
 * The counter's rate.  Set once by mips_clock_init() before the tick's
 * interrupt is let in; zero means the tick is not set up.
 */
static uint32_t clock_frequency;

/*
 * The ticks served since the tick was set up.  Only the tick increases
 * it; any context reads it.
 */
static volatile uint32_t clock_tick_count;

/*
 * Nonzero once the kernel takes the ticks: set by
 * mips_clock_start_kernel() just before the kernel starts.  Until then a
 * tick is only counted, since nothing of the kernel is set up.
 */
static int clock_kernel_running;

/*
 * Sets up the scheduling tick and the counter, and arms the first tick.
 *
 * The caller lets the tick's line in through Status.IM afterwards.
 */
void
mips_clock_init(
	void)
{
	/* The board arms its timer and reports the counter's rate. */
	clock_frequency = mips_board_timer_init(CLOCK_TICK_HZ);
	if (clock_frequency == 0)
		HAL_FATAL("mips board gives no counter rate");
}

/*
 * Serves one tick, which the board's timer interrupt has already taken
 * down.  Called by the board with interrupts off.
 */
void
mips_clock_tick(
	void)
{
	/* The tick is what the boot waits on and, later, what the kernel schedules on. */
	clock_tick_count++;

	/* The kernel ends the tick, which needs no controller, and schedules. */
	if (clock_kernel_running) {
		mips_irq_begin_local();
		kernel_timer_handler(0, MIPS_ACK_LOCAL);
	}
}

/*
 * Hands the ticks to the kernel from the next one on.
 */
void
mips_clock_start_kernel(
	void)
{
	/* Every later tick goes to kernel_timer_handler(). */
	clock_kernel_running = 1;
}

/*
 * Reports the tick's rate, which this timer runs at from boot and never
 * changes.
 */
uint32_t
hal_rtc_get_tick_hz(
	void)
{
	/* The rate the timer is programmed at. */
	return CLOCK_TICK_HZ;
}

/*
 * Keeps the tick's rate, which this timer cannot change, and reports it.
 */
uint32_t
hal_rtc_set_tick_hz(
	uint32_t hz)
{
	UNUSED_PARAMETER(hz);

	/* The rate the timer stays at. */
	return CLOCK_TICK_HZ;
}

/*
 * Reads the free-running counter and its rate.
 */
bool
hal_rtc_read_counter(
	uint64_t *value,
	uint64_t *freq_hz)
{
	/* Refuses a caller with nowhere to put the answer, or a clock not started. */
	if (value == NULL || freq_hz == NULL)
		return false;
	if (clock_frequency == 0)
		return false;

	/* Succeeded: the board's counter at its rate. */
	*value = mips_board_counter_read();
	*freq_hz = clock_frequency;
	return true;
}

/*
 * Refuses the time of day: the board's real-time clock is not read yet.
 */
bool
hal_rtc_read_epoch_time(
	uint64_t *unix_seconds)
{
	UNUSED_PARAMETER(unix_seconds);

	/* Failed: there is no clock the HAL reads. */
	return false;
}

/*
 * Reads the 64-bit counter, which runs at mips_clock_frequency().
 */
uint64_t
mips_clock_read_counter(
	void)
{
	/* Succeeded: the board's counter. */
	return mips_board_counter_read();
}

/*
 * Reports the rate of the 64-bit counter, zero before the tick is set up.
 */
uint64_t
mips_clock_frequency(
	void)
{
	/* Succeeded: the counter's rate. */
	return clock_frequency;
}

/*
 * Reports the ticks served since the tick was set up.
 */
uint32_t
mips_clock_ticks(
	void)
{
	/* Succeeded: the tick count. */
	return clock_tick_count;
}
