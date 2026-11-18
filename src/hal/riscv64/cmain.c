/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The riscv64 boot hart's first C code: from locore.S to the kernel.
 */

#include <hal/hal.h>
#include "asm.h"
#include "bootinfo.h"
#include "bsp.h"
#include "defs.h"
#include "fdt.h"
#include "int.h"
#include "loader.h"
#include "page.h"
#include "percpu.h"
#include "sbi.h"
#include "space.h"
#include "task.h"
#include "bootloader/include/riscv64-handoff.h"

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
static struct riscv64_fdt_info cmain_fdt;

static uintptr_t take_loader_handoff(uint64_t hart_id, uintptr_t handoff_phys);
static void check_device_tree(uintptr_t fdt_phys);
static void check_firmware_reserved(int error);

/*
 * Brings the boot hart from locore.S to kernel_entry().
 *
 * The firmware passes the hart's id and the physical address of its
 * device tree, or the UEFI loader the hart's id and its handoff, which
 * names the tree; locore.S adds where the image was loaded.  The tree is
 * read, the loader's additions taken, the display set up, then the memory
 * map, the tables, interrupts and the tick, in that order.
 */
void
riscv64_cmain(
	uint64_t hart_id,
	uintptr_t boot_argument,
	uint64_t kernel_phys_start)
{
	const volatile uint32_t *first_word;
	const struct zbl6_riscv64_handoff *handoff;
	uintptr_t fdt_phys;
	uint32_t magic;
	int loader_error;
	int error;

	/* The image's place and the boot hart's record come first: everything after may take a lock. */
	riscv64_kernel_image_init(kernel_phys_start);
	riscv64_smp_bootstrap(hart_id);

	/* Learns which firmware calls exist before the console uses them. */
	riscv64_sbi_probe();

	/* Refuses a boot without an argument. */
	if (boot_argument == 0)
		HAL_FATAL("riscv64 firmware passed no device tree");

	/* Reads the argument's first word: it is the loader's handoff or the firmware's device tree. */
	first_word = (const volatile uint32_t *)riscv64_phys_to_direct((uint64_t)boot_argument);
	magic = *first_word;

	/* Finds the tree: the loader names its copy, the firmware passes its own. */
	handoff = NULL;
	if (magic == ZBL6_HANDOFF_MAGIC) {
		fdt_phys = take_loader_handoff(hart_id, boot_argument);
		handoff = (const struct zbl6_riscv64_handoff *)riscv64_phys_to_direct((uint64_t)boot_argument);
	} else {
		fdt_phys = boot_argument;
	}

	/* Stops unless the tree is where the argument says. */
	check_device_tree(fdt_phys);

	/* Reads the tree through the direct map of the boot table. */
	error = riscv64_fdt_parse(riscv64_phys_to_direct((uint64_t)fdt_phys), CMAIN_FDT_READ_LIMIT, &cmain_fdt);
	if (error != RISCV64_FDT_OK) {
		hal_printf("riscv64 device tree: %s\n", riscv64_fdt_error(error));
		HAL_FATAL("invalid riscv64 device tree");
	}

	/*
	 * The loader's ramdisk and parameters stand in for the tree's: the
	 * firmware's tree describes no ramdisk the loader read, and its line is
	 * not the kernel's.
	 */
	if (handoff != NULL) {
		cmain_fdt.initrd_start = handoff->initrd_phys;
		cmain_fdt.initrd_end = handoff->initrd_phys + handoff->initrd_size;
	}

	/* Keeps the tree's facts and the boot hart, with the loader's parameter line when there is one. */
	riscv64_boot_record(hart_id, fdt_phys, &cmain_fdt);
	if (handoff != NULL)
		riscv64_boot_set_command_line(handoff->parameters.text, handoff->parameters.length);

	/* Takes the screen and the firmware's RAM a loader of the second layout names. */
	loader_error = RISCV64_LOADER_OK;
	if (handoff != NULL)
		loader_error = riscv64_boot_take_loader(handoff);

	/* Shows the display, which then holds what the console printed. */
	riscv64_board_init(riscv64_boot_fdt());

	/* Stops, now that the screen shows why, when the firmware's RAM cannot all be kept. */
	check_firmware_reserved(loader_error);

	/* Describes what was found. */
	hal_printf("RISCV64 ENTRY hart=%lu image=%lx\n", hart_id, kernel_phys_start);
	riscv64_sbi_describe();
	hal_printf("RISCV64 FDT PASS phys=%lx size=%u timebase=%lu\n",
		   (uint64_t)fdt_phys,
		   cmain_fdt.totalsize,
		   cmain_fdt.timebase_frequency);
	riscv64_board_describe();

	/* Fixes the number of harts before the kernel can ask for it. */
	riscv64_smp_configure();

	/* Sets up memory, the tables, the task switch and interrupts. */
	riscv64_page_init();
	riscv64_space_init();
	riscv64_context_selftest();
	riscv64_int_init();
	hal_irq_enable();

	/* Enters the kernel, which never returns. */
	kernel_entry(riscv64_kernel_handoff());
	HAL_FATAL("kernel_entry returned");
}

/*
 * Checks the UEFI loader's handoff and reports the device tree it names.
 *
 * The record is read here and not kept: the tree and the ramdisk it names
 * stay where the loader put them, and the parameters are copied out.
 */
static uintptr_t
take_loader_handoff(
	uint64_t hart_id,
	uintptr_t handoff_phys)
{
	const struct zbl6_riscv64_handoff *handoff;
	int readable;

	/* Views the record through the direct map of the boot table. */
	handoff = (const struct zbl6_riscv64_handoff *)riscv64_phys_to_direct((uint64_t)handoff_phys);

	/*
	 * Refuses a record older than the first layout or shorter than it.  A
	 * later layout only appends fields, so the ones read here stay where
	 * they are, and a loader of the first layout still boots this kernel.
	 */
	readable = riscv64_loader_check(handoff);
	if (readable == RISCV64_LOADER_OLD_VERSION)
		HAL_FATAL("riscv64 loader handoff of an unknown version");
	if (readable == RISCV64_LOADER_TOO_SHORT)
		HAL_FATAL("riscv64 loader handoff is too short");

	/* Refuses a record for another hart: the others are still in the firmware. */
	if (handoff->boot_hart != hart_id)
		HAL_FATAL("riscv64 loader handoff names another boot hart");

	/* Refuses a parameter line longer than its record. */
	if (handoff->parameters.length >= sizeof(handoff->parameters.text))
		HAL_FATAL("riscv64 loader parameters are too long");

	/* Refuses a ramdisk whose end does not fit. */
	if (handoff->initrd_size > ~0ULL - handoff->initrd_phys)
		HAL_FATAL("riscv64 loader ramdisk range overflows");

	/* Succeeded: the loader's copy of the device tree. */
	hal_printf("RISCV64 LOADER handoff=%lx fdt=%lx initrd=%lx+%lx\n",
		   (uint64_t)handoff_phys,
		   handoff->fdt_phys,
		   handoff->initrd_phys,
		   handoff->initrd_size);
	return (uintptr_t)handoff->fdt_phys;
}

/* Stops on a boot argument that does not lead to a device tree. */
static void
check_device_tree(
	uintptr_t fdt_phys)
{
	const volatile uint32_t *first_word;
	uint32_t magic;

	/* Refuses a missing tree. */
	if (fdt_phys == 0)
		HAL_FATAL("riscv64 firmware passed no device tree");

	/* Refuses memory that does not start with the tree's magic. */
	first_word = (const volatile uint32_t *)riscv64_phys_to_direct((uint64_t)fdt_phys);
	magic = *first_word;
	if (magic != CMAIN_FDT_MAGIC_LE)
		HAL_FATAL("riscv64 boot argument is not a device tree");
}

/*
 * Stops the boot when the firmware's RAM could not all be kept from the
 * allocator, rather than hand firmware memory to the kernel.
 */
static void
check_firmware_reserved(
	int error)
{
	/* Names why, one message for each reason. */
	switch (error) {
	case RISCV64_LOADER_OK:
		return;
	case RISCV64_LOADER_BAD_COUNT:
		HAL_FATAL("riscv64 loader handoff counts more firmware ranges than it holds");
		break;
	case RISCV64_LOADER_BAD_RANGE:
		HAL_FATAL("riscv64 loader handoff names a firmware range that wraps");
		break;
	case RISCV64_LOADER_OVERFLOW:
		HAL_FATAL("riscv64 firmware keeps more ranges than the loader handed over");
		break;
	case RISCV64_LOADER_TOO_MANY:
		HAL_FATAL("riscv64 firmware keeps more ranges than the HAL holds");
		break;
	default:
		HAL_FATAL("riscv64 loader handoff could not be read");
		break;
	}
}
