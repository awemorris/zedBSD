/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The riscv32 scheduling tick and counter.
 *
 * The counter is the time register, which runs at the device tree's
 * timebase on every hart alike.  Each hart's tick is armed through the
 * firmware's timer call, one interval ahead at a time.
 */

#include <hal/hal.h>
#include "asm.h"
#include "bootinfo.h"
#include "clock.h"
#include "irq.h"
#include "percpu.h"
#include "sbi.h"

/* The tick's rate, in hertz, which never changes. */
#define CLOCK_TICK_HZ	1000U

/* The time riscv32_clock_ticked() allows the first tick, as a fraction of a second. */
#define CLOCK_TICK_WAIT_DIVISOR	10U

/*
 * The counter's frequency and the counter ticks between two scheduling
 * ticks.  The boot hart sets both once, before any other hart starts; a
 * zero frequency means the tick is not set up.
 */
static uint64_t timer_frequency;
static uint64_t timer_interval;

/*
 * Starts the calling hart's scheduling tick.
 */
void
riscv32_clock_init(
	void)
{
	struct riscv32_percpu *cpu;

	/* The boot hart derives the tick's length from the timebase. */
	cpu = riscv32_percpu_current();
	if (cpu->logical_id == 0) {
		timer_frequency = riscv32_boot_fdt()->timebase_frequency;
		if (timer_frequency == 0)
			HAL_FATAL("riscv32 device tree gives no timebase");
		timer_interval = timer_frequency / CLOCK_TICK_HZ;
		if (timer_interval == 0)
			timer_interval = 1;
	}

	/* Arms the first tick one interval from now and lets its interrupt in. */
	cpu->timer_deadline = riscv32_read_time() + timer_interval;
	riscv32_sbi_set_timer(cpu->timer_deadline);
	riscv32_set_sie(RISCV32_SIE_STIE);
}

/*
 * Serves the calling hart's scheduling tick.
 *
 * The next deadline moves on by whole intervals past now and is armed
 * before the kernel sees the tick, so a tick that came late does not make
 * the following ones come early, and the kernel may switch tasks freely.
 */
void
riscv32_clock_interrupt(
	void)
{
	struct riscv32_percpu *cpu;
	uint64_t now;

	/* Counts the tick against this hart. */
	cpu = riscv32_percpu_current();
	now = riscv32_read_time();
	cpu->timer_ticks++;

	/* Moves the deadline past now in whole intervals and arms it. */
	do {
		cpu->timer_deadline += timer_interval;
	} while (cpu->timer_deadline <= now);
	riscv32_sbi_set_timer(cpu->timer_deadline);

	/* Refuses a tick while this hart still holds another interrupt. */
	if (cpu->active_ack != HAL_IRQ_ACK_NONE)
		HAL_FATAL("nested riscv32 IRQ acknowledgement");

	/* The kernel ends the tick, which needs no controller, and schedules. */
	cpu->active_ack = RISCV32_ACK_LOCAL;
	kernel_timer_handler(cpu->logical_id, RISCV32_ACK_LOCAL);
}

/*
 * Waits a short while for the boot hart's first tick and reports whether it came.
 *
 * The tick is counted by the interrupt alone, so the answer is written from
 * the caller's own context and never cuts into a line another part of the
 * boot is printing.  A caller with interrupts off is answered at once.
 */
bool
riscv32_clock_ticked(
	void)
{
	struct riscv32_percpu *boot_cpu;
	uint64_t deadline;
	uint64_t now;
	unsigned long ticks;
	bool masked;

	/* A tick not set up has nothing to wait for. */
	if (timer_frequency == 0U)
		return false;

	/* Answers from the count when the first tick is already in. */
	boot_cpu = riscv32_percpu_get(0);
	ticks = __atomic_load_n(&boot_cpu->timer_ticks, __ATOMIC_RELAXED);
	if (ticks != 0U)
		return true;

	/* A caller with interrupts off would wait for a tick that cannot arrive. */
	masked = riscv32_irq_masked();
	if (masked)
		return false;

	/* Waits up to a tenth of a second for the count to move. */
	now = riscv32_read_time();
	deadline = now + timer_frequency / CLOCK_TICK_WAIT_DIVISOR;
	while (now < deadline) {
		ticks = __atomic_load_n(&boot_cpu->timer_ticks, __ATOMIC_RELAXED);
		if (ticks != 0U)
			return true;
		hal_atomic_relax();
		now = riscv32_read_time();
	}

	/* Failed: the boot hart saw no tick in the time allowed. */
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

	/* Succeeded: the counter now and its frequency. */
	*value = riscv32_read_time();
	*freq_hz = timer_frequency;
	return true;
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
