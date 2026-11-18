/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The armv7 traps: interrupts, system calls and faults, from vectors.S to
 * the kernel.
 *
 * An undefined instruction is first offered to the floating-point unit's
 * lazy switch (fpu.c): the first floating-point instruction of a context
 * runs again once the unit holds the context's registers.
 *
 * A data or prefetch abort on a user page is first offered to the address
 * spaces: the first access and the first store of a page fault on this
 * processor only so that the HAL can record them (space.c), and those are
 * resolved without the kernel.  Every other fault is classified by its
 * fault status and given to the kernel.
 *
 * locore.S unmasks asynchronous aborts before the boot processor has its
 * record; one that the loader left pending is taken right there.  That
 * first one is noted for the boot log and the boot goes on; any other trap
 * before the record exists stops the processor.
 */

#include <hal/hal.h>
#include "asm.h"
#include "defs.h"
#include "fpu.h"
#include "int.h"
#include "irq.h"
#include "percpu.h"
#include "space.h"
#include "task.h"

/*
 * The classification of one exception for the kernel.
 */
struct int_fault {
	enum hal_trap_cause cause;
	enum hal_trap_mode mode;
	uintptr_t address;
};

/*
 * The asynchronous abort the loader left pending, taken before the boot
 * processor had its record: its fault status and address, and whether
 * there was one.  Written at most once, by that trap, before C runs, and
 * read by the boot afterwards.
 */
static int pending_abort_seen;
static uint32_t pending_abort_status;
static uint32_t pending_abort_address;

static int take_early_abort(uint32_t kind);
static unsigned fault_status(uint32_t status_register);
static void classify(unsigned status, int prefetch, int write, uintptr_t address, struct int_fault *fault);
static void take_interrupt(struct armv7_trap_frame *frame, int from_user);
static void take_syscall(struct armv7_trap_frame *frame);
static void take_abort(struct armv7_trap_frame *frame, uint32_t kind, int from_user);
static void take_undefined(struct armv7_trap_frame *frame, int from_user);
static void report_user_fault(struct armv7_trap_frame *frame, uint32_t kind, const struct int_fault *fault, uint32_t status);
static void report_kernel_fault(struct armv7_trap_frame *frame, uint32_t kind, const struct int_fault *fault, uint32_t status);
static int selftest_fault(struct armv7_trap_frame *frame, uint32_t kind, uintptr_t address, uint32_t status);
static void describe_frame(const struct armv7_trap_frame *frame, uint32_t kind);

/*
 * Handles one trap with its frame, from vectors.S with interrupts masked.
 */
void
armv7_trap(
	struct armv7_trap_frame *frame,
	uint32_t kind)
{
	struct armv7_percpu *cpu;
	int from_user;
	int taken;

	/* A trap before the processor has a record cannot be served, but for the loader's pending abort. */
	cpu = armv7_percpu_current();
	if (cpu == NULL) {
		taken = take_early_abort(kind);
		if (taken)
			return;
		describe_frame(frame, kind);
		armv7_halt_forever();
	}

	/* A trap from user mode leaves the user mode in the saved status. */
	from_user = 0;
	if ((frame->cpsr & ARMV7_PSR_MODE_MASK) == ARMV7_PSR_MODE_USR)
		from_user = 1;

	/* Hands the trap to the code that serves its kind. */
	switch (kind) {
	case ARMV7_TRAP_IRQ:
		take_interrupt(frame, from_user);
		return;
	case ARMV7_TRAP_SVC:
		if (from_user) {
			take_syscall(frame);
			return;
		}
		break;
	case ARMV7_TRAP_DATA_ABORT:
	case ARMV7_TRAP_PREFETCH_ABORT:
		take_abort(frame, kind, from_user);
		return;
	case ARMV7_TRAP_UNDEFINED:
		take_undefined(frame, from_user);
		return;
	default:
		break;
	}

	/* A kernel system call, an FIQ or a vector that is never taken stops the kernel. */
	describe_frame(frame, kind);
	HAL_FATAL("unexpected armv7 exception");
}

/*
 * Reports the asynchronous abort the loader left pending, which the boot
 * took and went past, and whether there was one.
 */
int
armv7_trap_pending_abort(
	uint32_t *status,
	uint32_t *address)
{
	/* No abort was pending when the boot unmasked them. */
	if (!pending_abort_seen)
		return 0;

	/* Succeeded: the abort's fault status and address. */
	*status = pending_abort_status;
	*address = pending_abort_address;
	return 1;
}

/*
 * Stops the kernel at a frame that would return to user mode with
 * interrupts masked, which vectors.S refuses.
 */
void
armv7_trap_return_refused(
	struct armv7_trap_frame *frame)
{
	/* Shows the frame, then stops. */
	describe_frame(frame, ARMV7_TRAP_UNUSED);
	HAL_FATAL("armv7 return to user mode with interrupts masked");
}

/*
 * Notes the first asynchronous abort taken before the boot processor has
 * its record, which the loader left pending, and reports whether it was
 * one.  The interrupted code resumes where it was.
 */
static int
take_early_abort(
	uint32_t kind)
{
	uint32_t status_register;
	unsigned status;

	/* Only a data abort can be the pending one, and only one is let through. */
	if (kind != ARMV7_TRAP_DATA_ABORT)
		return 0;
	if (pending_abort_seen)
		return 0;

	/* Only an asynchronous external abort is the loader's. */
	status_register = armv7_read_dfsr();
	status = fault_status(status_register);
	if (status != ARMV7_FS_ASYNC_EXTERNAL)
		return 0;

	/* Succeeded: kept for the boot log, and the boot goes on. */
	pending_abort_status = status_register;
	pending_abort_address = armv7_read_dfar();
	pending_abort_seen = 1;
	return 1;
}

/* Reads the five-bit fault status from a fault status register. */
static unsigned
fault_status(
	uint32_t status_register)
{
	unsigned status;

	/* FS[3:0] in bits 3 to 0 and FS[4] in bit 10. */
	status = (unsigned)(status_register & 0xfU);
	if ((status_register & 0x400U) != 0)
		status |= 0x10U;

	/* Succeeded: the status code. */
	return status;
}

/*
 * Turns a fault status into the kernel's cause, mode and address.
 */
static void
classify(
	unsigned status,
	int prefetch,
	int write,
	uintptr_t address,
	struct int_fault *fault)
{
	/* Starts from a fault with no mode and no address. */
	fault->cause = HAL_TRAP_CAUSE_OTHER;
	fault->mode = HAL_TRAP_MODE_NONE;
	fault->address = 0;

	/* Chooses the kernel's words for the status. */
	switch (status) {
	case ARMV7_FS_TRANSLATION_SECTION:
	case ARMV7_FS_TRANSLATION_PAGE:
	case ARMV7_FS_AF_SECTION:
	case ARMV7_FS_AF_PAGE:
	case ARMV7_FS_PERMISSION_SECTION:
	case ARMV7_FS_PERMISSION_PAGE:
	case ARMV7_FS_DOMAIN_SECTION:
	case ARMV7_FS_DOMAIN_PAGE:
		/* A missing or protected page, at the address of the access. */
		fault->cause = HAL_TRAP_CAUSE_PAGE_FAULT;
		fault->address = address;
		if (prefetch) {
			fault->mode = HAL_TRAP_MODE_EXEC;
		} else if (write) {
			fault->mode = HAL_TRAP_MODE_WRITE;
		} else {
			fault->mode = HAL_TRAP_MODE_READ;
		}

		break;
	case ARMV7_FS_ALIGNMENT:
		fault->cause = HAL_TRAP_CAUSE_ALIGNMENT;
		fault->address = address;
		break;
	case ARMV7_FS_DEBUG:
		/* A breakpoint instruction comes as a prefetch abort's debug event. */
		fault->cause = HAL_TRAP_CAUSE_DEBUG_POINT;
		if (prefetch)
			fault->cause = HAL_TRAP_CAUSE_BREAKPOINT;
		fault->address = address;
		break;
	case ARMV7_FS_ICACHE:
	case ARMV7_FS_EXTERNAL:
	case ARMV7_FS_WALK_EXTERNAL_L1:
	case ARMV7_FS_WALK_EXTERNAL_L2:
	case ARMV7_FS_ASYNC_EXTERNAL:
		fault->cause = HAL_TRAP_CAUSE_MACHINE_CHECK;
		break;
	default:
		break;
	}
}

/*
 * Serves one interrupt.
 *
 * An interrupt of user code is served with the task's user frame recorded
 * and returns through the kernel's return path, as a system call does.
 */
static void
take_interrupt(
	struct armv7_trap_frame *frame,
	int from_user)
{
	/* A user context is recorded before the kernel sees the interrupt. */
	if (from_user)
		armv7_task_enter_user_frame(frame);

	/* Takes the interrupt at the GIC and serves it. */
	armv7_irq_take();

	/* A user context goes back through the kernel's return path. */
	if (from_user) {
		kernel_user_return_handler();
		armv7_task_leave_user_frame();
	}
}

/*
 * Serves a system call: the number in r7, the arguments in r0 to r5, the
 * result back in r0.  The frame already returns past the svc.
 */
static void
take_syscall(
	struct armv7_trap_frame *frame)
{
	uintptr_t arguments[HAL_SYSCALL_ARGS];
	intptr_t result;
	unsigned index;

	/* Collects the arguments. */
	for (index = 0; index < HAL_SYSCALL_ARGS; index++)
		arguments[index] = (uintptr_t)frame->r[ARMV7_FRAME_R0 + index];

	/* The kernel owns accounting and its interruptible window. */
	armv7_task_enter_user_frame(frame);
	result = kernel_syscall_handler(frame->r[ARMV7_FRAME_R7], arguments);
	frame->r[ARMV7_FRAME_R0] = (uint32_t)result;
	kernel_user_return_handler();
	armv7_task_leave_user_frame();
}

/*
 * Serves a data or prefetch abort: resolves the first access or store of
 * a user page, or reports the fault to the kernel.
 */
static void
take_abort(
	struct armv7_trap_frame *frame,
	uint32_t kind,
	int from_user)
{
	struct int_fault fault;
	uint32_t status_register;
	uintptr_t address;
	unsigned status;
	int prefetch;
	int write;
	int resolved;
	int taken;

	/* Reads the abort's status and address. */
	prefetch = 0;
	write = 0;
	if (kind == ARMV7_TRAP_PREFETCH_ABORT) {
		prefetch = 1;
		status_register = armv7_read_ifsr();
		address = armv7_read_ifar();
	} else {
		status_register = armv7_read_dfsr();
		address = armv7_read_dfar();
		if ((status_register & ARMV7_DFSR_WNR) != 0)
			write = 1;
	}

	/* The fault's kind, from the status register's FS bits. */
	status = fault_status(status_register);

	/* A fault of a cache maintenance operation is never the page's first use. */
	resolved = 0;
	if (prefetch || (status_register & ARMV7_DFSR_CM) == 0)
		resolved = armv7_space_resolve_fault(address, status, write);

	/* The first access or store of a user page runs again. */
	if (resolved)
		return;

	/* Describes the fault for the kernel. */
	classify(status, prefetch, write, address, &fault);
	if (from_user) {
		report_user_fault(frame, kind, &fault, status_register);
		return;
	}

	/* The paging self-test's own faults are recorded and skipped. */
	taken = selftest_fault(frame, kind, address, status_register);
	if (taken)
		return;

	/* Any other kernel fault is the kernel's. */
	report_kernel_fault(frame, kind, &fault, status_register);
}

/*
 * Serves an undefined instruction: one the floating-point unit refused
 * while it was off runs again once the unit is on; any other is, for the
 * user, an illegal instruction or a floating-point exception for the
 * kernel to signal, and for the kernel its own fault.
 */
static void
take_undefined(
	struct armv7_trap_frame *frame,
	int from_user)
{
	struct int_fault fault;
	int taken;
	int exception;

	/* A Thumb instruction lies 2 bytes below the link register, not 4 as vectors.S took. */
	if ((frame->cpsr & ARMV7_PSR_T) != 0)
		frame->pc += 2U;

	/* The first floating-point instruction of the running context runs again with the unit on. */
	taken = armv7_fpu_take_trap(from_user);
	if (taken)
		return;

	/* An illegal instruction at the address of the instruction. */
	fault.cause = HAL_TRAP_CAUSE_ILLEGAL_INSN;
	fault.mode = HAL_TRAP_MODE_EXEC;
	fault.address = frame->pc;

	/* A user floating-point instruction the unit stopped with an exception is an arithmetic fault. */
	if (from_user) {
		exception = armv7_fpu_take_exception();
		if (exception)
			fault.cause = HAL_TRAP_CAUSE_ARITHMETIC;
	}

	/* The user's goes to the kernel with the frame recorded. */
	if (from_user) {
		report_user_fault(frame, ARMV7_TRAP_UNDEFINED, &fault, 0);
		return;
	}

	/* The kernel's is its own fault. */
	report_kernel_fault(frame, ARMV7_TRAP_UNDEFINED, &fault, 0);
}

/*
 * Hands a user fault to the kernel, which resolves it or signals the task.
 */
static void
report_user_fault(
	struct armv7_trap_frame *frame,
	uint32_t kind,
	const struct int_fault *fault,
	uint32_t status)
{
	int handled;

	/* Reports the fault with the frame recorded. */
	armv7_task_enter_user_frame(frame);
	handled = kernel_user_fault_handler(fault->cause,
					    fault->mode,
					    (uintptr_t)frame->pc,
					    fault->address,
					    (uintptr_t)kind,
					    (uintptr_t)status);

	/* The kernel always resolves a user fault, if only by a signal. */
	if (handled != HAL_TRAP_RET_SUCCESS) {
		armv7_task_leave_user_frame();
		describe_frame(frame, kind);
		HAL_FATAL("armv7 user fault handler failed");
	}

	/* The kernel's return path, then the frame is no longer the task's context. */
	kernel_user_return_handler();
	armv7_task_leave_user_frame();
}

/*
 * Hands a kernel fault to the kernel, and stops the processor when it
 * cannot resolve it.
 */
static void
report_kernel_fault(
	struct armv7_trap_frame *frame,
	uint32_t kind,
	const struct int_fault *fault,
	uint32_t status)
{
	int handled;

	/* Asks the kernel. */
	handled = kernel_sys_fault_handler(fault->cause,
					   fault->mode,
					   (uintptr_t)frame->pc,
					   fault->address,
					   (uintptr_t)kind,
					   (uintptr_t)status);

	/* A resolved fault resumes the interrupted code. */
	if (handled == HAL_TRAP_RET_SUCCESS)
		return;

	/* An unresolved one is shown and stops the processor. */
	hal_printf("armv7 fault status=%x address=%x\n", status, (uint32_t)fault->address);
	describe_frame(frame, kind);
	HAL_FATAL("unhandled armv7 kernel exception");
}

/*
 * Records a data abort the paging self-test expects, at the address it
 * armed, and skips the faulting instruction.  Reports whether it did.
 */
static int
selftest_fault(
	struct armv7_trap_frame *frame,
	uint32_t kind,
	uintptr_t address,
	uint32_t status)
{
	struct armv7_percpu *cpu;

	/* Only a data abort at the armed address is the test's. */
	cpu = armv7_percpu_current();
	if (!cpu->selftest_fault_armed)
		return 0;
	if (kind != ARMV7_TRAP_DATA_ABORT)
		return 0;
	if (address != cpu->selftest_fault_address)
		return 0;

	/* Records it once and steps over the store, which is one ARM instruction. */
	cpu->selftest_fault_armed = 0;
	cpu->selftest_fault_taken = 1;
	cpu->selftest_fault_status = status;
	frame->pc += 4U;

	/* Succeeded: the test sees the fault it expected. */
	return 1;
}

/* Prints a trap frame's registers for a fault that stops the processor. */
static void
describe_frame(
	const struct armv7_trap_frame *frame,
	uint32_t kind)
{
	unsigned index;

	/* The trap's own registers first. */
	hal_printf("armv7 trap kind=%u pc=%08x cpsr=%08x lr=%08x sp=%08x\n",
		   kind,
		   frame->pc,
		   frame->cpsr,
		   frame->lr_svc,
		   frame->sp_svc);
	hal_printf("user sp=%08x lr=%08x\n", frame->sp_usr, frame->lr_usr);

	/* Then the general registers, four to a line. */
	for (index = 0; index < 13U; index += 4U) {
		hal_printf("r%u=%08x", index, frame->r[index]);
		if (index + 1U < 13U)
			hal_printf(" r%u=%08x", index + 1U, frame->r[index + 1U]);
		if (index + 2U < 13U)
			hal_printf(" r%u=%08x", index + 2U, frame->r[index + 2U]);
		if (index + 3U < 13U)
			hal_printf(" r%u=%08x", index + 3U, frame->r[index + 3U]);
		hal_puts("\n");
	}
}
