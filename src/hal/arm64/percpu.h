/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The private arm64 per-CPU state.
 */

#ifndef KERN_HAL_ARM64_PERCPU_H
#define KERN_HAL_ARM64_PERCPU_H

#include <hal/hal.h>

/* The most CPUs the HAL brings up: the four cores of the BCM2711. */
#define ARM64_CPU_MAX 4U

struct arm64_task;

/*
 * The state one CPU keeps for itself.
 *
 * Each CPU finds its own record through TPIDR_EL1, which holds the
 * record's address from the CPU's first C code on.  A record is written
 * only by its own CPU, except the fields marked otherwise, which other
 * CPUs read with acquire loads.
 */
struct arm64_percpu {
	/* The record's own address, which TPIDR_EL1 holds. */
	struct arm64_percpu *self;

	/* The CPU's number in the HAL, 0 for the boot CPU. */
	hal_cpu_id_t logical_id;

	/* The CPU's affinity value in MPIDR_EL1. */
	uint64_t mpidr;

	/* Set once the CPU takes interrupts and runs its initial task; read by others. */
	volatile unsigned ready;

	/* The task running on this CPU. */
	struct arm64_task *running_task;

	/*
	 * The task whose context is still being left on this CPU.
	 *
	 * hal_task_context_switch() records the departing task here before the
	 * stack switch, and the arriving context, whether resumed or started
	 * for the first time, marks it as no longer running.  Until then
	 * another CPU must not resume it: its stack is still in use.
	 */
	struct arm64_task *switching_from;

	/* The address space TTBR0_EL1 selects on this CPU; read by others. */
	hal_space_t current_space;

	/* The acknowledgement of the interrupt this CPU is handling, or none. */
	hal_irq_ack_t active_ack;

	/* When this CPU's next scheduling tick is due, in counter ticks. */
	uint64_t timer_deadline;

	/* How many scheduling ticks this CPU has taken. */
	uint64_t timer_ticks;

	/*
	 * How many periods of the fastest tick one of this CPU's ticks spans
	 * (hal_rtc_set_tick_hz()); 0 before its tick starts, which counts as 1.
	 * Only the CPU itself writes it, with interrupts off.
	 */
	uint64_t timer_divisor;

	/* Whether this CPU's breakpoint and watchpoint registers hold a task's points. */
	int debug_points_loaded;

	/*
	 * The stall watch's view of this CPU (watch.c).  The CPU writes these
	 * fields with relaxed stores; another CPU reads them only to describe
	 * a stall, where a stale value costs no more than a wrong digit.
	 */

	/* The counter when this CPU's scheduling tick last began; 0 before the first. */
	uint64_t watch_beat_time;

	/* The counter when this CPU last turned its interrupts off; 0 while they are on. */
	uint64_t watch_irq_off_time;

	/* Where interrupts went off: the code that did it, and its caller. */
	uint64_t watch_irq_off_pc;
	uint64_t watch_irq_off_caller;

	/* The interrupt this CPU is handling plus one (0 for none), since when, and what it interrupted. */
	uint64_t watch_irq_id;
	uint64_t watch_irq_time;
	uint64_t watch_irq_elr;

	/* How many task switches this CPU has made, which shows its scheduler moving. */
	uint64_t watch_switches;

	/*
	 * Calls of hal_cpu_current() with interrupts off left before the CPU
	 * looks at its own tick again; only such calls, which cannot move to
	 * another CPU, touch it.
	 */
	unsigned watch_countdown;

	/* Nonzero while this CPU writes a stall report, so a fault inside it does not start another. */
	unsigned watch_reporting;

	/*
	 * Nonzero once this CPU stopped on purpose (parked after a panic, or
	 * halted for power-off): its silence is not a stall, and the other
	 * CPUs leave it out of their look at the heartbeats.
	 */
	unsigned watch_parked;
};

void arm64_percpu_bootstrap(hal_cpu_id_t cpu, uint64_t mpidr);
struct arm64_percpu *arm64_percpu_current(void);
struct arm64_percpu *arm64_percpu_get(hal_cpu_id_t cpu);
void arm64_smp_bootstrap(void);
void arm64_smp_configure(void);

#endif
