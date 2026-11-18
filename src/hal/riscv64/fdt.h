/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The facts the riscv64 HAL reads from the flattened device tree.
 */

#ifndef KERN_HAL_RISCV64_FDT_H
#define KERN_HAL_RISCV64_FDT_H

#include <hal/types.h>

/* The most entries of each kind the HAL keeps from the tree. */
#define RISCV64_FDT_MAX_CPUS		16U
#define RISCV64_FDT_MAX_MEMORY		8U
#define RISCV64_FDT_MAX_RESERVED	16U
#define RISCV64_FDT_MAX_CONTEXTS	64U

/* What the tree says the console UART is. */
#define RISCV64_FDT_UART_NONE		0U
#define RISCV64_FDT_UART_NS16550A	1U
#define RISCV64_FDT_UART_DW_APB		2U

/* The extensions of the boot processor the HAL records for the log. */
#define RISCV64_FDT_EXT_SSTC		0x1U
#define RISCV64_FDT_EXT_SVPBMT		0x2U
#define RISCV64_FDT_EXT_ZICBOM		0x4U
#define RISCV64_FDT_EXT_ZKR		0x8U

/* Why a tree could not be read. */
enum riscv64_fdt_error {
	RISCV64_FDT_OK = 0,
	RISCV64_FDT_BAD_MAGIC,
	RISCV64_FDT_BAD_SIZE,
	RISCV64_FDT_BAD_VERSION,
	RISCV64_FDT_BAD_STRUCTURE,
	RISCV64_FDT_TOO_DEEP,
	RISCV64_FDT_TOO_MANY
};

/*
 * One range of physical addresses: a bank of memory or a reservation.
 */
struct riscv64_fdt_range {
	uint64_t base;
	uint64_t size;
};

/*
 * One processor the tree lists under /cpus.
 *
 * usable says whether the HAL may run the kernel on it: the processor has
 * a translation mode (mmu-type), the base extensions the system is built
 * for (imafdc) and is not disabled.  A monitor core, which lacks the
 * translation mode or the float extensions, is listed but not usable.
 */
struct riscv64_fdt_cpu {
	uint64_t hart_id;
	uint32_t intc_phandle;
	int has_intc;
	int usable;
	uint32_t extensions;
};

/*
 * Everything the HAL keeps from the tree, as found by riscv64_fdt_parse().
 *
 * Strings stay in the tree: their places are recorded as offsets from its
 * start, so the record is valid only while the tree's memory is.  A zero
 * base or size means the tree has no such device.
 */
struct riscv64_fdt_info {
	uint32_t totalsize;
	uint64_t timebase_frequency;

	unsigned cpu_count;
	struct riscv64_fdt_cpu cpus[RISCV64_FDT_MAX_CPUS];

	unsigned memory_count;
	struct riscv64_fdt_range memory[RISCV64_FDT_MAX_MEMORY];

	/* Every reservation: the /memreserve/ block and /reserved-memory. */
	unsigned reserved_count;
	struct riscv64_fdt_range reserved[RISCV64_FDT_MAX_RESERVED];

	/* The reservations marked no-map, which must not be mapped at all. */
	unsigned nomap_count;
	struct riscv64_fdt_range nomap[RISCV64_FDT_MAX_RESERVED];

	uint64_t plic_base;
	uint64_t plic_size;
	uint32_t plic_sources;

	/* The PLIC's contexts in order: the phandle of each one's hart controller, and its interrupt. */
	unsigned plic_context_count;
	uint32_t plic_context_phandle[RISCV64_FDT_MAX_CONTEXTS];
	uint32_t plic_context_irq[RISCV64_FDT_MAX_CONTEXTS];

	unsigned uart_kind;
	uint64_t uart_base;
	uint64_t uart_size;
	uint32_t uart_reg_shift;
	uint32_t uart_reg_io_width;
	uint32_t uart_irq;

	uint64_t fwcfg_base;
	uint64_t fwcfg_size;

	uint64_t ccache_base;
	uint64_t ccache_size;

	uint32_t bootargs_offset;
	uint32_t bootargs_length;

	uint64_t initrd_start;
	uint64_t initrd_end;
};

int
riscv64_fdt_parse(
	const void *blob,
	size_t limit,
	struct riscv64_fdt_info *info);

const char *
riscv64_fdt_error(
	int error);

int
riscv64_fdt_s_context(
	const struct riscv64_fdt_info *info,
	uint64_t hart_id,
	unsigned *context);

#endif
