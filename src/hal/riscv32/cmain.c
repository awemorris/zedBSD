/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The riscv32 boot hart's first C code: from locore.S to the kernel.
 */

#include <hal/hal.h>
#include "asm.h"
#include "bootinfo.h"
#include "bsp.h"
#include "defs.h"
#include "fdt.h"
#include "int.h"
#include "page.h"
#include "percpu.h"
#include "sbi.h"
#include "space.h"
#include "task.h"

/* The most bytes of the firmware's device tree the parser may read. */
#define CMAIN_FDT_READ_LIMIT	(2U * 1024U * 1024U)

/*
 * The first word of a flattened device tree as it lies in memory, read as
 * a little-endian word: the big-endian magic 0xd00dfeed.
 *
 * Values referenced: Devicetree Specification (devicetree.org), the
 * flattened format's magic.
 */
#define CMAIN_FDT_MAGIC_LE	0xedfe0dd0U

/*
 * The device tree's record while it is parsed, before the boot record
 * keeps a copy.  It is too large for the boot stack.
 */
static struct riscv32_fdt_info cmain_fdt;

/*
 * Brings the boot hart from locore.S to kernel_entry().
 *
 * The firmware passes the hart's id and the physical address of its
 * device tree; locore.S adds where the image was loaded.  The tree is
 * read, the display set up, then the memory map, the tables, interrupts
 * and the tick, in that order.
 */
void
riscv32_cmain(
	unsigned long hart_id,
	uintptr_t boot_argument,
	uintptr_t kernel_phys_start)
{
	const volatile uint32_t *first_word;
	uint32_t magic;
	int error;

	/* The image's place and the boot hart's record come first: everything after may take a lock. */
	riscv32_kernel_image_init(kernel_phys_start);
	riscv32_smp_bootstrap(hart_id);

	/* Learns which firmware calls exist before the console uses them. */
	riscv32_sbi_probe();

	/* The boot argument must be a device tree: this entry is the firmware's own. */
	if (boot_argument == 0)
		HAL_FATAL("riscv32 firmware passed no device tree");
	first_word = (const volatile uint32_t *)riscv32_phys_to_direct((uint64_t)boot_argument);
	magic = *first_word;
	if (magic != CMAIN_FDT_MAGIC_LE)
		HAL_FATAL("riscv32 boot argument is not a device tree");

	/* Reads the tree through the direct map of the boot table. */
	error = riscv32_fdt_parse(riscv32_phys_to_direct((uint64_t)boot_argument), CMAIN_FDT_READ_LIMIT, &cmain_fdt);
	if (error != RISCV32_FDT_OK) {
		hal_printf("riscv32 device tree: %s\n", riscv32_fdt_error(error));
		HAL_FATAL("invalid riscv32 device tree");
	}

	/* Keeps the boot record and prepares the kernel's handoff. */
	riscv32_boot_record(hart_id, boot_argument, &cmain_fdt);

	/* Shows the display, which then holds what the console printed. */
	riscv32_board_init(riscv32_boot_fdt());

	/* Describes what was found. */
	hal_printf("RISCV32 ENTRY hart=%lu image=%lx\n", hart_id, (unsigned long)kernel_phys_start);
	riscv32_sbi_describe();
	hal_printf("RISCV32 FDT PASS phys=%lx size=%u timebase=%llu\n",
		   (unsigned long)boot_argument,
		   cmain_fdt.totalsize,
		   cmain_fdt.timebase_frequency);
	riscv32_board_describe();

	/* Fixes the number of harts before the kernel can ask for it. */
	riscv32_smp_configure();

	/* Sets up memory, the tables, the task switch and interrupts. */
	riscv32_page_init();
	riscv32_space_init();
	riscv32_context_selftest();
	riscv32_int_init();
	hal_irq_enable();

	/* Enters the kernel, which never returns. */
	kernel_entry(riscv32_kernel_handoff());
	HAL_FATAL("kernel_entry returned");
}
