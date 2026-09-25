/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef HAL_HAL_ARCH_AMD64_H
#define HAL_HAL_ARCH_AMD64_H

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


/*
 * Debugging
 */

#if !defined(__ASSEMBLER__) && !defined(_ASM_SRC_)
/*
 * The user integer registers of a task.  Segment selectors other than the
 * two the processor saves are not part of a task's user state here, and
 * the general-purpose segment base is the one the C library calls the
 * thread pointer.
 */
struct hal_gpregs {
	uint64_t rax, rbx, rcx, rdx;
	uint64_t rsi, rdi, rbp, rsp;
	uint64_t r8, r9, r10, r11;
	uint64_t r12, r13, r14, r15;
	uint64_t rip, rflags;
	uint64_t cs, ss;
	uint64_t fs_base;
	uint64_t gs_base;	/* reserved; user code here has no gs base */
};

/*
 * The x87 state.  Each of the eight registers is ten bytes wide and is
 * held in sixteen, which is how the processor writes them out.
 */
struct hal_fpregs {
	uint16_t control;
	uint16_t status;
	uint16_t tag;
	uint16_t opcode;
	uint64_t instruction_pointer;
	uint64_t data_pointer;
	uint8_t stack[8][16];
};

/*
 * The vector state.
 */
struct hal_vregs {
	uint32_t control;	/* mxcsr */
	uint32_t control_mask;	/* mxcsr_mask */
	uint8_t xmm[16][16];
};
#endif

/*
 * One instruction at a time is a property of the saved flags, so every
 * task can be asked for it.
 */
#define HAL_DEBUG_HAS_SINGLE_STEP	1

/*
 * Four debug registers, shared between instruction and data points: a set
 * of five never fits, whichever kinds it asks for.  A data point covers
 * one, two, four or eight bytes from an address that is a multiple of its
 * length.
 */
#define HAL_DEBUG_POINT_MAX		4
#define HAL_DEBUG_LENGTH_MAX		8

/*
 * The processor distinguishes an instruction point, a store, and a load
 * or store together.  It has no way to watch for a load alone.
 */
#define HAL_DEBUG_KIND_EXECUTE_OK	1
#define HAL_DEBUG_KIND_WRITE_OK		1
#define HAL_DEBUG_KIND_READ_OK		0
#define HAL_DEBUG_KIND_ACCESS_OK	1

#endif
