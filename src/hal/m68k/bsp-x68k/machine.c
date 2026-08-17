/* X68000 reset and power-control termination paths. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#include <hal/hal.h>
#include "mmio.h"

void x68k_machine_reset(void) __attribute__((noreturn));

void
hal_reset(void)
{
	(void)hal_irq_disable();
	x68k_machine_reset();
}

void
hal_poweroff(void)
{
	(void)hal_irq_disable();

	/* X68000 system port 8 requires this exact three-write sequence. */
	x68k_sysport_write(X68K_SYSPORT_POWEROFF_REG, 0x00U);
	x68k_sysport_write(X68K_SYSPORT_POWEROFF_REG, 0x0fU);
	x68k_sysport_write(X68K_SYSPORT_POWEROFF_REG, 0x0fU);

	/* Machines without soft-power control must not resume the kernel. */
	for (;;)
		__asm__ volatile("stop #0x2700" ::: "memory");
}
