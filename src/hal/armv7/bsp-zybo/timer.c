/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The ZYBO's tick and counter: the Cortex-A9 MPCore's private timer
 * gives each processor its periodic tick, and the global timer, one
 * 64-bit counter for both processors, is the free-running counter.  Both
 * count the MPCore's peripheral clock without a prescaler (slcr.c).
 *
 * The private timer reloads itself, so the ticks do not drift with the
 * time a handler takes; its event flag is cleared on each tick, which
 * ends the interrupt.
 *
 * Values referenced: Linux v6.12 arch/arm/include/asm/smp_twd.h (the
 * private timer's LOAD 0x00, CONTROL 0x08 with ENABLE, PERIODIC and
 * IT_ENABLE, INTSTAT 0x0C) and drivers/clocksource/arm_global_timer.c
 * (GT_COUNTER0 0x00, GT_COUNTER1 0x04, GT_CONTROL 0x08 with
 * TIMER_ENABLE); the private timer's interrupt, PPI 13, GIC interrupt 29,
 * from arch/arm/boot/dts/xilinx/zynq-7000.dtsi (scutimer).
 */

#include <hal/hal.h>
#include "../bsp.h"
#include "../defs.h"
#include "zybo.h"

/* The private timer's registers and bits. */
#define PRIVATE_LOAD		0x00U
#define PRIVATE_CONTROL		0x08U
#define PRIVATE_INTSTAT		0x0cU
#define PRIVATE_ENABLE		0x1U
#define PRIVATE_PERIODIC	0x2U
#define PRIVATE_IT_ENABLE	0x4U
#define PRIVATE_EVENT		0x1U

/* The global timer's registers and its enable. */
#define GLOBAL_COUNTER_LOW	0x00U
#define GLOBAL_COUNTER_HIGH	0x04U
#define GLOBAL_CONTROL		0x08U
#define GLOBAL_ENABLE		0x1U

/* The private timer's interrupt. */
#define TIMER_PRIVATE_IRQ	29U

/*
 * The private and global timers' registers in the device window, the
 * clock they count and the counts between two ticks.  The boot processor
 * sets them once, before the first tick; a zero frequency means not set
 * up.
 */
static volatile uint8_t *private_timer;
static volatile uint8_t *global_timer;
static uint64_t timer_frequency;
static uint32_t timer_interval;

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
	/* Succeeded: the private and global timers'. */
	return &timer_ops;
}

/*
 * Finds the timers' clock and starts the global timer from zero, once.
 */
static int
timer_init_boot(
	const struct armv7_fdt_info *info)
{
	volatile uint8_t *mpcore;
	uint64_t interval;

	UNUSED_PARAMETER(info);

	/* The timers are in the MPCore's private region, which the board mapped. */
	mpcore = zybo_mpcore();
	if (mpcore == NULL)
		return HAL_ERR_UNSUPPORTED;
	private_timer = mpcore + ZYBO_MPCORE_PRIVATE_TIMER;
	global_timer = mpcore + ZYBO_MPCORE_GLOBAL_TIMER;

	/* The peripheral clock, which both count. */
	timer_frequency = zybo_slcr_periphclk();
	if (timer_frequency == 0)
		return HAL_ERR_UNSUPPORTED;

	/* The counts of one tick, which the private timer's load register holds less one. */
	interval = timer_frequency / ARMV7_TICK_HZ;
	if (interval < 2U || interval > 0xffffffffULL)
		return HAL_ERR_UNSUPPORTED;
	timer_interval = (uint32_t)interval;

	/* Stops the global timer, sets it to zero and starts it, without a prescaler. */
	hal_mmio_write32(global_timer + GLOBAL_CONTROL, 0);
	hal_mmio_write32(global_timer + GLOBAL_COUNTER_LOW, 0);
	hal_mmio_write32(global_timer + GLOBAL_COUNTER_HIGH, 0);
	hal_mmio_write32(global_timer + GLOBAL_CONTROL, GLOBAL_ENABLE);

	/* Succeeded: the timers can be armed. */
	return HAL_OK;
}

/* Starts the calling processor's private timer, one tick ahead and periodic. */
static void
timer_init_cpu(
	void)
{
	/* Stops the timer and drops an event it may hold. */
	hal_mmio_write32(private_timer + PRIVATE_CONTROL, 0);
	hal_mmio_write32(private_timer + PRIVATE_INTSTAT, PRIVATE_EVENT);

	/* Loads a tick's counts and starts it, reloading itself, with its interrupt. */
	hal_mmio_write32(private_timer + PRIVATE_LOAD, timer_interval - 1U);
	hal_mmio_write32(private_timer + PRIVATE_CONTROL, PRIVATE_ENABLE | PRIVATE_PERIODIC | PRIVATE_IT_ENABLE);
}

/*
 * Clears the private timer's event, which takes its interrupt down; the
 * timer has already reloaded for the next tick.
 */
static void
timer_ack_tick(
	void)
{
	/* The event flag is cleared by writing it. */
	hal_mmio_write32(private_timer + PRIVATE_INTSTAT, PRIVATE_EVENT);
}

/*
 * Reads the global timer's 64-bit count, the upper half before and after
 * the lower so that a carry between the two reads is seen.
 */
static bool
timer_read_counter(
	uint64_t *counter,
	uint64_t *frequency)
{
	uint32_t high;
	uint32_t low;
	uint32_t high_again;

	/* A timer not set up has no frequency. */
	if (timer_frequency == 0)
		return false;

	/* Reads the halves until the upper one held still across the lower. */
	for (;;) {
		high = hal_mmio_read32(global_timer + GLOBAL_COUNTER_HIGH);
		low = hal_mmio_read32(global_timer + GLOBAL_COUNTER_LOW);
		high_again = hal_mmio_read32(global_timer + GLOBAL_COUNTER_HIGH);
		if (high == high_again)
			break;
	}

	/* Succeeded: the count now and its frequency. */
	*counter = ((uint64_t)high << 32) | low;
	*frequency = timer_frequency;
	return true;
}

/* Reports the tick's interrupt. */
static uint32_t
timer_tick_irq(
	void)
{
	/* Succeeded: the private timer's. */
	return TIMER_PRIVATE_IRQ;
}
