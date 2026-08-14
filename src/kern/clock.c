/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "kern/clock.h"

static volatile uint64_t kernel_ticks;

/* Until every BSP exposes a battery-backed RTC, use a deterministic valid
 * wall-clock epoch and advance it from the monotonic tick source. */
#define ZEDBSD_REALTIME_EPOCH_2026 1767225600L

void
kernel_timer_handler(void)
{
	kernel_ticks++;
}

uint64_t
zedbsd_kernel_ticks(void)
{
	uint64_t first, second;
	do {
		first = kernel_ticks;
		second = kernel_ticks;
	} while (first != second);
	return first;
}

uint64_t
zedbsd_kernel_milliseconds(void *context)
{
	(void)context;
	return zedbsd_kernel_ticks() * 10U;
}

void
zedbsd_clock_realtime(int32_t *seconds, int32_t *nanoseconds)
{
	uint64_t ticks = zedbsd_kernel_ticks();
	if (seconds != 0)
		*seconds = ZEDBSD_REALTIME_EPOCH_2026 + (int32_t)(ticks / 100U);
	if (nanoseconds != 0)
		*nanoseconds = (int32_t)((ticks % 100U) * 10000000U);
}
