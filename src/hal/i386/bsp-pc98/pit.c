/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The PC-98 8253 interval-timer implementation.
 */

#include <hal/hal.h>

#include "../asm.h"
#include "../clock.h"
#include "../defs.h"
#include "../irq.h"
#include "../pic.h"

/*
 * The BIOS work-area byte whose bit 7 names the system-clock series, and the
 * interval timer's input clock for each series.
 */
#define PC98_BIOS_SYSTEM_CLOCK	0x501U
#define PC98_SYSTEM_CLOCK_8MHZ	0x80U
#define PC98_PIT_CLOCK_8MHZ	1996800U
#define PC98_PIT_CLOCK_5MHZ	2457600U

/*
 * Programs the PC-98 interval timer and enables its IRQ.
 */
void
bsp_timer_init(
	void)
{
	uint32_t clock;
	uint16_t interval;

	/*
	 * Takes the timer's input clock from the series the BIOS reports:
	 * 1.9968 MHz on the 8 MHz series, 2.4576 MHz on the 5/10 MHz series.
	 */
	if ((*(volatile uint8_t *)(SYS_START + PC98_BIOS_SYSTEM_CLOCK) &
	    PC98_SYSTEM_CLOCK_8MHZ) != 0)
		clock = PC98_PIT_CLOCK_8MHZ;
	else
		clock = PC98_PIT_CLOCK_5MHZ;

	/* Programs channel zero for the HAL tick. */
	interval = (uint16_t)((clock + HAL_TIMER_FREQUENCY / 2U) /
	    HAL_TIMER_FREQUENCY);
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
