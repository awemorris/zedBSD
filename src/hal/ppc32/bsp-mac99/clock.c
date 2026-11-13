/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The tick: the processor's decrementer, which counts down at the time
 * base's rate and raises its exception when it passes zero.  The HAL
 * reloads it with the interval left to the next deadline each time, so a
 * late tick does not make the next ones late.
 */

#include <hal/hal.h>
#include "../asm.h"
#include "../percpu.h"
#include "bsp.h"

/* The tick's rate, in hertz, which never changes. */
#define CLOCK_TICK_HZ	100U

/* The time base's rate and the counts of one tick; zero before the clock starts. */
static uint64_t timebase_frequency;
static uint64_t tick_interval;

/* The count the next tick is due at. */
static uint64_t next_deadline;

static void arm(void);

/*
 * Starts the tick.
 */
void
mac99_clock_init(
	void)
{
	/* The rate comes from the firmware. */
	timebase_frequency = mac99_board()->timebase_frequency;
	if (timebase_frequency == 0)
		HAL_FATAL("ppc32 firmware reports no time base rate");
	tick_interval = timebase_frequency / CLOCK_TICK_HZ;

	/* The first tick one interval from now. */
	next_deadline = ppc32_read_timebase() + tick_interval;
	arm();
}

/*
 * Serves the tick: moves the deadline past now in whole intervals, arms
 * it, and hands the tick to the kernel.
 */
void
mac99_clock_interrupt(
	void)
{
	struct ppc32_percpu *cpu;
	uint64_t now;

	/* Counts the tick and arms the next one. */
	cpu = ppc32_percpu_current();
	cpu->timer_ticks++;
	now = ppc32_read_timebase();
	do {
		next_deadline += tick_interval;
	} while (next_deadline <= now);
	arm();

	/* Refuses a tick inside another interrupt. */
	if (cpu->active_ack != HAL_IRQ_ACK_NONE)
		HAL_FATAL("nested ppc32 IRQ acknowledgement");

	/* The kernel ends the tick, which needs no controller, and schedules. */
	cpu->active_ack = PPC32_ACK_LOCAL;
	kernel_timer_handler(cpu->logical_id, PPC32_ACK_LOCAL);
}

/*
 * Waits a short while for the first tick and reports whether it came.
 * With interrupts off, a decrementer that has passed zero counts as come.
 */
bool
mac99_clock_ticked(
	void)
{
	struct ppc32_percpu *cpu;
	uint64_t deadline;
	uint32_t decrementer;
	bool masked;
	unsigned long ticks;
	uint64_t now;

	/* A clock not started has nothing to wait for. */
	cpu = ppc32_percpu_current();
	if (tick_interval == 0)
		return false;

	/* Waits up to a tenth of a second for a tick, taken or due. */
	masked = ppc32_irq_masked();
	deadline = ppc32_read_timebase() + timebase_frequency / 10U;
	do {
		ticks = __atomic_load_n(&cpu->timer_ticks, __ATOMIC_RELAXED);
		if (ticks != 0)
			return true;
		__asm__ volatile("mfdec %0" : "=r"(decrementer));
		if (masked && (decrementer & 0x80000000U) != 0)
			return true;
		now = ppc32_read_timebase();
	} while (now < deadline);

	/* Failed: no tick in the time allowed. */
	return false;
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
 * Reads the counter and its frequency: the time base.
 */
bool
hal_rtc_read_counter(
	uint64_t *value,
	uint64_t *freq_hz)
{
	/* Refuses a caller with nowhere to put the answer, or a clock not started. */
	if (value == NULL || freq_hz == NULL)
		return false;
	if (timebase_frequency == 0)
		return false;
	*value = ppc32_read_timebase();
	*freq_hz = timebase_frequency;
	return true;
}

/*
 * Reports the wall-clock time; the HAL reads no battery-backed clock yet.
 */
bool
hal_rtc_read_epoch_time(
	uint64_t *unix_seconds)
{
	UNUSED_PARAMETER(unix_seconds);

	/* Failed: there is no clock to read. */
	return false;
}

/* Loads the decrementer with the counts left to the deadline. */
static void
arm(
	void)
{
	uint64_t now;
	uint64_t left;

	/* At least one count, at most what the register holds as positive. */
	now = ppc32_read_timebase();
	left = 1;
	if (next_deadline > now)
		left = next_deadline - now;
	if (left > 0x7fffffffU)
		left = 0x7fffffffU;
	ppc32_write_decrementer((uint32_t)left);
}
