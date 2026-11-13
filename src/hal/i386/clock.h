/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The board-provided i386 interval-timer contract.
 */

#ifndef _SYS_ARCH_X86_CLOCK_H_
#define _SYS_ARCH_X86_CLOCK_H_

#include <hal/hal.h>

/*
 * The tick's rate, in hertz: the one PIT of the machine interrupts every
 * CPU's scheduler at this rate, which never changes.
 */
#define I386_TICK_HZ	100U

void bsp_timer_init(void);
void clock_handler(void);	/* tick, from irq.c */

#endif
