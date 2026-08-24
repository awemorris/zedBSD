/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef HAL_HAL_ARCH_AARCH64_H
#define HAL_HAL_ARCH_AARCH64_H

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

#if !defined(__ASSEMBLER__) && !defined(_ASM_SRC_)

static inline bool
hal_atomic_uint_try_acquire(
	volatile unsigned *value)
{
	unsigned previous;
	unsigned status;

	__asm__ volatile("1: ldaxr %w0, [%2]\n"
			 "   cbnz %w0, 2f\n"
			 "   stxr %w1, %w3, [%2]\n"
			 "   cbnz %w1, 1b\n"
			 "   b 3f\n"
			 "2: clrex\n"
			 "3:\n"
			 : "=&r"(previous), "=&r"(status)
			 : "r"(value), "r"(1U)
			 : "memory");
	return previous == 0U;
}

static inline void
hal_atomic_relax(void)
{
	__asm__ volatile("yield" ::: "memory");
}

#endif

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
