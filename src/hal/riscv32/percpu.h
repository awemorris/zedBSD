/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The private riscv32 per-CPU state.
 */

#ifndef KERN_HAL_RISCV32_PERCPU_H
#define KERN_HAL_RISCV32_PERCPU_H

#include <hal/hal.h>
#include "defs.h"

struct riscv32_task;

/* The reasons one hart interrupts another, as bits of the receiver's word. */
#define RISCV32_IPI_NOTIFY	0x1UL
#define RISCV32_IPI_STOP	0x2UL

/*
 * The state one hart keeps for itself.
 *
 * In the kernel the thread pointer holds the record's address; while the
 * hart runs user code, sscratch holds it instead and the thread pointer is
 * the user's (trap.S swaps them).  A record is written only by its own
 * hart, except the fields marked otherwise, which other harts read with
 * acquire loads or change with atomic operations.
 */
struct riscv32_percpu {
	/* The record's own address. */
	struct riscv32_percpu *self;

	/* The top of the running task's kernel stack, where a trap from user starts. */
	uintptr_t kernel_sp;

	/* Where the trap entry keeps the user's stack pointer while it builds the frame. */
	uintptr_t user_sp;

	/* The hart's number in the HAL, 0 for the boot hart. */
	hal_cpu_id_t logical_id;

	/* The hart's id in the firmware and the device tree. */
	unsigned long hart_id;

	/* Set once the hart takes interrupts and runs its initial task; read by others. */
	volatile unsigned ready;

	/* The task running on this hart. */
	struct riscv32_task *running_task;

	/*
	 * The task whose context is still being left on this hart.
	 *
	 * hal_task_context_switch() records the departing task here before the
	 * stack switch, and the arriving context, whether resumed or started
	 * for the first time, marks it as no longer running.  Until then
	 * another hart must not resume it: its stack is still in use.
	 */
	struct riscv32_task *switching_from;

	/* The address space satp selects on this hart; read by others. */
	hal_space_t current_space;

	/* The acknowledgement of the interrupt this hart is handling, or none. */
	hal_irq_ack_t active_ack;

	/* When this hart's next scheduling tick is due, in counter ticks. */
	uint64_t timer_deadline;

	/* How many scheduling ticks this hart has taken. */
	unsigned long timer_ticks;

	/*
	 * Why other harts interrupted this one, one RISCV32_IPI_ bit each.
	 * Senders set bits with an atomic or before raising the interrupt;
	 * this hart takes them all with an atomic swap.
	 */
	unsigned long ipi_reasons;

	/* The PLIC context through which this hart takes supervisor external interrupts. */
	unsigned plic_context;

	/* Nonzero once the hart has a PLIC context. */
	int has_plic_context;
};

void
riscv32_percpu_bootstrap(
	hal_cpu_id_t cpu,
	unsigned long hart_id);

struct riscv32_percpu *
riscv32_percpu_current(
	void);

struct riscv32_percpu *
riscv32_percpu_get(
	hal_cpu_id_t cpu);

void
riscv32_smp_bootstrap(
	unsigned long hart_id);

void
riscv32_smp_configure(
	void);

void
riscv32_smp_take_ipi(
	void);

_Static_assert(__builtin_offsetof(struct riscv32_percpu, kernel_sp) == RISCV32_PERCPU_KERNEL_SP,
	       "trap.S reads the kernel stack top at this offset");
_Static_assert(__builtin_offsetof(struct riscv32_percpu, user_sp) == RISCV32_PERCPU_USER_SP,
	       "trap.S keeps the user stack pointer at this offset");

#endif
