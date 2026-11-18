/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The riscv32 per-CPU records and the register that finds them.
 */

#include <hal/hal.h>
#include "asm.h"
#include "percpu.h"

/*
 * One record for every hart the HAL can bring up.
 *
 * A record belongs to its hart from riscv32_percpu_bootstrap() on, and
 * lives as long as the kernel.  It is zero until then; a zero ready flag
 * is what tells the other harts that the hart is not up.
 */
static struct riscv32_percpu percpu_records[RISCV32_CPU_MAX];

/*
 * Makes the calling hart's record its own.
 *
 * The thread pointer is set to the record, so every later
 * riscv32_percpu_current() on this hart finds it.  It runs before the
 * hart takes any lock.
 */
void
riscv32_percpu_bootstrap(
	hal_cpu_id_t cpu,
	unsigned long hart_id)
{
	struct riscv32_percpu *record;

	/* A CPU number past the table is a fault in the bring-up. */
	if (cpu >= RISCV32_CPU_MAX)
		HAL_FATAL("riscv32 CPU number out of range");

	/* Fills the record before the hart can be seen through it. */
	record = &percpu_records[cpu];
	record->self = record;
	record->kernel_sp = 0;
	record->user_sp = 0;
	record->logical_id = cpu;
	record->hart_id = hart_id;
	record->running_task = NULL;
	record->switching_from = NULL;
	record->current_space = HAL_SPACE_SYS;
	record->active_ack = HAL_IRQ_ACK_NONE;
	record->timer_deadline = 0;
	record->timer_ticks = 0;
	record->ipi_reasons = 0;
	record->plic_context = 0;
	record->has_plic_context = 0;

	/* Publishes the record to this hart's own thread pointer. */
	riscv32_write_tp((uintptr_t)record);
}

/*
 * Reports the calling hart's record.
 */
struct riscv32_percpu *
riscv32_percpu_current(
	void)
{
	uintptr_t value;

	/* The thread pointer holds the record from the hart's bootstrap on. */
	value = riscv32_read_tp();

	/* Succeeded: the record of the hart this code runs on. */
	return (struct riscv32_percpu *)value;
}

/*
 * Reports the record of a CPU by its number, or NULL past the table.
 */
struct riscv32_percpu *
riscv32_percpu_get(
	hal_cpu_id_t cpu)
{
	/* A number past the table names no CPU. */
	if (cpu >= RISCV32_CPU_MAX)
		return NULL;

	/* Succeeded: the record, whether or not that hart is up. */
	return &percpu_records[cpu];
}
