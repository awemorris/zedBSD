/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#ifndef HAL_HAL_ARCH_M68030_H
#define HAL_HAL_ARCH_M68030_H

#define HAL_TASK_SIGNAL_FRAME_ALIGNMENT 4

#define HAL_TASK_SIGNAL_FRAME_HAS_RESTORER 1
#define HAL_TASK_SIGNAL_FRAME_HAS_SIGNO 1
#define HAL_TASK_SIGNAL_FRAME_HAS_INFO_POINTER 1

/* Offsets from the start of struct hal_task_signal_frame_head. */
#define HAL_TASK_SIGNAL_FRAME_RESTORER_OFFSET 0
#define HAL_TASK_SIGNAL_FRAME_SIGNO_OFFSET 4
#define HAL_TASK_SIGNAL_FRAME_INFO_POINTER_OFFSET 8
#define HAL_TASK_SIGNAL_FRAME_CONTEXT_POINTER_OFFSET 12
#define HAL_TASK_SIGNAL_FRAME_TOKEN_OFFSET 16

/* Offsets from SP after the signal handler has returned to the restorer. */
#define HAL_TASK_SIGNAL_RESTORER_TOKEN_OFFSET 12
#define HAL_TASK_SIGNAL_RESTORER_CONTEXT_OFFSET 8

#if !defined(__ASSEMBLER__) && !defined(_ASM_SRC_)
struct hal_task_signal_frame_head {
	uintptr_t restorer;
	uint32_t signo;
	uintptr_t info_pointer;
	uintptr_t context_pointer;
	uint32_t token;
};

_Static_assert(sizeof(uintptr_t) == 4, "MC68030 signal frame requires ILP32");
_Static_assert(sizeof(struct hal_task_signal_frame_head) == 20,
	"MC68030 signal frame head size");
_Static_assert(__builtin_offsetof(struct hal_task_signal_frame_head,
	restorer) == HAL_TASK_SIGNAL_FRAME_RESTORER_OFFSET,
	"MC68030 signal restorer offset");
_Static_assert(__builtin_offsetof(struct hal_task_signal_frame_head,
	signo) == HAL_TASK_SIGNAL_FRAME_SIGNO_OFFSET,
	"MC68030 signal number offset");
_Static_assert(__builtin_offsetof(struct hal_task_signal_frame_head,
	info_pointer) == HAL_TASK_SIGNAL_FRAME_INFO_POINTER_OFFSET,
	"MC68030 signal info offset");
_Static_assert(__builtin_offsetof(struct hal_task_signal_frame_head,
	context_pointer) == HAL_TASK_SIGNAL_FRAME_CONTEXT_POINTER_OFFSET,
	"MC68030 signal context offset");
_Static_assert(__builtin_offsetof(struct hal_task_signal_frame_head,
	token) == HAL_TASK_SIGNAL_FRAME_TOKEN_OFFSET,
	"MC68030 signal token offset");
_Static_assert(HAL_TASK_SIGNAL_RESTORER_TOKEN_OFFSET ==
	HAL_TASK_SIGNAL_FRAME_TOKEN_OFFSET - sizeof(uintptr_t),
	"MC68030 restorer token offset");
_Static_assert(HAL_TASK_SIGNAL_RESTORER_CONTEXT_OFFSET ==
	HAL_TASK_SIGNAL_FRAME_CONTEXT_POINTER_OFFSET - sizeof(uintptr_t),
	"MC68030 restorer context offset");
#endif

#endif
