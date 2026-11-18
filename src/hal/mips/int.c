/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The MIPS trap handling: the dispatch of each exception trap.S saves a
 * frame for, by Cause.ExcCode, and the Status every return erets with.
 *
 * The interrupts go to the board, which serves its tick and its
 * controllers.  A TLB invalid or modified exception on a mapped user page
 * brings the TLB up to date and retries the access; a coprocessor
 * unusable exception for coprocessor 1 hands the floating-point unit to
 * the running context.  A system call from user mode goes to the kernel,
 * and from the kernel only the HAL's own check makes one.  Every other
 * exception is a fault of the side that took it: a user fault goes to the
 * kernel with the task's frame, a kernel fault to the kernel's handler,
 * and one nobody takes is reported with the frame before the processor
 * stops.
 */

#include <hal/hal.h>
#include "asm.h"
#include "bsp.h"
#include "cpu.h"
#include "fpu.h"
#include "space.h"
#include "status.h"
#include "task.h"
#include "trap.h"

/*
 * The system call number the trap self-test makes, and the answer the
 * handler gives it.  Neither is a call of the kernel's.
 */
#define TRAP_SELFTEST_CALL	0x5a17c0deU
#define TRAP_SELFTEST_ANSWER	0xa5e83f21U

/* The coprocessor number of the floating-point unit in Cause.CE. */
#define TRAP_COPROCESSOR_FLOAT	1U

/* The bytes of one instruction, which a system call's return goes past. */
#define TRAP_INSTRUCTION_BYTES	4U

/*
 * What the dispatch learned of a fault for the kernel: its cause, the
 * kind of access and the address the access was to.
 */
struct trap_fault {
	enum hal_trap_cause cause;
	enum hal_trap_mode mode;
	uintptr_t address;
};

/*
 * The top of the kernel stack an exception from user mode moves to.
 *
 * Read by trap.S at every entry from user mode; set by the task switch
 * for the task it resumes.  Zero while no task has entered user mode.
 */
uintptr_t mips_kernel_stack_pointer;

/*
 * The interrupts the dispatch found no source for, since boot.  Kept for
 * the debugger; it only grows.
 */
static volatile uint32_t trap_spurious_interrupts;

/*
 * The paging self-test's handler of the kernel's own faults, NULL outside
 * the test.  Set and cleared by the test with interrupts off.
 */
static mips_trap_fault_hook_t trap_fault_hook;

static int trap_from_user(const struct mips_trap_frame *frame);
static void trap_interrupt(struct mips_trap_frame *frame);
static int trap_space(struct mips_trap_frame *frame, uint32_t code);
static int trap_float_unusable(const struct mips_trap_frame *frame, uint32_t code);
static void trap_user_system_call(struct mips_trap_frame *frame);
static void trap_kernel_system_call(struct mips_trap_frame *frame);
static void trap_user_fault(struct mips_trap_frame *frame, uint32_t code);
static void trap_kernel_fault(struct mips_trap_frame *frame, uint32_t code, int space_result);
static void classify(const struct mips_trap_frame *frame, uint32_t code, struct trap_fault *fault);
static void finish_status(struct mips_trap_frame *frame);
static void describe_frame(const struct mips_trap_frame *frame, uint32_t code);
static uint32_t trap_selftest_call(uint32_t number, uint32_t *error);

/*
 * Handles the exception whose frame trap.S saved.
 *
 * On return the frame's Status is the one the eret uses: rebuilt for a
 * return to user, the kernel's own with today's interrupt mask for a
 * return to the kernel.
 */
void
mips_trap(
	struct mips_trap_frame *frame)
{
	uint32_t code;
	int from_user;
	int space_result;
	int handled;

	/* The exception's code, from Cause, and the side that took it. */
	code = (frame->cause >> MIPS_CAUSE_CODE_SHIFT) & MIPS_CAUSE_CODE_MASK;
	from_user = trap_from_user(frame);

	/* The tick and the board's interrupts, on either side. */
	if (code == MIPS_EXCEPTION_INTERRUPT) {
		/* A user task's frame is published for the kernel's handlers. */
		if (from_user)
			mips_task_enter_user_frame(frame);

		/* Serves the interrupt. */
		trap_interrupt(frame);

		/* A user task returns through the kernel's return work. */
		if (from_user) {
			kernel_user_return_handler();
			mips_task_leave_user_frame();
		}

		/* Sets the Status the exception returns with. */
		finish_status(frame);
		return;
	}

	/* A mapped user page whose TLB entry was out of date: the access is retried. */
	space_result = trap_space(frame, code);
	if (space_result == MIPS_SPACE_FAULT_FIXED) {
		finish_status(frame);
		return;
	}

	/* The floating-point unit for the running context: the instruction is retried. */
	handled = trap_float_unusable(frame, code);
	if (handled) {
		finish_status(frame);
		return;
	}

	/* A system call: the kernel's from user, the HAL's own check from the kernel. */
	if (code == MIPS_EXCEPTION_SYSTEM_CALL) {
		if (from_user) {
			trap_user_system_call(frame);
		} else {
			trap_kernel_system_call(frame);
		}

		/* Sets the Status the exception returns with. */
		finish_status(frame);
		return;
	}

	/* Any other exception is a fault of whichever side took it. */
	if (from_user) {
		trap_user_fault(frame, code);
	} else {
		trap_kernel_fault(frame, code, space_result);
	}

	/* Sets the Status the exception returns with. */
	finish_status(frame);
}

/*
 * Checks that a system call from the kernel goes through the trap entry
 * and comes back with its answer and past its instruction.
 */
void
mips_trap_selftest(
	void)
{
	uint32_t answer;
	uint32_t error;

	/* Makes the test's call, which only this file's handler answers. */
	error = 1;
	answer = trap_selftest_call(TRAP_SELFTEST_CALL, &error);
	if (answer != TRAP_SELFTEST_ANSWER)
		HAL_FATAL("mips trap selftest answer");
	if (error != 0)
		HAL_FATAL("mips trap selftest error mark");

	/* Reports the passed check. */
	hal_puts(MIPS_HAL_NAME " HAL TRAP PASS\n");
}

/*
 * Installs or removes the paging self-test's handler of the kernel's own
 * faults.
 */
void
mips_trap_set_fault_hook(
	mips_trap_fault_hook_t hook)
{
	/* The hook takes the kernel's faults until it is removed. */
	trap_fault_hook = hook;
}

/* Reports whether the exception came from user mode, by the saved mode bits. */
static int
trap_from_user(
	const struct mips_trap_frame *frame)
{
	/* Any mode but the kernel's is user mode on this port. */
	if ((frame->status & MIPS_STATUS_MODE_MASK) != 0)
		return 1;
	return 0;
}

/* Serves every interrupt line that is both pending and unmasked. */
static void
trap_interrupt(
	struct mips_trap_frame *frame)
{
	uint32_t pending;
	bool served;

	/* The lines pending at the exception that the interrupted code let in. */
	pending = frame->cause & frame->status & MIPS_CAUSE_PENDING_MASK;

	/* The board serves its lines: its tick and its interrupt controllers. */
	served = mips_board_interrupt(pending);

	/* Counts an interrupt with no line the HAL serves. */
	if (!served)
		trap_spurious_interrupts++;
}

/*
 * Brings the TLB up to date for a TLB invalid or modified exception on a
 * mapped page that allows the access.
 *
 * Reports what the selected space found (MIPS_SPACE_FAULT_FIXED when the
 * access is to be retried), or MIPS_TRAP_NOT_SPACE for an exception that
 * is not the TLB's.
 */
static int
trap_space(
	struct mips_trap_frame *frame,
	uint32_t code)
{
	int access;
	int found;

	/* The access the exception was taken for. */
	if (code == MIPS_EXCEPTION_TLB_LOAD) {
		access = MIPS_SPACE_ACCESS_LOAD;
	} else if (code == MIPS_EXCEPTION_TLB_STORE) {
		access = MIPS_SPACE_ACCESS_STORE;
	} else if (code == MIPS_EXCEPTION_TLB_MODIFIED) {
		access = MIPS_SPACE_ACCESS_MODIFY;
	} else {
		return MIPS_TRAP_NOT_SPACE;
	}

	/* The selected space's table decides. */
	found = mips_space_fault(frame->bad_address, access);

	/* Succeeded: what the table found. */
	return found;
}

/*
 * Hands the floating-point unit to the running context after a
 * coprocessor unusable exception for coprocessor 1.  Reports whether it
 * did, so that the instruction is retried.
 */
static int
trap_float_unusable(
	const struct mips_trap_frame *frame,
	uint32_t code)
{
	uint32_t coprocessor;
	bool taken;

	/* Only the unusable exception of coprocessor 1 is the unit's. */
	if (code != MIPS_EXCEPTION_COPROCESSOR)
		return 0;
	coprocessor = (frame->cause >> MIPS_CAUSE_COPROCESSOR_SHIFT) & MIPS_CAUSE_COPROCESSOR_MASK;
	if (coprocessor != TRAP_COPROCESSOR_FLOAT)
		return 0;

	/* Loads the running context's registers when the unit is used. */
	taken = mips_fpu_take();
	if (!taken)
		return 0;

	/* Succeeded: the instruction is retried with the unit on. */
	return 1;
}

/*
 * Serves a system call from user mode.
 *
 * The number is v0 and the arguments a0 to a3, t0 and t1.  The return
 * goes past the syscall instruction before the kernel sees the frame, so
 * that the frame's restart address is the next instruction; the result
 * is v0 and the error mark a3 is cleared (the user ABI defines its use).
 */
static void
trap_user_system_call(
	struct mips_trap_frame *frame)
{
	uintptr_t arguments[HAL_SYSCALL_ARGS];
	intptr_t result;

	/* Copies the arguments before the kernel may change the frame. */
	arguments[0] = frame->r[MIPS_REGISTER_A0];
	arguments[1] = frame->r[MIPS_REGISTER_A1];
	arguments[2] = frame->r[MIPS_REGISTER_A2];
	arguments[3] = frame->r[MIPS_REGISTER_A3];
	arguments[4] = frame->r[MIPS_REGISTER_T0];
	arguments[5] = frame->r[MIPS_REGISTER_T1];
	frame->exception_pc += TRAP_INSTRUCTION_BYTES;

	/* The kernel serves the call with the frame as the user context. */
	mips_task_enter_user_frame(frame);
	result = kernel_syscall_handler(frame->r[MIPS_REGISTER_V0], arguments);
	frame->r[MIPS_REGISTER_V0] = (uintptr_t)result;
	frame->r[MIPS_REGISTER_A3] = 0;
	kernel_user_return_handler();
	mips_task_leave_user_frame();
}

/*
 * Answers a system call from the kernel.
 *
 * Only the trap self-test's call exists; any other number is a fault.
 * The result is v0, the error mark a3, and the return goes past the
 * syscall instruction.
 */
static void
trap_kernel_system_call(
	struct mips_trap_frame *frame)
{
	/* Refuses a call nothing answers. */
	if ((uint32_t)frame->r[MIPS_REGISTER_V0] != TRAP_SELFTEST_CALL) {
		trap_kernel_fault(frame, MIPS_EXCEPTION_SYSTEM_CALL, MIPS_TRAP_NOT_SPACE);
		return;
	}

	/*
	 * Answers the self-test and moves past the instruction. A 32-bit value
	 * lives sign-extended in a 64-bit register, which the caller's compare
	 * assumes, so the answer is widened through int32_t.
	 */
	frame->r[MIPS_REGISTER_V0] = (uintptr_t)(intptr_t)(int32_t)TRAP_SELFTEST_ANSWER;
	frame->r[MIPS_REGISTER_A3] = 0;
	frame->exception_pc += TRAP_INSTRUCTION_BYTES;
}

/* Hands a user fault to the kernel with the task's frame. */
static void
trap_user_fault(
	struct mips_trap_frame *frame,
	uint32_t code)
{
	struct trap_fault fault;
	int handled;

	/* A floating-point exception leaves the unit's Cause field clear for the return. */
	if (code == MIPS_EXCEPTION_FLOAT)
		(void)mips_fpu_clear_cause();

	/* The kernel sees the cause and the task's frame. */
	classify(frame, code, &fault);
	mips_task_enter_user_frame(frame);
	handled = kernel_user_fault_handler(fault.cause, fault.mode, frame->exception_pc, fault.address, code, frame->cause);

	/* A fault the kernel could not take stops the system. */
	if (handled != HAL_TRAP_RET_SUCCESS) {
		mips_task_leave_user_frame();
		describe_frame(frame, code);
		HAL_FATAL("mips user fault handler failed");
	}

	/* The kernel's return path, then the frame is no longer the task's context. */
	kernel_user_return_handler();
	mips_task_leave_user_frame();
}

/*
 * Hands a kernel fault to the self-test's hook while it runs, else to
 * the kernel's handler; stops when neither takes it.  space_result is
 * what the selected space found for a TLB exception.
 */
static void
trap_kernel_fault(
	struct mips_trap_frame *frame,
	uint32_t code,
	int space_result)
{
	struct trap_fault fault;
	int handled;
	int taken;

	/* The self-test's own faults go past their instruction. */
	if (trap_fault_hook != NULL) {
		taken = trap_fault_hook(frame, space_result);
		if (taken) {
			frame->exception_pc += TRAP_INSTRUCTION_BYTES;
			return;
		}
	}

	/* The kernel's handler sees the cause and the faulting instruction. */
	classify(frame, code, &fault);
	handled = kernel_sys_fault_handler(fault.cause, fault.mode, frame->exception_pc, fault.address, code, frame->cause);

	/* Handled: the return retries the instruction. */
	if (handled == HAL_TRAP_RET_SUCCESS)
		return;

	/* Anything else stops the system with the frame on the console. */
	describe_frame(frame, code);
	HAL_FATAL("unhandled mips kernel exception");
}

/* Tells the kernel what a fault was, from its exception code. */
static void
classify(
	const struct mips_trap_frame *frame,
	uint32_t code,
	struct trap_fault *fault)
{
	/* Starts from a fault with no mode and no address. */
	fault->cause = HAL_TRAP_CAUSE_OTHER;
	fault->mode = HAL_TRAP_MODE_NONE;
	fault->address = 0;

	/* Chooses by the exception code. */
	switch (code) {
	case MIPS_EXCEPTION_TLB_LOAD:
		/* A load, or the fetch of the faulting instruction itself. */
		fault->cause = HAL_TRAP_CAUSE_PAGE_FAULT;
		fault->address = frame->bad_address;
		fault->mode = HAL_TRAP_MODE_READ;
		if (frame->bad_address == frame->exception_pc)
			fault->mode = HAL_TRAP_MODE_EXEC;
		break;
	case MIPS_EXCEPTION_TLB_STORE:
	case MIPS_EXCEPTION_TLB_MODIFIED:
		fault->cause = HAL_TRAP_CAUSE_PAGE_FAULT;
		fault->address = frame->bad_address;
		fault->mode = HAL_TRAP_MODE_WRITE;
		break;
	case MIPS_EXCEPTION_ADDRESS_LOAD:
	case MIPS_EXCEPTION_ADDRESS_STORE:
		/* A user address is misaligned; a kernel one is out of reach. */
		fault->address = frame->bad_address;
		fault->cause = HAL_TRAP_CAUSE_ALIGNMENT;
		if (frame->bad_address >= MIPS_USER_LIMIT)
			fault->cause = HAL_TRAP_CAUSE_PROTECTION;
		fault->mode = HAL_TRAP_MODE_READ;
		if (code == MIPS_EXCEPTION_ADDRESS_STORE)
			fault->mode = HAL_TRAP_MODE_WRITE;
		break;
	case MIPS_EXCEPTION_BREAKPOINT:
		fault->cause = HAL_TRAP_CAUSE_BREAKPOINT;
		break;
	case MIPS_EXCEPTION_RESERVED:
	case MIPS_EXCEPTION_COPROCESSOR:
		fault->cause = HAL_TRAP_CAUSE_ILLEGAL_INSN;
		break;
	case MIPS_EXCEPTION_OVERFLOW:
	case MIPS_EXCEPTION_FLOAT:
		fault->cause = HAL_TRAP_CAUSE_ARITHMETIC;
		break;
	default:
		break;
	}
}

/*
 * Builds the Status of the return; a saved user Status is never trusted.
 *
 * Either way CU1 is on only for the owner of the floating-point unit: a
 * return to user asks the unit, and a return to the kernel takes CU1 as
 * the handling left it, since the handler gives the unit away by turning
 * CU1 off and takes it by turning CU1 on.
 */
static void
finish_status(
	struct mips_trap_frame *frame)
{
	struct mips_task *task;
	uint32_t current;
	bool owner;
	bool usable;

	/* Whether the running context owns the unit, and the Status the handling left. */
	task = mips_task_running();
	owner = mips_fpu_owned_by(task);
	usable = mips_fpu_usable();
	current = mips_read_status();

	/* A return to user builds the whole Status; a return to the kernel keeps its own. */
	if ((frame->status & MIPS_STATUS_MODE_MASK) != 0) {
		frame->status = mips_user_status(current, owner, usable);
	} else {
		frame->status = mips_kernel_status(frame->status, current);
		frame->status &= ~(uint32_t)MIPS_STATUS_COPROCESSOR_1;
		frame->status |= current & MIPS_STATUS_COPROCESSOR_1;
	}
}

/* Prints a frame's exception registers and its general registers. */
static void
describe_frame(
	const struct mips_trap_frame *frame,
	uint32_t code)
{
	unsigned index;

	/* The exception's registers first. */
	hal_printf(MIPS_HAL_NAME " TRAP FAULT code=%u cause=%08x epc=%08llx badvaddr=%08llx status=%08x\n",
		   code,
		   (uint32_t)frame->cause,
		   (unsigned long long)frame->exception_pc,
		   (unsigned long long)frame->bad_address,
		   (uint32_t)frame->status);

	/* Then the general registers, four to a line. */
	for (index = 0; index < 32U; index += 4U) {
		hal_printf("r%u=%08llx r%u=%08llx r%u=%08llx r%u=%08llx\n",
			   index,
			   (unsigned long long)frame->r[index],
			   index + 1U,
			   (unsigned long long)frame->r[index + 1U],
			   index + 2U,
			   (unsigned long long)frame->r[index + 2U],
			   index + 3U,
			   (unsigned long long)frame->r[index + 3U]);
	}
}

/* Makes a system call from the kernel with a number, reporting v0 and the a3 mark. */
static uint32_t
trap_selftest_call(
	uint32_t number,
	uint32_t *error)
{
	register uint32_t call_number __asm__("$2");
	register uint32_t mark __asm__("$7");

	/* Traps with the number in v0; the handler answers in v0 and a3. */
	call_number = number;
	mark = *error;
	__asm__ volatile("syscall" : "+r"(call_number), "+r"(mark) : : "memory");
	*error = mark;

	/* Succeeded: the handler's answer. */
	return call_number;
}
