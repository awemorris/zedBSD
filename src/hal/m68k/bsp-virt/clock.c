/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The virt board's clock: the first real-time clock counts nanoseconds of
 * wall time and raises its alarm interrupt at a set count, which the HAL
 * moves on by one tick each time it fires.
 *
 * Values referenced: QEMU v10.0.0 hw/rtc/goldfish_rtc.c (the registers:
 * time low and high, alarm low and high, interrupt enable, clear alarm,
 * alarm status, clear interrupt; the low word of the time latches the
 * high one, and the low word of the alarm arms it) and hw/m68k/virt.c
 * (the clock's interrupt, source 1 of the sixth controller).
 */

#include <hal/hal.h>
#include "../m68040/asm.h"
#include "../m68040/percpu.h"
#include "bsp.h"

/* The clock's registers, big-endian words. */
#define RTC_TIME_LOW		0x00U
#define RTC_TIME_HIGH		0x04U
#define RTC_ALARM_LOW		0x08U
#define RTC_ALARM_HIGH		0x0cU
#define RTC_IRQ_ENABLED		0x10U
#define RTC_CLEAR_INTERRUPT	0x1cU

/* The clock counts nanoseconds. */
#define RTC_FREQUENCY		1000000000ULL

/* The clock's interrupt number: source 1 of the sixth controller. */
#define RTC_IRQ			(1U + 5U * 32U)

/* The tick's rate, in hertz, which never changes. */
#define CLOCK_TICK_HZ		100U

/* The nanoseconds of one tick. */
#define TICK_NANOSECONDS	(RTC_FREQUENCY / CLOCK_TICK_HZ)

/* The count the next tick is due at; zero before the clock starts. */
static uint64_t next_deadline;

static volatile uint32_t *rtc_register(uint32_t offset);
static uint64_t read_time(void);
static void arm(uint64_t deadline);

/*
 * Starts the tick.
 */
void
virt_clock_init(
	void)
{
	const struct virt_board_info *board;

	/* The board must have the clock. */
	board = virt_board();
	if (board->rtc_base == 0)
		HAL_FATAL("m68k virt board has no clock");

	/* Arms the first tick one interval from now and lets its interrupt in. */
	next_deadline = read_time() + TICK_NANOSECONDS;
	arm(next_deadline);
	*rtc_register(RTC_IRQ_ENABLED) = 1U;
	virt_pic_enable_clock(RTC_IRQ);
}

/*
 * Serves the tick.  The next deadline moves on by whole intervals past
 * now and is armed before the kernel sees the tick.
 */
void
virt_clock_interrupt(
	void)
{
	struct m68040_percpu *cpu;
	uint64_t now;

	/* Lowers the interrupt and counts the tick. */
	*rtc_register(RTC_CLEAR_INTERRUPT) = 1U;
	cpu = m68040_percpu_current();
	cpu->timer_ticks++;

	/* Moves the deadline past now in whole intervals and arms it. */
	now = read_time();
	do {
		next_deadline += TICK_NANOSECONDS;
	} while (next_deadline <= now);
	arm(next_deadline);

	/* Refuses a tick inside another interrupt. */
	if (cpu->active_ack != HAL_IRQ_ACK_NONE)
		HAL_FATAL("nested m68k virt IRQ acknowledgement");

	/* The kernel ends the tick, which needs no controller, and schedules. */
	cpu->active_ack = M68040_ACK_LOCAL;
	kernel_timer_handler(cpu->logical_id, M68040_ACK_LOCAL);
}

/*
 * Waits a short while for the first tick and reports whether it came.
 * With interrupts off, a tick raised at the controller and not yet taken
 * counts as come.
 */
bool
virt_clock_ticked(
	void)
{
	struct m68040_percpu *cpu;
	uint64_t deadline;
	bool masked;
	unsigned long ticks;
	uint64_t now;
	bool raised;

	/* A clock not started has nothing to wait for. */
	cpu = m68040_percpu_current();
	if (next_deadline == 0)
		return false;

	/* Waits up to a tenth of a second for a tick, taken or raised. */
	masked = m68040_irq_masked();
	deadline = read_time() + RTC_FREQUENCY / 10U;
	do {
		ticks = __atomic_load_n(&cpu->timer_ticks, __ATOMIC_RELAXED);
		if (ticks != 0)
			return true;
		if (masked) {
			raised = virt_pic_pending(RTC_IRQ);
			if (raised)
				return true;
		}

		/* Samples the clock for the next turn. */
		now = read_time();
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
 * Reads the counter and its frequency: the clock's nanoseconds.
 */
bool
hal_rtc_read_counter(
	uint64_t *value,
	uint64_t *freq_hz)
{
	/* Refuses a caller with nowhere to put the answer, or a clock not started. */
	if (value == NULL || freq_hz == NULL)
		return false;
	if (next_deadline == 0)
		return false;

	/* Succeeded: the count now and its frequency. */
	*value = read_time();
	*freq_hz = RTC_FREQUENCY;
	return true;
}

/*
 * Reports the wall-clock time: the clock counts from the epoch.
 */
bool
hal_rtc_read_epoch_time(
	uint64_t *unix_seconds)
{
	const struct virt_board_info *board;

	/* Refuses a caller with nowhere to put the answer, or no clock. */
	board = virt_board();
	if (unix_seconds == NULL || board->rtc_base == 0)
		return false;

	/* Succeeded: whole seconds. */
	*unix_seconds = read_time() / RTC_FREQUENCY;
	return true;
}

/* Reports a register of the clock. */
static volatile uint32_t *
rtc_register(
	uint32_t offset)
{
	/* The first clock of the board. */
	return (volatile uint32_t *)(uintptr_t)(virt_board()->rtc_base + offset);
}

/* Reads the count: the low word first, which latches the high one. */
static uint64_t
read_time(
	void)
{
	uint32_t low;
	uint32_t high;
	bool enabled;

	/* The two reads must not be split by another reader. */
	enabled = m68040_irq_save();
	low = *rtc_register(RTC_TIME_LOW);
	high = *rtc_register(RTC_TIME_HIGH);
	m68040_irq_restore(enabled);

	/* Succeeded: the count now. */
	return ((uint64_t)high << 32) | low;
}

/* Arms the alarm: the high word first, then the low word, which arms it. */
static void
arm(
	uint64_t deadline)
{
	*rtc_register(RTC_ALARM_HIGH) = (uint32_t)(deadline >> 32);
	*rtc_register(RTC_ALARM_LOW) = (uint32_t)deadline;
}
