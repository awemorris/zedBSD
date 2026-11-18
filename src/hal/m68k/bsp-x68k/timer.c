/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The X68000's periodic tick, from the MFP's timer C.
 *
 * The timer divides the MFP's 4 MHz clock by a prescaler and a count; the
 * pair closest to the tick's rate is chosen.  The X68000 has no free
 * running counter the HAL reads, so the tick is the kernel's only clock.
 */

#include <hal/hal.h>
#include "../defs.h"
#include "mmio.h"

/*
 * The MFP's clock, timer C's vector, and the registers and bits it uses.
 *
 * Values referenced: the MC68901 MFP's register layout (interrupt enable
 * and mask B, vector register, timer C and D control, timer C data) and
 * the X68000's MFP clock and vector base.
 */
#define X68K_MFP_CLOCK_HZ	4000000U
#define X68K_TIMER_VECTOR	0x45
#define MFP_IERB		4U
#define MFP_IMRB		10U
#define MFP_VR			11U
#define MFP_TCDCR		14U
#define MFP_TCDR		17U
#define MFP_TIMER_C_BIT		0x20U
#define MFP_VECTOR_BASE		0x40U
#define MFP_TIMER_D_CONTROL	0x0fU

/* The tick's rate, in hertz, which never changes. */
#define X68K_TICK_HZ		100U

/* The largest count timer C takes. */
#define TIMER_DATA_MAX		255U

/*
 * The prescaler divisors of the timer, by their control value minus one.
 */
static const uint16_t timer_divisors[] = { 4, 10, 16, 50, 64, 100, 200 };

static void timer_interrupt(int irq, hal_irq_ack_t acknowledge, void *argument);

/*
 * Starts the periodic tick at a rate.
 */
void
x68k_timer_init(
	uint32_t frequency)
{
	uint32_t denominator;
	uint32_t data;
	uint32_t actual;
	uint32_t error_hz;
	uint32_t best_data;
	uint32_t best_error;
	unsigned best_control;
	unsigned index;
	uint8_t control;
	uint8_t enabled;
	int error;

	/* A tick needs a rate. */
	if (frequency == 0)
		HAL_FATAL("invalid X68k timer frequency");

	/* Picks the prescaler and the count that come closest to the rate. */
	best_control = 0;
	best_data = 0;
	best_error = UINT32_MAX;
	for (index = 0; index < sizeof(timer_divisors) / sizeof(timer_divisors[0]); index++) {
		/* The rounded count for this prescaler, if the timer can take it. */
		denominator = (uint32_t)timer_divisors[index] * frequency;
		data = (X68K_MFP_CLOCK_HZ + denominator / 2U) / denominator;
		if (data == 0 || data > TIMER_DATA_MAX)
			continue;

		/* Keeps the pair whose rate is the closest so far. */
		actual = X68K_MFP_CLOCK_HZ / ((uint32_t)timer_divisors[index] * data);
		error_hz = frequency - actual;
		if (actual > frequency)
			error_hz = actual - frequency;
		if (error_hz < best_error) {
			best_error = error_hz;
			best_control = index + 1U;
			best_data = data;
		}
	}
	if (best_control == 0)
		HAL_FATAL("unsupported X68k timer frequency");

	/* Stops timer C, keeping timer D, and sets the vector base and the count. */
	(void)hal_irq_disable();
	control = x68k_mfp_read(MFP_TCDCR);
	x68k_mfp_write(MFP_TCDCR, (uint8_t)(control & MFP_TIMER_D_CONTROL));
	x68k_mfp_write(MFP_VR, MFP_VECTOR_BASE);
	x68k_mfp_write(MFP_TCDR, (uint8_t)best_data);

	/* Routes the timer's vector to the tick. */
	error = hal_irq_register(X68K_TIMER_VECTOR, timer_interrupt, NULL);
	if (error != HAL_OK)
		HAL_FATAL("X68k timer vector is taken");

	/* Enables and unmasks timer C's interrupt. */
	enabled = x68k_mfp_read(MFP_IERB);
	x68k_mfp_write(MFP_IERB, (uint8_t)(enabled | MFP_TIMER_C_BIT));
	enabled = x68k_mfp_read(MFP_IMRB);
	x68k_mfp_write(MFP_IMRB, (uint8_t)(enabled | MFP_TIMER_C_BIT));

	/* Starts timer C with the chosen prescaler. */
	x68k_mfp_write(MFP_TCDCR, (uint8_t)((control & MFP_TIMER_D_CONTROL) | (best_control << 4)));
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
	return X68K_TICK_HZ;
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
	return X68K_TICK_HZ;
}

/*
 * Reports that no counter can be read: the X68000 has none the HAL reads.
 */
bool
hal_rtc_read_counter(
	uint64_t *counter,
	uint64_t *freq_hz)
{
	UNUSED_PARAMETER(counter);
	UNUSED_PARAMETER(freq_hz);

	/* Failed: no counter. */
	return false;
}

/*
 * Reports that the wall-clock time cannot be read: the board's clock chip
 * is not driven.
 */
bool
hal_rtc_read_epoch_time(
	uint64_t *unix_seconds)
{
	UNUSED_PARAMETER(unix_seconds);

	/* Failed: no clock is read. */
	return false;
}

/* Hands one tick to the kernel, which acknowledges it. */
static void
timer_interrupt(
	int irq,
	hal_irq_ack_t acknowledge,
	void *argument)
{
	UNUSED_PARAMETER(irq);
	UNUSED_PARAMETER(argument);

	/* The kernel's tick on the only processor. */
	kernel_timer_handler(0, acknowledge);
}
