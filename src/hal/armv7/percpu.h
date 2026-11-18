/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The private armv7 per-CPU state.
 */

#ifndef KERN_HAL_ARMV7_PERCPU_H
#define KERN_HAL_ARMV7_PERCPU_H

#include <hal/hal.h>
#include "defs.h"

struct armv7_task;
struct armv7_fpu_context;

/* The reasons one processor interrupts another, as bits of the receiver's word. */
#define ARMV7_IPI_NOTIFY	0x1UL
#define ARMV7_IPI_STOP		0x2UL

/*
 * The state one processor keeps for itself.
 *
 * TPIDRPRW holds the record's address from the processor's bootstrap on.
 * A record is written only by its own processor, except the fields marked
 * otherwise, which other processors read with acquire loads or change with
 * atomic operations.
 */
struct armv7_percpu {
	/* The record's own address. */
	struct armv7_percpu *self;

	/* The processor's number in the HAL, 0 for the boot processor. */
	hal_cpu_id_t logical_id;

	/* The processor's MPIDR, which names it in the device tree. */
	uint32_t mpidr;

	/* Set once the processor takes interrupts and runs its initial task; read by others. */
	volatile unsigned ready;

	/* The task running on this processor. */
	struct armv7_task *running_task;

	/*
	 * The task whose context is still being left on this processor.
	 *
	 * hal_task_context_switch() records the departing task here before the
	 * stack switch, and the arriving context, whether resumed or started
	 * for the first time, marks it as no longer running.  Until then
	 * another processor must not resume it: its stack is still in use.
	 */
	struct armv7_task *switching_from;

	/* The address space TTBR0 selects on this processor; read by others. */
	hal_space_t current_space;

	/* The acknowledgement of the interrupt this processor is handling, or none. */
	hal_irq_ack_t active_ack;

	/* When this processor's next scheduling tick is due, in counter ticks. */
	uint64_t timer_deadline;

	/* How many scheduling ticks this processor has taken; read by the boot's wait. */
	unsigned long timer_ticks;

	/*
	 * Why other processors interrupted this one, one ARMV7_IPI_ bit each.
	 * Senders set bits with an atomic or before raising the interrupt;
	 * this processor takes them all with an atomic swap.
	 */
	unsigned long ipi_reasons;

	/*
	 * The paging self-test's expected kernel fault: while armed, a data
	 * abort the HAL does not resolve at fault_address is recorded here and
	 * the faulting store is skipped, instead of going to the kernel.
	 */
	int selftest_fault_armed;
	uintptr_t selftest_fault_address;
	int selftest_fault_taken;
	unsigned selftest_fault_status;

	/*
	 * The floating-point unit's lazy switch (fpu.c): the context whose
	 * registers the unit holds, or NULL; and the context that runs now,
	 * whose registers the unit loads at its first floating-point
	 * instruction.  Changed with interrupts off.
	 */
	struct armv7_fpu_context *fpu_owner;
	struct armv7_fpu_context *fpu_selected;

	/*
	 * Set only while the boot's floating-point self-test runs: a kernel
	 * floating-point instruction then takes the unit for the selected
	 * context, as a user one does, instead of stopping the kernel.
	 */
	int fpu_kernel_test;
};

void
armv7_percpu_bootstrap(
	hal_cpu_id_t cpu,
	uint32_t mpidr);

struct armv7_percpu *
armv7_percpu_current(
	void);

struct armv7_percpu *
armv7_percpu_get(
	hal_cpu_id_t cpu);

void
armv7_smp_bootstrap(
	void);

void
armv7_smp_take_ipi(
	uint32_t sgi,
	hal_irq_ack_t acknowledge);

#endif
