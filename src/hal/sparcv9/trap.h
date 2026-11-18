/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The SPARC V9 HAL's traps.
 */

#ifndef KERN_HAL_SPARCV9_TRAP_H
#define KERN_HAL_SPARCV9_TRAP_H

#include <hal/types.h>

/*
 * The user registers a trap from user mode saves on the task's kernel
 * stack: the user stack pointer, the system call number and the six
 * argument registers, which carry the call's result back.
 *
 * The trap entry in trap-entry.S fills it by fixed offsets.
 */
struct sparcv9_user_trap_frame {
	uintptr_t old_sp;
	uint64_t syscall_number;
	uint64_t out[6];
};

/*
 * Points the processor at the trap table.
 */
void
sparcv9_trap_init(
	void);

/*
 * Handles a trap taken in privileged mode.
 *
 * Returns 1 when the trapping instruction is done and is skipped, or 0
 * when it is retried.
 */
int
sparcv9_trap_dispatch(
	uint64_t trap_type,
	uintptr_t pc,
	uintptr_t next_pc,
	uint64_t tstate);

/*
 * Handles a trap taken in user mode.
 *
 * Returns 1 when the trapping instruction is done and is skipped, or 0
 * when it is retried.
 */
int
sparcv9_user_trap_dispatch(
	uint64_t trap_type,
	uintptr_t pc,
	uintptr_t next_pc,
	uint64_t tstate,
	struct sparcv9_user_trap_frame *frame);

/*
 * Brings in the entry page and the stack page of a new user program, then
 * returns to enter it.
 */
void
sparcv9_user_task_prepare(
	uintptr_t entry,
	uintptr_t stack_pointer);

#endif
