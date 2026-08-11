/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "kern/clock.h"

static volatile uint64_t kernel_ticks;

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
