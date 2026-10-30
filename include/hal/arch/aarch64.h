/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef HAL_HAL_ARCH_AARCH64_H
#define HAL_HAL_ARCH_AARCH64_H

/*
 * The periodic tick, in hertz.  Every time the kernel keeps is counted in
 * these ticks, and every conversion to and from milliseconds uses this value.
 * Vulkan rendering on the desktop wants a millisecond tick.
 */
#define HAL_TIMER_FREQUENCY	(1000U)

/*
 * Signal Frame
 */

/*
 * Signal frame head. (signal stack frame head)
 */
#if !defined(__ASSEMBLER__) && !defined(_ASM_SRC_)
struct hal_task_signal_frame_head {
	uint32_t token;
	uint32_t reserved;
	uintptr_t context_pointer;
};
#endif

/*
 * Alignment.
 */
#define HAL_TASK_SIGNAL_FRAME_ALIGNMENT			16

/*
 * Signal frame style.
 */
#define HAL_TASK_SIGNAL_FRAME_HAS_RESTORER		0
#define HAL_TASK_SIGNAL_FRAME_HAS_SIGNO			0
#define HAL_TASK_SIGNAL_FRAME_HAS_INFO_POINTER		0

/*
 * Offsets from both the frame head and SP on entry to the restorer.
 */
#define HAL_TASK_SIGNAL_FRAME_TOKEN_OFFSET		0
#define HAL_TASK_SIGNAL_FRAME_CONTEXT_POINTER_OFFSET	8
#define HAL_TASK_SIGNAL_RESTORER_TOKEN_OFFSET		0
#define HAL_TASK_SIGNAL_RESTORER_CONTEXT_OFFSET		8

/*
 * Atomic
 */

/*
 * Atomic style. (Use CAS)
 */
#define HAL_ATOMIC_STYLE	HAL_ATOMIC_STYLE_NATIVE

/*
 * Debugging
 */

#if !defined(__ASSEMBLER__) && !defined(_ASM_SRC_)
/*
 * The user integer registers of a task: the thirty-one general registers
 * (x30 is the link register), the stack pointer, the address of the next
 * instruction, the saved processor state, and the thread pointer the C
 * library keeps in TPIDR_EL0.
 */
struct hal_gpregs {
	uint64_t x[31];
	uint64_t sp;
	uint64_t pc;
	uint64_t pstate;
	uint64_t tpidr;
};

/*
 * The floating-point and SIMD state: the thirty-two 128-bit registers and
 * the status and control registers that go with them.
 */
struct hal_fpregs {
	uint8_t v[32][16];
	uint32_t fpsr;
	uint32_t fpcr;
};

/*
 * The vector state beyond the floating-point registers.  The processors
 * this port runs on have no such state (the SIMD registers are the
 * floating-point ones above), so the set is empty and reading or writing
 * it is refused.
 */
struct hal_vregs {
	uint32_t reserved;
};
#endif

/*
 * One instruction at a time is a property of the saved processor state
 * together with the debug control register, so every task can be asked
 * for it.
 */
#define HAL_DEBUG_HAS_SINGLE_STEP	1

/*
 * Instruction points and data points have banks of their own: up to four
 * of each are used, and the processor may have fewer (the architecture
 * guarantees two of each), which is decided when a set is given.  A data
 * point covers one, two, four or eight bytes from an address that is a
 * multiple of its length.
 */
#define HAL_DEBUG_POINT_MAX		8
#define HAL_DEBUG_LENGTH_MAX		8

/*
 * The processor watches for a load, a store, or either.
 */
#define HAL_DEBUG_KIND_EXECUTE_OK	1
#define HAL_DEBUG_KIND_WRITE_OK		1
#define HAL_DEBUG_KIND_READ_OK		1
#define HAL_DEBUG_KIND_ACCESS_OK	1

/*
 * Checks
 */
#if !defined(__ASSEMBLER__) && !defined(_ASM_SRC_)
_Static_assert(
	sizeof(uintptr_t) == 8,
	"AArch64 signal frame requires LP64");
_Static_assert(
	sizeof(struct hal_task_signal_frame_head) == 16,
	"AArch64 signal frame head size");
_Static_assert(
	__builtin_offsetof(struct hal_task_signal_frame_head,
			   token) == HAL_TASK_SIGNAL_FRAME_TOKEN_OFFSET,
	"AArch64 signal token offset");
_Static_assert(
	__builtin_offsetof(struct hal_task_signal_frame_head,
			   context_pointer) ==
		HAL_TASK_SIGNAL_FRAME_CONTEXT_POINTER_OFFSET,
	"AArch64 signal context offset");
_Static_assert(
	HAL_TASK_SIGNAL_RESTORER_TOKEN_OFFSET ==
		HAL_TASK_SIGNAL_FRAME_TOKEN_OFFSET,
	"AArch64 restorer token offset");
_Static_assert(
	HAL_TASK_SIGNAL_RESTORER_CONTEXT_OFFSET ==
		HAL_TASK_SIGNAL_FRAME_CONTEXT_POINTER_OFFSET,
	"AArch64 restorer context offset");
#endif

#endif
