/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The sandbox's set of calls (ws168-p002; uapi/sandbox.h, plan/ws168/
 * phase001 section 3.3).
 *
 * The set is written as what is allowed, so a call added to the kernel
 * later is refused in a sandbox until the set names it.  The base set:
 * exit, the files the process holds (read, write, pread, pwrite, readv,
 * writev, lseek, fstat, close: no call here makes a descriptor),
 * anonymous private memory that is never executable (mmap, munmap,
 * mprotect, brk), the time, its own signals and identity, yielding,
 * user synchronization and atomics, and entropy.  SANDBOX_ALLOW_THREADS
 * adds the thread calls.  A call outside the set ends the process with
 * SIGKILL, or answers EPERM when the request asked for that (tests).
 */

#include "kern/sandbox.h"

#include "kern/kmem.h"
#include "kern/klog.h"
#include "kern/lock.h"
#include "kern/process.h"
#include "kern/signal.h"
#include <kern/kcrt.h>

#include <uapi/errno.h>
#include <uapi/mman.h>
#include <uapi/sandbox.h>
#include <uapi/signal.h>
#include <uapi/syscall.h>
#include <uapi/termios.h>

/* The calls every sandbox allows. */
static const uint16_t sandbox_base[] = {
	KERN_SYS_exit,
	KERN_SYS_thread_exit,
	KERN_SYS_read,
	KERN_SYS_write,
	KERN_SYS_pread,
	KERN_SYS_pwrite,
	KERN_SYS_readv,
	KERN_SYS_writev,
	KERN_SYS_lseek,
	KERN_SYS_fstat,
	KERN_SYS_close,
	KERN_SYS_mmap,
	KERN_SYS_munmap,
	KERN_SYS_mprotect,
	KERN_SYS_brk,
	KERN_SYS_clock_gettime,
	KERN_SYS_clock_getres,
	KERN_SYS_nanosleep,
	KERN_SYS_sched_yield,
	KERN_SYS_getpid,
	KERN_SYS_thread_self,
	KERN_SYS_getrlimit,
	KERN_SYS_sigprocmask,
	KERN_SYS_sigaction,
	KERN_SYS_sigreturn,
	KERN_SYS_thread_kill,
	KERN_SYS_usync,
	KERN_SYS_atomic,
	KERN_SYS_getentropy,
};

/* The calls SANDBOX_ALLOW_THREADS adds. */
static const uint16_t sandbox_threads[] = {
	KERN_SYS_thread_create,
	KERN_SYS_thread_join,
	KERN_SYS_thread_detach,
	KERN_SYS_thread_cancel,
};

static void sandbox_allow(struct sandbox *sandbox, const uint16_t *calls, size_t count);

/*
 * Makes a sandbox's set from the request's flags and allow bits (both
 * already known to be ones this kernel knows).  Returns it, or NULL
 * without memory.
 */
struct sandbox *
sandbox_create(
	uint32_t flags,
	uint64_t allow)
{
	struct sandbox *sandbox;

	/* The empty set. */
	sandbox = kern_calloc(1, sizeof(*sandbox));
	if (sandbox == NULL)
		return NULL;
	sandbox->flags = flags;
	sandbox->allow = allow;

	/* The base set, and what the allow bits add. */
	sandbox_allow(sandbox, sandbox_base, sizeof(sandbox_base) / sizeof(sandbox_base[0]));
	if ((allow & SANDBOX_ALLOW_THREADS) != 0U)
		sandbox_allow(sandbox, sandbox_threads, sizeof(sandbox_threads) / sizeof(sandbox_threads[0]));

	/* Succeeded: the set. */
	return sandbox;
}

/* Frees a sandbox (the process is going away). */
void
sandbox_free(
	struct sandbox *sandbox)
{
	kern_free(sandbox);
}

/*
 * Tells whether a call with its arguments is in a sandbox's set: its
 * number allowed, and for the memory calls, no file, device or shared
 * mapping and nothing executable.
 */
int
sandbox_permits(
	const struct sandbox *sandbox,
	uint32_t number,
	const uintptr_t args[6])
{
	uintptr_t flags;
	uintptr_t prot;
	int fd;

	/* A number the set does not name. */
	if (number >= SANDBOX_CALLS_MAX)
		return 0;
	if ((sandbox->calls[number / 8U] & (1U << (number % 8U))) == 0U)
		return 0;

	/* mmap: anonymous and private only, never executable. */
	if (number == KERN_SYS_mmap) {
		prot = args[2];
		flags = args[3];
		fd = (int)args[4];
		if ((flags & MAP_ANONYMOUS) == 0U || (flags & MAP_SHARED) != 0U)
			return 0;
		if (fd != -1)
			return 0;
		if ((prot & PROT_EXEC) != 0U)
			return 0;
	}

	/* mprotect: never makes anything executable. */
	if (number == KERN_SYS_mprotect) {
		prot = args[2];
		if ((prot & PROT_EXEC) != 0U)
			return 0;
	}

	/* Succeeded: allowed. */
	return 1;
}

/*
 * Answers, without the call, the one question a sandbox's process may ask
 * of a file it holds that the set does not let it make: whether it is a
 * terminal (ioctl TCGETS, which the C library's start asks of fd 1).  It
 * is told "not a terminal" (ENOTTY), which reveals nothing and reaches no
 * file or driver.  Returns 1 with the answer, 0 for any other call.
 */
int
sandbox_answers(
	uint32_t number,
	const uintptr_t args[6],
	intptr_t *result)
{
	/* Only the terminal query. */
	if (number != KERN_SYS_ioctl || args[1] != (uintptr_t)TCGETS)
		return 0;

	/* Succeeded: not a terminal. */
	*result = -ENOTTY;
	return 1;
}

/*
 * Refuses a call outside the set: said in the log (at most
 * SANDBOX_DENIALS_LOGGED lines a process), then the process is ended
 * with SIGKILL, unless its request asked for EPERM.  Returns the call's
 * result, -EPERM.
 */
intptr_t
sandbox_deny(
	struct process *process,
	uint32_t number)
{
	unsigned long irq;
	unsigned denials;

	/* The denial counted. */
	irq = spin_lock_irqsave(&process->lock);

	process->sandbox->denials++;
	denials = process->sandbox->denials;

	spin_unlock_irqrestore(&process->lock, irq);

	/* Said in the log, a few times. */
	if (denials <= SANDBOX_DENIALS_LOGGED)
		kern_logf("SANDBOX deny pid=%d call=%u\n", (int)process->pid, (unsigned)number);

	/* Ended, unless the request asked for the error. */
	if ((process->sandbox->flags & SANDBOX_SPAWN_DENY_ERRNO) == 0U)
		(void)signal_send_process(process, SIGKILL);

	/* The call's result. */
	return -EPERM;
}

/* Adds calls to a set. */
static void
sandbox_allow(
	struct sandbox *sandbox,
	const uint16_t *calls,
	size_t count)
{
	size_t index;
	unsigned number;

	/* Each call's bit. */
	for (index = 0U; index < count; index++) {
		number = calls[index];
		if (number < SANDBOX_CALLS_MAX)
			sandbox->calls[number / 8U] |= (uint8_t)(1U << (number % 8U));
	}
}
