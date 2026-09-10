/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * User trap handling and the test probes that observe it.
 *
 * The HAL reports every user-mode interrupt and fault here.  A page fault
 * is first offered to the address space; every other fault, or a page
 * fault the address space rejects, becomes a signal.  The probe records
 * let tests observe the last trap without a debugger.
 */

#include "kern/user-probe.h"
#include "kern/process.h"
#include "kern/sched.h"
#include "kern/signal.h"
#include "kern/thread.h"
#include "kern/vmspace.h"

#include <errno.h>
#include <hal/hal.h>
#include <string.h>

volatile struct user_int_probe user_int_probe;
volatile struct user_fault_probe user_fault_probe;

_Static_assert(sizeof(user_int_probe.eip) == sizeof(uintptr_t),
	       "user trap PC width must match uintptr_t");
_Static_assert(sizeof(user_fault_probe.fault_address) == sizeof(uintptr_t),
	       "user fault address width must match uintptr_t");

/*
 * Records one system call entry in the interrupt probe.
 *
 * The fixed entry carries no vector or privilege; those fields stay zero.
 */
void
user_probe_syscall(
	uint32_t number)
{
	struct thread *thread;

	/* Ignores a call outside any process. */
	thread = curthread;
	if (thread == NULL || thread->proc == NULL)
		return;

	/* Records the call, publishing the magic last. */
	user_int_probe.count++;
	user_int_probe.vector = 0;
	user_int_probe.cs = 0;
	user_int_probe.eip = 0;
	user_int_probe.eax = number;
	user_int_probe.pid = thread->proc->pid;
	user_int_probe.tid = thread->tid;
	hal_compiler_barrier();
	user_int_probe.magic = USER_INT_PROBE_MAGIC;
}

/*
 * Handles a user-mode fault.
 *
 * A page fault that the address space resolves is retried transparently.
 * Every other fault is recorded in the fault probe and delivered to the
 * process as the matching signal.
 */
int
kernel_user_fault_handler(
	int cause,
	int mode,
	uintptr_t pc,
	uintptr_t address,
	uintptr_t vector,
	uintptr_t error_code)
{
	struct thread *thread;
	struct signal_info info;
	uint32_t required;
	int signo;
	int page_fault_error;
	int result;

	thread = curthread;
	page_fault_error = 0;
	result = HAL_TRAP_RET_FAILED;

	/* User-fault callbacks use the same masked HAL frame contract as syscalls. */
	if (hal_irq_disable())
		HAL_FATAL("user fault callback entered with IRQs enabled");
	sched_accounting_kernel_enter();
	hal_irq_enable();

	/* Ignores a fault outside any process. */
	if (thread == NULL || thread->proc == NULL)
		goto out;

	/* Offers a page fault to the address space first. */
	if (cause == HAL_TRAP_CAUSE_PAGE_FAULT && thread->proc->vmspace != NULL) {
		if (mode == HAL_TRAP_MODE_EXEC)
			required = HAL_SPACE_EXEC;
		else if (mode == HAL_TRAP_MODE_WRITE)
			required = HAL_SPACE_WRITE;
		else
			required = HAL_SPACE_READ;
		page_fault_error = vmspace_fault(thread->proc->vmspace,
		    address, required);
		if (page_fault_error == 0) {
			result = HAL_TRAP_RET_SUCCESS;
			goto out;
		}
	}

	/* Retains the fault on the thread for the signal frame. */
	thread->fault_vector = (uint32_t)vector;
	thread->fault_eip = pc;
	thread->fault_address = address;

	/* Records the fault, publishing the magic last. */
	user_fault_probe.count++;
	user_fault_probe.vector = (uint32_t)vector;
	user_fault_probe.cs = 0;
	user_fault_probe.eip = pc;
	user_fault_probe.error_code = error_code;
	user_fault_probe.fault_address = address;
	user_fault_probe.pid = thread->proc->pid;
	user_fault_probe.tid = thread->tid;
	hal_compiler_barrier();
	user_fault_probe.magic = USER_FAULT_PROBE_MAGIC;

	/* Maps the fault cause to a signal. */
	switch (cause) {
	case HAL_TRAP_CAUSE_ARITHMETIC:
		signo = SIGFPE;
		break;
	case HAL_TRAP_CAUSE_BREAKPOINT:
		signo = SIGTRAP;
		break;
	case HAL_TRAP_CAUSE_ILLEGAL_INSN:
		signo = SIGILL;
		break;
	case HAL_TRAP_CAUSE_PAGE_FAULT:
		if (page_fault_error == ENXIO)
			signo = SIGBUS;
		else
			signo = SIGSEGV;
		break;
	case HAL_TRAP_CAUSE_PROTECTION:
		signo = SIGSEGV;
		break;
	default:
		signo = SIGBUS;
		break;
	}

	/* Describes the fault for the signal handler. */
	memset(&info, 0, sizeof(info));
	if (cause == HAL_TRAP_CAUSE_PAGE_FAULT)
		info.address = address;
	else
		info.address = pc;

	/* Selects the signal code. */
	switch (signo) {
	case SIGFPE:
		info.code = FPE_INTDIV;
		break;
	case SIGTRAP:
		info.code = TRAP_BRKPT;
		break;
	case SIGILL:
		info.code = ILL_ILLOPC;
		break;
	case SIGSEGV:
		if (page_fault_error == EACCES)
			info.code = SEGV_ACCERR;
		else
			info.code = SEGV_MAPERR;
		break;
	case SIGBUS:
		if (cause == HAL_TRAP_CAUSE_ALIGNMENT)
			info.code = BUS_ADRALN;
		else
			info.code = BUS_ADRERR;
		break;
	default:
		info.code = SI_KERNEL;
		break;
	}

	/* Delivers the signal; a delivered signal completes the trap. */
	if (signal_send_process_info(thread->proc, signo, &info) == 0)
		result = HAL_TRAP_RET_SUCCESS;

out:
	/* Restores the masked frame contract before returning to the HAL. */
	if (!hal_irq_disable())
		HAL_FATAL("user fault callback returned with IRQs disabled");
	sched_accounting_kernel_leave();

	/* Reports whether the trap was handled. */
	return result;
}

/*
 * Clears both probes.
 */
void
user_probe_init(
	void)
{
	/* Invalidates the records and restarts their counters. */
	user_int_probe.magic = 0;
	user_int_probe.count = 0;
	user_fault_probe.magic = 0;
	user_fault_probe.count = 0;
}

/*
 * Handles a supervisor-mode fault.
 *
 * Nothing in the kernel can resume from one yet, so every fault is left to
 * the HAL, which prints its register diagnostics and stops.  The arguments
 * are the same as for a user fault so that a later fixup table can use them.
 */
int
kernel_sys_fault_handler(
	int cause,
	int mode,
	uintptr_t pc,
	uintptr_t address,
	uintptr_t vector,
	uintptr_t error_code)
{
	(void)cause;
	(void)mode;
	(void)pc;
	(void)address;
	(void)vector;
	(void)error_code;

	/* Failed. */
	return HAL_TRAP_RET_FAILED;
}
