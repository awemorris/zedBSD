/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

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
 * Records a user-mode software interrupt in the interrupt probe.
 */
void
kernel_user_int_handler(
	uint32_t vector,
	uint32_t privilege,
	uintptr_t pc,
	uintptr_t value)
{
	struct thread *thread;

	thread = curthread;

	/* Ignores an interrupt outside any process. */
	if (thread == NULL || thread->proc == NULL)
		return;

	/* Records the trap, publishing the magic last. */
	user_int_probe.count++;
	user_int_probe.vector = vector;
	user_int_probe.cs = privilege;
	user_int_probe.eip = pc;
	user_int_probe.eax = value;
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
	uint32_t vector,
	uint32_t privilege,
	uintptr_t pc,
	uintptr_t error_code,
	uintptr_t fault_address)
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
	if (vector == 14U && thread->proc->vmspace != NULL) {
		if (error_code & 0x10U)
			required = HAL_SPACE_EXEC;
		else if (error_code & 2U)
			required = HAL_SPACE_WRITE;
		else
			required = HAL_SPACE_READ;
		page_fault_error = vmspace_fault(thread->proc->vmspace,
		    fault_address, required);
		if (page_fault_error == 0) {
			result = HAL_TRAP_RET_SUCCESS;
			goto out;
		}
	}

	/* Retains the fault on the thread for the signal frame. */
	thread->fault_vector = vector;
	thread->fault_eip = pc;
	thread->fault_address = fault_address;

	/* Records the fault, publishing the magic last. */
	user_fault_probe.count++;
	user_fault_probe.vector = vector;
	user_fault_probe.cs = privilege;
	user_fault_probe.eip = pc;
	user_fault_probe.error_code = error_code;
	user_fault_probe.fault_address = fault_address;
	user_fault_probe.pid = thread->proc->pid;
	user_fault_probe.tid = thread->tid;
	hal_compiler_barrier();
	user_fault_probe.magic = USER_FAULT_PROBE_MAGIC;

	/* Maps the fault vector to a signal. */
	switch (vector) {
	case 0:
		signo = SIGFPE;
		break;
	case 3:
		signo = SIGTRAP;
		break;
	case 6:
		signo = SIGILL;
		break;
	case 14:
		if (page_fault_error == ENXIO)
			signo = SIGBUS;
		else
			signo = SIGSEGV;
		break;
	case 10:
	case 11:
	case 12:
	case 13:
		signo = SIGSEGV;
		break;
	default:
		signo = SIGBUS;
		break;
	}

	/* Describes the fault for the signal handler. */
	memset(&info, 0, sizeof(info));
	if (vector == 14U)
		info.address = fault_address;
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
