/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The SPARC V9 HAL's periodic tick, from the processor's cycle counter and
 * its compare register.
 *
 * The compare register raises interrupt level 14 when the counter reaches
 * it.  Each tick moves the deadline on by one interval, skipping intervals
 * already past, so a late tick does not bring a burst of others.
 */

#include <hal/hal.h>
#include "asi.h"
#include "defs.h"
#include "irq.h"
#include "timer.h"

/* The tick's rate, in hertz, which never changes. */
#define TIMER_TICK_HZ	100U

/*
 * The counter's rate, the counts in one tick, and the count of the next
 * tick.  The first two are set once by sparcv9_timer_init(); the deadline
 * moves on with every tick, which interrupts are off for.
 */
static uint64_t tick_frequency;
static uint64_t tick_interval;
static uint64_t tick_deadline;

/*
 * The ticks taken at trap level 2, not yet handed to the kernel.
 *
 * A tick may interrupt a user system call while its trap is being taken.
 * The scheduler can switch only the outer context, so such a tick is
 * acknowledged at once and handed over at the next tick taken at trap
 * level 1.  It saturates rather than wraps.
 */
static unsigned deferred_ticks;

/*
 * Starts the periodic tick from the processor's cycle counter.
 */
void
sparcv9_timer_init(
	uint64_t frequency)
{
	/* A counter that does not count cannot make a tick. */
	if (frequency == 0)
		HAL_FATAL("invalid SPARC V9 timer frequency");

	/* Divides the counter's rate into ticks, never fewer than one count. */
	tick_frequency = frequency;
	tick_interval = tick_frequency / TIMER_TICK_HZ;
	if (tick_interval == 0)
		tick_interval = 1;

	/* Arms the first tick one interval from now. */
	tick_deadline = sparcv9_read_tick() + tick_interval;
	sparcv9_clear_tick_interrupt();
	sparcv9_write_tick_compare(tick_deadline);
}

/*
 * Handles one tick interrupt.
 */
void
sparcv9_timer_interrupt(
	void)
{
	hal_irq_ack_t acknowledge;
	uint64_t now;
	uint64_t trap_level;

	/* Starts the handling of the tick. */
	acknowledge = sparcv9_irq_begin(0);

	/* Clears the interrupt and arms the next tick past the current count. */
	now = sparcv9_read_tick();
	sparcv9_clear_tick_interrupt();
	do {
		tick_deadline += tick_interval;
	} while (tick_deadline <= now);
	sparcv9_write_tick_compare(tick_deadline);

	/* Keeps a tick taken at trap level 2 for the next one at level 1. */
	trap_level = sparcv9_read_trap_level();
	if (trap_level > 1) {
		hal_irq_send_eoi(acknowledge);
		if (deferred_ticks != ~0U)
			deferred_ticks++;
		return;
	}

	/* Hands the tick to the kernel, which acknowledges it. */
	kernel_timer_handler(0, acknowledge);

	/* Hands over the ticks kept from trap level 2, each acknowledged on its own. */
	while (deferred_ticks != 0) {
		deferred_ticks--;
		acknowledge = sparcv9_irq_begin(0);
		kernel_timer_handler(0, acknowledge);
	}
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
	return TIMER_TICK_HZ;
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
	return TIMER_TICK_HZ;
}

/*
 * Reads the processor's cycle counter and its rate.
 */
bool
hal_rtc_read_counter(
	uint64_t *counter,
	uint64_t *freq_hz)
{
	/* Refuses missing destinations, or a counter not yet started. */
	if (counter == NULL || freq_hz == NULL)
		return false;
	if (tick_frequency == 0U)
		return false;

	/* Succeeded: the count and the rate. */
	*counter = sparcv9_read_tick();
	*freq_hz = tick_frequency;
	return true;
}

/*
 * Reports that the wall-clock time cannot be read: the board's clock chip
 * is not driven.
 */
bool
hal_rtc_read_epoch_time(
	uint64_t *unix_seconds)
{
	UNUSED_PARAMETER(unix_seconds);

	/* Failed: no clock is read. */
	return false;
}
