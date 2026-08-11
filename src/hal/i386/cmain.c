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

	/* Physical memory ranges (with BSP device windows reserved). */
	i386_page_init();
	i386_space_init();

	/* Build the IDT while interrupts remain disabled. */
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
