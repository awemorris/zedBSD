/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The armv7 per-CPU records and the register that finds them.
 */

#include <hal/hal.h>
#include "asm.h"
#include "percpu.h"

/*
 * One record for every processor the HAL can bring up.
 *
 * A record belongs to its processor from armv7_percpu_bootstrap() on, and
 * lives as long as the kernel.  It is zero until then; a zero ready flag
 * is what tells the other processors that it is not up.
 */
static struct armv7_percpu percpu_records[ARMV7_CPU_MAX];

/*
 * Makes the calling processor's record its own.
 *
 * TPIDRPRW is set to the record, so every later armv7_percpu_current() on
 * this processor finds it.  It runs before the processor takes any lock.
 */
void
armv7_percpu_bootstrap(
	hal_cpu_id_t cpu,
	uint32_t mpidr)
{
	struct armv7_percpu *record;

	/* A CPU number past the table is a fault in the bring-up. */
	if (cpu >= ARMV7_CPU_MAX)
		HAL_FATAL("armv7 CPU number out of range");

	/* Fills the record before the processor can be seen through it. */
	record = &percpu_records[cpu];
	hal_memset(record, 0, sizeof(*record));
	record->self = record;
	record->logical_id = cpu;
	record->mpidr = mpidr;
	record->running_task = NULL;
	record->switching_from = NULL;
	record->current_space = HAL_SPACE_SYS;
	record->active_ack = HAL_IRQ_ACK_NONE;

	/* Publishes the record to this processor's own thread register. */
	armv7_write_tpidrprw((uintptr_t)record);
}

/*
 * Reports the calling processor's record, or NULL before its bootstrap.
 */
struct armv7_percpu *
armv7_percpu_current(
	void)
{
	uintptr_t value;

	/* TPIDRPRW holds the record from the processor's bootstrap on. */
	value = armv7_read_tpidrprw();

	/* Succeeded: the record of the processor this code runs on. */
	return (struct armv7_percpu *)value;
}

/*
 * Reports the record of a CPU by its number, or NULL past the table.
 */
struct armv7_percpu *
armv7_percpu_get(
	hal_cpu_id_t cpu)
{
	/* A number past the table names no CPU. */
	if (cpu >= ARMV7_CPU_MAX)
		return NULL;

	/* Succeeded: the record, whether or not that processor is up. */
	return &percpu_records[cpu];
}
