/* zedBSD MC68030/X68000 early C entry. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#include <hal/hal.h>
#include "bsp.h"
#include "../space.h"

extern char m68k030_bootstrap_empty_root[];
void m68k_int_init(void);

void
m68k_x68k_cmain(const struct zedbsd_x68k_handoff *handoff)
{
	bsp_cons_init();
	hal_puts("X68K MC68030 ENTRY\n");
	if (!x68k_boot_handoff_valid(handoff))
		HAL_FATAL("invalid X68k loader handoff");
	x68k_boot_init(handoff);
	(void)hal_irq_disable();
	m68k030_page_init();
	m68k030_space_init((uintptr_t)m68k030_bootstrap_empty_root);
	m68k_int_init();
	hal_timer_set_freq(100);
	hal_puts("X68K MMU/VBR/TIMER PASS\n");
	kernel_entry(x68k_boot_handoff());
	HAL_FATAL("X68k kernel_entry returned");
}
