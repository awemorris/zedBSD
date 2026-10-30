/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef HAL_HAL_ARCH_M68040_H
#define HAL_HAL_ARCH_M68040_H

/*
 * The periodic tick, in hertz.  Every time the kernel keeps is counted in
 * these ticks, and every conversion to and from milliseconds uses this value.
 * The same rate as the MC68040 port.
 */
#define HAL_TIMER_FREQUENCY	(100U)

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
#define HAL_TASK_SIGNAL_FRAME_ALIGNMENT			4

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
 * Offsets from SP after the signal handler has returned to the restorer.
 */
#define HAL_TASK_SIGNAL_RESTORER_TOKEN_OFFSET		12
#define HAL_TASK_SIGNAL_RESTORER_CONTEXT_OFFSET		8

/*
 * Atomic
 */

/*
 * Atomic style: the port is uniprocessor, so the read-modify-write and
 * 64-bit operations are serialized with interrupts off (the enter style).
 */
#define HAL_ATOMIC_STYLE	HAL_ATOMIC_STYLE_ENTER

/*
 * Debugging
 */

#if !defined(__ASSEMBLER__) && !defined(_ASM_SRC_)
/*
 * The user integer registers of a task: the eight data and eight address
 * registers (a[7] is the user stack pointer), the status register and the
 * address of the next instruction.
 */
struct hal_gpregs {
	uint32_t d[8];
	uint32_t a[8];
	uint32_t sr;
	uint32_t pc;
};

/*
 * The floating-point state: the eight 96-bit registers in the extended
 * memory format, then the control, status and instruction address
 * registers.
 */
struct hal_fpregs {
	uint32_t fp[8][3];
	uint32_t fpcr;
	uint32_t fpsr;
	uint32_t fpiar;
};

/* The processor has no vector unit; the set is empty and is refused. */
struct hal_vregs {
	uint32_t reserved;
};
#endif

/* No single-step or hardware debug point is offered to a task yet. */
#define HAL_DEBUG_HAS_SINGLE_STEP	0
#define HAL_DEBUG_POINT_MAX		1
#define HAL_DEBUG_LENGTH_MAX		4

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
	"MC68040 signal frame requires ILP32");
_Static_assert(
	sizeof(struct hal_task_signal_frame_head) == 20,
	"MC68040 signal frame head size");
_Static_assert(
	__builtin_offsetof(struct hal_task_signal_frame_head,
			   restorer) == HAL_TASK_SIGNAL_FRAME_RESTORER_OFFSET,
	"MC68040 signal restorer offset");
_Static_assert(
	__builtin_offsetof(struct hal_task_signal_frame_head,
			   signo) == HAL_TASK_SIGNAL_FRAME_SIGNO_OFFSET,
	"MC68040 signal number offset");
_Static_assert(
	__builtin_offsetof(struct hal_task_signal_frame_head,
			   info_pointer) ==
		HAL_TASK_SIGNAL_FRAME_INFO_POINTER_OFFSET,
	"MC68040 signal info offset");
_Static_assert(
	__builtin_offsetof(struct hal_task_signal_frame_head,
			   context_pointer) ==
		HAL_TASK_SIGNAL_FRAME_CONTEXT_POINTER_OFFSET,
	"MC68040 signal context offset");
_Static_assert(
	__builtin_offsetof(struct hal_task_signal_frame_head,
			   token) == HAL_TASK_SIGNAL_FRAME_TOKEN_OFFSET,
	"MC68040 signal token offset");
_Static_assert(
	HAL_TASK_SIGNAL_RESTORER_TOKEN_OFFSET ==
		HAL_TASK_SIGNAL_FRAME_TOKEN_OFFSET - sizeof(uintptr_t),
	"MC68040 restorer token offset");
_Static_assert(
	HAL_TASK_SIGNAL_RESTORER_CONTEXT_OFFSET ==
		HAL_TASK_SIGNAL_FRAME_CONTEXT_POINTER_OFFSET -
			sizeof(uintptr_t),
	"MC68040 restorer context offset");
#endif

#endif
