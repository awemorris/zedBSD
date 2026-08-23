/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#ifndef HAL_HAL_ARCH_AMD64_H
#define HAL_HAL_ARCH_AMD64_H

#define HAL_TASK_SIGNAL_FRAME_ALIGNMENT 16

#define HAL_TASK_SIGNAL_FRAME_HAS_RESTORER 1
#define HAL_TASK_SIGNAL_FRAME_HAS_SIGNO 0
#define HAL_TASK_SIGNAL_FRAME_HAS_INFO_POINTER 0

/* Offsets from the start of struct hal_task_signal_frame_head. */
#define HAL_TASK_SIGNAL_FRAME_RESTORER_OFFSET 0
#define HAL_TASK_SIGNAL_FRAME_TOKEN_OFFSET 8
#define HAL_TASK_SIGNAL_FRAME_CONTEXT_POINTER_OFFSET 16

/* Offsets from RSP after the signal handler has returned to the restorer. */
#define HAL_TASK_SIGNAL_RESTORER_TOKEN_OFFSET 0
#define HAL_TASK_SIGNAL_RESTORER_CONTEXT_OFFSET 8

#if !defined(__ASSEMBLER__) && !defined(_ASM_SRC_)
struct hal_task_signal_frame_head {
	uintptr_t restorer;
	uint64_t token;
	uintptr_t context_pointer;
};

_Static_assert(sizeof(uintptr_t) == 8, "amd64 signal frame requires LP64");
_Static_assert(sizeof(struct hal_task_signal_frame_head) == 24,
	"amd64 signal frame head size");
_Static_assert(__builtin_offsetof(struct hal_task_signal_frame_head,
	restorer) == HAL_TASK_SIGNAL_FRAME_RESTORER_OFFSET,
	"amd64 signal restorer offset");
_Static_assert(__builtin_offsetof(struct hal_task_signal_frame_head,
	token) == HAL_TASK_SIGNAL_FRAME_TOKEN_OFFSET,
	"amd64 signal token offset");
_Static_assert(__builtin_offsetof(struct hal_task_signal_frame_head,
	context_pointer) == HAL_TASK_SIGNAL_FRAME_CONTEXT_POINTER_OFFSET,
	"amd64 signal context offset");
_Static_assert(HAL_TASK_SIGNAL_RESTORER_TOKEN_OFFSET ==
	HAL_TASK_SIGNAL_FRAME_TOKEN_OFFSET - sizeof(uintptr_t),
	"amd64 restorer token offset");
_Static_assert(HAL_TASK_SIGNAL_RESTORER_CONTEXT_OFFSET ==
	HAL_TASK_SIGNAL_FRAME_CONTEXT_POINTER_OFFSET - sizeof(uintptr_t),
	"amd64 restorer context offset");
#endif

#endif
