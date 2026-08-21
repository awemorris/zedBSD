/* zedBSD SPARC V9/sun4u early C entry. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#include <kern/sun4u/boot.h>
#include <hal/hal.h>
#include "bsp.h"
#include "space.h"
#include "trap.h"
#include "task.h"
#include "bsp-sun4u/uart.h"

void sparcv9_io_init(uint64 base);
void sparcv9_timer_init(uint64 frequency);

static int
handoff_valid(const struct sun4u_boot_handoff *handoff)
{
	return handoff != (const struct sun4u_boot_handoff *)0 &&
	    handoff->common.magic == ZEDBSD_HANDOFF_MAGIC &&
	    handoff->common.version == ZEDBSD_HANDOFF_VERSION_SUN4U &&
	    handoff->common.size == sizeof(*handoff) &&
	    handoff->common.boot_partition_scheme ==
	    ZEDBSD_PARTITION_SCHEME_SUN &&
	    handoff->extension_magic == ZEDBSD_SUN4U_HANDOFF_MAGIC &&
	    handoff->extension_version == ZEDBSD_SUN4U_HANDOFF_VERSION &&
	    handoff->extension_size == sizeof(*handoff) -
	    sizeof(handoff->common) && handoff->installed_count != 0 &&
	    handoff->installed_count <= ZEDBSD_SUN4U_MAX_MEMORY_RANGES &&
	    handoff->available_count != 0 && handoff->available_count <=
	    ZEDBSD_SUN4U_MAX_MEMORY_RANGES && handoff->tick_frequency != 0 &&
	    handoff->pci_io_base != 0 && handoff->serial_io_offset != 0 &&
	    handoff->ide_vendor == 0x1095 && handoff->ide_device == 0x0646;
}

void
sparcv9_cmain(const struct sun4u_boot_handoff *handoff)
{
	if (!handoff_valid(handoff))
		for (;;)
			__asm__ volatile("nop");
	sun4u_uart_init(handoff->pci_io_base, handoff->serial_io_offset);
	sun4u_uart_puts("SPARCV9 ENTRY\n");
	sun4u_uart_puts("SPARCV9 HANDOFF PASS\n");
	sun4u_boot_init(handoff);
	sun4u_cons_init();
	(void)hal_irq_disable();
	sparcv9_trap_init();
	sparcv9_page_init();
	sparcv9_space_init();
	sparcv9_io_init(handoff->pci_io_base);
	sparcv9_context_selftest();
	sparcv9_timer_init(handoff->tick_frequency);
	hal_puts("SPARCV9 TIMER PASS\n");
	kernel_entry(sun4u_boot_handoff());
	HAL_FATAL("SPARC V9 kernel_entry returned");
}
