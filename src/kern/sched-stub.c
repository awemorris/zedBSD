/*
 * Scheduler stubs for the single-task bring-up
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 *
 * The HAL's IRQ layer calls into the scheduler contract; until the
 * priority round-robin scheduler is ported (the multitasking phase),
 * the kernel runs as one task and these keep the contract satisfied:
 * the tick counts, nothing ever reschedules, and any path that would
 * block on an IRQ service task is a design error caught loudly.
 */

#include <kern/sched.h>
#include <hal/hal.h>

void
sched_init(void)
{
}

void
sched_link(task_t t, int list, int priority, int opt)
{
	(void)t;
	(void)list;
	(void)priority;
	(void)opt;
	HAL_FATAL("sched_link before the scheduler exists");
}

void
sched_yield(void)
{
	HAL_FATAL("sched_yield before the scheduler exists");
}

void
sched_clock_handler(void)
{
	/* Single task: the tick has no one else to run. */
}
