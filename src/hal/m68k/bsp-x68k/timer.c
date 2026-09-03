/* X68000 MFP Timer C scheduler clock. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#include <hal/hal.h>
#include "mmio.h"

#define X68K_MFP_CLOCK_HZ 4000000U
#define X68K_TIMER_VECTOR 0x45
#define MFP_IERB 4U
#define MFP_IMRB 10U
#define MFP_VR 11U
#define MFP_TCDCR 14U
#define MFP_TCDR 17U
#define MFP_TIMER_C_BIT 0x20U

static uint64_t timer_ticks;

static void
timer_interrupt(int irq, hal_irq_ack_t acknowledge, void *argument)
{
	(void)irq;
	(void)argument;
	timer_ticks++;
	kernel_timer_handler(0, acknowledge);
}

void
x68k_timer_init(uint32_t frequency)
{
	static const uint16_t divisors[] = { 4, 10, 16, 50, 64, 100, 200 };
	unsigned best_control = 0, index;
	uint32_t best_data = 0, best_error = UINT32_MAX;
	uint8_t control, enabled;

	if (frequency == 0)
		HAL_FATAL("invalid X68k timer frequency");
	for (index = 0; index < sizeof(divisors) / sizeof(divisors[0]); index++) {
		uint32_t denominator = (uint32_t)divisors[index] * frequency;
		uint32_t data = (X68K_MFP_CLOCK_HZ + denominator / 2U) / denominator;
		uint32_t actual, error;
		if (data == 0 || data > 255U)
			continue;
		actual = X68K_MFP_CLOCK_HZ / ((uint32_t)divisors[index] * data);
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
}

bool
hal_rtc_read_counter(uint64_t *counter, uint64_t *freq_hz)
{
	(void)counter;
	(void)freq_hz;
	return false;
}

bool
hal_rtc_read_epoch_time(uint64_t *unix_seconds)
{
	(void)unix_seconds;
	return false;
}
