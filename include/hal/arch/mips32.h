/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef HAL_HAL_ARCH_MIPS32_H
#define HAL_HAL_ARCH_MIPS32_H

/*
 * The periodic tick, in hertz.  As on the other ports the kernel keeps a
 * millisecond tick; the HAL programs the board's timer source (the
 * coprocessor 0 counter, or a SoC timer where the core has none) from it.
 */
#define HAL_TIMER_FREQUENCY	(1000U)

/*
 * Signal Frame
 */

/*
 * Signal frame head: the 16-byte head of the other 32-bit ports, its
 * pointer 32 bits wide and the rest of its slot kept as padding, without
 * a restorer, signal number or siginfo pointer in it.
 */
#if !defined(__ASSEMBLER__) && !defined(_ASM_SRC_)
struct hal_task_signal_frame_head {
	uint32_t token;
	uint32_t reserved;
	uintptr_t context_pointer;
	uint32_t padding;
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
 * Atomic style: a 32-bit MIPS processor has load-linked and
 * store-conditional on words only, with no 64-bit form, so the
 * read-modify-write and 64-bit operations are serialized under one guard
 * taken with the swap below (the enter style).
 */
#define HAL_ATOMIC_STYLE	HAL_ATOMIC_STYLE_ENTER

/*
 * Debugging
 */

#if !defined(__ASSEMBLER__) && !defined(_ASM_SRC_)
/*
 * The user integer registers of a task: the thirty-two general registers
 * with r[0] always zero and r[26], r[27] (the exception registers k0, k1)
 * read as zero, the multiply and divide results, the restart address,
 * and the status, cause and bad address the task last trapped with.
 */
struct hal_gpregs {
	uint32_t r[32];
	uint32_t hi;
	uint32_t lo;
	uint32_t pc;
	uint32_t status;
	uint32_t cause;
	uint32_t badvaddr;
};

/*
 * The floating-point state: the thirty-two 64-bit registers the unit has
 * with Status.FR set, its control and status register and its
 * implementation register.
 */
struct hal_fpregs {
	uint64_t f[32];
	uint32_t fcsr;
	uint32_t fir;
};

/*
 * The vector state.  The processors this port runs on have no vector
 * unit, so the set is empty and reading or writing it is refused.
 */
struct hal_vregs {
	uint32_t reserved;
};
#endif

/*
 * No single-step bit is given to a task here yet.
 */
#define HAL_DEBUG_HAS_SINGLE_STEP	0

/*
 * No hardware debug point can be given to a task (the watch registers
 * are not used yet).  The array bound stays one so that the kernel's
 * tables keep a size; a set with any point in it is refused.
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
	sizeof(uintptr_t) == 4,
	"mips32 signal frame requires ILP32");
_Static_assert(
	sizeof(struct hal_task_signal_frame_head) == 16,
	"mips32 signal frame head size");
_Static_assert(
	__builtin_offsetof(struct hal_task_signal_frame_head,
			   token) == HAL_TASK_SIGNAL_FRAME_TOKEN_OFFSET,
	"mips32 signal token offset");
_Static_assert(
	__builtin_offsetof(struct hal_task_signal_frame_head,
			   context_pointer) ==
		HAL_TASK_SIGNAL_FRAME_CONTEXT_POINTER_OFFSET,
	"mips32 signal context offset");
_Static_assert(
	HAL_TASK_SIGNAL_RESTORER_TOKEN_OFFSET ==
		HAL_TASK_SIGNAL_FRAME_TOKEN_OFFSET,
	"mips32 restorer token offset");
_Static_assert(
	HAL_TASK_SIGNAL_RESTORER_CONTEXT_OFFSET ==
		HAL_TASK_SIGNAL_FRAME_CONTEXT_POINTER_OFFSET,
	"mips32 restorer context offset");
#endif

#endif
