/* X68000 MFP Timer C scheduler clock. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#include <hal/hal.h>
#include <kern/sched.h>
#include "mmio.h"

#define X68K_MFP_CLOCK_HZ 4000000U
#define X68K_TIMER_VECTOR 0x45
#define MFP_IERB 4U
#define MFP_IMRB 10U
#define MFP_VR 11U
#define MFP_TCDCR 14U
#define MFP_TCDR 17U
#define MFP_TIMER_C_BIT 0x20U

static uint64 timer_ticks;
static uint32 timer_frequency;

static void
timer_interrupt(void *argument)
{
	(void)argument;
	timer_ticks++;
	kernel_timer_handler();
	sched_clock();
}

void
hal_timer_set_freq(uint32 frequency)
{
	static const uint16 divisors[] = { 4, 10, 16, 50, 64, 100, 200 };
	unsigned best_control = 0, index;
	uint32 best_data = 0, best_error = UINT32_MAX;
	uint8_t control, enabled;

	if (frequency == 0)
		HAL_FATAL("invalid X68k timer frequency");
	for (index = 0; index < sizeof(divisors) / sizeof(divisors[0]); index++) {
		uint32 denominator = (uint32)divisors[index] * frequency;
		uint32 data = (X68K_MFP_CLOCK_HZ + denominator / 2U) / denominator;
		uint32 actual, error;
		if (data == 0 || data > 255U)
			continue;
		actual = X68K_MFP_CLOCK_HZ / ((uint32)divisors[index] * data);
		error = actual > frequency ? actual - frequency : frequency - actual;
		if (error < best_error) {
			best_error = error;
			best_control = index + 1U;
			best_data = data;
		}
	}
	if (best_control == 0)
		HAL_FATAL("unsupported X68k timer frequency");

	(void)hal_irq_disable();
	control = x68k_mfp_read(MFP_TCDCR);
	x68k_mfp_write(MFP_TCDCR, (uint8_t)(control & 0x0fU));
	x68k_mfp_write(MFP_VR, 0x40U);
	x68k_mfp_write(MFP_TCDR, (uint8_t)best_data);
	hal_irq_set_handler(X68K_TIMER_VECTOR, timer_interrupt, NULL);
	enabled = x68k_mfp_read(MFP_IERB);
	x68k_mfp_write(MFP_IERB, (uint8_t)(enabled | MFP_TIMER_C_BIT));
	enabled = x68k_mfp_read(MFP_IMRB);
	x68k_mfp_write(MFP_IMRB, (uint8_t)(enabled | MFP_TIMER_C_BIT));
	x68k_mfp_write(MFP_TCDCR, (uint8_t)((control & 0x0fU) |
	    (best_control << 4)));
	timer_frequency = frequency;
}

uint64 hal_timer_get_tick(void) { return timer_ticks; }
uint64 hal_timer_read_rtc(void)
{ return timer_frequency != 0 ? timer_ticks / timer_frequency : 0; }
hal_clock_t clock_get_tick_count(void) { return (hal_clock_t)timer_ticks; }
