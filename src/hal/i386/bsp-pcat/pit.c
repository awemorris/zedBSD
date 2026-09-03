/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The PC/AT 8254 interval-timer and CMOS-clock implementation.
 */

#include <hal/hal.h>

#include "../../x86/rtc.h"
#include "../asm.h"
#include "../clock.h"
#include "../defs.h"
#include "../irq.h"
#include "../pic.h"

static uint8_t cmos_read(uint8_t index, void *context);

/*
 * Programs the PC/AT interval timer and enables its IRQ.
 */
void
bsp_timer_init(
	void)
{
	uint16_t divisor;

	/* Programs channel zero for the fixed 100 Hz periodic rate. */
	divisor = (uint16_t)(1193182U / 100U);
	asm_outb(0x43U, 0x34U);
	asm_outb(0x40U, (uint8_t)divisor);
	asm_outb(0x40U, (uint8_t)(divisor >> 8));

	/* Enables timer delivery at the interrupt controller. */
	pic_set_irq_mask(IRQ_TIMER, 0);
}

/*
 * Handles board-specific work for one PC/AT timer tick.
 */
void
clock_handler(
	void)
{
	/* The generic timer path owns all work for this tick. */
}

/*
 * Reports that PC/AT has no monotonic firmware counter.
 */
bool
hal_rtc_read_counter(
	uint64_t *counter,
	uint64_t *freq_hz)
{
	UNUSED_PARAMETER(counter);
	UNUSED_PARAMETER(freq_hz);

	/* Reports the absence of a monotonic RTC counter. */
	return false;
}

/*
 * Reads epoch time from the PC/AT CMOS clock.
 */
bool
hal_rtc_read_epoch_time(
	uint64_t *seconds)
{
	bool result;

	/* Samples the CMOS RTC through the NMI-preserving byte callback. */
	result = x86_cmos_rtc_read(cmos_read, NULL, seconds);

	/* Returns the CMOS sampling result. */
	return result;
}

/* Reads one CMOS byte while preserving the NMI selector bit. */
static uint8_t
cmos_read(
	uint8_t index,
	void *context)
{
	uint8_t selector;
	uint8_t value;

	UNUSED_PARAMETER(context);

	/* Selects and reads the CMOS register before restoring the selector. */
	selector = asm_inb(0x70U);
	asm_outb(0x70U, (uint8_t)((selector & 0x80U) | index));
	value = asm_inb(0x71U);
	asm_outb(0x70U, selector);

	/* Returns the sampled CMOS byte. */
	return value;
}
