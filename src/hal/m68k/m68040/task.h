/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The m68040 tasks.
 */

#ifndef KERN_HAL_M68040_TASK_H
#define KERN_HAL_M68040_TASK_H

#include <hal/hal.h>
#include "int.h"

/*
 * The kernel stack of each created task.  Interrupts nest on it, and the
 * deepest file system and driver paths come close to 16 KiB before an
 * interrupt adds its frames, so it is twice that.
 */
#define M68040_SYS_STACK_SIZE	32768U

/*
 * One saved context the kernel's threads run in.
 *
 * A task lives from hal_task_create() (or hal_task_create_for_init_context())
 * to hal_task_destroy(), which only a task that runs nowhere reaches.
 */
struct m68040_task {
	struct m68040_task *next;
	hal_space_t space;

	/*
	 * The processor the task runs on, or -1 while it runs nowhere.  It stays set
	 * on the processor a task is switched away from until the next context
	 * there has left the task's stack (m68040_task_finish_switch()).
	 * Read by other processors with an acquire.
	 */
	int run_cpu;

	/* The processor the task is next resumed on (hal_task_transfer()). */
	hal_cpu_id_t target_cpu;

	/* The kernel stack, its allocation, and where the next switch resumes it. */
	void *sys_stack;
	void *sys_stack_allocation;
	uintptr_t sys_stack_top;
	uintptr_t resume_sp;

	/* The floating-point registers while the task does not run. */
	struct hal_fpregs fpregs;

	void *private_data;

	/*
	 * The user thread pointer a task that has not entered user mode yet
	 * starts with; afterwards the thread pointer lives in the user frame.
	 */
	uintptr_t tls;

	/* The frame the task entered the kernel from user with, or NULL. */
	struct m68040_trap_frame *active_user_frame;

	/*
	 * The frame a task built by hal_task_create() or forked will first
	 * return to user through, until it runs; NULL afterwards.
	 */
	struct m68040_trap_frame *initial_user_frame;

	/* The contexts interrupted by signal handlers, innermost last. */
	struct m68040_trap_frame signal_frame[HAL_SIGNAL_NEST_MAX];
	struct hal_fpregs signal_fpregs[HAL_SIGNAL_NEST_MAX];
	uint32_t signal_token[HAL_SIGNAL_NEST_MAX];
	unsigned signal_depth;
};

void m68040_task_returned(void) __attribute__((noreturn));
void m68040_context_selftest(void);
void m68040_task_enter_user_frame(struct m68040_trap_frame *frame);
void m68040_task_leave_user_frame(void);
struct m68040_task *m68040_task_running(void);
void m68040_task_finish_switch(void);
void m68040_task_first_user_entry(void);
struct m68040_trap_frame *m68040_task_user_frame(struct m68040_task *task);
void m68040_task_memory_stats(uint32_t *count, size_t *bytes);

#endif
