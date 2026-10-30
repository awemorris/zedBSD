/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The stages of the early boot, counted in blinks of the activity LED, so
 * that how far a board gets can be seen without a serial console or a
 * working display.  Each board drives its own LED (activity-led.c).
 */

#include <hal/hal.h>
#include "led.h"

/* How long a blink is lit and dark, and the pause after a stage. */
#define BLINK_ON_MS		250U
#define BLINK_OFF_MS		350U
#define STAGE_PAUSE_MS		1200U

static uint64_t counter(void);
static uint64_t counter_frequency(void);

/*
 * Shows a stage of the boot: as many blinks as its number, then a pause.
 */
void
rpi_led_stage(
	unsigned stage)
{
	unsigned blink;

	/* Blinks once for each step of the stage's number. */
	for (blink = 0; blink < stage; blink++) {
		rpi_led_set(1);
		rpi_delay_ms(BLINK_ON_MS);
		rpi_led_set(0);
		rpi_delay_ms(BLINK_OFF_MS);
	}

	/* The pause that separates one stage from the next. */
	rpi_delay_ms(STAGE_PAUSE_MS);
}

/*
 * Waits for a number of milliseconds on the generic timer.
 */
void
rpi_delay_ms(
	unsigned milliseconds)
{
	uint64_t frequency;
	uint64_t deadline;
	uint64_t now;

	/* The count the wait ends at. */
	frequency = counter_frequency();
	now = counter();
	deadline = now + frequency / 1000U * milliseconds;

	/* Samples the counter until it reaches the deadline. */
	while (now < deadline)
		now = counter();
}

/* Reads the physical count of the generic timer. */
static uint64_t
counter(
	void)
{
	uint64_t value;

	/* The barrier keeps the sample from being taken early. */
	__asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(value));

	/* Succeeded: the counter's value now. */
	return value;
}

/* Reads the frequency of the generic timer. */
static uint64_t
counter_frequency(
	void)
{
	uint64_t value;

	/* The firmware sets CNTFRQ_EL0 to the counter's rate in hertz. */
	__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(value));

	/* Succeeded: the counter's ticks per second. */
	return value;
}
