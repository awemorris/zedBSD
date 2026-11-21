/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The board-provided i386 interval-timer contract.
 */

#ifndef HAL_I386_CLOCK_H
#define HAL_I386_CLOCK_H

#include <hal/hal.h>

/*
 * The tick's rate, in hertz: the one PIT of the machine interrupts every
 * CPU's scheduler at this rate, which never changes.  A build may set
 * another rate on the compiler's command line.  The PIT divides its input
 * clock by a 16-bit count, so the slowest rate is about 19 Hz on PC/AT
 * (1.193182 MHz) and 38 Hz on PC-98 (2.4576 MHz); a slower one does not
 * build.
 */
#ifndef I386_TICK_HZ
#define I386_TICK_HZ	100U
#endif

void bsp_timer_init(void);
void clock_handler(void);	/* tick, from irq.c */

#endif
