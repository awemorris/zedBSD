/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <hal/hal.h>
#include "../asm.h"
#include "../clock.h"
#include "../../x86/rtc.h"
#include "lapic.h"
#include "timecounter.h"

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

int
bsp_timer_init(void)
{
	int error;

	first_tick = 1;
	error = amd64_lapic_timer_start();
	if (error != HAL_OK)
		return error;
	hal_puts("A64 TIMER READY\n");
	return HAL_OK;
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
hal_rtc_read_counter(uint64_t *counter, uint64_t *freq_hz)
{
	return amd64_timecounter_read(counter, freq_hz);
}

bool
hal_rtc_read_epoch_time(uint64_t *unix_seconds)
{
	return x86_cmos_rtc_read(cmos_read, NULL, unix_seconds);
}
