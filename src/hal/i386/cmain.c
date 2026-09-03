/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The i386 C bootstrap entry point.
 */

#include <hal/hal.h>

#include "asm.h"
#include "bsp.h"
#include "clock.h"
#include "i386.h"
#include "irq.h"
#include "space.h"

void i386_page_init(void);
void i386_int_init(void);
void kernel_entry(const void *handoff);

/*
 * Initializes the i386 HAL and enters the kernel.
 */
void
cmain(
	const void *raw_boot_info)
{
	const void *handoff;

	/* Initializes the board handoff and the bootstrap console. */
	bsp_boot_init(raw_boot_info);
	i386_bsp_cons_init();

	/* Prints the board-specific startup banner. */
#if defined(HAL_BOARD_PC98)
	hal_puts("NEC PC-9800 ｼﾘｰｽﾞ ﾊﾟｰｿﾅﾙ ｺﾝﾋﾟｭｰﾀ\n\n");
#else
	hal_puts("\nzedBSD 0.0.1\n"
	    "Copyright (C) 2005, 2026, Awe Morris.\n\n");
#endif
#if defined(HAL_BOARD_PC98)
	hal_puts("zedBSD ｵﾍﾟﾚｰﾃｨﾝｸﾞ ｼｽﾃﾑ ﾊﾞｰｼﾞｮﾝ 0.0.1\n"
	    "Copyright (C) 2005, 2026, Awe Morris.\n\n");
#endif

	/* Initializes memory, interrupts, timers, and console input. */
	i386_page_init();
	i386_space_init();
	i386_int_init();
	irq_init();
	bsp_timer_init();
	(void)i386_interrupt_select();
	i386_bsp_cons_irq_init();

	/* Converts the board handoff and transfers control to the kernel. */
	handoff = bsp_kernel_handoff(raw_boot_info);
	kernel_entry(handoff);
}
