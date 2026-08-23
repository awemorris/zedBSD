/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef HAL_HAL_ARCH_AARCH64_H
#define HAL_HAL_ARCH_AARCH64_H

#define HAL_TASK_SIGNAL_FRAME_ALIGNMENT	16

#define HAL_TASK_SIGNAL_FRAME_HAS_RESTORER	0
#define HAL_TASK_SIGNAL_FRAME_HAS_SIGNO	0
#define HAL_TASK_SIGNAL_FRAME_HAS_INFO_POINTER	0

/*
 * Offsets from both the frame head and SP on entry to the restorer.
 */
#define HAL_TASK_SIGNAL_FRAME_TOKEN_OFFSET	0
#define HAL_TASK_SIGNAL_FRAME_CONTEXT_POINTER_OFFSET	8
#define HAL_TASK_SIGNAL_RESTORER_TOKEN_OFFSET	0
#define HAL_TASK_SIGNAL_RESTORER_CONTEXT_OFFSET	8

#if !defined(__ASSEMBLER__) && !defined(_ASM_SRC_)
struct hal_task_signal_frame_head {
	uint32_t token;
	uint32_t reserved;
	uintptr_t context_pointer;
};

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
