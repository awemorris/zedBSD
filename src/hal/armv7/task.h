/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The armv7 tasks.
 */

#ifndef KERN_HAL_ARMV7_TASK_H
#define KERN_HAL_ARMV7_TASK_H

#include <hal/hal.h>
#include "fpu.h"
#include "int.h"

/*
 * The kernel stack of each created task.  Interrupts nest on it, and the
 * deepest file system and driver paths come close to 16 KiB before an
 * interrupt adds its frames, so it is twice that.
 */
#define ARMV7_SYS_STACK_SIZE	32768U

/*
 * The words asm_task_dispatch() keeps on a stack it leaves: one word of
 * padding that keeps the stack 8-byte aligned, r4 to r11 and the link
 * register.
 */
#define ARMV7_DISPATCH_WORDS	10U
#define ARMV7_DISPATCH_R4	1U
#define ARMV7_DISPATCH_R5	2U
#define ARMV7_DISPATCH_LR	9U

/*
 * One saved context the kernel's threads run in.
 *
 * A task lives from hal_task_create() (or hal_task_create_for_init_context())
 * to hal_task_destroy(), which only a task that runs nowhere reaches.
 */
struct armv7_task {
	struct armv7_task *next;
	hal_space_t space;

	/*
	 * The processor the task runs on, or -1 while it runs nowhere.  It
	 * stays set on the processor a task is switched away from until the
	 * next context there has left the task's stack
	 * (armv7_task_finish_switch()).  Read by other processors with an
	 * acquire.
	 */
	int run_cpu;

	/* The processor the task is next resumed on (hal_task_transfer()). */
	hal_cpu_id_t target_cpu;

	/* The kernel stack, its allocation, and where the next switch resumes it. */
	void *sys_stack;
	void *sys_stack_allocation;
	uintptr_t sys_stack_top;
	uintptr_t resume_sp;

	/*
	 * The task's floating-point and Advanced SIMD registers, switched
	 * lazily (fpu.c).  The kernel never uses the unit.
	 */
	struct armv7_fpu_context fpu;

	void *private_data;

	/* The user thread pointer, which TPIDRURO holds while the task runs. */
	uintptr_t tls;

	/* The frame the task entered the kernel from user with, or NULL. */
	struct armv7_trap_frame *active_user_frame;

	/*
	 * The frame a task built by hal_task_create() or forked will first
	 * return to user through, until it runs; NULL afterwards.
	 */
	struct armv7_trap_frame *initial_user_frame;

	/*
	 * The contexts interrupted by signal handlers, innermost last, and
	 * their floating-point registers.
	 */
	struct armv7_trap_frame signal_frame[HAL_SIGNAL_NEST_MAX];
	struct hal_fpregs signal_fpregs[HAL_SIGNAL_NEST_MAX] __attribute__((aligned(8)));
	uint32_t signal_token[HAL_SIGNAL_NEST_MAX];
	unsigned signal_depth;
};

void
asm_task_dispatch(
	uintptr_t *save,
	const uintptr_t *load);

void
armv7_kernel_task_entry(
	void);

void
armv7_user_frame_entry(
	void);

void
armv7_task_returned(
	void)
__attribute__((noreturn));

void
armv7_context_selftest(
	void);

void
armv7_task_enter_user_frame(
	struct armv7_trap_frame *frame);

void
armv7_task_leave_user_frame(
	void);

struct armv7_task *
armv7_task_running(
	void);

void
armv7_task_finish_switch(
	void);

void
armv7_task_first_user_entry(
	void);

struct armv7_trap_frame *
armv7_task_user_frame(
	struct armv7_task *task);

void
armv7_task_memory_stats(
	uint32_t *count,
	size_t *bytes);

#endif
