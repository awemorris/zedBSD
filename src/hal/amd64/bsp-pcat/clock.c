/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The amd64 PC/AT scheduler clock and real-time clock bridge.
 */

#include <hal/hal.h>
#include "../asm.h"
#include "../clock.h"
#include "../defs.h"
#include "../../x86/rtc.h"
#include "lapic.h"
#include "timecounter.h"

static int first_tick = 1;

static uint8_t cmos_read(uint8_t index, void *context);

/*
 * Starts the BSP local APIC scheduler timer.
 */
int
bsp_timer_init(
	void)
{
	int error;

	/* Rearms the one-time timer diagnostic. */
	first_tick = 1;

	/* Starts the periodic local APIC timer. */
	error = amd64_lapic_timer_start();
	if (error != HAL_OK)
		return error;

	/* Reports an operational scheduler timer. */
	hal_puts("A64 TIMER READY\n");

	/* Reports successful timer initialization. */
	return HAL_OK;
}

/*
 * Handles one local scheduler clock tick.
 */
void
clock_handler(
	void)
{
	hal_cpu_id_t cpu;

	/* Limits the one-time diagnostic to the BSP. */
	cpu = hal_cpu_current();
	if (cpu != 0)
		return;

	/* Publishes the first BSP tick exactly once. */
	if (__atomic_exchange_n(&first_tick, 0, __ATOMIC_ACQ_REL) != 0)
		hal_puts("A64 TIMER TICK\n");
}

/*
 * Reads the monotonic amd64 timecounter.
 */
bool
hal_rtc_read_counter(
	uint64_t *counter,
	uint64_t *frequency_hz)
{
	bool result;

	/* Delegates to the validated serialized TSC reader. */
	result = amd64_timecounter_read(counter, frequency_hz);

	/* Returns the timecounter availability result. */
	return result;
}

/*
 * Reads wall-clock epoch time from the PC/AT CMOS.
 */
bool
hal_rtc_read_epoch_time(
	uint64_t *unix_seconds)
{
	bool result;

	/* Uses the shared x86 RTC decoder with the ordered CMOS reader. */
	result = x86_cmos_rtc_read(cmos_read, NULL, unix_seconds);

	/* Returns the CMOS validation result. */
	return result;
}

/* Reads one CMOS register while preserving the NMI selector bit. */
static uint8_t
cmos_read(
	uint8_t index,
	void *context)
{
	uint8_t selector;
	uint8_t value;

	UNUSED_PARAMETER(context);

	/* Selects and reads the register without changing NMI state. */
	selector = asm_inb(0x70U);
	asm_outb(0x70U, (uint8_t)((selector & 0x80U) | index));
	value = asm_inb(0x71U);
	asm_outb(0x70U, selector);

	/* Returns the sampled CMOS byte. */
	return value;
}
