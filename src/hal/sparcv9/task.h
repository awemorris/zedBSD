/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The SPARC V9 HAL's tasks: a register-window context, a kernel stack and
 * the user context a trap left.
 */

#ifndef KERN_HAL_SPARCV9_TASK_H
#define KERN_HAL_SPARCV9_TASK_H

#include <hal/hal.h>
#include "trap.h"

/* The kernel stack of a task. */
#define SPARCV9_SYS_STACK_SIZE	32768U

/* The register windows of the UltraSPARC-I/II. */
#define SPARCV9_NWINDOWS	8U

/*
 * Every register window of a processor and the registers that say how
 * they are used, saved while another context runs.
 *
 * context.S and trap-entry.S save and load it by fixed offsets: the
 * windows' locals and ins first, then the window registers, then the
 * globals.
 */
struct sparcv9_window_context {
	uint64_t window[SPARCV9_NWINDOWS][16];
	uint64_t cwp;
	uint64_t cansave;
	uint64_t canrestore;
	uint64_t otherwin;
	uint64_t cleanwin;
	uint64_t wstate;
	uint64_t global[8];
};

_Static_assert(__builtin_offsetof(struct sparcv9_window_context, cwp) == 1024U, "context.S saves cwp at 1024");
_Static_assert(__builtin_offsetof(struct sparcv9_window_context, wstate) == 1064U, "context.S saves wstate at 1064");
_Static_assert(__builtin_offsetof(struct sparcv9_window_context, global) == 1072U, "context.S saves the globals from 1072");

/*
 * One schedulable context of the HAL.
 *
 * Created by hal_task_create() or hal_task_create_for_init_context() and
 * kept on the task list until hal_task_destroy().  windows is the context
 * the task switches with; trap_windows is the user context the last trap
 * from user mode saved, which the trap entry reloads on the way back.
 * The active_ fields describe the user trap being handled, and the
 * signal_ arrays keep the context of each signal handler entered.
 */
struct sparcv9_task {
	struct sparcv9_task *next;
	hal_space_t space;
	int run_cpu;
	void *stack_allocation;
	void *stack;
	uintptr_t trap_sp;
	void *private_data;
	uintptr_t tls;
	uint64_t trap_level;
	uint64_t trap_pc;
	uint64_t trap_next_pc;
	uint64_t trap_state;
	uint64_t trap_type;
	struct sparcv9_window_context windows;
	struct sparcv9_window_context trap_windows;
	struct sparcv9_user_trap_frame *active_user_frame;
	uint64_t active_pc;
	uint64_t active_next_pc;
	uint64_t active_tstate;
	uint64_t active_trap_type;
	struct sparcv9_window_context signal_windows[HAL_SIGNAL_NEST_MAX];
	struct sparcv9_user_trap_frame signal_frame[HAL_SIGNAL_NEST_MAX];
	uint64_t signal_pc[HAL_SIGNAL_NEST_MAX];
	uint64_t signal_next_pc[HAL_SIGNAL_NEST_MAX];
	uint64_t signal_tstate[HAL_SIGNAL_NEST_MAX];
	uint32_t signal_token[HAL_SIGNAL_NEST_MAX];
	uint32_t signal_depth;
};

/*
 * The top of the running task's kernel stack, biased, which a trap from
 * user mode switches to; the trap entry reads it.
 */
extern uintptr_t sparcv9_current_trap_sp;

/*
 * The context a trap from user mode saves the user's windows into: the
 * running task's trap_windows.  The trap entry reads it.
 */
extern struct sparcv9_window_context *sparcv9_current_user_windows;

/*
 * Saves the processor's windows into one context and loads another.
 */
void
sparcv9_task_dispatch(
	struct sparcv9_window_context *save,
	const struct sparcv9_window_context *load);

/*
 * Reads the thread pointer register.
 */
uintptr_t
sparcv9_tls_read(
	void);

/*
 * Writes the thread pointer register.
 */
void
sparcv9_tls_write(
	uintptr_t value);

/*
 * The first code of a new kernel task, a new user task and a forked user
 * task, entered through the context hal_task_create() builds.
 */
void
sparcv9_kernel_task_entry(
	void);

void
sparcv9_user_task_entry(
	void);

void
sparcv9_user_fork_entry(
	void);

/*
 * Records the user trap being handled, so fork, exec and signals can
 * replace the context it returns to.
 */
void
sparcv9_task_enter_user_frame(
	struct sparcv9_user_trap_frame *frame,
	uintptr_t pc,
	uintptr_t next_pc,
	uint64_t tstate,
	uint64_t trap_type);

/*
 * Ends the user trap being handled.
 */
void
sparcv9_task_leave_user_frame(
	void);

/*
 * Stops a kernel task whose function returned.
 */
void
sparcv9_task_returned(
	void) __attribute__((noreturn));

/*
 * Reports the number of tasks and the bytes of their kernel stacks.
 */
void
sparcv9_task_memory_stats(
	uint32_t *count,
	size_t *stack_bytes);

#endif
