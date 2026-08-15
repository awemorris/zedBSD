/*
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#include "kern/user-probe.h"
#include "kern/process.h"
#include "kern/signal.h"
#include "kern/thread.h"
#include "kern/vmspace.h"

#include <errno.h>
#include <hal/hal.h>

volatile struct user_int_probe user_int_probe;
volatile struct user_fault_probe user_fault_probe;

_Static_assert(sizeof(user_int_probe.pc) == sizeof(uintptr_t),
	       "user trap PC width must match uintptr_t");
_Static_assert(sizeof(user_fault_probe.fault_address) == sizeof(uintptr_t),
	       "user fault address width must match uintptr_t");

static void
observe_user_int(const struct hal_user_trap *trap)
{
	struct thread *thread = curthread;
	if (trap == NULL || thread == NULL || thread->proc == NULL)
		return;
	user_int_probe.count++;
	user_int_probe.raw_vector = trap->raw_vector;
	user_int_probe.user_mode = 3U;
	user_int_probe.pc = trap->pc;
	user_int_probe.result = trap->result;
	user_int_probe.pid = thread->proc->pid;
	user_int_probe.tid = thread->tid;
	hal_compiler_barrier();
	user_int_probe.magic = USER_INT_PROBE_MAGIC;
}

static int
observe_user_fault(const struct hal_user_trap *trap)
{
	struct thread *thread = curthread;
	int signo;
	int page_fault_error = 0;
	if (thread == NULL || thread->proc == NULL || trap == NULL)
		return HAL_TRAP_RET_FAILED;
	if (trap->cause == HAL_TRAP_CAUSE_PAGE_FAULT &&
	    thread->proc->vmspace != NULL) {
		uint32_t required = trap->access == HAL_TRAP_MODE_EXEC ?
			HAL_SPACE_EXEC : trap->access == HAL_TRAP_MODE_WRITE ?
			HAL_SPACE_WRITE : HAL_SPACE_READ;
		hal_irq_enable();
		page_fault_error = vmspace_fault(thread->proc->vmspace,
				      trap->fault_address, required);
		(void)hal_irq_disable();
		if (page_fault_error == 0)
			return HAL_TRAP_RET_SUCCESS;
	}
	thread->fault_cause = trap->cause;
	thread->fault_raw_vector = trap->raw_vector;
	thread->fault_status = trap->status;
	thread->fault_pc = trap->pc;
	thread->fault_address = trap->fault_address;
	user_fault_probe.count++;
	user_fault_probe.raw_vector = trap->raw_vector;
	user_fault_probe.user_mode = 3U;
	user_fault_probe.pc = trap->pc;
	user_fault_probe.status = trap->status;
	user_fault_probe.fault_address = trap->fault_address;
	user_fault_probe.pid = thread->proc->pid;
	user_fault_probe.tid = thread->tid;
	hal_compiler_barrier();
	user_fault_probe.magic = USER_FAULT_PROBE_MAGIC;
	switch (trap->cause) {
	case HAL_TRAP_CAUSE_ARITHMETIC: signo = SIGFPE; break;
	case HAL_TRAP_CAUSE_BREAKPOINT: signo = SIGTRAP; break;
	case HAL_TRAP_CAUSE_ILLEGAL_INSN: signo = SIGILL; break;
	case HAL_TRAP_CAUSE_PAGE_FAULT:
		signo = page_fault_error == ENXIO ? SIGBUS : SIGSEGV; break;
	case HAL_TRAP_CAUSE_ALIGNMENT:
	case HAL_TRAP_CAUSE_BUS:
	case HAL_TRAP_CAUSE_MACHINE_CHECK:
		signo = SIGBUS; break;
	default: signo = SIGBUS; break;
	}
	if (signal_send_process(thread->proc, signo) != 0)
		return HAL_TRAP_RET_FAILED;
	return HAL_TRAP_RET_SUCCESS;
}

void
user_probe_init(void)
{
	user_int_probe.magic = 0;
	user_int_probe.count = 0;
	user_fault_probe.magic = 0;
	user_fault_probe.count = 0;
	hal_user_int_set_handler(observe_user_int);
	hal_user_fault_set_handler(observe_user_fault);
}
