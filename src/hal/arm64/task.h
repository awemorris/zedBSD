/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef KERN_HAL_ARM64_TASK_H
#define KERN_HAL_ARM64_TASK_H

#include <hal/hal.h>
#include "int.h"

/*
 * The kernel stack of each created task.  Interrupts nest on it, and the
 * deepest file system and driver paths come close to 16 KiB before an
 * interrupt adds its frames; the amd64 kernel overran its 16 KiB stacks
 * into the memory below them (BUG-321).
 */
#define ARM64_SYS_STACK_SIZE 32768U

struct arm64_task {
	struct arm64_task *next;
	hal_space_t space;

	/*
	 * The CPU the task runs on, or -1 while it runs nowhere.  It stays set
	 * on the CPU a task is switched away from until the next context there
	 * has left the task's stack (arm64_task_finish_switch()).  Read by
	 * other CPUs with an acquire.
	 */
	int run_cpu;

	/* The CPU the task is next resumed on (hal_task_transfer()). */
	hal_cpu_id_t target_cpu;

	void *sys_stack;
	void *sys_stack_allocation;
	uintptr_t resume_sp;
	uint8_t fpregs[528] __attribute__((aligned(16)));
	void *private_data;
	uintptr_t tls;
	void *active_user_frame;
	struct arm64_exception_frame signal_frame[HAL_SIGNAL_NEST_MAX];
	uint8_t signal_fpregs[HAL_SIGNAL_NEST_MAX][528]
	    __attribute__((aligned(16)));
	uint32_t signal_token[HAL_SIGNAL_NEST_MAX];
	unsigned signal_depth;

	/*
	 * What a debugger asked of the task (debug.c): whether it returns to
	 * user one instruction at a time, and the hardware points it runs
	 * with.  They are loaded into the processor when the task is switched
	 * to.
	 */
	int single_step;
	struct hal_debug_point debug_points[HAL_DEBUG_POINT_MAX];
	unsigned debug_point_count;
};

void asm_task_dispatch(uintptr_t *save,const uintptr_t *load);
void arm64_fp_save(void *state);
void arm64_fp_restore(const void *state);
void arm64_kernel_task_entry(void);
void arm64_user_task_entry(void);
void arm64_user_frame_entry(void);
void arm64_task_returned(void) __attribute__((noreturn));
void arm64_context_selftest(void);
void arm64_task_enter_user_frame(void *);
void arm64_task_leave_user_frame(void);
struct arm64_task *arm64_task_running(void);
void arm64_task_finish_switch(void);
void arm64_debug_init(void);
void arm64_debug_switch(struct arm64_task *to);

#endif
