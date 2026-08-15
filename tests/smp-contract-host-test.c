/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
/* Compile-time and single-process checks for the SMP object contracts. */

#include <assert.h>
#include <stddef.h>
#include <stdio.h>

#include <hal/hal.h>
#include <kern/atomic.h>
#include <kern/lock.h>
#include <kern/sched.h>
#include <kern/thread.h>
#include <kern/waitq.h>

_Static_assert(HAL_CPU_MASK_WORDS * 64U >= HAL_CPU_MAX,
    "CPU mask must cover every public CPU identifier");
_Static_assert(LOCK_RANK_SCHEDULER > LOCK_RANK_NETWORK,
    "scheduler lock is the terminal lock rank");
_Static_assert(offsetof(struct thread, sched) !=
    offsetof(struct thread, wait_token),
    "scheduler and condition-wait links must remain independent");

int
main(void)
{
	struct hal_cpu_mask mask;
	refcount_t references;

	hal_cpu_mask_zero(&mask);
	assert(!hal_cpu_mask_test(&mask, 0));
	assert(!hal_cpu_mask_test(&mask, HAL_CPU_MAX - 1U));
	hal_cpu_mask_set(&mask, 0);
	hal_cpu_mask_set(&mask, HAL_CPU_MAX - 1U);
	assert(hal_cpu_mask_test(&mask, 0));
	assert(hal_cpu_mask_test(&mask, HAL_CPU_MAX - 1U));
	hal_cpu_mask_clear(&mask, 0);
	assert(!hal_cpu_mask_test(&mask, 0));
	assert(hal_cpu_mask_test(&mask, HAL_CPU_MAX - 1U));

	refcount_init(&references, 1);
	assert(refcount_tryget(&references));
	assert(!refcount_put(&references));
	assert(refcount_put(&references));
	assert(!refcount_tryget(&references));

	puts("zedBSD SMP contract host tests: PASS");
	return 0;
}
