/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <hal/hal.h>
#include "../clock.h"
#include "lapic.h"

static int first_tick = 1;

void
bsp_timer_init(void)
{
	first_tick = 1;
	amd64_lapic_timer_start();
}

void
clock_handler(void)
{
	if (hal_cpu_current() == 0 &&
	    __atomic_exchange_n(&first_tick, 0, __ATOMIC_ACQ_REL) != 0) {
		hal_puts("A64 TIMER TICK\n");
	}
}

bool
hal_rtc_read(uint64 *unix_seconds)
{
	(void)unix_seconds;
	return false;
}
