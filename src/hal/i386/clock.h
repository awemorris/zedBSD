/*
 * Interval timer (BSP-provided PIT programming).
 */

#ifndef _SYS_ARCH_X86_CLOCK_H_
#define _SYS_ARCH_X86_CLOCK_H_

#include <hal/hal.h>

void bsp_timer_init(void);
void clock_handler(void);	/* tick, from irq.c */

#endif
