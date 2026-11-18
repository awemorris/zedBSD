/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The armv7 scheduling tick and counter.
 *
 * The timer is the board's (bsp.h): the generic timer on the emulator's
 * virt board, the Cortex-A9 private and global timers on the ZYBO.  This
 * file counts the ticks, and hands them to the kernel once it has asked
 * for them; until then the HAL ends each tick itself, so the boot can see
 * the tick run before the kernel exists.
 */

#include <hal/hal.h>
#include "bootinfo.h"
#include "bsp.h"
#include "clock.h"
#include "gic.h"
#include "percpu.h"

/* The interrupt number that names no tick, before the timer is set up. */
#define CLOCK_NO_IRQ	0xffffffffU

/*
 * The board's timer, and the tick's interrupt.  Set once by the boot
 * processor before any tick; NULL and CLOCK_NO_IRQ before.
 */
static const struct armv7_clock_ops *clock_ops;
static uint32_t clock_irq = CLOCK_NO_IRQ;

/*
 * Nonzero once the kernel takes the ticks: from then on each tick goes to
 * kernel_timer_handler(), which ends it.  Set once by the boot processor
 * with interrupts off.
 */
static int clock_kernel_started;

/*
 * Starts the boot processor's scheduling tick, with its interrupt
 * unmasked at the GIC.
 */
void
armv7_clock_init(
	void)
{
	const struct armv7_clock_ops *ops;
	int error;

	/* Reads what the board's timer needs once. */
	ops = bsp_clock();
	error = ops->init_boot(armv7_boot_fdt());
	if (error != HAL_OK)
		HAL_FATAL("armv7 board timer could not be set up");

	/* Publishes the timer before its first tick can arrive. */
	clock_ops = ops;
	clock_irq = ops->tick_irq();

	/* Arms this processor's first tick and lets its interrupt in. */
	ops->init_cpu();
	armv7_gic_unmask(clock_irq);
}

/*
 * Reports the tick's GIC interrupt, or a number no interrupt has before
 * the timer is set up.
 */
uint32_t
armv7_clock_tick_irq(
	void)
{
	/* Succeeded: the number fixed at boot. */
	return clock_irq;
}

/*
 * Serves the calling processor's scheduling tick.
 *
 * The board's timer is ended and armed for the next tick first, so the
 * kernel may switch tasks freely.  Before the kernel takes the ticks the
 * HAL ends the interrupt itself.
 */
void
armv7_clock_interrupt(
	hal_irq_ack_t acknowledge)
{
	struct armv7_percpu *cpu;

	/* Counts the tick against this processor. */
	cpu = armv7_percpu_current();
	__atomic_store_n(&cpu->timer_ticks, cpu->timer_ticks + 1UL, __ATOMIC_RELAXED);

	/* Ends the timer's own interrupt and arms the next tick. */
	clock_ops->ack_tick();

	/* Before the kernel takes the ticks, the HAL ends the interrupt. */
	if (!clock_kernel_started) {
		hal_irq_send_eoi(acknowledge);
		return;
	}

	/* The kernel ends the tick and schedules on this processor. */
	kernel_timer_handler(cpu->logical_id, acknowledge);
}

/*
 * Hands the ticks to the kernel.
 */
void
armv7_clock_start_kernel(
	void)
{
	/* Every later tick goes to kernel_timer_handler(). */
	clock_kernel_started = 1;
}

/*
 * Reports how many ticks the boot processor has taken.
 */
unsigned long
armv7_clock_ticks(
	void)
{
	struct armv7_percpu *boot_cpu;
	unsigned long ticks;

	/* The boot processor's count, which its interrupt moves. */
	boot_cpu = armv7_percpu_get(0);
	ticks = __atomic_load_n(&boot_cpu->timer_ticks, __ATOMIC_RELAXED);

	/* Succeeded: the count. */
	return ticks;
}

/*
 * Reads the board's free-running counter and its frequency, for the HAL's
 * own waits.  Reports false before the timer is set up.
 */
bool
armv7_clock_read(
	uint64_t *counter,
	uint64_t *frequency)
{
	bool read;

	/* A timer not set up has no counter. */
	if (clock_ops == NULL)
		return false;

	/* The board reads its counter. */
	read = clock_ops->read_counter(counter, frequency);
	if (!read)
		return false;

	/* Succeeded: the counter and its frequency. */
	return true;
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
	return ARMV7_TICK_HZ;
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
	return ARMV7_TICK_HZ;
}

/*
 * Reads the counter and its frequency.
 *
 * The counter is the same on every processor and runs through every idle
 * state.
 */
bool
hal_rtc_read_counter(
	uint64_t *value,
	uint64_t *freq_hz)
{
	uint64_t counter;
	uint64_t frequency;
	bool read;

	/* Refuses a caller with nowhere to put the answer. */
	if (value == NULL || freq_hz == NULL)
		return false;

	/* Reads the board's counter; neither output changes on a failure. */
	read = armv7_clock_read(&counter, &frequency);
	if (!read)
		return false;

	/* Succeeded: the counter now and its frequency. */
	*value = counter;
	*freq_hz = frequency;
	return true;
}

/*
 * Reports the wall-clock time; the boards have no battery-backed clock the
 * HAL reads.
 */
bool
hal_rtc_read_epoch_time(
	uint64_t *unix_seconds)
{
	UNUSED_PARAMETER(unix_seconds);

	/* Failed: there is no clock to read. */
	return false;
}
