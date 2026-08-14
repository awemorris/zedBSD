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

_Static_assert(sizeof(user_int_probe.eip) == sizeof(uintptr_t),
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
	user_int_probe.vector = trap->vector;
	user_int_probe.cs = trap->cs;
	user_int_probe.eip = trap->eip;
	user_int_probe.eax = trap->eax;
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
	if (trap->vector == 14U && thread->proc->vmspace != NULL) {
		uint32_t required = (trap->error_code & 0x10U) ? HAL_SPACE_EXEC :
			(trap->error_code & 2U) ? HAL_SPACE_WRITE : HAL_SPACE_READ;
		hal_irq_enable();
		page_fault_error = vmspace_fault(thread->proc->vmspace,
				      trap->fault_address, required);
		(void)hal_irq_disable();
		if (page_fault_error == 0)
			return HAL_TRAP_RET_SUCCESS;
	}
	thread->fault_vector = trap->vector;
	thread->fault_eip = trap->eip;
	thread->fault_address = trap->fault_address;
	user_fault_probe.count++;
	user_fault_probe.vector = trap->vector;
	user_fault_probe.cs = trap->cs;
	user_fault_probe.eip = trap->eip;
	user_fault_probe.error_code = trap->error_code;
	user_fault_probe.fault_address = trap->fault_address;
	user_fault_probe.pid = thread->proc->pid;
	user_fault_probe.tid = thread->tid;
	hal_compiler_barrier();
	user_fault_probe.magic = USER_FAULT_PROBE_MAGIC;
	switch (trap->vector) {
	case 0: signo = SIGFPE; break;
	case 3: signo = SIGTRAP; break;
	case 6: signo = SIGILL; break;
	case 14:
		signo = page_fault_error == ENXIO ? SIGBUS : SIGSEGV; break;
	case 10: case 11: case 12: case 13:
		signo = SIGSEGV; break;
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
