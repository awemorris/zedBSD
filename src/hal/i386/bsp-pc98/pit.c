/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The PC-98 8253 interval-timer implementation.
 */

#include "../asm.h"
#include "../clock.h"
#include "../defs.h"
#include "../irq.h"
#include "../pic.h"

/*
 * Programs the PC-98 interval timer and enables its IRQ.
 */
void
bsp_timer_init(
	void)
{
	uint16_t interval;

	/* Programs channel zero for the board's fixed periodic interval. */
	interval = 19968;
	asm_outb(0x77, 0x34);
	asm_outb(0x71, (uint8_t)(interval & 0xff));
	asm_outb(0x71, (uint8_t)(interval >> 8));

	/* Enables timer delivery at the interrupt controller. */
	pic_set_irq_mask(IRQ_TIMER, 0);
}

/*
 * Handles board-specific work for one PC-98 timer tick.
 */
void
clock_handler(
	void)
{
	/* The generic timer path owns all work for this tick. */
}

/*
 * Reports that PC-98 has no monotonic firmware counter.
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
 * Reports that PC-98 has no supported epoch-time source.
 */
bool
hal_rtc_read_epoch_time(
	uint64_t *seconds)
{
	UNUSED_PARAMETER(seconds);

	/* Reports the absence of an epoch-time source. */
	return false;
}
