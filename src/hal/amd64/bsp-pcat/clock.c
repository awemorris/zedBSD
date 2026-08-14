/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <hal/hal.h>
#include "../asm.h"
#include "../clock.h"
#include "../irq.h"
#include "../pic.h"

static hal_clock_t ticks;

void
bsp_timer_init(void)
{
	uint16_t divisor = (uint16_t)(1193182U / 100U);
	ticks = 0;
	asm_outb(0x43U, 0x34U);
	asm_outb(0x40U, (uint8_t)divisor);
	asm_outb(0x40U, (uint8_t)(divisor >> 8));
	pic_set_irq_mask(IRQ_TIMER, 0);
}

hal_clock_t clock_get_tick_count(void) { return ticks; }
void
clock_handler(void)
{
	ticks++;
	if (ticks == 1)
		hal_puts("A64 TIMER TICK\n");
}
