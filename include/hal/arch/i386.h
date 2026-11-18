/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef HAL_HAL_ARCH_I386_H
#define HAL_HAL_ARCH_I386_H

/*
 * Signal Frame
 */

/*
 * Signal frame head. (signal stack frame head)
 */
#if !defined(__ASSEMBLER__) && !defined(_ASM_SRC_)
struct hal_task_signal_frame_head {
	uintptr_t restorer;
	uint32_t signo;
	uintptr_t info_pointer;
	uintptr_t context_pointer;
	uint32_t token;
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
#define HAL_TASK_SIGNAL_FRAME_HAS_SIGNO			1
#define HAL_TASK_SIGNAL_FRAME_HAS_INFO_POINTER		1

/*
 * Offsets from the start of struct hal_task_signal_frame_head.
 */
#define HAL_TASK_SIGNAL_FRAME_RESTORER_OFFSET		0
#define HAL_TASK_SIGNAL_FRAME_SIGNO_OFFSET		4
#define HAL_TASK_SIGNAL_FRAME_INFO_POINTER_OFFSET	8
#define HAL_TASK_SIGNAL_FRAME_CONTEXT_POINTER_OFFSET	12
#define HAL_TASK_SIGNAL_FRAME_TOKEN_OFFSET		16

/*
 * Offsets from ESP after the signal handler has returned to the restorer.
 */
#define HAL_TASK_SIGNAL_RESTORER_TOKEN_OFFSET		12
#define HAL_TASK_SIGNAL_RESTORER_CONTEXT_OFFSET		8

/*
 * Atomic
 */

/*
 * Atomic style. (Use enter style instead of CAS)
 */
#define HAL_ATOMIC_STYLE	HAL_ATOMIC_STYLE_ENTER

/*
 * Checks
 */
#if !defined(__ASSEMBLER__) && !defined(_ASM_SRC_)
_Static_assert(
	sizeof(uintptr_t) == 4,
	"i386 signal frame requires ILP32");
_Static_assert(
	sizeof(struct hal_task_signal_frame_head) == 20,
	"i386 signal frame head size");
_Static_assert(
	__builtin_offsetof(struct hal_task_signal_frame_head,
			   restorer) == HAL_TASK_SIGNAL_FRAME_RESTORER_OFFSET,
	"i386 signal restorer offset");
_Static_assert(
	__builtin_offsetof(struct hal_task_signal_frame_head,
			   signo) == HAL_TASK_SIGNAL_FRAME_SIGNO_OFFSET,
	"i386 signal number offset");
_Static_assert(
	__builtin_offsetof(struct hal_task_signal_frame_head,
			   info_pointer) ==
		HAL_TASK_SIGNAL_FRAME_INFO_POINTER_OFFSET,
	"i386 signal info offset");
_Static_assert(
	__builtin_offsetof(struct hal_task_signal_frame_head,
			   context_pointer) ==
		HAL_TASK_SIGNAL_FRAME_CONTEXT_POINTER_OFFSET,
	"i386 signal context offset");
_Static_assert(
	__builtin_offsetof(struct hal_task_signal_frame_head,
			   token) == HAL_TASK_SIGNAL_FRAME_TOKEN_OFFSET,
	"i386 signal token offset");
_Static_assert(
	HAL_TASK_SIGNAL_RESTORER_TOKEN_OFFSET ==
		HAL_TASK_SIGNAL_FRAME_TOKEN_OFFSET - sizeof(uintptr_t),
	"i386 restorer token offset");
_Static_assert(
	HAL_TASK_SIGNAL_RESTORER_CONTEXT_OFFSET ==
		HAL_TASK_SIGNAL_FRAME_CONTEXT_POINTER_OFFSET -
			sizeof(uintptr_t),
	"i386 restorer context offset");

/*
 * The integer registers of a stopped task, as the HAL reports them.
 */
struct hal_gpregs {
	uint32_t eax, ebx, ecx, edx;
	uint32_t esi, edi, ebp, esp;
	uint32_t eip, eflags;
	uint32_t cs, ss, ds, es, fs, gs;
};

/*
 * The x87 state.
 */
struct hal_fpregs {
	uint16_t control;
	uint16_t status;
	uint16_t tag;
	uint16_t opcode;
	uint32_t instruction_pointer;
	uint32_t data_pointer;
	uint8_t stack[8][16];
};

/*
 * The vector state.
 */
struct hal_vregs {
	uint32_t control;
	uint32_t control_mask;
	uint8_t xmm[8][16];
};
#endif

#define HAL_DEBUG_POINT_MAX		4
#define HAL_DEBUG_HAS_SINGLE_STEP	1

#endif
