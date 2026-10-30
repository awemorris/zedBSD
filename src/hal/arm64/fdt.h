/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * What the HAL reads from the board's device tree.
 *
 * Each board has its own parser (bsp-rpi4/fdt.c, bsp-rpi5/fdt.c,
 * bsp-s905x5m-common/fdt.c), since the boards name their peripherals
 * differently, and each fills this record.
 */

#ifndef KERN_HAL_ARM64_FDT_H
#define KERN_HAL_ARM64_FDT_H

#include <hal/types.h>

/* The most RAM ranges, reserved ranges and CPUs the record holds. */
#define ARM64_FDT_MAX_MEMORY 8
#define ARM64_FDT_MAX_RESERVED 24
#define ARM64_FDT_MAX_NO_MAP 8
#define ARM64_FDT_MAX_CPUS 8

/*
 * One CPU the device tree names: its affinity (the cpu node's reg, which
 * is MPIDR_EL1's affinity fields) and how the firmware holds it back.
 */
struct arm64_fdt_cpu {
	uint64_t mpidr;

	/* Where a spin-table CPU waits for its entry address; 0 when none was given. */
	uint64_t release_addr;

	/* Nonzero when enable-method names "spin-table". */
	uint32_t spin_table;

	/* Nonzero when enable-method names "psci": the firmware starts the CPU on request. */
	uint32_t psci;
};

/*
 * One range of physical memory: RAM a memory node lists, or memory the
 * firmware keeps for itself.
 */
struct arm64_fdt_range {
	uint64_t base;
	uint64_t size;
};

/*
 * What the boot takes from the device tree.
 *
 * The boot CPU fills one at the start of the boot and keeps a copy for
 * the rest of the HAL (boot.c); nothing changes it afterwards except the
 * framebuffer's reservation.
 */
struct arm64_fdt_info {
	/* The blob's size from its header. */
	uint32_t totalsize;

	/* Nonzero when a node names the board the parser is built for. */
	int compatible_board;

	/* The RAM and the reserved ranges, each table filled from its start. */
	unsigned memory_count;
	unsigned reserved_count;
	struct arm64_fdt_range memory[ARM64_FDT_MAX_MEMORY];
	struct arm64_fdt_range reserved[ARM64_FDT_MAX_RESERVED];

	/*
	 * The reserved ranges the tree marks no-map (the secure monitor's
	 * memory): kept out of the kernel's direct map as well, so that no
	 * speculative access reaches memory the normal world may not touch.
	 * A tree with no such range leaves it empty.
	 */
	unsigned no_map_count;
	struct arm64_fdt_range no_map[ARM64_FDT_MAX_NO_MAP];

	/*
	 * The CPU addresses (and interrupts) of the peripherals the HAL uses; 0
	 * when absent.  The mailbox is the firmware's, on a board whose firmware
	 * has one.
	 */
	uint64_t uart_base;
	uint32_t uart_irq;
	uint64_t mailbox_base;
	uint64_t gic_dist_base;
	uint64_t gic_cpu_base;
	uint64_t sdhci_base;
	uint32_t sdhci_irq;

	/*
	 * A framebuffer the bootloader left the display scanning out (a
	 * simple-framebuffer node): its base and size, its size in pixels, its
	 * row length in bytes, 1 when red is in a pixel's low byte, and 1
	 * when the top byte is an alpha the display blends by.  Zero when the
	 * tree names none (a board whose firmware gives the framebuffer).
	 */
	uint64_t framebuffer_base;
	uint64_t framebuffer_size;
	uint32_t framebuffer_width;
	uint32_t framebuffer_height;
	uint32_t framebuffer_stride;
	uint32_t framebuffer_red_low;
	uint32_t framebuffer_alpha;

	/*
	 * Where /chosen/bootargs is in the blob (the firmware's command line,
	 * cmdline.txt), and its length with the terminator.  Zero when absent.
	 */
	uint32_t bootargs_offset;
	uint32_t bootargs_length;

	/* The CPUs of /cpus in the order the tree lists them. */
	unsigned cpu_count;
	struct arm64_fdt_cpu cpus[ARM64_FDT_MAX_CPUS];
};

int arm64_fdt_parse(const void *blob, size_t available, struct arm64_fdt_info *info);
const char *arm64_fdt_error(int error);

#endif
