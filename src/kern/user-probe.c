/*
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#include "kern/user-probe.h"
#include "kern/process.h"
#include "kern/thread.h"
#include "kern/vmspace.h"

#include <hal/hal.h>

volatile struct user_int_probe user_int_probe;
volatile struct user_fault_probe user_fault_probe;

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
	if (thread == NULL || thread->proc == NULL || trap == NULL)
		return HAL_TRAP_RET_FAILED;
	if (trap->vector == 14U && thread->proc->vmspace != NULL) {
		uint32_t required = (trap->error_code & 0x10U) ? HAL_SPACE_EXEC :
			(trap->error_code & 2U) ? HAL_SPACE_WRITE : HAL_SPACE_READ;
		int error;

		hal_irq_enable();
		error = vmspace_fault(thread->proc->vmspace,
				      trap->fault_address, required);
		(void)hal_irq_disable();
		if (error == 0)
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
	exit1(-(int)trap->vector);
	return HAL_TRAP_RET_FAILED;
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
