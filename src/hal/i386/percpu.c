/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The i386 per-CPU GDT and task-state implementation.
 */

#include <hal/hal.h>

#include "apic-topology.h"
#include "defs.h"
#include "percpu.h"

struct gdtr {
	uint16_t limit;
	uint32_t base;
} __attribute__((packed));

static uint64_t gdts[I386_APIC_MAX_CPUS][6] __attribute__((aligned(16)));
static uint8_t tsses[I386_APIC_MAX_CPUS][104] __attribute__((aligned(16)));

static uint64_t tss_descriptor(uintptr_t base_address);

/*
 * Initializes the GDT and task-state segment for one CPU.
 */
void
i386_percpu_init(
	hal_cpu_id_t cpu,
	uintptr_t stack)
{
	struct gdtr descriptor;
	uint32_t *tss;
	uint16_t selector;

	/* Rejects a logical CPU outside the fixed per-CPU tables. */
	if (cpu >= I386_APIC_MAX_CPUS)
		HAL_FATAL("invalid i386 per-CPU ID");

	/* Builds the null, kernel, and user segment descriptors. */
	gdts[cpu][0] = 0;
	gdts[cpu][1] = 0x00cf9a000000ffffULL;
	gdts[cpu][2] = 0x00cf92000000ffffULL;
	gdts[cpu][3] = 0x00cff8000000ffffULL;
	gdts[cpu][4] = 0x00cff2000000ffffULL;

	/* Initializes the CPU's task-state segment and kernel stack. */
	hal_memset(tsses[cpu], 0, sizeof(tsses[cpu]));
	tss = (uint32_t *)tsses[cpu];
	tss[1] = (uint32_t)stack;
	tss[2] = SEG_SYS_DATA;
	*(uint16_t *)(tsses[cpu] + 102) = sizeof(tsses[cpu]);

	/* Installs the task-state descriptor into the final GDT slot. */
	gdts[cpu][5] = tss_descriptor((uintptr_t)tsses[cpu]);
	descriptor.limit = sizeof(gdts[cpu]) - 1U;
	descriptor.base = (uint32_t)(uintptr_t)gdts[cpu];

	/* Loads the CPU-local GDT and refreshes every segment selector. */
	__asm__ volatile("lgdt %0" : : "m"(descriptor) : "memory");
	__asm__ volatile("ljmp $0x08,$1f;1:" : : : "memory");
	__asm__ volatile(
		"movw %0,%%ds;movw %0,%%es;movw %0,%%fs;"
		"movw %0,%%gs;movw %0,%%ss"
		:
		: "r"((uint16_t)SEG_SYS_DATA)
		: "memory");

	/* Activates the CPU-local task-state segment. */
	selector = SEG_TSS;
	__asm__ volatile("ltr %0" : : "r"(selector) : "memory");
}

/*
 * Updates one CPU's ring-zero interrupt stack.
 */
void
i386_percpu_set_kernel_stack(
	hal_cpu_id_t cpu,
	uintptr_t stack)
{
	/* Rejects a logical CPU outside the fixed task-state table. */
	if (cpu >= I386_APIC_MAX_CPUS)
		HAL_FATAL("invalid i386 TSS CPU");

	/* Publishes the new privilege-transition stack pointer. */
	((uint32_t *)tsses[cpu])[1] = (uint32_t)stack;
}

/* Encodes one available 32-bit task-state segment descriptor. */
static uint64_t
tss_descriptor(
	uintptr_t base_address)
{
	uint64_t descriptor;

	/* Encodes the fixed limit and split base-address fields. */
	descriptor = 103U;
	descriptor |= (uint64_t)(base_address & 0xffffU) << 16;
	descriptor |= (uint64_t)((base_address >> 16) & 0xffU) << 32;
	descriptor |= (uint64_t)0x89U << 40;
	descriptor |= (uint64_t)((base_address >> 24) & 0xffU) << 56;

	/* Returns the complete descriptor image. */
	return descriptor;
}
