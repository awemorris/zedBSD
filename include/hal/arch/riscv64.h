/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef HAL_HAL_ARCH_RISCV64_H
#define HAL_HAL_ARCH_RISCV64_H

/*
 * The periodic tick, in hertz.  As on the other 64-bit ports the kernel
 * keeps a millisecond tick; the counter runs at the timebase the device
 * tree reports, and the HAL derives the interval from this value.
 */
#define HAL_TIMER_FREQUENCY	(1000U)

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
 * Atomic style: the A extension's atomic swap.
 */
#define HAL_ATOMIC_STYLE	HAL_ATOMIC_STYLE_NATIVE

/*
 * Debugging
 */

#if !defined(__ASSEMBLER__) && !defined(_ASM_SRC_)
/*
 * The user integer registers of a task: the thirty-two general registers
 * with x[0] always zero (x1 is the return address, x2 the stack pointer,
 * x4 the thread pointer), and the address of the next instruction.
 */
struct hal_gpregs {
	uint64_t x[32];
	uint64_t pc;
};

/*
 * The floating-point state: the thirty-two 64-bit registers and the
 * control and status register.
 */
struct hal_fpregs {
	uint64_t f[32];
	uint32_t fcsr;
	uint32_t reserved;
};

/*
 * The vector state.  The processors this port runs on have no vector
 * extension, so the set is empty and reading or writing it is refused.
 */
struct hal_vregs {
	uint32_t reserved;
};
#endif

/*
 * Supervisor mode has no single-step bit and the firmware does not
 * delegate the debug triggers, so a task cannot be asked to take one
 * instruction here yet.
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
	"riscv64 signal frame requires LP64");
_Static_assert(
	sizeof(struct hal_task_signal_frame_head) == 16,
	"riscv64 signal frame head size");
_Static_assert(
	__builtin_offsetof(struct hal_task_signal_frame_head,
			   token) == HAL_TASK_SIGNAL_FRAME_TOKEN_OFFSET,
	"riscv64 signal token offset");
_Static_assert(
	__builtin_offsetof(struct hal_task_signal_frame_head,
			   context_pointer) ==
		HAL_TASK_SIGNAL_FRAME_CONTEXT_POINTER_OFFSET,
	"riscv64 signal context offset");
_Static_assert(
	HAL_TASK_SIGNAL_RESTORER_TOKEN_OFFSET ==
		HAL_TASK_SIGNAL_FRAME_TOKEN_OFFSET,
	"riscv64 restorer token offset");
_Static_assert(
	HAL_TASK_SIGNAL_RESTORER_CONTEXT_OFFSET ==
		HAL_TASK_SIGNAL_FRAME_CONTEXT_POINTER_OFFSET,
	"riscv64 restorer context offset");
#endif

#endif
