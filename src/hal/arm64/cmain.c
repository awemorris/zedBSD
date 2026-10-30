/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Raspberry Pi boot CPU's first C code: from locore.S to the kernel.
 */

#include <hal/hal.h>
#include "asm.h"
#include "bsp.h"
#include "defs.h"
#include "int.h"
#include "percpu.h"
#include "space.h"
#include "task.h"
#include "watch.h"
#include "fdt.h"
#include "framebuffer.h"
#include "led.h"

/* The most bytes of the firmware's device tree the parser may read. */
#define ARM64_FDT_READ_LIMIT (2U * 1024U * 1024U)

/* The RAM locore.S maps for the boot: the first gigabyte. */
#define BOOT_RAM_LIMIT 0x40000000UL

/*
 * Brings the boot CPU from locore.S to kernel_entry().
 *
 * The firmware hands over the physical address of its device tree.  The
 * console, the memory map, the page tables, the exception vectors, the
 * interrupt controller and the tick are set up in that order, each stage
 * counted on the activity LED.
 */
void
arm64_cmain(
	uintptr_t fdt_phys)
{
	const void *fdt;
	struct arm64_fdt_info info;
	uint64_t level;
	int error;
	int framebuffer;

	/* The boot CPU's record comes first: everything after may take a lock. */
	arm64_smp_bootstrap();

	/* Opens the early console and starts counting stages on the LED. */
	prekern_bsp_cons_init();
	arm64_led_init();
	arm64_led_stage(1);
	hal_puts(ARM64_BOARD_TAG " ENTRY\n");

	/* The kernel runs at EL1; locore.S has left EL2 if it entered there. */
	level = arm64_current_el();
	if (level != 1)
		HAL_FATAL("AArch64 kernel did not enter EL1");
	hal_puts(ARM64_BOARD_TAG " EL1 PASS\n");

	/* Requires the firmware's device tree. */
	if (fdt_phys == 0)
		HAL_FATAL("missing firmware FDT");

	/* The boot tables map only the first gigabyte of RAM, where the tree must lie. */
	if (fdt_phys >= BOOT_RAM_LIMIT - ARM64_FDT_READ_LIMIT)
		HAL_FATAL("firmware FDT above the boot map");

	/* Parses the tree through the direct map. */
	fdt = (const void *)(ARM64_DIRECT_BASE + fdt_phys);
	error = arm64_fdt_parse(fdt, ARM64_FDT_READ_LIMIT, &info);
	if (error != 0) {
		hal_printf("FDT parse error: %s\n", arm64_fdt_error(error));
		HAL_FATAL("invalid firmware FDT");
	}

	/* Reports the tree and the peripherals it names, and counts the stage. */
	hal_printf(ARM64_BOARD_TAG " FDT PASS phys=%p size=%u\n", (void *)fdt_phys, info.totalsize);
	arm64_led_stage(2);
	hal_printf(ARM64_BOARD_TAG " MMIO uart=%llx gic=%llx/%llx sd=%llx\n",
		   info.uart_base,
		   info.gic_dist_base,
		   info.gic_cpu_base,
		   info.sdhci_base);

	/* Keeps the tree's record for the rest of the HAL and the kernel's handoff. */
	arm64_boot_set_info(&info, fdt_phys);

	/* Fixes the number of CPUs before the kernel can ask for it. */
	arm64_smp_configure();

	/* Brings up the framebuffer console, or stays on the UART without one. */
	framebuffer = arm64_bsp_framebuffer_init();
	if (framebuffer == 0) {
		hal_puts(ARM64_BOARD_TAG " FRAMEBUFFER PASS\n");
		arm64_framebuffer_describe();
		arm64_led_stage(3);
		arm64_framebuffer_test_pattern();
		arm64_framebuffer_describe();
	} else {
		hal_puts(ARM64_BOARD_TAG " framebuffer unavailable; UART-only console\n");
		arm64_led_stage(6);
	}

	/* Sets up memory, the tables, the task switch, interrupts and debug. */
	arm64_page_init();
	arm64_space_init();
	arm64_watch_init();
	arm64_context_selftest();
	arm64_int_init();
	arm64_debug_init();
	prekern_bsp_cons_irq_init();
	hal_irq_enable();

	/* The LED stays lit once the kernel proper starts. */
	arm64_led_stage(4);
	arm64_led_set(1);
	kernel_entry(arm64_kernel_handoff());

	/* The kernel never returns; a halt keeps the CPU if it does. */
	for (;;)
		arm64_wfi();
}
