/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <hal/hal.h>
#include "../asm.h"
#include "../clock.h"
#include "../../x86/rtc.h"
#include "lapic.h"

static int first_tick = 1;

static uint8_t
cmos_read(uint8_t index, void *context)
{
	uint8_t selector;

	(void)context;
	selector = asm_inb(0x70U);
	asm_outb(0x70U, (uint8_t)((selector & 0x80U) | index));
	index = asm_inb(0x71U);
	asm_outb(0x70U, selector);
	return index;
}

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
hal_rtc_read(uint64_t *unix_seconds)
{
	return x86_cmos_rtc_read(cmos_read, NULL, unix_seconds);
}
