/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#include <hal/hal.h>
#include "i386.h"
#include "irq.h"
#include "clock.h"
#include "asm.h"
#include "space.h"
#include "bsp.h"

void i386_page_init(void);
void i386_int_init(void);
void kernel_entry(const void *handoff);

void
cmain(const void *raw_boot_info)
{
	const void *handoff;

	bsp_boot_init(raw_boot_info);
	i386_bsp_cons_init();
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

	i386_page_init();
	i386_space_init();
	i386_int_init();
	irq_init();
	bsp_timer_init();

	handoff = bsp_kernel_handoff(raw_boot_info);
	kernel_entry(handoff);
}
