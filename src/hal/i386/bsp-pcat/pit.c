/* PC/AT 8254 PIT and CMOS clock. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <hal/hal.h>
#include "../asm.h"
#include "../clock.h"
#include "../irq.h"
#include "../pic.h"
#include "../../x86/rtc.h"
static uint8_t cmos_read(uint8_t index,void*context){uint8_t selector;(void)context;selector=asm_inb(0x70U);asm_outb(0x70U,(uint8_t)((selector&0x80U)|index));index=asm_inb(0x71U);asm_outb(0x70U,selector);return index;}
void bsp_timer_init(void){uint16_t divisor=(uint16_t)(1193182U/100U);asm_outb(0x43U,0x34U);asm_outb(0x40U,(uint8_t)divisor);asm_outb(0x40U,(uint8_t)(divisor>>8));pic_set_irq_mask(IRQ_TIMER,0);}
void clock_handler(void){}
bool hal_rtc_read(uint64_t*seconds){return x86_cmos_rtc_read(cmos_read,NULL,seconds);}
