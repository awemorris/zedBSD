/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Tracing another process.
 *
 * Every request here needs the same two things first: that the caller is
 * the one tracing the named process, and that the process has stopped.  A
 * debugger reads and writes a stopped program; letting it do so while the
 * program runs would report registers that were true a moment ago.
 *
 * The register sets the hardware layer holds are copied field by field
 * into the ones this system publishes, rather than handed outward: what a
 * process may see is an interface that is kept, and the kernel's internal
 * form belongs to the machine it describes.
 */

#include "kern/process.h"
#include "kern/thread.h"
#include "kern/vmspace.h"
#include "kern/kmem.h"
#include "kern/page.h"
#include "kern/signal.h"
#include "kern/sched.h"
#include <uapi/ptrace.h>
#include <uapi/reg.h>
#include <uapi/errno.h>
#include <hal/hal.h>
#include <kern/kcrt.h>

/*
 * Finds a thread of a process by the identifier a debugger names it with.
 * A zero identifier means the thread the stop was reported for.
 */
static struct thread *
trace_thread(
	struct process *process,
	tid_t tid)
{
	struct thread *thread;

	/* Takes the stopping thread when none is named. */
	if (tid == 0)
		tid = process->trace_thread;

	/* Process each remaining element. */
	for (thread = process->threads; thread != NULL;
	     thread = thread->proc_next) {
		/* Handles the thread condition. */
		if (thread->tid == tid)
			return thread;
	}

	/* Reports that the process has no such thread. */
	return NULL;
}

/*
 * Finds the thread that follows one in the process's list, or the first
 * when none is named.  The order is whatever the list holds; what a
 * caller needs is to reach every thread exactly once.
 */
static struct thread *
trace_thread_after(
	struct process *process,
	tid_t previous)
{
	struct thread *thread;

	/* Takes the first thread when none is named. */
	if (previous == 0)
		return process->threads;

	/* Process each remaining element. */
	for (thread = process->threads; thread != NULL;
	     thread = thread->proc_next) {
		/* Handles the thread condition. */
		if (thread->tid == previous)
			return thread->proc_next;
	}

	/* Reports that the named thread is no longer there. */
	return NULL;
}

/*
 * Translates between the kinds a process asks for and the kinds the
 * hardware layer is told, which are two spellings of the same four.
 */
static unsigned
trace_debug_kind(
	unsigned request)
{
	/* Dispatch the selected operation case. */
	switch (request) {
	case PTRACE_DEBUG_EXECUTE:
		return HAL_DEBUG_KIND_EXECUTE;
	case PTRACE_DEBUG_WRITE:
		return HAL_DEBUG_KIND_WRITE;
	case PTRACE_DEBUG_READ:
		return HAL_DEBUG_KIND_READ;
	default:
		return HAL_DEBUG_KIND_ACCESS;
	}
}

static unsigned
trace_debug_request(
	unsigned kind)
{
	/* Dispatch the selected operation case. */
	switch (kind) {
	case HAL_DEBUG_KIND_EXECUTE:
		return PTRACE_DEBUG_EXECUTE;
	case HAL_DEBUG_KIND_WRITE:
		return PTRACE_DEBUG_WRITE;
	case HAL_DEBUG_KIND_READ:
		return PTRACE_DEBUG_READ;
	default:
		return PTRACE_DEBUG_ACCESS;
	}
}

/*
 * Answers whether the caller may make requests about this process, and
 * gives back a reference to it.
 */
static int
trace_subject(
	pid_t pid,
	struct process **result)
{
	struct process *process;
	struct process *caller;

	caller = curthread != NULL ? curthread->proc : NULL;

	/* Rejects a caller with no process. */
	if (caller == NULL)
		return EINVAL;
	process = process_find_ref(pid);

	/* Rejects a process that is not there. */
	if (process == NULL)
		return ESRCH;

	/* Rejects a process this caller is not tracing. */
	if (!process_trace_is_tracer(process, caller)) {
		process_release(process);
		return ESRCH;
	}

	/* Rejects a process that has not stopped. */
	if (process->state != PROCESS_STOPPED) {
		process_release(process);
		return EBUSY;
	}
	*result = process;

	/* Succeeded. */
	return 0;
}

static int trace_write(struct process *process, uintptr_t address,
	const void *source, size_t length, int instruction);

/*
 * Moves a block of memory between the tracer and the traced process.
 *
 * A debugger writes into the text it is stopping, which is mapped without
 * write permission.  Where the mapping was allowed to become writable the
 * permission is raised for the copy and put back afterwards; the process
 * is stopped, so no thread of it sees the difference.
 */
static int
trace_io(
	struct process *process,
	struct ptrace_io_desc *request)
{
	uintptr_t address;
	int error;
	void *buffer;

	address = (uintptr_t)request->piod_offs;

	/*
	 * The auxiliary vector is read from where exec left it, by offset
	 * within the vector; a read past its end returns what is left,
	 * which at the end is nothing.
	 */
	if (request->piod_op == PIOD_READ_AUXV) {
		if (address >= process->auxv_size)
			request->piod_len = 0;
		else if (request->piod_len > process->auxv_size - address)
			request->piod_len = process->auxv_size - address;
		if (request->piod_len == 0)
			return 0;
		address += process->auxv_address;
		request->piod_op = PIOD_READ_D;
	}

	/* Rejects an empty or unreasonable transfer. */
	if (request->piod_len == 0 || request->piod_len > 0x100000U)
		return EINVAL;
	buffer = kern_malloc(request->piod_len);

	/* Rejects a transfer there is no room to stage. */
	if (buffer == NULL)
		return ENOMEM;

	/* Reads from the traced process. */
	if (request->piod_op == PIOD_READ_D ||
	    request->piod_op == PIOD_READ_I) {
		error = vmspace_copy_from(process->vmspace, buffer, address,
		    request->piod_len);
		if (error == 0) {
			error = vmspace_copy_to(curthread->proc->vmspace,
			    (uintptr_t)request->piod_addr, buffer,
			    request->piod_len);
		}
		kern_free(buffer);

		/* Reports the outcome of the read. */
		return error;
	}

	/* Rejects an operation this call does not define. */
	if (request->piod_op != PIOD_WRITE_D &&
	    request->piod_op != PIOD_WRITE_I) {
		kern_free(buffer);
		return EINVAL;
	}

	/* Takes what is to be written from the tracer. */
	error = vmspace_copy_from(curthread->proc->vmspace, buffer,
	    (uintptr_t)request->piod_addr, request->piod_len);
	if (error == 0) {
		error = trace_write(process, address, buffer,
		    request->piod_len, request->piod_op == PIOD_WRITE_I);
	}
	kern_free(buffer);

	/* Reports the outcome of the write. */
	return error;
}

/*
 * Writes into the traced process, including where it is not writable.
 *
 * A debugger writes into the text it is stopping, which is mapped without
 * write permission.  Where the mapping was allowed to become writable the
 * permission is raised for the copy and put back afterwards; the process
 * is stopped, so no thread of it sees the difference.
 */
static int
trace_write(
	struct process *process,
	uintptr_t address,
	const void *source,
	size_t length,
	int instruction)
{
	struct vm_region *region;
	uintptr_t page;
	size_t span;
	uint32_t prot;
	uint32_t max_prot;
	int raised;
	int error;

	/* Finds what the destination is allowed to become. */
	raised = 0;
	page = address & ~(uintptr_t)(KERN_PAGE_SIZE - 1U);
	span = (address + length - page + KERN_PAGE_SIZE - 1U) &
	    ~(size_t)(KERN_PAGE_SIZE - 1U);
	region = vmspace_find_region(process->vmspace, address, length);

	/* Rejects an address the process has nothing mapped at. */
	if (region == NULL)
		return EFAULT;
	prot = region->prot;
	max_prot = region->max_prot;

	/* Raises the permission only where the mapping allows it. */
	if ((prot & HAL_SPACE_WRITE) == 0) {
		if ((max_prot & HAL_SPACE_WRITE) == 0)
			return EACCES;
		error = vmspace_protect(process->vmspace, page, span,
		    prot | HAL_SPACE_WRITE);
		if (error != 0)
			return error;
		raised = 1;
	}

	error = vmspace_copy_to(process->vmspace, address, source, length);

	/* Puts the permission back before anything runs again. */
	if (raised)
		(void)vmspace_protect(process->vmspace, page, span, prot);

	/*
	 * Instruction fetch does not see a store on every machine, so the
	 * written range is made visible to it.
	 */
	if (error == 0 && (instruction || (prot & HAL_SPACE_EXEC) != 0))
		hal_icache_invalidate_range(address, length);

	/* Reports the outcome. */
	return error;
}

/*
 * Copies the integer registers outward, under the names a process sees.
 */
static void
trace_gpregs_out(
	const struct hal_gpregs *from,
	struct reg *to)
{
	to->r_rax = from->rax;
	to->r_rbx = from->rbx;
	to->r_rcx = from->rcx;
	to->r_rdx = from->rdx;
	to->r_rsi = from->rsi;
	to->r_rdi = from->rdi;
	to->r_rbp = from->rbp;
	to->r_rsp = from->rsp;
	to->r_r8 = from->r8;
	to->r_r9 = from->r9;
	to->r_r10 = from->r10;
	to->r_r11 = from->r11;
	to->r_r12 = from->r12;
	to->r_r13 = from->r13;
	to->r_r14 = from->r14;
	to->r_r15 = from->r15;
	to->r_rip = from->rip;
	to->r_rflags = from->rflags;
	to->r_cs = from->cs;
	to->r_ss = from->ss;
	to->r_fs_base = from->fs_base;
	to->r_gs_base = from->gs_base;
}

/*
 * Copies the integer registers inward.
 */
static void
trace_gpregs_in(
	const struct reg *from,
	struct hal_gpregs *to)
{
	to->rax = from->r_rax;
	to->rbx = from->r_rbx;
	to->rcx = from->r_rcx;
	to->rdx = from->r_rdx;
	to->rsi = from->r_rsi;
	to->rdi = from->r_rdi;
	to->rbp = from->r_rbp;
	to->rsp = from->r_rsp;
	to->r8 = from->r_r8;
	to->r9 = from->r_r9;
	to->r10 = from->r_r10;
	to->r11 = from->r_r11;
	to->r12 = from->r_r12;
	to->r13 = from->r_r13;
	to->r14 = from->r_r14;
	to->r15 = from->r_r15;
	to->rip = from->r_rip;
	to->rflags = from->r_rflags;
	to->cs = from->r_cs;
	to->ss = from->r_ss;
	to->fs_base = from->r_fs_base;
	to->gs_base = from->r_gs_base;
}

/*
 * Handles ptrace(2).
 */
int
kern_ptrace(
	int request,
	pid_t pid,
	uintptr_t address,
	int data,
	intptr_t *result)
{
	struct process *process;
	struct process *caller;
	struct thread *thread;
	struct hal_gpregs gpregs;
	struct hal_fpregs fpregs;
	struct hal_vregs vregs;
	struct reg user_reg;
	struct fpreg user_fpreg;
	struct xmmreg user_xmmreg;
	struct ptrace_io_desc io;
	struct ptrace_state state;
	int error;

	caller = curthread != NULL ? curthread->proc : NULL;
	*result = 0;

	/* Rejects a caller with no process. */
	if (caller == NULL)
		return EINVAL;

	/* A process asking to be traced names nothing else. */
	if (request == PT_TRACE_ME)
		return process_trace_self();

	/* Attaching is the one request the process has not stopped for. */
	if (request == PT_ATTACH) {
		process = process_find_ref(pid);
		if (process == NULL)
			return ESRCH;
		error = process_trace_attach(process, caller);
		if (error == 0)
			error = signal_send_process(process, SIGSTOP);
		process_release(process);
		return error;
	}

	error = trace_subject(pid, &process);
	if (error != 0)
		return error;

	switch (request) {
	case PT_DETACH:
		/*
		 * A process that is let go runs as though it had never been
		 * traced.  One instruction at a time is a request the
		 * processor keeps making until it is told otherwise, so it
		 * is withdrawn from every thread here; left set, it would
		 * trap the process to death with nobody to catch it.
		 */
		for (thread = process->threads; thread != NULL;
		     thread = thread->proc_next)
			(void)hal_task_set_single_step(thread->task, 0);
		error = process_trace_detach(process, caller);
		if (error == 0)
			(void)process_continue(process, 0);
		break;

	case PT_CONTINUE:
	case PT_STEP:
		thread = trace_thread(process, 0);
		if (thread == NULL) {
			error = ESRCH;
			break;
		}

		/*
		 * The request says whether the thread takes one instruction
		 * or runs on, and which signal it carries out of the stop.
		 */
		if (data < 0 || data >= NSIG) {
			error = EINVAL;
			break;
		}
		if (hal_task_set_single_step(thread->task,
		    request == PT_STEP) != 0) {
			error = ENOTSUP;
			break;
		}
		process->trace_signal = data;
		(void)process_continue(process, 0);
		error = 0;
		break;

	case PT_KILL:
		error = signal_send_process(process, SIGKILL);
		if (error == 0)
			(void)process_continue(process, 0);
		break;

	case PT_GETREGS:
		thread = trace_thread(process, (tid_t)data);
		if (thread == NULL) {
			error = ESRCH;
			break;
		}
		if (hal_task_get_user_gpregs(thread->task, &gpregs) != 0) {
			error = EIO;
			break;
		}
		trace_gpregs_out(&gpregs, &user_reg);
		error = vmspace_copy_to(caller->vmspace, address, &user_reg,
		    sizeof(user_reg));
		break;

	case PT_SETREGS:
		thread = trace_thread(process, (tid_t)data);
		if (thread == NULL) {
			error = ESRCH;
			break;
		}
		error = vmspace_copy_from(caller->vmspace, &user_reg, address,
		    sizeof(user_reg));
		if (error != 0)
			break;
		trace_gpregs_in(&user_reg, &gpregs);
		if (hal_task_set_user_gpregs(thread->task, &gpregs) != 0)
			error = EINVAL;
		break;

	case PT_GETFPREGS:
		thread = trace_thread(process, (tid_t)data);
		if (thread == NULL) {
			error = ESRCH;
			break;
		}
		if (hal_task_get_user_fpregs(thread->task, &fpregs) != 0) {
			error = EIO;
			break;
		}
		user_fpreg.fp_control = fpregs.control;
		user_fpreg.fp_status = fpregs.status;
		user_fpreg.fp_tag = fpregs.tag;
		user_fpreg.fp_opcode = fpregs.opcode;
		user_fpreg.fp_instruction_pointer = fpregs.instruction_pointer;
		user_fpreg.fp_data_pointer = fpregs.data_pointer;
		kern_memcpy(user_fpreg.fp_stack, fpregs.stack,
		    sizeof(user_fpreg.fp_stack));
		error = vmspace_copy_to(caller->vmspace, address, &user_fpreg,
		    sizeof(user_fpreg));
		break;

	case PT_SETFPREGS:
		thread = trace_thread(process, (tid_t)data);
		if (thread == NULL) {
			error = ESRCH;
			break;
		}
		error = vmspace_copy_from(caller->vmspace, &user_fpreg,
		    address, sizeof(user_fpreg));
		if (error != 0)
			break;
		fpregs.control = user_fpreg.fp_control;
		fpregs.status = user_fpreg.fp_status;
		fpregs.tag = user_fpreg.fp_tag;
		fpregs.opcode = user_fpreg.fp_opcode;
		fpregs.instruction_pointer = user_fpreg.fp_instruction_pointer;
		fpregs.data_pointer = user_fpreg.fp_data_pointer;
		kern_memcpy(fpregs.stack, user_fpreg.fp_stack,
		    sizeof(fpregs.stack));
		if (hal_task_set_user_fpregs(thread->task, &fpregs) != 0)
			error = EINVAL;
		break;

	case PT_GETXMMREGS:
		thread = trace_thread(process, (tid_t)data);
		if (thread == NULL) {
			error = ESRCH;
			break;
		}
		if (hal_task_get_user_vregs(thread->task, &vregs) != 0) {
			error = EIO;
			break;
		}
		user_xmmreg.xmm_control = vregs.control;
		user_xmmreg.xmm_control_mask = vregs.control_mask;
		kern_memcpy(user_xmmreg.xmm_register, vregs.xmm,
		    sizeof(user_xmmreg.xmm_register));
		error = vmspace_copy_to(caller->vmspace, address,
		    &user_xmmreg, sizeof(user_xmmreg));
		break;

	case PT_SETXMMREGS:
		thread = trace_thread(process, (tid_t)data);
		if (thread == NULL) {
			error = ESRCH;
			break;
		}
		error = vmspace_copy_from(caller->vmspace, &user_xmmreg,
		    address, sizeof(user_xmmreg));
		if (error != 0)
			break;
		vregs.control = user_xmmreg.xmm_control;
		vregs.control_mask = user_xmmreg.xmm_control_mask;
		kern_memcpy(vregs.xmm, user_xmmreg.xmm_register,
		    sizeof(vregs.xmm));
		if (hal_task_set_user_vregs(thread->task, &vregs) != 0)
			error = EINVAL;
		break;

	case PT_IO:
		error = vmspace_copy_from(caller->vmspace, &io, address,
		    sizeof(io));
		if (error == 0)
			error = trace_io(process, &io);
		if (error == 0)
			error = vmspace_copy_to(caller->vmspace, address, &io,
			    sizeof(io));
		break;

	case PT_READ_I:
	case PT_READ_D:
		/*
		 * The word forms exist because software written for another
		 * system asks for them.  A debugger of any size uses PT_IO
		 * instead, which moves a block rather than a word.
		 */
		{
			uintptr_t word;

			error = vmspace_copy_from(process->vmspace, &word,
			    address, sizeof(word));
			if (error == 0)
				*result = (intptr_t)word;
		}
		break;

	case PT_WRITE_I:
	case PT_WRITE_D:
		/*
		 * What is written is the request's own data word, so what
		 * reaches the process is as wide as that word and no wider.
		 */
		{
			int word;

			word = data;
			error = trace_write(process, address, &word,
			    sizeof(word),
			    request == PT_WRITE_I);
		}
		break;

	case PT_GET_THREAD_FIRST:
	case PT_GET_THREAD_NEXT:
		{
			struct ptrace_thread_state threads;

			error = vmspace_copy_from(caller->vmspace, &threads,
			    address, sizeof(threads));
			if (error != 0)
				break;
			thread = trace_thread_after(process,
			    request == PT_GET_THREAD_FIRST
			    ? 0 : (tid_t)threads.pts_thread);

			/* Reports the end of the list as no such thread. */
			if (thread == NULL) {
				error = ESRCH;
				break;
			}
			threads.pts_thread = (int)thread->tid;
			error = vmspace_copy_to(caller->vmspace, address,
			    &threads, sizeof(threads));
		}
		break;

	case PT_SET_DEBUG_POINTS:
		{
			struct ptrace_debug_points points;
			struct hal_debug_point hardware[HAL_DEBUG_POINT_MAX];
			unsigned index;

			error = vmspace_copy_from(caller->vmspace, &points,
			    address, sizeof(points));
			if (error != 0)
				break;

			/* Refuses a set larger than this processor holds. */
			if (points.pdps_count > HAL_DEBUG_POINT_MAX) {
				error = ENOSPC;
				break;
			}
			thread = trace_thread(process,
			    (tid_t)points.pdps_thread);
			if (thread == NULL) {
				error = ESRCH;
				break;
			}

			/* Process each element required by the operation. */
			for (index = 0; index < points.pdps_count; index++) {
				hardware[index].address =
				    points.pdps_point[index].pdp_address;
				hardware[index].length =
				    points.pdps_point[index].pdp_length;
				hardware[index].kind =
				    trace_debug_kind(
					points.pdps_point[index].pdp_kind);
			}

			/*
			 * The processor decides whether this particular set
			 * fits; the count and the kinds were already what it
			 * publishes as possible.
			 */
			if (hal_task_set_debug_points(thread->task, hardware,
			    points.pdps_count) != 0)
				error = ENOSPC;
		}
		break;

	case PT_GET_DEBUG_POINTS:
		{
			struct ptrace_debug_points points;
			struct hal_debug_point hardware[HAL_DEBUG_POINT_MAX];
			unsigned held;
			unsigned index;

			error = vmspace_copy_from(caller->vmspace, &points,
			    address, sizeof(points));
			if (error != 0)
				break;
			thread = trace_thread(process,
			    (tid_t)points.pdps_thread);
			if (thread == NULL) {
				error = ESRCH;
				break;
			}
			held = 0;
			if (hal_task_get_debug_points(thread->task, hardware,
			    HAL_DEBUG_POINT_MAX, &held) != 0) {
				error = EIO;
				break;
			}
			points.pdps_count = held;

			/* Process each element required by the operation. */
			for (index = 0; index < held; index++) {
				points.pdps_point[index].pdp_address =
				    hardware[index].address;
				points.pdps_point[index].pdp_length =
				    hardware[index].length;
				points.pdps_point[index].pdp_kind =
				    trace_debug_request(hardware[index].kind);
			}
			error = vmspace_copy_to(caller->vmspace, address,
			    &points, sizeof(points));
		}
		break;

	case PT_GET_SIGINFO:
		{
			struct ptrace_siginfo siginfo;

			kern_memset(&siginfo, 0, sizeof(siginfo));
			siginfo.psi_siginfo = process->trace_siginfo;
			siginfo.psi_thread = (int)process->trace_thread;
			error = vmspace_copy_to(caller->vmspace, address,
			    &siginfo, sizeof(siginfo));
		}
		break;

	case PT_GET_PROCESS_STATE:
		state.pe_report_event = process->trace_stop_kind;
		state.pe_thread = (int)process->trace_thread;
		error = vmspace_copy_to(caller->vmspace, address, &state,
		    sizeof(state));
		break;

	default:
		error = EINVAL;
		break;
	}

	process_release(process);

	/* Reports the outcome. */
	return error;
}
