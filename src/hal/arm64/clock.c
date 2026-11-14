/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The arm64 scheduling tick and counter, from the generic timer.
 *
 * Each CPU's tick comes at the multiples of its period on the shared
 * counter, so that every CPU takes its tick at the same moment.  A CPU's
 * period is a whole number of the fastest tick's periods
 * (hal_rtc_set_tick_hz()), so a slow CPU's ticks fall on a fast one's.
 */

#include <hal/hal.h>
#include "asm.h"
#include "defs.h"
#include "percpu.h"
#include "watch.h"
#include "clock.h"
#include "gic.h"

/* The private interrupt of each CPU's physical timer (CNTP). */
#define ARM64_CNTP_INTID 30U

/* The fastest tick, in hertz, which every CPU starts at. */
#define CLOCK_TICK_HZ_MAX 1000U

/* The slowest tick, as the most periods of the fastest one a tick spans. */
#define CLOCK_TICK_DIVISOR_MAX 100U

/*
 * The counter's frequency and the counter ticks of the fastest tick's
 * period.  The boot CPU sets both once, before any other CPU starts; every
 * CPU's timer uses them.  A zero frequency means the timer is not set up.
 */
static uint64_t timer_frequency;
static uint64_t timer_base;

static uint64_t counter(void);
static void set_cval(uint64_t value);
static uint64_t cpu_divisor(const struct arm64_percpu *cpu);
static void arm_next(struct arm64_percpu *cpu, uint64_t now);

/*
 * Starts the calling CPU's scheduling tick.
 *
 * Each CPU has its own physical timer, delivered as its private interrupt
 * 30; the boot CPU also reads the counter's frequency.
 */
void
arm64_timer_init(
	void)
{
	struct arm64_percpu *cpu;
	uint64_t now;

	/* Finds the calling CPU's record, which holds its deadline. */
	cpu = arm64_percpu_current();

	/* The boot CPU reads the frequency and derives the tick's length. */
	if (cpu->logical_id == 0) {
		__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(timer_frequency));

		/* A timer that does not count cannot tick. */
		if (timer_frequency == 0)
			HAL_FATAL("zero AArch64 timer frequency");

		/* The counter ticks of the fastest tick's period, at least one. */
		timer_base = timer_frequency / CLOCK_TICK_HZ_MAX;
		if (timer_base == 0)
			timer_base = 1;
	}

	/* Starts at the fastest rate, at the next multiple of the period. */
	if (cpu->timer_divisor == 0)
		cpu->timer_divisor = 1;
	now = counter();
	arm_next(cpu, now);

	/* Enables the timer and its interrupt on this CPU. */
	__asm__ volatile("msr cntp_ctl_el0, %0\n\tisb" : : "r"((uint64_t)1) : "memory");
	arm64_gic_unmask(ARM64_CNTP_INTID);
}

/*
 * Serves the calling CPU's scheduling tick.
 *
 * The next deadline is the next multiple of the period past now, so a
 * tick that came late does not make the following ones come early.
 */
void
arm64_timer_interrupt(
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

	/* Moves the deadline to the next multiple of the period. */
	arm_next(cpu, now);

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
 * Reports the calling CPU's tick rate.
 */
uint32_t
hal_rtc_get_tick_hz(
	void)
{
	struct arm64_percpu *cpu;
	uint64_t divisor;
	bool enabled;

	/* Reads the record with interrupts off, so the CPU cannot change under it. */
	enabled = hal_irq_disable();
	cpu = arm64_percpu_current();
	divisor = cpu_divisor(cpu);
	if (enabled)
		hal_irq_enable();

	/* Succeeded: the fastest rate over the periods a tick spans. */
	return (uint32_t)(CLOCK_TICK_HZ_MAX / divisor);
}

/*
 * Sets the calling CPU's tick rate to the nearest whole number of the
 * fastest tick's periods, from 1000 Hz down to 10 Hz, and reports it.
 *
 * The tick already armed is moved at once to the next multiple of the new
 * period, so neither a fast tick is left over after a slowdown nor a slow
 * one waited for after a speedup.
 */
uint32_t
hal_rtc_set_tick_hz(
	uint32_t hz)
{
	struct arm64_percpu *cpu;
	uint64_t divisor;
	uint64_t before;
	uint64_t now;
	bool enabled;

	/* The nearest number of periods, within the bounds. */
	divisor = CLOCK_TICK_DIVISOR_MAX;
	if (hz != 0U)
		divisor = (CLOCK_TICK_HZ_MAX + hz / 2U) / hz;
	if (divisor == 0U)
		divisor = 1U;
	if (divisor > CLOCK_TICK_DIVISOR_MAX)
		divisor = CLOCK_TICK_DIVISOR_MAX;

	/* Changes this CPU's period with interrupts off, so its tick cannot run between. */
	enabled = hal_irq_disable();
	cpu = arm64_percpu_current();
	before = cpu_divisor(cpu);
	cpu->timer_divisor = divisor;

	/* The next tick moves now, once the timer runs. */
	if (divisor != before && timer_frequency != 0U && cpu->timer_deadline != 0U) {
		now = counter();
		arm_next(cpu, now);
	}
	if (enabled)
		hal_irq_enable();

	/* Succeeded: the rate the tick now runs at. */
	return (uint32_t)(CLOCK_TICK_HZ_MAX / divisor);
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

/* Reports the periods of the fastest tick one of a CPU's ticks spans. */
static uint64_t
cpu_divisor(
	const struct arm64_percpu *cpu)
{
	/* A tick not started yet starts at the fastest rate. */
	if (cpu->timer_divisor == 0U)
		return 1U;

	/* Succeeded: the CPU's own. */
	return cpu->timer_divisor;
}

/* Arms a CPU's next tick at the first multiple of its period after now. */
static void
arm_next(
	struct arm64_percpu *cpu,
	uint64_t now)
{
	uint64_t period;

	/* The multiple of the period after now, on every CPU's shared counter. */
	period = timer_base * cpu_divisor(cpu);
	cpu->timer_deadline = (now / period + 1U) * period;
	set_cval(cpu->timer_deadline);
}

/* Sets the calling CPU's timer compare value. */
static void
set_cval(
	uint64_t value)
{
	/* The timer fires once the counter reaches the value. */
	__asm__ volatile("msr cntp_cval_el0, %0" : : "r"(value));
}
