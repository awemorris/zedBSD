/*
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 *
 * HAL C entry point, reached from locore with the GDT, initial paging,
 * the startup stack, and a panic IDT already in place.
 */

#include <hal/hal.h>
#include "i386.h"
#include "irq.h"
#include "clock.h"
#include "asm.h"
#include "space.h"

/* page.c */
void i386_page_init(void);
/* int.c */
void i386_int_init(void);
/* Kernel-side entry (the embedding kernel provides it). */
void kernel_entry(const void *handoff);

void
cmain(const void *handoff)
{
	/* First, the console, so every later failure can speak. */
	bsp_cons_init();
	/* hal_puts() decodes UTF-8 as a string.  hal_printf() emits literal
	 * format bytes one at a time, which is only suitable for ASCII text. */
	hal_puts("NEC PC-9800 ｼﾘｰｽﾞ ﾊﾟｰｿﾅﾙ ｺﾝﾋﾟｭｰﾀ\n\n");
	hal_puts("zedBSD ｵﾍﾟﾚｰﾃｨﾝｸﾞ ｼｽﾃﾑ ﾊﾞｰｼﾞｮﾝ 0.0.1\n\n");

	/* Physical memory ranges (with BSP device windows reserved). */
	hal_printf("boot: HAL physical-memory initialization\n");
	i386_page_init();
	hal_printf("boot: HAL address-space initialization\n");
	i386_space_init();

	/* Build the IDT while interrupts remain disabled. */
	hal_printf("boot: HAL interrupt and timer initialization\n");
	i386_int_init();

	/* Interrupt controller (all IRQs masked) and the interval timer. */
	irq_init();
	bsp_timer_init();
	/* kernel_entry enables interrupts after task/scheduler initialization. */

	/*
	 * Task management (i386_task_init) needs the kernel allocator and
	 * is brought up by the kernel once its heap exists, before the
	 * scheduler starts.
	 */

	kernel_entry(handoff);
}
