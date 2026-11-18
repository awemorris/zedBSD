/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The facts the armv7 HAL reads from the flattened device tree.
 */

#ifndef KERN_HAL_ARMV7_FDT_H
#define KERN_HAL_ARMV7_FDT_H

#include <hal/types.h>

/* The most entries of each kind the HAL keeps from the tree. */
#define ARMV7_FDT_MAX_CPUS		8U
#define ARMV7_FDT_MAX_MEMORY		8U
#define ARMV7_FDT_MAX_RESERVED		16U

/* The firmware call conduit the tree names for PSCI. */
#define ARMV7_FDT_PSCI_NONE		0U
#define ARMV7_FDT_PSCI_HVC		1U
#define ARMV7_FDT_PSCI_SMC		2U

/* Why a tree could not be read. */
enum armv7_fdt_error {
	ARMV7_FDT_OK = 0,
	ARMV7_FDT_BAD_MAGIC,
	ARMV7_FDT_BAD_SIZE,
	ARMV7_FDT_BAD_VERSION,
	ARMV7_FDT_BAD_STRUCTURE,
	ARMV7_FDT_TOO_DEEP,
	ARMV7_FDT_TOO_MANY
};

/*
 * One range of physical addresses: a bank of memory, a reservation or a
 * device's registers.
 */
struct armv7_fdt_range {
	uint64_t base;
	uint64_t size;
};

/*
 * Everything the HAL keeps from the tree, as found by armv7_fdt_parse().
 *
 * Strings stay in the tree: their places are recorded as offsets from its
 * start, so the record is valid only while the tree's memory is.  A zero
 * base or size means the tree has no such device.
 */
struct armv7_fdt_info {
	uint32_t totalsize;

	/* The processors under /cpus, by the affinity their reg gives. */
	unsigned cpu_count;
	uint32_t cpu_mpidr[ARMV7_FDT_MAX_CPUS];

	unsigned memory_count;
	struct armv7_fdt_range memory[ARMV7_FDT_MAX_MEMORY];

	/* Every reservation: the /memreserve/ block and /reserved-memory. */
	unsigned reserved_count;
	struct armv7_fdt_range reserved[ARMV7_FDT_MAX_RESERVED];

	/* The GIC's distributor and CPU interface, its first two reg entries. */
	struct armv7_fdt_range gic_distributor;
	struct armv7_fdt_range gic_cpu_interface;

	/*
	 * The architected timer: the private interrupts of its secure and
	 * non-secure physical timers as GIC interrupt numbers, and the
	 * frequency the tree states, zero when it leaves it to CNTFRQ.
	 */
	int has_timer;
	uint32_t timer_secure_irq;
	uint32_t timer_nonsecure_irq;
	uint32_t timer_frequency;

	/* The first PL011 UART and its interrupt as a GIC number. */
	struct armv7_fdt_range uart;
	uint32_t uart_irq;

	struct armv7_fdt_range fwcfg;

	/* The PSCI conduit, ARMV7_FDT_PSCI_*. */
	unsigned psci_method;

	uint32_t bootargs_offset;
	uint32_t bootargs_length;

	uint64_t initrd_start;
	uint64_t initrd_end;
};

int
armv7_fdt_parse(
	const void *blob,
	size_t limit,
	struct armv7_fdt_info *info);

const char *
armv7_fdt_error(
	int error);

#endif
