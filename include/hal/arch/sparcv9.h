/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef HAL_HAL_ARCH_SPARCV9_H
#define HAL_HAL_ARCH_SPARCV9_H

/*
 * Signal Frame
 */

/*
 * Signal frame head. (signal stack frame head)
 */
#if !defined(__ASSEMBLER__) && !defined(_ASM_SRC_)
struct hal_task_signal_frame_head {
	uint64_t register_window[16];
	uint64_t token;
	uintptr_t context_pointer;
	uint64_t caller_frame_tail[4];
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
 * Offsets from the start of struct hal_task_signal_frame_head.
 */
#define HAL_TASK_SIGNAL_FRAME_TOKEN_OFFSET		128
#define HAL_TASK_SIGNAL_FRAME_CONTEXT_POINTER_OFFSET	136

/*
 * SPARC V9 keeps a 2047-byte stack bias in the architectural SP register.
 */
#define HAL_TASK_SIGNAL_RESTORER_TOKEN_OFFSET	2175
#define HAL_TASK_SIGNAL_RESTORER_CONTEXT_OFFSET	2183

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
	return __atomic_exchange_n(value, 1U, __ATOMIC_ACQUIRE) == 0U;
}

static inline void
hal_atomic_relax(void)
{
	__asm__ volatile("nop" ::: "memory");
}
#endif

/*
 * Checks
 */
#if !defined(__ASSEMBLER__) && !defined(_ASM_SRC_)
_Static_assert(
	sizeof(uintptr_t) == 8,
	"SPARC V9 signal frame requires LP64");
_Static_assert(
	sizeof(struct hal_task_signal_frame_head) == 176,
	"SPARC V9 signal caller frame size");
_Static_assert(
	__builtin_offsetof(struct hal_task_signal_frame_head,
			   token) == HAL_TASK_SIGNAL_FRAME_TOKEN_OFFSET,
	"SPARC V9 signal token offset");
_Static_assert(
	__builtin_offsetof(struct hal_task_signal_frame_head,
			   context_pointer) ==
		HAL_TASK_SIGNAL_FRAME_CONTEXT_POINTER_OFFSET,
	"SPARC V9 signal context offset");
_Static_assert(
	HAL_TASK_SIGNAL_RESTORER_TOKEN_OFFSET ==
		2047 + HAL_TASK_SIGNAL_FRAME_TOKEN_OFFSET,
	"SPARC V9 restorer token offset");
_Static_assert(
	HAL_TASK_SIGNAL_RESTORER_CONTEXT_OFFSET ==
		2047 + HAL_TASK_SIGNAL_FRAME_CONTEXT_POINTER_OFFSET,
	"SPARC V9 restorer context offset");
#endif

#endif
