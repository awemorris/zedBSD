/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Raspberry Pi scheduling tick and counter, from the generic timer.
 */

#include <hal/hal.h>
#include "../asm.h"
#include "../defs.h"
#include "../percpu.h"
#include "../watch.h"
#include "clock.h"
#include "gic.h"

/* The private interrupt of each CPU's physical timer (CNTP). */
#define ARM64_CNTP_INTID 30U

/*
 * The counter's frequency and the counter ticks between two scheduling
 * ticks.  The boot CPU sets both once, before any other CPU starts; every
 * CPU's timer uses them.  A zero frequency means the timer is not set up.
 */
static uint64_t timer_frequency;
static uint64_t timer_interval;

static uint64_t counter(void);
static void set_cval(uint64_t value);

/*
 * Starts the calling CPU's scheduling tick.
 *
 * Each CPU has its own physical timer, delivered as its private interrupt
 * 30; the boot CPU also reads the counter's frequency.
 */
void
rpi_timer_init(
	void)
{
	struct arm64_percpu *cpu;

	/* Finds the calling CPU's record, which holds its deadline. */
	cpu = arm64_percpu_current();

	/* The boot CPU reads the frequency and derives the tick's length. */
	if (cpu->logical_id == 0) {
		__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(timer_frequency));

		/* A timer that does not count cannot tick. */
		if (timer_frequency == 0)
			HAL_FATAL("zero AArch64 timer frequency");

		/* The counter ticks of one scheduling tick, at least one. */
		timer_interval = timer_frequency / HAL_TIMER_FREQUENCY;
		if (timer_interval == 0)
			timer_interval = 1;
	}

	/* Arms the first tick one interval from now. */
	cpu->timer_deadline = counter() + timer_interval;
	set_cval(cpu->timer_deadline);

	/* Enables the timer and its interrupt on this CPU. */
	__asm__ volatile("msr cntp_ctl_el0, %0\n\tisb" : : "r"((uint64_t)1) : "memory");
	rpi_gic_unmask(ARM64_CNTP_INTID);
}

/*
 * Serves the calling CPU's scheduling tick.
 *
 * The next deadline moves on by whole intervals past now, so a tick that
 * came late does not make the following ones come early.
 */
void
rpi_timer_interrupt(
	hal_irq_ack_t acknowledge)
{
	struct arm64_percpu *cpu;
	uint64_t now;

	/* Counts the tick against this CPU. */
	cpu = arm64_percpu_current();
	now = counter();
	cpu->timer_ticks++;

	/* The heartbeat the stall watch reads, and now and then its look at the other CPUs. */
	arm64_watch_beat(cpu, now);

	/* Moves the deadline past now in whole intervals. */
	do {
		cpu->timer_deadline += timer_interval;
	} while (cpu->timer_deadline <= now);
	set_cval(cpu->timer_deadline);

	/* The boot CPU's first tick marks the timer working in the boot log. */
	if (cpu->logical_id == 0 && cpu->timer_ticks == 1)
		hal_puts("ARM64 TIMER TICK\n");

	/* The kernel ends the interrupt and schedules on this CPU. */
	kernel_timer_handler(cpu->logical_id, acknowledge);
}

/*
 * Reads the counter and its frequency.
 *
 * The counter is the same on every CPU and runs through every idle state.
 */
bool
hal_rtc_read_counter(
	uint64_t *value,
	uint64_t *freq_hz)
{
	/* Refuses a caller with nowhere to put the answer. */
	if (value == NULL || freq_hz == NULL)
		return false;

	/* Refuses before the timer is set up. */
	if (timer_frequency == 0U)
		return false;

	/* Samples the counter and reports its frequency. */
	*value = counter();
	*freq_hz = timer_frequency;

	/* Succeeded: both values are filled. */
	return true;
}

/*
 * Reports the wall-clock time; the board has no battery-backed clock.
 */
bool
hal_rtc_read_epoch_time(
	uint64_t *unix_seconds)
{
	UNUSED_PARAMETER(unix_seconds);

	/* Failed: there is no clock to read. */
	return false;
}

/* Samples the physical counter at this point of the program. */
static uint64_t
counter(
	void)
{
	uint64_t value;

	/* The barrier keeps the sample from being taken early. */
	__asm__ volatile("isb\n\tmrs %0, cntpct_el0" : "=r"(value) : : "memory");

	/* Succeeded: the counter's value now. */
	return value;
}

/* Sets the calling CPU's timer compare value. */
static void
set_cval(
	uint64_t value)
{
	/* The timer fires once the counter reaches the value. */
	__asm__ volatile("msr cntp_cval_el0, %0" : : "r"(value));
}
