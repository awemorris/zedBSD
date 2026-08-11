/*
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#include "kern/user-probe.h"
#include "kern/process.h"
#include "kern/thread.h"

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

static void
observe_user_fault(const struct hal_user_trap *trap)
{
	struct thread *thread = curthread;
	if (thread == NULL || thread->proc == NULL || trap == NULL)
		return;
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
