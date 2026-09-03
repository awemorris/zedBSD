/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * CPU-local amd64 HAL state selected through IA32_GS_BASE.
 */

#include <hal/hal.h>
#include "asm.h"
#include "percpu.h"

static struct amd64_percpu cpu_states[AMD64_SMP_MAX_CPUS];

/*
 * Initializes and selects the bootstrap CPU's private state.
 */
void
amd64_percpu_bootstrap(
	void)
{
	struct amd64_percpu *cpu;

	/* Clears the bootstrap slot and establishes its identity and space. */
	cpu = &cpu_states[0];
	hal_memset(cpu, 0, sizeof(*cpu));
	cpu->logical_id = 0;
	cpu->current_space = HAL_SPACE_SYS;

	/* Publishes the slot through the current CPU's GS base. */
	amd64_percpu_select(cpu);
}

/*
 * Looks up private state for a logical CPU identifier.
 */
struct amd64_percpu *
amd64_percpu_get(
	hal_cpu_id_t cpu)
{
	/* Rejects identifiers beyond the fixed amd64 CPU-state table. */
	if (cpu >= AMD64_SMP_MAX_CPUS)
		return NULL;

	/* Returns the requested private state slot. */
	return &cpu_states[cpu];
}

/*
 * Returns the current CPU's selected private state.
 */
struct amd64_percpu *
amd64_percpu_current(
	void)
{
	struct amd64_percpu *cpu;

	/* Reads the state pointer published in IA32_GS_BASE. */
	cpu = (struct amd64_percpu *)(uintptr_t)
	    asm_read_msr(AMD64_MSR_GS_BASE);

	/* Rejects an absent or self-inconsistent selection. */
	if (cpu == NULL || cpu->self != cpu)
		HAL_FATAL("amd64 per-CPU state is not selected");

	/* Returns the validated current-CPU state. */
	return cpu;
}

/*
 * Selects private state for the current CPU.
 */
void
amd64_percpu_select(
	struct amd64_percpu *cpu)
{
	/* Rejects an absent state before changing GS base. */
	if (cpu == NULL)
		HAL_FATAL("invalid amd64 per-CPU state");

	/* Establishes the self-check before publishing the state pointer. */
	cpu->self = cpu;
	asm_write_msr(AMD64_MSR_GS_BASE, (uint64_t)(uintptr_t)cpu);
}

/*
 * Pushes an interrupt acknowledgement record for the current CPU.
 */
hal_irq_ack_t
amd64_irq_ack_begin(
	uint32_t vector,
	int irq)
{
	struct amd64_percpu *cpu;
	struct amd64_irq_ack *record;

	/* Selects the current CPU's bounded acknowledgement stack. */
	cpu = amd64_percpu_current();
	if (cpu->acknowledgement_depth >= AMD64_IRQ_ACK_DEPTH)
		HAL_FATAL("amd64 IRQ acknowledgement stack overflow");

	/* Claims the next record and rejects stale nested ownership. */
	record = &cpu->acknowledgements[cpu->acknowledgement_depth++];
	if (record->active)
		HAL_FATAL("stale amd64 IRQ acknowledgement");

	/* Records the interrupt identity before exposing the opaque token. */
	record->vector = vector;
	record->irq = (uint32_t)irq;
	record->active = 1;

	/* Returns the record address as the HAL acknowledgement cookie. */
	return (hal_irq_ack_t)(uintptr_t)record;
}
