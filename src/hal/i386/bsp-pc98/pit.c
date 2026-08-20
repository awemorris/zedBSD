/* PC-98 8253 PIT clock source. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "../clock.h"
#include "../irq.h"
#include "../asm.h"
#include "../pic.h"
void bsp_timer_init(void){uint16 interval=19968;asm_outb(0x77,0x34);asm_outb(0x71,(uint8)(interval&0xff));asm_outb(0x71,(uint8)(interval>>8));pic_set_irq_mask(IRQ_TIMER,0);}
void clock_handler(void){}
bool hal_rtc_read(uint64*seconds){(void)seconds;return false;}
