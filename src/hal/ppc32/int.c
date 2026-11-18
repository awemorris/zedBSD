/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The 32-bit PowerPC exception dispatch: the vectors, the decrementer to
 * the clock, external interrupts to the board, sc to the kernel's system
 * calls, a storage fault first to the page table's reload and then to
 * the kernel's fault handlers.
 */

#include <hal/hal.h>
#include "bsp-mac99/bsp.h"
#include "asm.h"
#include "defs.h"
#include "int.h"
#include "space.h"
#include "task.h"

/* The program exception's reasons in the saved machine state. */
#define PROGRAM_TRAP		0x00020000U
#define PROGRAM_PRIVILEGED	0x00040000U
#define PROGRAM_ILLEGAL		0x00080000U

/* The instruction fault's reasons in the saved machine state. */
#define ISI_NOT_FOUND		0x40000000U
#define ISI_PROTECTION		0x08000000U

/* The data cache's line, which the vector copy is pushed by. */
#define CACHE_LINE		32U

/*
 * The classification of one exception for the kernel.
 */
struct int_fault {
	enum hal_trap_cause cause;
	enum hal_trap_mode mode;
	uintptr_t address;
};

static void classify(const struct ppc32_trap_frame *frame, struct int_fault *fault);
static int take_storage_fault(struct ppc32_trap_frame *frame);
static void take_syscall(struct ppc32_trap_frame *frame);
static void take_user_fault(struct ppc32_trap_frame *frame);
static void take_kernel_fault(struct ppc32_trap_frame *frame);
static void describe_frame(const struct ppc32_trap_frame *frame);

/*
 * Copies the vector stub to every vector, each loading its own offset,
 * and makes the copies visible to instruction fetch.
 */
void
ppc32_int_init(
	void)
{
	uint32_t *destination;
	uintptr_t vector;
	uintptr_t line;
	size_t words;
	size_t index;
	size_t li_index;

	/* The stub's length and the place of its li in words. */
	words = (size_t)(ppc32_vector_stub_end - ppc32_vector_stub);
	li_index = (size_t)(ppc32_vector_stub_li - ppc32_vector_stub);

	/* One copy per vector, its li loading the vector's offset. */
	for (vector = PPC32_VECTOR_FIRST; vector < PPC32_VECTOR_END; vector += PPC32_VECTOR_STRIDE) {
		destination = (uint32_t *)vector;
		for (index = 0; index < words; index++)
			destination[index] = ppc32_vector_stub[index];
		destination[li_index] = ppc32_vector_stub_li[0] | (uint32_t)vector;
	}

	/* Pushes the copies to memory and drops stale instructions. */
	for (line = PPC32_VECTOR_FIRST; line < PPC32_VECTOR_END; line += CACHE_LINE)
		__asm__ volatile("dcbst 0,%0" : : "r"(line) : "memory");
	__asm__ volatile("sync" : : : "memory");
	for (line = PPC32_VECTOR_FIRST; line < PPC32_VECTOR_END; line += CACHE_LINE)
		__asm__ volatile("icbi 0,%0" : : "r"(line) : "memory");
	__asm__ volatile("sync\n\tisync" : : : "memory");
	hal_puts("PPC32 TRAP READY\n");
}

/*
 * Takes one exception, called by the entry with the frame it built.
 */
void
ppc32_trap(
	struct ppc32_trap_frame *frame)
{
	int from_user;
	int reloaded;

	/* The saved machine state says where the processor came from. */
	from_user = 0;
	if ((frame->msr & PPC32_MSR_PR) != 0)
		from_user = 1;

	/* The tick, and the board's interrupts. */
	if (frame->vector == PPC32_VECTOR_DECREMENTER || frame->vector == PPC32_VECTOR_EXTERNAL) {
		if (from_user)
			ppc32_task_enter_user_frame(frame);
		if (frame->vector == PPC32_VECTOR_DECREMENTER) {
			mac99_clock_sample(frame->pc);
			mac99_clock_interrupt();
		} else {
			mac99_take_external();
		}

		/* A user context goes back through the kernel's return path. */
		if (from_user) {
			kernel_user_return_handler();
			ppc32_task_leave_user_frame();
		}

		return;
	}

	/* A user page the hashed table had no room for, put back. */
	if (frame->vector == PPC32_VECTOR_DSI || frame->vector == PPC32_VECTOR_ISI) {
		reloaded = take_storage_fault(frame);
		if (reloaded)
			return;
	}

	/* sc from user is a system call. */
	if (from_user && frame->vector == PPC32_VECTOR_SYSCALL) {
		take_syscall(frame);
		return;
	}

	/* Any other exception is a fault of whichever side took it. */
	if (from_user) {
		take_user_fault(frame);
		return;
	}

	/* Anything else is the kernel's own fault. */
	take_kernel_fault(frame);
}

/*
 * Gives the page table the chance to put back a user page, or to make a
 * page writable at its first write; reports whether it did.
 */
static int
take_storage_fault(
	struct ppc32_trap_frame *frame)
{
	uintptr_t address;
	int store;
	int execute;
	int reloaded;

	/* The fault's address and kind. */
	if (frame->vector == PPC32_VECTOR_ISI) {
		if ((frame->msr & ISI_NOT_FOUND) == 0)
			return 0;
		address = frame->pc;
		store = 0;
		execute = 1;
	} else {
		if ((frame->dsisr & (PPC32_DSISR_NOT_FOUND | PPC32_DSISR_PROTECTION)) == 0)
			return 0;
		address = frame->dar;
		store = (frame->dsisr & PPC32_DSISR_STORE) != 0;
		execute = 0;
	}

	/* Only the user range has pages to put back. */
	reloaded = ppc32_space_reload(address, store, execute);
	if (!reloaded)
		return 0;

	/* Succeeded: the access is retried. */
	return 1;
}

/* Turns an exception into the kernel's cause, access mode and address. */
static void
classify(
	const struct ppc32_trap_frame *frame,
	struct int_fault *fault)
{
	/* Starts from a fault with no mode and no address. */
	fault->cause = HAL_TRAP_CAUSE_OTHER;
	fault->mode = HAL_TRAP_MODE_NONE;
	fault->address = 0;

	/* Chooses by the vector. */
	switch (frame->vector) {
	case PPC32_VECTOR_DSI:
		fault->cause = HAL_TRAP_CAUSE_PAGE_FAULT;
		fault->address = frame->dar;
		fault->mode = HAL_TRAP_MODE_READ;
		if ((frame->dsisr & PPC32_DSISR_STORE) != 0)
			fault->mode = HAL_TRAP_MODE_WRITE;
		break;
	case PPC32_VECTOR_ISI:
		fault->cause = HAL_TRAP_CAUSE_PAGE_FAULT;
		fault->address = frame->pc;
		fault->mode = HAL_TRAP_MODE_EXEC;
		break;
	case PPC32_VECTOR_ALIGNMENT:
		fault->cause = HAL_TRAP_CAUSE_ALIGNMENT;
		fault->address = frame->dar;
		break;
	case PPC32_VECTOR_PROGRAM:
		fault->cause = HAL_TRAP_CAUSE_ILLEGAL_INSN;
		if ((frame->msr & PROGRAM_TRAP) != 0)
			fault->cause = HAL_TRAP_CAUSE_BREAKPOINT;
		break;
	case PPC32_VECTOR_TRACE:
		fault->cause = HAL_TRAP_CAUSE_BREAKPOINT;
		break;
	default:
		break;
	}
}

/*
 * Serves a system call: the number in r0, the arguments in r3 to r8, the
 * result back in r3.  sc leaves the program counter past the instruction.
 */
static void
take_syscall(
	struct ppc32_trap_frame *frame)
{
	uintptr_t arguments[HAL_SYSCALL_ARGS];
	intptr_t result;
	unsigned index;

	/* Copies the arguments before the kernel may change the frame. */
	for (index = 0; index < HAL_SYSCALL_ARGS; index++)
		arguments[index] = frame->r[3U + index];

	/* The kernel serves the call with the frame as the user context. */
	ppc32_task_enter_user_frame(frame);
	result = kernel_syscall_handler(frame->r[0], arguments);
	frame->r[3] = (uint32_t)result;
	kernel_user_return_handler();
	ppc32_task_leave_user_frame();
}

/*
 * Hands a fault of user code to the kernel, which either fixes it or
 * arranges the task's end.
 */
static void
take_user_fault(
	struct ppc32_trap_frame *frame)
{
	struct int_fault fault;
	int handled;

	/* The kernel sees the cause and the task's frame. */
	classify(frame, &fault);
	ppc32_task_enter_user_frame(frame);
	handled = kernel_user_fault_handler(fault.cause, fault.mode, frame->pc, fault.address, frame->vector, frame->dsisr);

	/* A fault the kernel could not take stops the system. */
	if (handled != HAL_TRAP_RET_SUCCESS) {
		ppc32_task_leave_user_frame();
		describe_frame(frame);
		HAL_FATAL("ppc32 user fault handler failed");
	}

	/* The kernel's return path, then the frame is no longer the task's context. */
	kernel_user_return_handler();
	ppc32_task_leave_user_frame();
}

/*
 * Hands a fault of the kernel to the kernel's handler, which may fix it;
 * any other is fatal.
 */
static void
take_kernel_fault(
	struct ppc32_trap_frame *frame)
{
	struct int_fault fault;
	int handled;

	/* The kernel's handler sees the cause and the faulting instruction. */
	classify(frame, &fault);
	handled = kernel_sys_fault_handler(fault.cause, fault.mode, frame->pc, fault.address, frame->vector, frame->dsisr);

	/* Handled: the return retries the instruction. */
	if (handled == HAL_TRAP_RET_SUCCESS)
		return;

	/* Anything else stops the system with the frame on the console. */
	describe_frame(frame);
	HAL_FATAL("unhandled ppc32 kernel exception");
}

/* Writes a frame's registers to the console. */
static void
describe_frame(
	const struct ppc32_trap_frame *frame)
{
	unsigned index;

	/* The special registers first. */
	hal_printf("ppc32 trap vector=%x pc=%x msr=%x lr=%x dar=%x dsisr=%x\n",
		   frame->vector, frame->pc, frame->msr, frame->lr, frame->dar, frame->dsisr);

	/* Then the general registers, four to a line. */
	for (index = 0; index < 32U; index += 4U) {
		hal_printf("r%u=%x r%u=%x r%u=%x r%u=%x\n",
			   index, frame->r[index], index + 1U, frame->r[index + 1U],
			   index + 2U, frame->r[index + 2U], index + 3U, frame->r[index + 3U]);
	}
}
