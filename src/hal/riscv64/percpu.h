/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The private riscv64 per-CPU state.
 */

#ifndef KERN_HAL_RISCV64_PERCPU_H
#define KERN_HAL_RISCV64_PERCPU_H

#include <hal/hal.h>
#include "defs.h"

struct riscv64_task;

/* The reasons one hart interrupts another, as bits of the receiver's word. */
#define RISCV64_IPI_NOTIFY	0x1UL
#define RISCV64_IPI_STOP	0x2UL

/*
 * The state one hart keeps for itself.
 *
 * In the kernel the thread pointer holds the record's address; while the
 * hart runs user code, sscratch holds it instead and the thread pointer is
 * the user's (trap.S swaps them).  A record is written only by its own
 * hart, except the fields marked otherwise, which other harts read with
 * acquire loads or change with atomic operations.
 */
struct riscv64_percpu {
	/* The record's own address. */
	struct riscv64_percpu *self;

	/* The top of the running task's kernel stack, where a trap from user starts. */
	uintptr_t kernel_sp;

	/* Where the trap entry keeps the user's stack pointer while it builds the frame. */
	uintptr_t user_sp;

	/* The hart's number in the HAL, 0 for the boot hart. */
	hal_cpu_id_t logical_id;

	/* The hart's id in the firmware and the device tree. */
	uint64_t hart_id;

	/* Set once the hart takes interrupts and runs its initial task; read by others. */
	volatile unsigned ready;

	/* The task running on this hart. */
	struct riscv64_task *running_task;

	/*
	 * The task whose context is still being left on this hart.
	 *
	 * hal_task_context_switch() records the departing task here before the
	 * stack switch, and the arriving context, whether resumed or started
	 * for the first time, marks it as no longer running.  Until then
	 * another hart must not resume it: its stack is still in use.
	 */
	struct riscv64_task *switching_from;

	/* The address space satp selects on this hart; read by others. */
	hal_space_t current_space;

	/* The acknowledgement of the interrupt this hart is handling, or none. */
	hal_irq_ack_t active_ack;

	/* When this hart's next scheduling tick is due, in counter ticks. */
	uint64_t timer_deadline;

	/* How many scheduling ticks this hart has taken. */
	uint64_t timer_ticks;

	/*
	 * How many periods of the fastest tick one of this hart's ticks spans
	 * (hal_rtc_set_tick_hz()); 0 before its tick starts, which counts as 1.
	 * Only the hart itself writes it, with interrupts off.
	 */
	uint64_t timer_divisor;

	/*
	 * Why other harts interrupted this one, one RISCV64_IPI_ bit each.
	 * Senders set bits with an atomic or before raising the interrupt;
	 * this hart takes them all with an atomic swap.
	 */
	unsigned long ipi_reasons;

	/* The PLIC context through which this hart takes supervisor external interrupts. */
	unsigned plic_context;

	/* Nonzero once the hart has a PLIC context. */
	int has_plic_context;
};

_Static_assert(__builtin_offsetof(struct riscv64_percpu, kernel_sp) == RISCV64_PERCPU_KERNEL_SP,
	       "trap.S reads the kernel stack top at this offset");
_Static_assert(__builtin_offsetof(struct riscv64_percpu, user_sp) == RISCV64_PERCPU_USER_SP,
	       "trap.S keeps the user stack pointer at this offset");

void
riscv64_percpu_bootstrap(
	hal_cpu_id_t cpu,
	uint64_t hart_id);

struct riscv64_percpu *
riscv64_percpu_current(
	void);

struct riscv64_percpu *
riscv64_percpu_get(
	hal_cpu_id_t cpu);

void
riscv64_smp_bootstrap(
	uint64_t hart_id);

void
riscv64_smp_configure(
	void);

void
riscv64_smp_take_ipi(
	void);

#endif
