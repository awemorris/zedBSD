/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <hal/hal.h>
#include "../asm.h"
#include "../clock.h"
#include "../irq.h"
#include "../pic.h"

void
bsp_timer_init(void)
{
	uint16_t divisor = (uint16_t)(1193182U / 100U);
	asm_outb(0x43U, 0x34U);
	asm_outb(0x40U, (uint8_t)divisor);
	asm_outb(0x40U, (uint8_t)(divisor >> 8));
	pic_set_irq_mask(IRQ_TIMER, 0);
}

void clock_handler(void) { }

bool
hal_rtc_read(uint64 *unix_seconds)
{
	(void)unix_seconds;
	return false;
}
