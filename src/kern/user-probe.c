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
#include <string.h>

volatile struct user_int_probe user_int_probe;
volatile struct user_fault_probe user_fault_probe;

_Static_assert(sizeof(user_int_probe.eip) == sizeof(uintptr_t),
	       "user trap PC width must match uintptr_t");
_Static_assert(sizeof(user_fault_probe.fault_address) == sizeof(uintptr_t),
	       "user fault address width must match uintptr_t");

void
kernel_user_int_handler(uint32 vector, uint32 privilege, uintptr_t pc,
	uintptr_t value)
{
	struct thread *thread = curthread;
	if (thread == NULL || thread->proc == NULL)
		return;
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

int
kernel_user_fault_handler(uint32 vector, uint32 privilege, uintptr_t pc,
	uintptr_t error_code, uintptr_t fault_address)
{
	struct thread *thread = curthread;
	struct signal_info info;
	int signo;
	int page_fault_error = 0;
	if (thread == NULL || thread->proc == NULL)
		return HAL_TRAP_RET_FAILED;
	if (vector == 14U && thread->proc->vmspace != NULL) {
		uint32_t required = (error_code & 0x10U) ? HAL_SPACE_EXEC :
			(error_code & 2U) ? HAL_SPACE_WRITE : HAL_SPACE_READ;
		hal_irq_enable();
		page_fault_error = vmspace_fault(thread->proc->vmspace,
				      fault_address, required);
		(void)hal_irq_disable();
		if (page_fault_error == 0)
			return HAL_TRAP_RET_SUCCESS;
	}
	thread->fault_vector = vector;
	thread->fault_eip = pc;
	thread->fault_address = fault_address;
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
	switch (vector) {
	case 0: signo = SIGFPE; break;
	case 3: signo = SIGTRAP; break;
	case 6: signo = SIGILL; break;
	case 14:
		signo = page_fault_error == ENXIO ? SIGBUS : SIGSEGV; break;
	case 10: case 11: case 12: case 13:
		signo = SIGSEGV; break;
	default: signo = SIGBUS; break;
	}
	memset(&info, 0, sizeof(info));
	info.address = vector == 14U ? fault_address : pc;
	switch (signo) {
	case SIGFPE: info.code = FPE_INTDIV; break;
	case SIGTRAP: info.code = TRAP_BRKPT; break;
	case SIGILL: info.code = ILL_ILLOPC; break;
	case SIGSEGV:
		info.code = page_fault_error == EACCES ? SEGV_ACCERR :
		    SEGV_MAPERR;
		break;
	case SIGBUS: info.code = BUS_ADRERR; break;
	default: info.code = SI_KERNEL; break;
	}
	if (signal_send_process_info(thread->proc, signo, &info) != 0)
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
}
