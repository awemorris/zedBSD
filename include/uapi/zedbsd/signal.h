/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */
#ifndef ZEDBSD_UAPI_SIGNAL_H
#define ZEDBSD_UAPI_SIGNAL_H
#include <stdint.h>
#include <stddef.h>
#include <zedbsd/types.h>
/*
 * One bit per signal.  Signal 63 is reserved to libc and bit 63 is unused,
 * leaving a fixed-width ABI with room for the classic and realtime sets.
 */
typedef uint64_t sigset_t;
#define NSIG	64
#define SIGHUP	1
#define SIGINT	2
#define SIGQUIT	3
#define SIGILL	4
#define SIGTRAP	5
#define SIGABRT	6
#define SIGVTALRM	7
#define SIGFPE	8
#define SIGKILL	9
#define SIGBUS	10
#define SIGSEGV	11
#define SIGPROF	12
#define SIGPIPE	13
#define SIGALRM	14
#define SIGTERM	15
#define SIGUSR1	16
#define SIGUSR2	17
#define SIGCHLD	18
#define SIGCONT	19
#define SIGSTOP	20
#define SIGTSTP	21
#define SIGTTIN	22
#define SIGTTOU	23
#define SIGURG	24
#define SIGWINCH	25
#define SIGIO	26
#define SIGPOLL	SIGIO
#define SIGXCPU	27
#define SIGXFSZ	28
#define SIGRTMIN	29
#define SIGRTMAX	62
/*
 * Implementation namespace: this is deliberately outside the public
 * SIGRTMIN..SIGRTMAX interval.  libc uses it to turn SIGEV_THREAD timer
 * expiry into work for its notification thread.
 */
#define __ZEDBSD_SIGEV_THREAD_SIGNAL	63
#define SIG_BLOCK	0
#define SIG_UNBLOCK	1
#define SIG_SETMASK	2
#define SA_RESTART	0x0001U
#define SA_NOCLDSTOP	0x0002U
#define SA_NOCLDWAIT	0x0004U
#define SA_NODEFER	0x0008U
#define SA_RESETHAND	0x0010U
#define SA_SIGINFO	0x0020U
/*
 * Run the handler on the alternate stack installed by sigaltstack(2).
 */
#define SA_ONSTACK	0x0040U
#define SIG_DFL	0ULL
#define SIG_IGN	1ULL

/*
 * si_code values are positive for kernel-generated events and non-positive
 * for process-generated events.  All public signal records use fixed-width
 * fields so their layout is identical in the ILP32 and LP64 ABIs.
 */
#define SI_USER	0
#define SI_QUEUE	(-1)
#define SI_TIMER	(-2)
#define SI_KERNEL	0x80
#define ILL_ILLOPC	1
#define FPE_INTDIV	1
#define SEGV_MAPERR	1
#define SEGV_ACCERR	2
#define BUS_ADRALN	1
#define BUS_ADRERR	2
#define TRAP_BRKPT	1
#define CLD_EXITED	1
#define CLD_KILLED	2
#define CLD_DUMPED	3
#define CLD_TRAPPED	4
#define CLD_STOPPED	5
#define CLD_CONTINUED	6

union sigval {
	int32_t sival_int;
	void *sival_ptr;
	uint64_t __sival_pad;
};

/*
 * pthread_attr_t is a libc type.  Giving its implementation tag a forward
 * declaration lets sigevent expose the standard pointer type without making
 * the kernel UAPI depend on the pthread header.
 */
struct __pthread_attr;

#define SIGEV_NONE	0
#define SIGEV_SIGNAL	1
#define SIGEV_THREAD	2
struct sigevent {
	int32_t sigev_notify;
	int32_t sigev_signo;
	union sigval sigev_value;
	void (
		*sigev_notify_function)(
		union sigval);
#ifndef ZEDBSD_USER_ABI_LP64
	uint32_t __sigev_notify_function_pad;
#endif
	struct __pthread_attr *sigev_notify_attributes;
#ifndef ZEDBSD_USER_ABI_LP64
	uint32_t __sigev_notify_attributes_pad;
#endif
};

typedef struct siginfo {
	int32_t si_signo;
	int32_t si_errno;
	int32_t si_code;
	uint32_t si_reserved0;
	int32_t si_pid;
	uint32_t si_uid;
	int32_t si_status;
	uint32_t si_reserved1;
	uint64_t si_addr;
	union sigval si_value;
	uint64_t si_reserved[10];
} siginfo_t;

#define SS_ONSTACK	0x0001
#define SS_DISABLE	0x0002
#define MINSIGSTKSZ	8192U
#define SIGSTKSZ	32768U
struct sigaltstack_record {
	uapi_ptr_t ss_sp;
#ifndef ZEDBSD_USER_ABI_LP64
	uint32_t ss_pointer_pad;
#endif
	uint64_t ss_size;
	int32_t ss_flags;
	uint32_t ss_reserved;
};

/*
 * mc_pc, mc_sp, and mc_retval describe the interrupted user context.  The
 * first ABI revision deliberately permits sigreturn to adopt only
 * uc_sigmask; changing machine-context fields makes sigreturn fail with
 * EINVAL.  This keeps privileged architecture state opaque to userland.
 */
typedef struct mcontext {
	uint64_t mc_pc;
	uint64_t mc_sp;
	int64_t mc_retval;
	uint64_t mc_reserved[5];
} mcontext_t;

typedef struct ucontext {
	uint64_t uc_flags;
	uint64_t uc_link;
	sigset_t uc_sigmask;
	mcontext_t uc_mcontext;
	uint64_t uc_reserved[5];
} ucontext_t;

struct sigaction {
	uint64_t sa_handler;
	sigset_t sa_mask;
	uint32_t sa_flags;
	uint32_t __sa_reserved;
	uint64_t sa_restorer;
};
#endif
