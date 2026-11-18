/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The thread interface of the kernel's core and the machine layer (HAL):
 * a thread made from a task the HAL forked, and a secondary CPU's first
 * thread.  Only the kernel's core includes this header; drivers use
 * <kern/thread.h>.
 */

#ifndef KERN_KERN_THREAD_PRIVATE_H
#define KERN_KERN_THREAD_PRIVATE_H

#include <hal/hal.h>
#include <kern/thread.h>

/* The kernel's limits are the HAL's. */
_Static_assert(SIGNAL_NEST_MAX == HAL_SIGNAL_NEST_MAX, "SIGNAL_NEST_MAX is not the HAL's");
_Static_assert(THREAD_SYSCALL_ARGS == HAL_SYSCALL_ARGS, "THREAD_SYSCALL_ARGS is not the HAL's");

/* The kernel's handles are the HAL's. */
_Static_assert(__builtin_types_compatible_p(kern_task_handle_t, hal_task_t),
	       "kern_task_handle_t is not hal_task_t");
_Static_assert(__builtin_types_compatible_p(kern_cpu_id_t, hal_cpu_id_t),
	       "kern_cpu_id_t is not hal_cpu_id_t");

int
thread_fork(
	struct process *process,
	hal_task_t task,
	struct thread **result);

void
thread_init_secondary(
	hal_cpu_id_t cpu);

#endif
