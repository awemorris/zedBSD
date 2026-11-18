/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The riscv64 traps: interrupts, system calls and faults, from trap.S to
 * the kernel.
 */

#include <hal/hal.h>
#include "asm.h"
#include "clock.h"
#include "defs.h"
#include "int.h"
#include "irq.h"
#include "percpu.h"
#include "plic.h"
#include "task.h"

/* The width of a virtual address, whose top bit an address is sign-extended from. */
#define INT_VA_SIGN_BIT		((uint64_t)1 << (RISCV64_VA_BITS - 1))

/*
 * The trap entry in trap.S, which saves the frame and calls riscv64_trap();
 * stvec points at it.
 */
void riscv64_trap_entry(void);

/*
 * The classification of one exception for the kernel.
 */
struct int_fault {
	enum hal_trap_cause cause;
	enum hal_trap_mode mode;
	uintptr_t address;
};

static void classify(const struct riscv64_trap_frame *frame, struct int_fault *fault);
static void take_interrupt(struct riscv64_trap_frame *frame, uint64_t code, int from_user);
static void take_syscall(struct riscv64_trap_frame *frame);
static void take_user_fault(struct riscv64_trap_frame *frame);
static void take_kernel_fault(struct riscv64_trap_frame *frame);
static void describe_frame(const struct riscv64_trap_frame *frame);

/*
 * Points the boot hart's traps at the HAL's entry and lets the
 * interrupts in: the notifications, the tick and the PLIC.
 */
void
riscv64_int_init(
	void)
{
	/* Every trap enters trap.S from here on. */
	riscv64_write_stvec((uintptr_t)riscv64_trap_entry);

	/* The controller, with every source off. */
	riscv64_plic_init();

	/* Lets the notifications and the PLIC's interrupts reach the hart. */
	riscv64_set_sie(RISCV64_SIE_SSIE | RISCV64_SIE_SEIE);

	/* Starts the boot hart's tick, which enables its own interrupt. */
	riscv64_clock_init();

	/* Reports that traps are taken from here on. */
	hal_puts("RISCV64 TRAP READY\n");
}

/*
 * Points a secondary hart's traps at the HAL's entry and closes its PLIC
 * context; its tick is started separately, once it has a task.
 */
void
riscv64_int_init_secondary(
	void)
{
	/* Every trap enters trap.S from here on. */
	riscv64_write_stvec((uintptr_t)riscv64_trap_entry);

	/* The hart takes no source but lets the notifications in. */
	riscv64_plic_init_hart();
	riscv64_set_sie(RISCV64_SIE_SSIE | RISCV64_SIE_SEIE);
}

/*
 * Handles one trap with its frame, from trap.S with interrupts off.
 */
void
riscv64_trap(
	struct riscv64_trap_frame *frame)
{
	uint64_t cause;
	int from_user;

	/* A trap from user mode leaves the previous-privilege bit clear. */
	from_user = 0;
	if ((frame->sstatus & RISCV64_SSTATUS_SPP) == 0)
		from_user = 1;

	/* An interrupt has the top bit of the cause set. */
	cause = frame->scause;
	if ((cause & RISCV64_CAUSE_INTERRUPT) != 0) {
		take_interrupt(frame, cause & ~(uint64_t)RISCV64_CAUSE_INTERRUPT, from_user);
		return;
	}

	/* A system call from user mode. */
	if (from_user && cause == RISCV64_EXC_USER_ECALL) {
		take_syscall(frame);
		return;
	}

	/* A fault, the user's or the kernel's. */
	if (from_user) {
		take_user_fault(frame);
		return;
	}

	/* A kernel fault. */
	take_kernel_fault(frame);
}

/*
 * Turns an exception into the kernel's cause, mode and address.
 *
 * The address of an instruction fault is sign-extended from the virtual
 * address width: some processors report it without the extension when a
 * fetch crosses a page boundary, and for every other address the
 * extension changes nothing.
 */
static void
classify(
	const struct riscv64_trap_frame *frame,
	struct int_fault *fault)
{
	uintptr_t address;

	/*
	 * Procedure referenced: Linux 6.8.12 arch/riscv/errata/sifive/errata.c
	 * (the CIP-453 erratum: instruction fault addresses that lack the sign
	 * extension).
	 */
	address = (uintptr_t)frame->stval;
	if (frame->scause == RISCV64_EXC_INSN_PAGE_FAULT || frame->scause == RISCV64_EXC_INSN_ACCESS) {
		if ((address & INT_VA_SIGN_BIT) != 0)
			address |= ~(uintptr_t)(INT_VA_SIGN_BIT - 1U);
	}

	/* Starts from a fault with no mode and no address. */
	fault->cause = HAL_TRAP_CAUSE_OTHER;
	fault->mode = HAL_TRAP_MODE_NONE;
	fault->address = 0;

	/* Chooses the kernel's words for the exception. */
	switch (frame->scause) {
	case RISCV64_EXC_INSN_PAGE_FAULT:
		fault->cause = HAL_TRAP_CAUSE_PAGE_FAULT;
		fault->mode = HAL_TRAP_MODE_EXEC;
		fault->address = address;
		break;
	case RISCV64_EXC_LOAD_PAGE_FAULT:
		fault->cause = HAL_TRAP_CAUSE_PAGE_FAULT;
		fault->mode = HAL_TRAP_MODE_READ;
		fault->address = address;
		break;
	case RISCV64_EXC_STORE_PAGE_FAULT:
		fault->cause = HAL_TRAP_CAUSE_PAGE_FAULT;
		fault->mode = HAL_TRAP_MODE_WRITE;
		fault->address = address;
		break;
	case RISCV64_EXC_INSN_ACCESS:
	case RISCV64_EXC_LOAD_ACCESS:
	case RISCV64_EXC_STORE_ACCESS:
		fault->address = address;
		break;
	case RISCV64_EXC_INSN_MISALIGNED:
	case RISCV64_EXC_LOAD_MISALIGNED:
	case RISCV64_EXC_STORE_MISALIGNED:
		fault->cause = HAL_TRAP_CAUSE_ALIGNMENT;
		fault->address = address;
		break;
	case RISCV64_EXC_ILLEGAL_INSN:
		fault->cause = HAL_TRAP_CAUSE_ILLEGAL_INSN;
		break;
	case RISCV64_EXC_BREAKPOINT:
		fault->cause = HAL_TRAP_CAUSE_BREAKPOINT;
		break;
	default:
		break;
	}
}

/*
 * Serves one interrupt: a notification, the tick or the PLIC.
 *
 * An interrupt of user code is served with the task's user frame recorded
 * and returns through the kernel's return path, as a system call does.
 */
static void
take_interrupt(
	struct riscv64_trap_frame *frame,
	uint64_t code,
	int from_user)
{
	/* A user context is recorded before the kernel sees the interrupt. */
	if (from_user)
		riscv64_task_enter_user_frame(frame);

	/* Hands the interrupt to the code that serves its kind. */
	switch (code) {
	case RISCV64_IRQ_SOFTWARE:
		/* Another hart's notification or stop. */
		riscv64_smp_take_ipi();
		break;
	case RISCV64_IRQ_TIMER:
		/* The hart's tick. */
		riscv64_clock_interrupt();
		break;
	case RISCV64_IRQ_EXTERNAL:
		/* The PLIC's sources. */
		riscv64_irq_take_external();
		break;
	default:
		/* An interrupt this port never enables. */
		hal_printf("unexpected riscv64 interrupt %lu\n", code);
		break;
	}

	/* A user context goes back through the kernel's return path. */
	if (from_user) {
		kernel_user_return_handler();
		riscv64_task_leave_user_frame();
	}
}

/*
 * Serves a system call: the number in a7, the arguments in a0 to a5, the
 * result back in a0, and the return past the four-byte ecall.
 */
static void
take_syscall(
	struct riscv64_trap_frame *frame)
{
	uintptr_t arguments[HAL_SYSCALL_ARGS];
	intptr_t result;
	unsigned index;

	/* Collects the arguments. */
	for (index = 0; index < HAL_SYSCALL_ARGS; index++)
		arguments[index] = (uintptr_t)frame->x[RISCV64_FRAME_A0 + index];

	/* Returns past the ecall instruction. */
	frame->sepc += 4U;

	/*
	 * Records the user context, then lets the kernel serve the call; the
	 * kernel owns accounting and its interruptible window.
	 */
	riscv64_task_enter_user_frame(frame);
	result = kernel_syscall_handler((uint32_t)frame->x[RISCV64_FRAME_A7], arguments);

	/* Puts the result where the caller reads it. */
	frame->x[RISCV64_FRAME_A0] = (uint64_t)result;

	/* Runs the kernel's work due before the return to user. */
	kernel_user_return_handler();
	riscv64_task_leave_user_frame();
}

/*
 * Hands a user fault to the kernel, which resolves it or signals the task.
 */
static void
take_user_fault(
	struct riscv64_trap_frame *frame)
{
	struct int_fault fault;
	int handled;

	/* Describes the fault in the kernel's words. */
	classify(frame, &fault);

	/* Reports it with the frame recorded. */
	riscv64_task_enter_user_frame(frame);
	handled = kernel_user_fault_handler(fault.cause,
					    fault.mode,
					    (uintptr_t)frame->sepc,
					    fault.address,
					    (uintptr_t)frame->scause,
					    (uintptr_t)frame->stval);

	/* The kernel always resolves a user fault, if only by a signal. */
	if (handled != HAL_TRAP_RET_SUCCESS) {
		riscv64_task_leave_user_frame();
		describe_frame(frame);
		HAL_FATAL("riscv64 user fault handler failed");
	}

	/* Runs the kernel's work due before the return to user. */
	kernel_user_return_handler();
	riscv64_task_leave_user_frame();
}

/*
 * Hands a kernel fault to the kernel, and stops the hart when it cannot
 * resolve it.
 */
static void
take_kernel_fault(
	struct riscv64_trap_frame *frame)
{
	struct int_fault fault;
	int handled;

	/* Describes the fault and asks the kernel. */
	classify(frame, &fault);
	handled = kernel_sys_fault_handler(fault.cause,
					   fault.mode,
					   (uintptr_t)frame->sepc,
					   fault.address,
					   (uintptr_t)frame->scause,
					   (uintptr_t)frame->stval);

	/* An unresolved fault is shown and stops the hart. */
	if (handled != HAL_TRAP_RET_SUCCESS) {
		describe_frame(frame);
		HAL_FATAL("unhandled riscv64 kernel exception");
	}
}

/* Prints a trap frame's registers for a fault that stops the hart. */
static void
describe_frame(
	const struct riscv64_trap_frame *frame)
{
	unsigned index;

	/* The trap's own registers first. */
	hal_printf("riscv64 trap scause=%lx stval=%lx sepc=%lx sstatus=%lx\n",
		   frame->scause,
		   frame->stval,
		   frame->sepc,
		   frame->sstatus);

	/* Then the general registers, four to a line. */
	for (index = 1; index < 32U; index += 4U) {
		/* The first three registers of the line. */
		hal_printf("x%u=%lx x%u=%lx x%u=%lx",
			   index,
			   frame->x[index],
			   index + 1U,
			   frame->x[index + 1U],
			   index + 2U,
			   frame->x[index + 2U]);

		/* The fourth, which the last line does not have. */
		if (index + 3U < 32U)
			hal_printf(" x%u=%lx", index + 3U, frame->x[index + 3U]);

		/* Ends the line. */
		hal_puts("\n");
	}
}
