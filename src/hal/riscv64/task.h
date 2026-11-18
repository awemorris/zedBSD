/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The riscv64 tasks.
 */

#ifndef KERN_HAL_RISCV64_TASK_H
#define KERN_HAL_RISCV64_TASK_H

#include <hal/hal.h>
#include "int.h"

/*
 * The kernel stack of each created task.  Interrupts nest on it, and the
 * deepest file system and driver paths come close to 16 KiB before an
 * interrupt adds its frames, so it is twice that.
 */
#define RISCV64_SYS_STACK_SIZE	32768U

/*
 * The words asm_task_dispatch() keeps on a stack it leaves: s0 to s11, ra
 * and one word of padding that keeps the stack 16-byte aligned.
 */
#define RISCV64_DISPATCH_WORDS	14U
#define RISCV64_DISPATCH_S0	0U
#define RISCV64_DISPATCH_S1	1U
#define RISCV64_DISPATCH_RA	12U

/*
 * One saved context the kernel's threads run in.
 *
 * A task lives from hal_task_create() (or hal_task_create_for_init_context())
 * to hal_task_destroy(), which only a task that runs nowhere reaches.
 */
struct riscv64_task {
	struct riscv64_task *next;
	hal_space_t space;

	/*
	 * The hart the task runs on, or -1 while it runs nowhere.  It stays set
	 * on the hart a task is switched away from until the next context
	 * there has left the task's stack (riscv64_task_finish_switch()).
	 * Read by other harts with an acquire.
	 */
	int run_cpu;

	/* The hart the task is next resumed on (hal_task_transfer()). */
	hal_cpu_id_t target_cpu;

	/* The kernel stack, its allocation, and where the next switch resumes it. */
	void *sys_stack;
	void *sys_stack_allocation;
	uintptr_t sys_stack_top;
	uintptr_t resume_sp;

	/* The floating-point registers while the task does not run. */
	struct hal_fpregs fpregs __attribute__((aligned(16)));

	void *private_data;

	/*
	 * The user thread pointer a task that has not entered user mode yet
	 * starts with; afterwards the thread pointer lives in the user frame.
	 */
	uintptr_t tls;

	/* The frame the task entered the kernel from user with, or NULL. */
	struct riscv64_trap_frame *active_user_frame;

	/*
	 * The frame a task built by hal_task_create() or forked will first
	 * return to user through, until it runs; NULL afterwards.
	 */
	struct riscv64_trap_frame *initial_user_frame;

	/* The contexts interrupted by signal handlers, innermost last. */
	struct riscv64_trap_frame signal_frame[HAL_SIGNAL_NEST_MAX];
	struct hal_fpregs signal_fpregs[HAL_SIGNAL_NEST_MAX];
	uint32_t signal_token[HAL_SIGNAL_NEST_MAX];
	unsigned signal_depth;
};

void
asm_task_dispatch(
	uintptr_t *save,
	const uintptr_t *load);

void
riscv64_fp_save(
	struct hal_fpregs *state);

void
riscv64_fp_restore(
	const struct hal_fpregs *state);

void
riscv64_kernel_task_entry(
	void);

void
riscv64_user_frame_entry(
	void);

void
riscv64_task_returned(
	void)
__attribute__((noreturn));

void
riscv64_context_selftest(
	void);

void
riscv64_task_enter_user_frame(
	struct riscv64_trap_frame *frame);

void
riscv64_task_leave_user_frame(
	void);

struct riscv64_task *
riscv64_task_running(
	void);

void
riscv64_task_finish_switch(
	void);

void
riscv64_task_first_user_entry(
	void);

struct riscv64_trap_frame *
riscv64_task_user_frame(
	struct riscv64_task *task);

void
riscv64_task_memory_stats(
	uint32_t *count,
	size_t *bytes);

#endif
