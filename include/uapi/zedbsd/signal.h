/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_UAPI_SIGNAL_H
#define ZEDBSD_UAPI_SIGNAL_H
#include <stdint.h>
typedef uint32_t sigset_t;
#define NSIG 32
#define SIGHUP 1
#define SIGINT 2
#define SIGQUIT 3
#define SIGILL 4
#define SIGTRAP 5
#define SIGABRT 6
#define SIGFPE 8
#define SIGKILL 9
#define SIGBUS 10
#define SIGSEGV 11
#define SIGPIPE 13
#define SIGALRM 14
#define SIGTERM 15
#define SIGUSR1 16
#define SIGUSR2 17
#define SIGCHLD 18
#define SIGCONT 19
#define SIGSTOP 20
#define SIGTSTP 21
#define SIGTTIN 22
#define SIGTTOU 23
#define SIG_BLOCK 0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2
#define SA_RESTART 0x0001U
#define SA_NOCLDSTOP 0x0002U
#define SA_NOCLDWAIT 0x0004U
#define SA_NODEFER 0x0008U
#define SA_RESETHAND 0x0010U
#define SIG_DFL 0ULL
#define SIG_IGN 1ULL
struct sigaction {
	uint64_t sa_handler;
	sigset_t sa_mask;
	uint32_t sa_flags;
	uint64_t sa_restorer;
};
#endif
