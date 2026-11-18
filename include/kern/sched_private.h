/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The scheduler's interface to the kernel's core and to the machine layer
 * (HAL): the calls the boot, the timer, the trap entry and exit, and the
 * other CPUs make.  Only the kernel's core includes this header; drivers
 * use <kern/sched.h>.
 */

#ifndef KERN_KERN_SCHED_PRIVATE_H
#define KERN_KERN_SCHED_PRIVATE_H

#include <hal/hal.h>
#include <kern/sched.h>
#include <kern/types.h>

/* The kernel's CPU number and thread handle are the HAL's. */
_Static_assert(__builtin_types_compatible_p(kern_cpu_id_t, hal_cpu_id_t),
	       "kern_cpu_id_t is not hal_cpu_id_t");
_Static_assert(__builtin_types_compatible_p(kern_task_handle_t, hal_task_t),
	       "kern_task_handle_t is not hal_task_t");

/* The kernel's limit on CPUs is the HAL's. */
_Static_assert(KERN_CPU_MAX == HAL_CPU_MAX, "KERN_CPU_MAX is not HAL_CPU_MAX");

void
sched_wait_task(void);

void
sched_exit_current(void)
__attribute__((noreturn));

void
sched_clock_cpu(
	hal_cpu_id_t cpu,
	uint64_t now);

/*
 * Mark kernel execution entered from, and returning to, user mode.  These
 * calls classify scheduler ticks without exposing architecture context
 * details through the HAL task interface.
 */
void
sched_accounting_kernel_enter(void);

void
sched_accounting_kernel_leave(void);

void
sched_idle(void) __attribute__((noreturn));

void
sched_secondary_init(
	hal_cpu_id_t cpu) __attribute__((noreturn));

int
sched_wait_others_online(void);

void
sched_cpu_notify(
	hal_cpu_id_t cpu);

#ifdef KERN_SCHED_TEST
int
sched_test_cpu_online(
	hal_cpu_id_t,
	struct thread *);
#endif

#endif
