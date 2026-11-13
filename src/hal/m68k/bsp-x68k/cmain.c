/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The X68000's first C code: from the MC68030 bootstrap to kernel_entry().
 */

#include <hal/hal.h>
#include "bsp.h"
#include "../space.h"

/* The empty user root table the bootstrap built, in contract.S. */
extern char m68k030_bootstrap_empty_root[];

/*
 * Sets the exception vectors; in trap.c.
 */
void
m68k_int_init(
	void);

/*
 * Starts the periodic tick; in timer.c.
 */
void
x68k_timer_init(
	uint32_t frequency);

/*
 * Brings the processor from the bootstrap to kernel_entry().
 *
 * The console comes first, so the rest is seen; then the handoff, the
 * memory map, the MMU's user half, the vectors and the tick.
 */
void
m68k_x68k_cmain(
	const struct x68k_boot_handoff *handoff)
{
	/* Shows the console. */
	x68k_cons_init();
	hal_puts("X68K MC68030 ENTRY\n");

	/* Keeps the loader's handoff, which it checks. */
	x68k_boot_init(handoff);

	/* Sets up memory, the MMU and the vectors with interrupts off, then the tick. */
	(void)hal_irq_disable();
	m68k030_page_init();
	m68k030_space_init((uintptr_t)m68k030_bootstrap_empty_root);
	m68k_int_init();
	x68k_timer_init(hal_rtc_get_tick_hz());

	/* Enters the kernel, which never returns. */
	kernel_entry(x68k_kernel_handoff());
	HAL_FATAL("X68k kernel_entry returned");
}
