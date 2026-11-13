/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The virt board's tick: the architected generic timer, its physical
 * counter and the non-secure physical timer, which the processor runs as
 * it enters non-secure.
 *
 * Each processor's tick is armed one interval ahead at a time through the
 * compare value, and moves on by whole intervals, so a late tick does not
 * make the following ones early.
 */

#include <hal/hal.h>
#include "../asm.h"
#include "../bsp.h"
#include "../percpu.h"

/*
 * The non-secure physical timer's private interrupt when the device tree
 * names no timer.
 *
 * Values referenced: ARM DDI 0406C, the generic timer's PPI assignments as
 * the emulator wires them: PPI 14, GIC interrupt 30.
 */
#define TIMER_DEFAULT_IRQ	30U

/* CNTP_CTL: the timer enabled, its interrupt not masked. */
#define TIMER_CTL_ENABLE	0x1U

/*
 * The counter's frequency, the counter ticks between two scheduling ticks
 * and the tick's interrupt.  The boot processor sets them once, before
 * any other processor starts; a zero frequency means not set up.
 */
static uint64_t timer_frequency;
static uint64_t timer_interval;
static uint32_t timer_irq;

static int timer_init_boot(const struct armv7_fdt_info *info);
static void timer_init_cpu(void);
static void timer_ack_tick(void);
static bool timer_read_counter(uint64_t *counter, uint64_t *frequency);
static uint32_t timer_tick_irq(void);

/*
 * The board's timer operations.
 */
static const struct armv7_clock_ops timer_ops = {
	timer_init_boot,
	timer_init_cpu,
	timer_ack_tick,
	timer_read_counter,
	timer_tick_irq
};

/*
 * Reports the board's timer operations.
 */
const struct armv7_clock_ops *
bsp_clock(
	void)
{
	/* Succeeded: the generic timer's. */
	return &timer_ops;
}

/* Reads the counter's frequency and the tick's interrupt, once. */
static int
timer_init_boot(
	const struct armv7_fdt_info *info)
{
	/* The tree's frequency, or the one the emulator put in CNTFRQ. */
	timer_frequency = info->timer_frequency;
	if (timer_frequency == 0)
		timer_frequency = armv7_read_cntfrq();
	if (timer_frequency == 0)
		return HAL_ERR_UNSUPPORTED;

	/* The tick's length in counter ticks, at least one. */
	timer_interval = timer_frequency / ARMV7_TICK_HZ;
	if (timer_interval == 0)
		timer_interval = 1;

	/* The non-secure physical timer's interrupt. */
	timer_irq = TIMER_DEFAULT_IRQ;
	if (info->has_timer)
		timer_irq = info->timer_nonsecure_irq;

	/* Succeeded: the timer can be armed. */
	return HAL_OK;
}

/* Arms the calling processor's first tick and turns its timer on. */
static void
timer_init_cpu(
	void)
{
	struct armv7_percpu *cpu;
	uint64_t now;

	/* One interval from now. */
	cpu = armv7_percpu_current();
	now = armv7_read_cntpct();
	cpu->timer_deadline = now + timer_interval;
	armv7_write_cntp_cval(cpu->timer_deadline);

	/* Enables the timer with its interrupt. */
	armv7_write_cntp_ctl(TIMER_CTL_ENABLE);
}

/*
 * Arms the next tick, past now in whole intervals, which also takes the
 * timer's level interrupt down.
 */
static void
timer_ack_tick(
	void)
{
	struct armv7_percpu *cpu;
	uint64_t now;

	/* Moves the deadline past now and arms it. */
	cpu = armv7_percpu_current();
	now = armv7_read_cntpct();
	do {
		cpu->timer_deadline += timer_interval;
	} while (cpu->timer_deadline <= now);
	armv7_write_cntp_cval(cpu->timer_deadline);
}

/* Reads the physical counter and its frequency. */
static bool
timer_read_counter(
	uint64_t *counter,
	uint64_t *frequency)
{
	/* A timer not set up has no frequency. */
	if (timer_frequency == 0)
		return false;

	/* Succeeded: the counter now and its frequency. */
	*counter = armv7_read_cntpct();
	*frequency = timer_frequency;
	return true;
}

/* Reports the tick's interrupt. */
static uint32_t
timer_tick_irq(
	void)
{
	/* Succeeded: the number set at boot. */
	return timer_irq;
}
