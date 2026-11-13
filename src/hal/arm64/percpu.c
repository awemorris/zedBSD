/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The arm64 per-CPU records and the register that finds them.
 */

#include <hal/hal.h>
#include "percpu.h"

/*
 * One record for every CPU the HAL can bring up.
 *
 * A record belongs to its CPU from arm64_percpu_bootstrap() on, and lives
 * as long as the kernel.  It is zero until then; a zero ready flag is what
 * tells the other CPUs that the CPU is not up.
 */
static struct arm64_percpu percpu_records[ARM64_CPU_MAX];

/*
 * Makes the calling CPU's record its own.
 *
 * TPIDR_EL1 is set to the record, so every later arm64_percpu_current()
 * on this CPU finds it.  It runs before the CPU takes any lock.
 */
void
arm64_percpu_bootstrap(
	hal_cpu_id_t cpu,
	uint64_t mpidr)
{
	struct arm64_percpu *record;

	/* A CPU number past the table is a fault in the bring-up. */
	if (cpu >= ARM64_CPU_MAX)
		HAL_FATAL("arm64 CPU number out of range");

	/* Fills the record before the CPU can be seen through it. */
	record = &percpu_records[cpu];
	record->self = record;
	record->logical_id = cpu;
	record->mpidr = mpidr;
	record->running_task = NULL;
	record->switching_from = NULL;
	record->current_space = HAL_SPACE_SYS;
	record->active_ack = HAL_IRQ_ACK_NONE;
	record->timer_deadline = 0;
	record->timer_ticks = 0;
	record->timer_divisor = 0;
	record->debug_points_loaded = 0;

	/* Starts the stall watch's view empty: no tick yet, interrupts as the caller has them. */
	record->watch_beat_time = 0;
	record->watch_irq_off_time = 0;
	record->watch_irq_off_pc = 0;
	record->watch_irq_off_caller = 0;
	record->watch_irq_id = 0;
	record->watch_irq_time = 0;
	record->watch_irq_elr = 0;
	record->watch_switches = 0;
	record->watch_countdown = 1;
	record->watch_reporting = 0;
	record->watch_parked = 0;

	/* Publishes the record to this CPU's own register. */
	__asm__ volatile("msr tpidr_el1, %0\n\tisb"
			 :
			 : "r"((uint64_t)(uintptr_t)record)
			 : "memory");
}

/*
 * Reports the calling CPU's record.
 */
struct arm64_percpu *
arm64_percpu_current(
	void)
{
	uint64_t value;

	/* TPIDR_EL1 holds the record from the CPU's bootstrap on. */
	__asm__ volatile("mrs %0, tpidr_el1" : "=r"(value));

	/* Succeeded: the record of the CPU this code runs on. */
	return (struct arm64_percpu *)(uintptr_t)value;
}

/*
 * Reports the record of a CPU by its number, or NULL past the table.
 */
struct arm64_percpu *
arm64_percpu_get(
	hal_cpu_id_t cpu)
{
	/* A number past the table names no CPU. */
	if (cpu >= ARM64_CPU_MAX)
		return NULL;

	/* Succeeded: the record, whether or not that CPU is up. */
	return &percpu_records[cpu];
}
