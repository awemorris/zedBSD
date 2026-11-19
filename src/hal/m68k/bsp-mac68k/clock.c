/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Macintosh's clock: VIA1's two timers.
 *
 * Timer 1 runs free from its latch and raises the tick, 100 times a
 * second.  Timer 2 counts down without stopping and wraps every 65536
 * counts; the HAL adds up how far it went at each tick and at each read,
 * which makes the counter the kernel measures time with.  A wrap is
 * 84 ms, so no turn is missed while the tick comes.
 *
 * Values referenced: Linux master arch/m68k/include/asm/mac_via.h (the
 * timer registers and the auxiliary control bits) and arch/m68k/mac/via.c
 * (the VIA clock, 783360 Hz); QEMU v10.0.0 hw/misc/mos6522.c (timer 1's
 * free-running period, the latch plus two counts).
 */

#include <hal/hal.h>
#include "../m68040/asm.h"
#include "../m68040/percpu.h"
#include "bsp.h"

/* The timer registers, 0x200 apart. */
#define VIA_T1C_LOW		0x0800U
#define VIA_T1C_HIGH		0x0a00U
#define VIA_T1L_LOW		0x0c00U
#define VIA_T1L_HIGH		0x0e00U
#define VIA_T2C_LOW		0x1000U
#define VIA_T2C_HIGH		0x1200U
#define VIA_ACR			0x1600U

/*
 * The auxiliary control: timer 1 free-running with no output on port B,
 * timer 2 counting the clock; the shift register's bits stay as they are.
 */
#define VIA_ACR_TIMER_MASK	0xe0U
#define VIA_ACR_T1_CONTINUOUS	0x40U

/* The clock the timers count. */
#define VIA_CLOCK_HZ		783360U

/* The tick's rate, in hertz, which never changes. */
#define CLOCK_TICK_HZ		100U

/* Timer 1's latch for the tick: its period is the latch plus two counts. */
#define CLOCK_TICK_LATCH	(VIA_CLOCK_HZ / CLOCK_TICK_HZ - 2U)

/*
 * The counts timer 2 has gone down since the clock started, and its value
 * at the last sample.  Kept with interrupts off; the total is zero until
 * the clock starts.
 */
static uint64_t counter_total;
static uint16_t counter_last;

/* Nonzero once the timers run. */
static int clock_started;

static volatile uint8_t *via1_register(uint32_t offset);
static uint16_t read_timer2(void);
static uint64_t sample_counter(void);

/*
 * Starts the counter and the tick.
 */
void
mac68k_clock_init(
	void)
{
	uint8_t value;
	bool enabled;

	/* Both timers are set up with interrupts off. */
	enabled = m68040_irq_save();

	/* Timer 1 free-running, timer 2 counting the clock. */
	value = *via1_register(VIA_ACR);
	*via1_register(VIA_ACR) = (uint8_t)((value & (uint8_t)~VIA_ACR_TIMER_MASK) | VIA_ACR_T1_CONTINUOUS);

	/* Timer 2 starts from the top; writing its high byte starts it. */
	*via1_register(VIA_T2C_LOW) = 0xff;
	*via1_register(VIA_T2C_HIGH) = 0xff;
	counter_last = read_timer2();
	counter_total = 0;

	/* Timer 1 loads its latch and starts; writing the counter's high byte starts it. */
	*via1_register(VIA_T1L_LOW) = (uint8_t)(CLOCK_TICK_LATCH & 0xffU);
	*via1_register(VIA_T1L_HIGH) = (uint8_t)(CLOCK_TICK_LATCH >> 8);
	*via1_register(VIA_T1C_LOW) = (uint8_t)(CLOCK_TICK_LATCH & 0xffU);
	*via1_register(VIA_T1C_HIGH) = (uint8_t)(CLOCK_TICK_LATCH >> 8);
	clock_started = 1;

	m68040_irq_restore(enabled);

	/* The tick's interrupt is let through. */
	mac68k_via_enable_clock();
}

/*
 * Serves the tick: the VIA cleared its flag, the counter is brought up to
 * date, and the kernel sees the tick.
 */
void
mac68k_clock_interrupt(
	void)
{
	struct m68040_percpu *cpu;

	/* Counts the tick and takes in timer 2's progress. */
	cpu = m68040_percpu_current();
	cpu->timer_ticks++;
	(void)sample_counter();

	/* Refuses a tick inside another interrupt. */
	if (cpu->active_ack != HAL_IRQ_ACK_NONE)
		HAL_FATAL("nested mac68k IRQ acknowledgement");

	/* The kernel ends the tick, which needs nothing more of the VIA, and schedules. */
	cpu->active_ack = M68040_ACK_LOCAL;
	kernel_timer_handler(cpu->logical_id, M68040_ACK_LOCAL);
}

/*
 * Waits a short while for the first tick and reports whether it came.
 * With interrupts off, a tick raised at the VIA and not yet taken counts
 * as come.
 */
bool
mac68k_clock_ticked(
	void)
{
	struct m68040_percpu *cpu;
	uint64_t deadline;
	uint64_t now;
	unsigned long ticks;
	bool masked;
	bool raised;

	/* A clock not started has nothing to wait for. */
	cpu = m68040_percpu_current();
	if (!clock_started)
		return false;

	/* Waits up to a tenth of a second for a tick, taken or raised. */
	masked = m68040_irq_masked();
	deadline = sample_counter() + VIA_CLOCK_HZ / 10U;
	do {
		ticks = __atomic_load_n(&cpu->timer_ticks, __ATOMIC_RELAXED);
		if (ticks != 0)
			return true;
		if (masked) {
			raised = mac68k_via_clock_pending();
			if (raised)
				return true;
		}

		/* Samples the counter for the next turn. */
		now = sample_counter();
	} while (now < deadline);

	/* Failed: no tick in the time allowed. */
	return false;
}

/*
 * Reports the tick's rate, which the timer runs at from boot and never
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
 * Keeps the tick's rate, which this timer does not change, and reports it.
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
 * Reads the counter and its frequency: timer 2's counts since the clock
 * started.
 */
bool
hal_rtc_read_counter(
	uint64_t *value,
	uint64_t *freq_hz)
{
	/* Refuses a caller with nowhere to put the answer, or a clock not started. */
	if (value == NULL || freq_hz == NULL)
		return false;
	if (!clock_started)
		return false;

	/* Succeeded: the count now and its frequency. */
	*value = sample_counter();
	*freq_hz = VIA_CLOCK_HZ;
	return true;
}

/*
 * Reports the wall-clock time.  The HAL does not read the machine's
 * real-time clock yet, so it has none to report.
 */
bool
hal_rtc_read_epoch_time(
	uint64_t *unix_seconds)
{
	UNUSED_PARAMETER(unix_seconds);

	/* Failed: no wall-clock time. */
	return false;
}

/* Reports one register of VIA1. */
static volatile uint8_t *
via1_register(
	uint32_t offset)
{
	/* The registers are bytes 0x200 apart. */
	return (volatile uint8_t *)(uintptr_t)(MAC68K_VIA1_BASE + offset);
}

/*
 * Reads timer 2.  The high byte is read on both sides of the low one; when
 * it changed, the low byte is read again, after the borrow.
 */
static uint16_t
read_timer2(
	void)
{
	uint8_t high;
	uint8_t low;
	uint8_t high_again;

	/* Reads high, low, high. */
	high = *via1_register(VIA_T2C_HIGH);
	low = *via1_register(VIA_T2C_LOW);
	high_again = *via1_register(VIA_T2C_HIGH);

	/* A borrow between the reads: the low byte is taken again with the new high byte. */
	if (high != high_again) {
		low = *via1_register(VIA_T2C_LOW);
		high = high_again;
	}

	/* Succeeded: the counter. */
	return (uint16_t)(((unsigned)high << 8) | low);
}

/*
 * Adds timer 2's progress since the last sample to the total and reports
 * the total.
 */
static uint64_t
sample_counter(
	void)
{
	uint16_t now;
	uint64_t total;
	bool enabled;

	/* The read and the sum are one step against the tick. */
	enabled = m68040_irq_save();

	now = read_timer2();
	counter_total += (uint16_t)(counter_last - now);
	counter_last = now;
	total = counter_total;

	m68040_irq_restore(enabled);

	/* Succeeded: the counts since the clock started. */
	return total;
}
