/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The riscv32 traps: interrupts, system calls and faults, from trap.S to
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

void riscv32_trap_entry(void);

/*
 * The classification of one exception for the kernel.
 */
struct int_fault {
	enum hal_trap_cause cause;
	enum hal_trap_mode mode;
	uintptr_t address;
};

static void classify(const struct riscv32_trap_frame *frame, struct int_fault *fault);
static void take_interrupt(struct riscv32_trap_frame *frame, uint64_t code, int from_user);
static void take_syscall(struct riscv32_trap_frame *frame);
static void take_user_fault(struct riscv32_trap_frame *frame);
static void take_kernel_fault(struct riscv32_trap_frame *frame);
static void describe_frame(const struct riscv32_trap_frame *frame);

/*
 * Points the boot hart's traps at the HAL's entry and lets the
 * interrupts in: the notifications, the tick and the PLIC.
 */
void
riscv32_int_init(
	void)
{
	/* Every trap enters trap.S from here on. */
	riscv32_write_stvec((uintptr_t)riscv32_trap_entry);

	/* The controller, with every source off. */
	riscv32_plic_init();

	/* Lets the notifications and the PLIC's interrupts reach the hart; the tick enables its own. */
	riscv32_set_sie(RISCV32_SIE_SSIE | RISCV32_SIE_SEIE);
	riscv32_clock_init();
	hal_puts("RISCV32 TRAP READY\n");
}

/*
 * Points a secondary hart's traps at the HAL's entry and closes its PLIC
 * context; its tick is started separately, once it has a task.
 */
void
riscv32_int_init_secondary(
	void)
{
	/* Every trap enters trap.S from here on. */
	riscv32_write_stvec((uintptr_t)riscv32_trap_entry);

	/* The hart takes no source but lets the notifications in. */
	riscv32_plic_init_hart();
	riscv32_set_sie(RISCV32_SIE_SSIE | RISCV32_SIE_SEIE);
}

/*
 * Handles one trap with its frame, from trap.S with interrupts off.
 */
void
riscv32_trap(
	struct riscv32_trap_frame *frame)
{
	uint64_t cause;
	int from_user;

	/* A trap from user mode leaves the previous-privilege bit clear. */
	from_user = 0;
	if ((frame->sstatus & RISCV32_SSTATUS_SPP) == 0)
		from_user = 1;

	/* An interrupt has the top bit of the cause set. */
	cause = frame->scause;
	if ((cause & RISCV32_CAUSE_INTERRUPT) != 0) {
		take_interrupt(frame, cause & ~(uint64_t)RISCV32_CAUSE_INTERRUPT, from_user);
		return;
	}

	/* A system call from user mode. */
	if (from_user && cause == RISCV32_EXC_USER_ECALL) {
		take_syscall(frame);
		return;
	}

	/* A fault, the user's or the kernel's. */
	if (from_user) {
		take_user_fault(frame);
		return;
	}

	/* Anything else is the kernel's own fault. */
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
	const struct riscv32_trap_frame *frame,
	struct int_fault *fault)
{
	uintptr_t address;

	/* Sv32 addresses span the whole register, so stval is the address. */
	address = (uintptr_t)frame->stval;

	/* Starts from a fault with no mode and no address. */
	fault->cause = HAL_TRAP_CAUSE_OTHER;
	fault->mode = HAL_TRAP_MODE_NONE;
	fault->address = 0;

	/* Chooses the kernel's words for the exception. */
	switch (frame->scause) {
	case RISCV32_EXC_INSN_PAGE_FAULT:
		fault->cause = HAL_TRAP_CAUSE_PAGE_FAULT;
		fault->mode = HAL_TRAP_MODE_EXEC;
		fault->address = address;
		break;
	case RISCV32_EXC_LOAD_PAGE_FAULT:
		fault->cause = HAL_TRAP_CAUSE_PAGE_FAULT;
		fault->mode = HAL_TRAP_MODE_READ;
		fault->address = address;
		break;
	case RISCV32_EXC_STORE_PAGE_FAULT:
		fault->cause = HAL_TRAP_CAUSE_PAGE_FAULT;
		fault->mode = HAL_TRAP_MODE_WRITE;
		fault->address = address;
		break;
	case RISCV32_EXC_INSN_ACCESS:
	case RISCV32_EXC_LOAD_ACCESS:
	case RISCV32_EXC_STORE_ACCESS:
		fault->address = address;
		break;
	case RISCV32_EXC_INSN_MISALIGNED:
	case RISCV32_EXC_LOAD_MISALIGNED:
	case RISCV32_EXC_STORE_MISALIGNED:
		fault->cause = HAL_TRAP_CAUSE_ALIGNMENT;
		fault->address = address;
		break;
	case RISCV32_EXC_ILLEGAL_INSN:
		fault->cause = HAL_TRAP_CAUSE_ILLEGAL_INSN;
		break;
	case RISCV32_EXC_BREAKPOINT:
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
	struct riscv32_trap_frame *frame,
	uint64_t code,
	int from_user)
{
	/* A user context is recorded before the kernel sees the interrupt. */
	if (from_user)
		riscv32_task_enter_user_frame(frame);

	/* Hands the interrupt to the code that serves its kind. */
	switch (code) {
	case RISCV32_IRQ_SOFTWARE:
		riscv32_smp_take_ipi();
		break;
	case RISCV32_IRQ_TIMER:
		riscv32_clock_interrupt();
		break;
	case RISCV32_IRQ_EXTERNAL:
		riscv32_irq_take_external();
		break;
	default:
		hal_printf("unexpected riscv32 interrupt %llu\n", code);
		break;
	}

	/* A user context goes back through the kernel's return path. */
	if (from_user) {
		kernel_user_return_handler();
		riscv32_task_leave_user_frame();
	}
}

/*
 * Serves a system call: the number in a7, the arguments in a0 to a5, the
 * result back in a0, and the return past the four-byte ecall.
 */
static void
take_syscall(
	struct riscv32_trap_frame *frame)
{
	uintptr_t arguments[HAL_SYSCALL_ARGS];
	intptr_t result;
	unsigned index;

	/* Collects the arguments and returns past the instruction. */
	for (index = 0; index < HAL_SYSCALL_ARGS; index++)
		arguments[index] = (uintptr_t)frame->x[RISCV32_FRAME_A0 + index];
	frame->sepc += 4U;

	/* The kernel owns accounting and its interruptible window. */
	riscv32_task_enter_user_frame(frame);
	result = kernel_syscall_handler((uint32_t)frame->x[RISCV32_FRAME_A7], arguments);
	frame->x[RISCV32_FRAME_A0] = (uint64_t)result;
	kernel_user_return_handler();
	riscv32_task_leave_user_frame();
}

/*
 * Hands a user fault to the kernel, which resolves it or signals the task.
 */
static void
take_user_fault(
	struct riscv32_trap_frame *frame)
{
	struct int_fault fault;
	int handled;

	/* Describes the fault and reports it with the frame recorded. */
	classify(frame, &fault);
	riscv32_task_enter_user_frame(frame);
	handled = kernel_user_fault_handler(fault.cause,
					    fault.mode,
					    (uintptr_t)frame->sepc,
					    fault.address,
					    (uintptr_t)frame->scause,
					    (uintptr_t)frame->stval);

	/* The kernel always resolves a user fault, if only by a signal. */
	if (handled != HAL_TRAP_RET_SUCCESS) {
		riscv32_task_leave_user_frame();
		describe_frame(frame);
		HAL_FATAL("riscv32 user fault handler failed");
	}

	/* The kernel's return path, then the frame is no longer the task's context. */
	kernel_user_return_handler();
	riscv32_task_leave_user_frame();
}

/*
 * Hands a kernel fault to the kernel, and stops the hart when it cannot
 * resolve it.
 */
static void
take_kernel_fault(
	struct riscv32_trap_frame *frame)
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

	/* A resolved fault resumes the interrupted code. */
	if (handled == HAL_TRAP_RET_SUCCESS)
		return;

	/* An unresolved one is shown and stops the hart. */
	describe_frame(frame);
	HAL_FATAL("unhandled riscv32 kernel exception");
}

/* Prints a trap frame's registers for a fault that stops the hart. */
static void
describe_frame(
	const struct riscv32_trap_frame *frame)
{
	unsigned index;

	/* The trap's own registers first. */
	hal_printf("riscv32 trap scause=%lx stval=%lx sepc=%lx sstatus=%lx\n",
		   frame->scause,
		   frame->stval,
		   frame->sepc,
		   frame->sstatus);

	/* Then the general registers, four to a line. */
	for (index = 1; index < 32U; index += 4U) {
		hal_printf("x%u=%lx x%u=%lx x%u=%lx",
			   index,
			   frame->x[index],
			   index + 1U,
			   frame->x[index + 1U],
			   index + 2U,
			   frame->x[index + 2U]);
		if (index + 3U < 32U)
			hal_printf(" x%u=%lx", index + 3U, frame->x[index + 3U]);
		hal_puts("\n");
	}
}
