/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef HAL_HAL_ARCH_AMD64_H
#define HAL_HAL_ARCH_AMD64_H

/*
 * Signal Frame
 */

/*
 * Signal frame head. (signal stack frame head)
 */
#if !defined(__ASSEMBLER__) && !defined(_ASM_SRC_)
struct hal_task_signal_frame_head {
	uintptr_t restorer;
	uint64_t token;
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
#define HAL_TASK_SIGNAL_FRAME_HAS_RESTORER		1
#define HAL_TASK_SIGNAL_FRAME_HAS_SIGNO			0
#define HAL_TASK_SIGNAL_FRAME_HAS_INFO_POINTER		0

/*
 * Offsets from the start of struct hal_task_signal_frame_head.
 */
#define HAL_TASK_SIGNAL_FRAME_RESTORER_OFFSET		0
#define HAL_TASK_SIGNAL_FRAME_TOKEN_OFFSET		8
#define HAL_TASK_SIGNAL_FRAME_CONTEXT_POINTER_OFFSET	16

/*
 * Offsets from RSP after the signal handler has returned to the restorer.
 */
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
	unsigned previous = 1U;

	__asm__ volatile("xchgl %0, %1"
			 : "+r"(previous), "+m"(*value)
			 :
			 : "memory");
	return previous == 0U;
}

static inline void
hal_atomic_relax(void)
{
	__asm__ volatile("pause" ::: "memory");
}

#endif

/*
 * Checks
 */
#if !defined(__ASSEMBLER__) && !defined(_ASM_SRC_)
_Static_assert(
	sizeof(uintptr_t) == 8,
	"amd64 signal frame requires LP64");
_Static_assert(
	sizeof(struct hal_task_signal_frame_head) == 24,
	"amd64 signal frame head size");
_Static_assert(
	__builtin_offsetof(struct hal_task_signal_frame_head,
			   restorer) == HAL_TASK_SIGNAL_FRAME_RESTORER_OFFSET,
	"amd64 signal restorer offset");
_Static_assert(
	__builtin_offsetof(struct hal_task_signal_frame_head,
			   token) == HAL_TASK_SIGNAL_FRAME_TOKEN_OFFSET,
	"amd64 signal token offset");
_Static_assert(
	__builtin_offsetof(struct hal_task_signal_frame_head,
			   context_pointer) ==
		HAL_TASK_SIGNAL_FRAME_CONTEXT_POINTER_OFFSET,
	"amd64 signal context offset");
_Static_assert(
	HAL_TASK_SIGNAL_RESTORER_TOKEN_OFFSET ==
		HAL_TASK_SIGNAL_FRAME_TOKEN_OFFSET - sizeof(uintptr_t),
	"amd64 restorer token offset");
_Static_assert(
	HAL_TASK_SIGNAL_RESTORER_CONTEXT_OFFSET ==
		HAL_TASK_SIGNAL_FRAME_CONTEXT_POINTER_OFFSET -
			sizeof(uintptr_t),
	"amd64 restorer context offset");
#endif

#endif
