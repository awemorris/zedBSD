/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef HAL_HAL_ARCH_PPC64_H
#define HAL_HAL_ARCH_PPC64_H

/*
 * The periodic tick, in hertz.  The decrementer runs at the timebase the
 * firmware reports, and the HAL derives the interval from this value.
 */
#define HAL_TIMER_FREQUENCY	(100U)

/*
 * Signal Frame
 */

/*
 * Signal frame head: the same 16-byte head as AArch64, without a
 * restorer, signal number or siginfo pointer in it.
 */
#if !defined(__ASSEMBLER__) && !defined(_ASM_SRC_)
struct hal_task_signal_frame_head {
	uint32_t token;
	uint32_t reserved;
	uintptr_t context_pointer;
};
#endif

#define HAL_TASK_SIGNAL_FRAME_ALIGNMENT			16

#define HAL_TASK_SIGNAL_FRAME_HAS_RESTORER		0
#define HAL_TASK_SIGNAL_FRAME_HAS_SIGNO			0
#define HAL_TASK_SIGNAL_FRAME_HAS_INFO_POINTER		0

#define HAL_TASK_SIGNAL_FRAME_TOKEN_OFFSET		0
#define HAL_TASK_SIGNAL_FRAME_CONTEXT_POINTER_OFFSET	8
#define HAL_TASK_SIGNAL_RESTORER_TOKEN_OFFSET		0
#define HAL_TASK_SIGNAL_RESTORER_CONTEXT_OFFSET		8

/*
 * Atomic
 */

/*
 * Atomic style: the port is uniprocessor, so the read-modify-write and
 * 64-bit operations are serialized with interrupts off (the enter style).
 */
#define HAL_ATOMIC_STYLE	HAL_ATOMIC_STYLE_ENTER

/*
 * Debugging
 */

#if !defined(__ASSEMBLER__) && !defined(_ASM_SRC_)
/*
 * The user integer registers of a task: the thirty-two general registers
 * (r1 is the stack pointer, r2 the thread pointer), the address of the
 * next instruction, the machine state, and the link, count, condition and
 * fixed-point exception registers.
 */
struct hal_gpregs {
	uint64_t r[32];
	uint64_t pc;
	uint64_t msr;
	uint64_t lr;
	uint64_t ctr;
	uint64_t cr;
	uint64_t xer;
};

/*
 * The floating-point state: the thirty-two 64-bit registers and the
 * status and control register.
 */
struct hal_fpregs {
	uint64_t f[32];
	uint32_t fpscr;
	uint32_t reserved;
};

/*
 * The vector state.  The vector unit is not offered, so the set is empty
 * and reading or writing it is refused.
 */
struct hal_vregs {
	uint32_t reserved;
};
#endif

/*
 * Neither tracing nor hardware debug points are offered yet.
 */
#define HAL_DEBUG_HAS_SINGLE_STEP	0

/*
 * No hardware debug point can be given to a task.  The array bound stays
 * one so that the kernel's tables keep a size; a set with any point in
 * it is refused.
 */
#define HAL_DEBUG_POINT_MAX		1
#define HAL_DEBUG_LENGTH_MAX		8

#define HAL_DEBUG_KIND_EXECUTE_OK	0
#define HAL_DEBUG_KIND_WRITE_OK		0
#define HAL_DEBUG_KIND_READ_OK		0
#define HAL_DEBUG_KIND_ACCESS_OK	0

/*
 * Checks
 */
#if !defined(__ASSEMBLER__) && !defined(_ASM_SRC_)
_Static_assert(
	sizeof(uintptr_t) == 8,
	"ppc64 signal frame requires LP64");
_Static_assert(
	sizeof(struct hal_task_signal_frame_head) == 16,
	"ppc64 signal frame head size");
_Static_assert(
	__builtin_offsetof(struct hal_task_signal_frame_head,
			   token) == HAL_TASK_SIGNAL_FRAME_TOKEN_OFFSET,
	"ppc64 signal token offset");
_Static_assert(
	__builtin_offsetof(struct hal_task_signal_frame_head,
			   context_pointer) ==
		HAL_TASK_SIGNAL_FRAME_CONTEXT_POINTER_OFFSET,
	"ppc64 signal context offset");
_Static_assert(
	HAL_TASK_SIGNAL_RESTORER_TOKEN_OFFSET ==
		HAL_TASK_SIGNAL_FRAME_TOKEN_OFFSET,
	"ppc64 restorer token offset");
_Static_assert(
	HAL_TASK_SIGNAL_RESTORER_CONTEXT_OFFSET ==
		HAL_TASK_SIGNAL_FRAME_CONTEXT_POINTER_OFFSET,
	"ppc64 restorer context offset");
#endif

#endif
