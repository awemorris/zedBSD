/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The MIPS tasks and the context switch.
 */

#ifndef KERN_HAL_MIPS_TASK_H
#define KERN_HAL_MIPS_TASK_H

#include <hal/hal.h>
#include "trap.h"

/*
 * The kernel stack of each created task.  Interrupts nest on it, and the
 * deepest file system and driver paths come close to 16 KiB before an
 * interrupt adds its frames, so it is twice that.
 */
#define MIPS_SYS_STACK_SIZE	32768U

/*
 * One saved context the kernel's threads run in.
 *
 * A task lives from hal_task_create() (or hal_task_create_for_init_context())
 * to hal_task_destroy(), which only a task that runs nowhere reaches.  Its
 * floating-point registers are in fpregs while another context owns the
 * unit, and in the unit while the task owns it (fpu.c).
 */
struct mips_task {
	struct mips_task *next;
	hal_space_t space;

	/*
	 * The processor the task runs on, or -1 while it runs nowhere.  It
	 * stays set on the task switched away from until the next context
	 * has left the task's stack (mips_task_finish_switch()).
	 */
	int run_cpu;

	/* The processor the task is next resumed on (hal_task_transfer()). */
	hal_cpu_id_t target_cpu;

	/* The kernel stack, its allocation, and where the next switch resumes it. */
	void *sys_stack;
	void *sys_stack_allocation;
	uintptr_t sys_stack_top;
	uintptr_t resume_sp;

	/* The floating-point registers while the unit does not hold them. */
	struct hal_fpregs fpregs __attribute__((aligned(8)));

	/* The kernel's own pointer for the task (hal_task_set_private()). */
	void *private_data;

	/*
	 * The user thread pointer.  The frame has no register for it (the
	 * user reads it through the user-local register once the user ABI
	 * is defined), so the task keeps it.
	 */
	uintptr_t tls;

	/* The frame the task entered the kernel from user with, or NULL. */
	struct mips_trap_frame *active_user_frame;

	/*
	 * The frame a task built by hal_task_create() or forked will first
	 * return to user through, until it runs; NULL afterwards.
	 */
	struct mips_trap_frame *initial_user_frame;

	/* The contexts interrupted by signal handlers, innermost last. */
	struct mips_trap_frame signal_frame[HAL_SIGNAL_NEST_MAX];
	struct hal_fpregs signal_fpregs[HAL_SIGNAL_NEST_MAX] __attribute__((aligned(8)));
	uint32_t signal_token[HAL_SIGNAL_NEST_MAX];
	unsigned signal_depth;
};

void
mips_task_dispatch(
	uintptr_t *save,
	const uintptr_t *load);

void
mips_kernel_task_entry(
	void);

void
mips_user_frame_entry(
	void);

void
mips_task_returned(
	void) __attribute__((noreturn));

void
mips_context_selftest(
	void);

void
mips_task_enter_user_frame(
	struct mips_trap_frame *frame);

void
mips_task_leave_user_frame(
	void);

struct mips_task *
mips_task_running(
	void);

void
mips_task_finish_switch(
	void);

void
mips_task_first_user_entry(
	void);

struct mips_trap_frame *
mips_task_user_frame(
	struct mips_task *task);

void
mips_task_memory_stats(
	uint32_t *count,
	size_t *bytes);

#endif
