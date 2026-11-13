/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The riscv64 scheduling tick and counter.
 *
 * The counter is the time register, which runs at the device tree's
 * timebase on every hart alike.  Each hart's tick is armed through the
 * firmware's timer call, at the next multiple of its period on that
 * counter, so that every hart takes its tick at the same moment.  A
 * hart's period is a whole number of the fastest tick's periods
 * (hal_rtc_set_tick_hz()), so a slow hart's ticks fall on a fast one's.
 */

#include <hal/hal.h>
#include "asm.h"
#include "bootinfo.h"
#include "clock.h"
#include "irq.h"
#include "percpu.h"
#include "sbi.h"

/* The time riscv64_clock_ticked() allows the first tick, as a fraction of a second. */
#define CLOCK_TICK_WAIT_DIVISOR	10U

/* The fastest tick, in hertz, which every hart starts at. */
#define CLOCK_TICK_HZ_MAX	1000U

/* The slowest tick, as the most periods of the fastest one a tick spans. */
#define CLOCK_TICK_DIVISOR_MAX	100U

/*
 * The counter's frequency.  The boot hart sets it once, with timer_base,
 * before any other hart starts; zero means the tick is not set up.
 */
static uint64_t timer_frequency;

/* The counter ticks of the fastest tick's period, set with timer_frequency. */
static uint64_t timer_base;

static uint64_t hart_divisor(const struct riscv64_percpu *cpu);
static void arm_next(struct riscv64_percpu *cpu, uint64_t now);

/*
 * Starts the calling hart's scheduling tick.
 */
void
riscv64_clock_init(
	void)
{
	const struct riscv64_fdt_info *info;
	struct riscv64_percpu *cpu;
	uint64_t now;

	/* The boot hart derives the tick's length from the timebase. */
	cpu = riscv64_percpu_current();
	if (cpu->logical_id == 0) {
		/* The counter's frequency, without which there is no tick. */
		info = riscv64_boot_fdt();
		timer_frequency = info->timebase_frequency;
		if (timer_frequency == 0)
			HAL_FATAL("riscv64 device tree gives no timebase");

		/* The counter ticks of the fastest tick's period, at least one. */
		timer_base = timer_frequency / CLOCK_TICK_HZ_MAX;
		if (timer_base == 0)
			timer_base = 1;
	}

	/* Arms the first tick, at the fastest rate, and lets its interrupt in. */
	if (cpu->timer_divisor == 0)
		cpu->timer_divisor = 1;
	now = riscv64_read_time();
	arm_next(cpu, now);
	riscv64_set_sie(RISCV64_SIE_STIE);
}

/*
 * Serves the calling hart's scheduling tick.
 *
 * The next deadline is the next multiple of the period past now and is
 * armed before the kernel sees the tick, so a tick that came late does not
 * make the following ones come early, and the kernel may switch tasks
 * freely.
 */
void
riscv64_clock_interrupt(
	void)
{
	struct riscv64_percpu *cpu;
	uint64_t now;

	/* Counts the tick against this hart. */
	cpu = riscv64_percpu_current();
	now = riscv64_read_time();
	cpu->timer_ticks++;

	/* Arms the next tick, at the next multiple of the period. */
	arm_next(cpu, now);

	/* Refuses a tick while this hart still holds another interrupt. */
	if (cpu->active_ack != HAL_IRQ_ACK_NONE)
		HAL_FATAL("nested riscv64 IRQ acknowledgement");

	/* The kernel ends the tick, which needs no controller, and schedules. */
	cpu->active_ack = RISCV64_ACK_LOCAL;
	kernel_timer_handler(cpu->logical_id, RISCV64_ACK_LOCAL);
}

/*
 * Waits a short while for the boot hart's first tick and reports whether it came.
 *
 * The tick is counted by the interrupt alone, so the answer is written from
 * the caller's own context and never cuts into a line another part of the
 * boot is printing.  A caller with interrupts off is answered at once.
 */
bool
riscv64_clock_ticked(
	void)
{
	struct riscv64_percpu *boot_cpu;
	uint64_t deadline;
	uint64_t now;
	uint64_t ticks;
	bool masked;

	/* A tick not set up has nothing to wait for. */
	if (timer_frequency == 0U)
		return false;

	/* Answers from the count when the first tick is already in. */
	boot_cpu = riscv64_percpu_get(0);
	ticks = __atomic_load_n(&boot_cpu->timer_ticks, __ATOMIC_RELAXED);
	if (ticks != 0U)
		return true;

	/* A caller with interrupts off would wait for a tick that cannot arrive. */
	masked = riscv64_irq_masked();
	if (masked)
		return false;

	/* Waits up to a tenth of a second for the count to move. */
	now = riscv64_read_time();
	deadline = now + timer_frequency / CLOCK_TICK_WAIT_DIVISOR;
	while (now < deadline) {
		/* Leaves the wait once the count has moved. */
		ticks = __atomic_load_n(&boot_cpu->timer_ticks, __ATOMIC_RELAXED);
		if (ticks != 0U)
			break;

		/* Waits a moment before looking again. */
		hal_atomic_relax();
		now = riscv64_read_time();
	}

	/* The boot hart saw no tick in the time allowed. */
	if (ticks == 0U)
		return false;

	/* Succeeded: the first tick came. */
	return true;
}

/*
 * Reads the counter and its frequency.
 *
 * The counter is the same on every hart and runs through every idle state.
 */
bool
hal_rtc_read_counter(
	uint64_t *value,
	uint64_t *freq_hz)
{
	/* Refuses a caller with nowhere to put the answer, or a tick not set up. */
	if (value == NULL || freq_hz == NULL)
		return false;
	if (timer_frequency == 0U)
		return false;

	/* Reads the counter now, with its frequency. */
	*value = riscv64_read_time();
	*freq_hz = timer_frequency;

	/* Succeeded: both are the caller's. */
	return true;
}

/*
 * Reports the calling hart's tick rate.
 */
uint32_t
hal_rtc_get_tick_hz(
	void)
{
	struct riscv64_percpu *cpu;
	uint64_t divisor;
	bool enabled;

	/* Reads the record with interrupts off, so the hart cannot change under it. */
	enabled = hal_irq_disable();
	cpu = riscv64_percpu_current();
	divisor = hart_divisor(cpu);
	if (enabled)
		hal_irq_enable();

	/* Succeeded: the fastest rate over the periods a tick spans. */
	return (uint32_t)(CLOCK_TICK_HZ_MAX / divisor);
}

/*
 * Sets the calling hart's tick rate to the nearest whole number of the
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
	struct riscv64_percpu *cpu;
	uint64_t divisor;
	uint64_t before;
	bool enabled;

	/* The nearest number of periods, within the bounds. */
	divisor = CLOCK_TICK_DIVISOR_MAX;
	if (hz != 0U)
		divisor = (CLOCK_TICK_HZ_MAX + hz / 2U) / hz;
	if (divisor == 0U)
		divisor = 1U;
	if (divisor > CLOCK_TICK_DIVISOR_MAX)
		divisor = CLOCK_TICK_DIVISOR_MAX;

	/* Changes this hart's period with interrupts off, so its tick cannot run between. */
	enabled = hal_irq_disable();
	cpu = riscv64_percpu_current();
	before = hart_divisor(cpu);
	cpu->timer_divisor = divisor;

	/* The next tick moves now, once the timer runs. */
	if (divisor != before && timer_frequency != 0U && cpu->timer_deadline != 0U)
		arm_next(cpu, riscv64_read_time());
	if (enabled)
		hal_irq_enable();

	/* Succeeded: the rate the tick now runs at. */
	return (uint32_t)(CLOCK_TICK_HZ_MAX / divisor);
}

/*
 * Reports the wall-clock time; the HAL reads no battery-backed clock.
 */
bool
hal_rtc_read_epoch_time(
	uint64_t *unix_seconds)
{
	UNUSED_PARAMETER(unix_seconds);

	/* Failed: there is no clock to read. */
	return false;
}

/* Reports the periods of the fastest tick one of a hart's ticks spans. */
static uint64_t
hart_divisor(
	const struct riscv64_percpu *cpu)
{
	/* A tick not started yet starts at the fastest rate. */
	if (cpu->timer_divisor == 0U)
		return 1U;

	/* Succeeded: the hart's own. */
	return cpu->timer_divisor;
}

/* Arms a hart's next tick at the first multiple of its period after now. */
static void
arm_next(
	struct riscv64_percpu *cpu,
	uint64_t now)
{
	uint64_t period;

	/* The multiple of the period after now, on every hart's shared counter. */
	period = timer_base * hart_divisor(cpu);
	cpu->timer_deadline = (now / period + 1U) * period;
	riscv64_sbi_set_timer(cpu->timer_deadline);
}
