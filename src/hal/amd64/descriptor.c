/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Per-CPU amd64 global descriptor tables and task-state segments.
 */

#include <hal/hal.h>
#include "defs.h"
#include "descriptor.h"

struct amd64_tss {
	uint32_t reserved0;
	uint64_t rsp0, rsp1, rsp2;
	uint64_t reserved1;
	uint64_t ist1, ist2, ist3, ist4, ist5, ist6, ist7;
	uint64_t reserved2;
	uint16_t reserved3;
	uint16_t iomap_base;
} __attribute__((packed));

struct table_descriptor {
	uint16_t limit;
	uint64_t base;
} __attribute__((packed));

struct descriptor_state {
	uint64_t gdt[7] __attribute__((aligned(16)));
	struct amd64_tss tss __attribute__((aligned(16)));
	uint8_t double_fault_stack[PAGE_SIZE * 4] __attribute__((aligned(16)));
};

typedef char amd64_tss_size_must_be_104[
	sizeof(struct amd64_tss) == 104 ? 1 : -1];

static struct descriptor_state states[AMD64_SMP_MAX_CPUS];

/*
 * Initializes descriptor state for the current CPU.
 */
void
amd64_descriptor_init(
	void)
{
	struct table_descriptor gdtr;
	struct descriptor_state *state;
	uintptr_t base;
	uint64_t low;

	/* Selects and clears the current CPU's private descriptor state. */
	state = &states[hal_cpu_current()];
	base = (uintptr_t)&state->tss;
	hal_memset(state, 0, sizeof(*state));

	/* Installs kernel and user code and data descriptors unchanged. */
	state->gdt[1] = 0x00af9a000000ffffULL;
	state->gdt[2] = 0x00cf92000000ffffULL;
	state->gdt[3] = 0x00affa000000ffffULL;
	state->gdt[4] = 0x00cff2000000ffffULL;

	/* Encodes the private TSS as the final two GDT entries. */
	low = (sizeof(state->tss) - 1U) & 0xffffU;
	low |= (uint64_t)(base & 0xffffffU) << 16;
	low |= (uint64_t)0x89U << 40;
	low |= (uint64_t)((sizeof(state->tss) - 1U) >> 16 & 0x0fU) << 48;
	low |= (uint64_t)((base >> 24) & 0xffU) << 56;
	state->gdt[5] = low;
	state->gdt[6] = base >> 32;

	/* Provides IST1 for double faults and disables the I/O bitmap. */
	state->tss.ist1 = (uintptr_t)state->double_fault_stack +
	    sizeof(state->double_fault_stack);
	state->tss.iomap_base = sizeof(state->tss);

	/* Loads the completed GDT and its TSS descriptor. */
	gdtr.limit = sizeof(state->gdt) - 1U;
	gdtr.base = (uintptr_t)state->gdt;
	amd64_load_gdt(&gdtr);
}

/*
 * Updates the current CPU's ring-zero entry stack.
 */
void
amd64_set_tss_rsp0(
	uintptr_t stack_top)
{
	/* Publishes the stack used by privilege-level transitions. */
	states[hal_cpu_current()].tss.rsp0 = stack_top;
}
