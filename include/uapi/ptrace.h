/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Tracing another process.
 *
 * The requests carry the names the BSDs give them, so that a debugger
 * written against one of those recognises what it is asking for.  The
 * numbers are this system's own: nothing here is a binary interface
 * shared with another system, only a source one.
 */

#ifndef KERN_UAPI_PTRACE_H
#define KERN_UAPI_PTRACE_H

#include <stdint.h>
#include <stddef.h>
#include <uapi/signal.h>

/*
 * Requests a process makes about itself.
 */
#define PT_TRACE_ME		0

/*
 * Requests a tracer makes about a process it has stopped.
 */
#define PT_READ_I		1
#define PT_READ_D		2
#define PT_WRITE_I		3
#define PT_WRITE_D		4
#define PT_CONTINUE		5
#define PT_KILL			6
#define PT_ATTACH		7
#define PT_DETACH		8
#define PT_IO			9
#define PT_STEP			10
#define PT_GETREGS		11
#define PT_SETREGS		12
#define PT_GETFPREGS		13
#define PT_SETFPREGS		14
#define PT_GETXMMREGS		15
#define PT_SETXMMREGS		16
#define PT_GET_THREAD_FIRST	17
#define PT_GET_THREAD_NEXT	18
#define PT_GET_PROCESS_STATE	19

/*
 * The hardware debug points a thread runs with.
 *
 * The BSDs hand out their processor's debug registers here, under names
 * that only mean anything on the processor that has them.  This system
 * asks instead for the points themselves, because what a debugger wants
 * is to watch an address, and how many registers that takes -- or whether
 * the same registers also serve instruction points -- differs between
 * processors and is not the debugger's business.
 */
#define PT_GET_DEBUG_POINTS	20
#define PT_SET_DEBUG_POINTS	21

/*
 * What the signal that stopped a thread said about itself.
 *
 * A debugger reads this to learn what a stop it was told about really
 * was: which address a debug point matched, where a fault happened, or
 * who sent a signal.  The thread is the one the stop was reported for.
 */
#define PT_GET_SIGINFO		22

/*
 * A block of memory moved between the tracer and the traced process.
 * piod_offs is the address in the traced process and piod_addr the
 * buffer in the tracer; on return piod_len is what was moved.
 */
struct ptrace_io_desc {
	int piod_op;
	void *piod_offs;
	void *piod_addr;
	size_t piod_len;
};

#define PIOD_READ_D		1
#define PIOD_WRITE_D		2
#define PIOD_READ_I		3
#define PIOD_WRITE_I		4
#define PIOD_READ_AUXV		5	/* piod_offs is an offset into the vector */

/*
 * What a stopped process was doing.  The thread is the one the stop is
 * reported for; a stop that belongs to no single thread reports zero.
 */
struct ptrace_state {
	int pe_report_event;
	int pe_thread;
};

#define PTRACE_STOP_SIGNAL	0	/* a signal was about to be delivered */
#define PTRACE_STOP_BREAKPOINT	1	/* the thread reached a breakpoint */
#define PTRACE_STOP_STEP	2	/* the thread took one instruction */
#define PTRACE_STOP_WATCHPOINT	3	/* a debug point matched */
#define PTRACE_STOP_EXEC	4	/* the process replaced its image */

/*
 * The signal a stop was reported for.
 */
struct ptrace_siginfo {
	siginfo_t psi_siginfo;
	int psi_thread;
};

/*
 * One thread of a traced process, as the enumeration reports it.
 */
struct ptrace_thread_state {
	int pts_thread;
};

/*
 * What one hardware debug point watches for.
 */
#define PTRACE_DEBUG_EXECUTE	0
#define PTRACE_DEBUG_WRITE	1
#define PTRACE_DEBUG_READ	2
#define PTRACE_DEBUG_ACCESS	3

struct ptrace_debug_point {
	uintptr_t pdp_address;
	unsigned pdp_length;	/* bytes covered; one for an instruction */
	unsigned pdp_kind;
};

/*
 * The complete set of debug points a thread is to run with, given and
 * reported together: a processor decides for itself whether a particular
 * set fits, and refuses the request rather than the individual point.
 */
#define PTRACE_DEBUG_POINT_MAX	8

struct ptrace_debug_points {
	int pdps_thread;
	unsigned pdps_count;
	struct ptrace_debug_point pdps_point[PTRACE_DEBUG_POINT_MAX];
};

#endif
