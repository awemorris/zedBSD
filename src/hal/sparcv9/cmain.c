/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The SPARC V9 HAL's first C code: from locore.S to kernel_entry().
 */

#include <kern/boot.h>
#include <hal/hal.h>
#include "bsp.h"
#include "io.h"
#include "page.h"
#include "space.h"
#include "timer.h"
#include "trap.h"
#include "bsp-sun4u/console.h"
#include "bsp-sun4u/framebuffer.h"
#include "bsp-sun4u/uart.h"

/* The PCI identity of the only IDE controller the HAL accepts, the CMD646. */
#define CMAIN_IDE_VENDOR	0x1095U
#define CMAIN_IDE_DEVICE	0x0646U

static int handoff_valid(const struct sun4u_boot_handoff *handoff);
static int handoff_identity_valid(const struct sun4u_boot_handoff *handoff);
static int handoff_machine_valid(const struct sun4u_boot_handoff *handoff);

/*
 * Brings the processor from locore.S to kernel_entry().
 *
 * The loader passes its handoff.  The serial port and the screen come
 * first, so the rest is seen; then the traps, the memory map, the MMU and
 * the tick, in that order.
 */
void
sparcv9_cmain(
	const struct sun4u_boot_handoff *handoff)
{
	int valid;
	int shown;

	/* A handoff the HAL cannot read leaves nothing to report to: it spins. */
	valid = handoff_valid(handoff);
	if (!valid) {
		for (;;)
			__asm__ volatile("nop");
	}

	/* Starts the serial port and keeps a copy of the handoff. */
	sun4u_uart_init(handoff->pci_io_base, handoff->serial_io_offset);
	sun4u_boot_init(handoff);
	sparcv9_io_init(handoff->pci_io_base);

	/* Shows the screen, which then holds what the console printed. */
	shown = sun4u_framebuffer_init(handoff);
	if (shown == HAL_OK)
		sun4u_console_attach();
	hal_printf("SPARCV9 ENTRY tick=%lu display=%x\n", handoff->tick_frequency, (uint32_t)handoff->display_vendor);

	/* Sets up the traps, memory and the MMU with interrupts off. */
	(void)hal_irq_disable();
	sparcv9_trap_init();
	sparcv9_page_init();
	sparcv9_space_init();

	/* Starts the tick; the kernel enables interrupts when it is ready for it. */
	sparcv9_timer_init(handoff->tick_frequency);

	/* Enters the kernel, which never returns. */
	kernel_entry(sun4u_kernel_handoff());
	HAL_FATAL("SPARC V9 kernel_entry returned");
}

/* Reports whether the loader's handoff is one the HAL can use. */
static int
handoff_valid(
	const struct sun4u_boot_handoff *handoff)
{
	int identity;
	int machine;

	/* Refuses a missing record. */
	if (handoff == NULL)
		return 0;

	/* Refuses a record of another layout. */
	identity = handoff_identity_valid(handoff);
	if (!identity)
		return 0;

	/* Refuses a machine the HAL cannot run on. */
	machine = handoff_machine_valid(handoff);
	if (!machine)
		return 0;

	/* The handoff is usable. */
	return 1;
}

/* Reports whether a handoff has the layout this HAL was built for. */
static int
handoff_identity_valid(
	const struct sun4u_boot_handoff *handoff)
{
	/* The common record names the loader's version and size, and a Sun label. */
	if (handoff->common.magic != KERN_HANDOFF_MAGIC)
		return 0;
	if (handoff->common.version != SUN4U_HANDOFF_COMMON_VERSION)
		return 0;
	if (handoff->common.size != sizeof(*handoff))
		return 0;
	if (handoff->common.boot_partition_scheme != KERN_PARTITION_SCHEME_SUN)
		return 0;

	/* The sun4u part names its magic, version and size. */
	if (handoff->extension_magic != SUN4U_HANDOFF_EXTENSION_MAGIC)
		return 0;
	if (handoff->extension_version != SUN4U_HANDOFF_EXTENSION_VERSION)
		return 0;
	if (handoff->extension_size != sizeof(*handoff) - sizeof(handoff->common))
		return 0;

	/* The layout is this HAL's. */
	return 1;
}

/* Reports whether a handoff describes a machine the HAL runs on. */
static int
handoff_machine_valid(
	const struct sun4u_boot_handoff *handoff)
{
	/* Some installed and some available memory, within what the record holds. */
	if (handoff->installed_count == 0 || handoff->installed_count > SUN4U_MAX_MEMORY_RANGES)
		return 0;
	if (handoff->available_count == 0 || handoff->available_count > SUN4U_MAX_MEMORY_RANGES)
		return 0;

	/* A tick that counts, the PCI I/O space and the serial port. */
	if (handoff->tick_frequency == 0)
		return 0;
	if (handoff->pci_io_base == 0 || handoff->serial_io_offset == 0)
		return 0;

	/* The CMD646 IDE controller the kernel's disk driver drives. */
	if (handoff->ide_vendor != CMAIN_IDE_VENDOR || handoff->ide_device != CMAIN_IDE_DEVICE)
		return 0;

	/* The machine is one the HAL runs on. */
	return 1;
}
