/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_SIGNAL_H
#define ZEDBSD_SIGNAL_H
#include <zedbsd/signal.h>
#include <sys/types.h>
#include <time.h>
typedef void (*sighandler_t)(int);
typedef void (*siginfo_handler_t)(int, siginfo_t *, void *);
typedef struct {
	void *ss_sp;
	size_t ss_size;
	int ss_flags;
} stack_t;
#define SIG_ERR ((sighandler_t)-1)
int sigaction(int, const struct sigaction *, struct sigaction *);
int sigprocmask(int, const sigset_t *, sigset_t *);
int sigpending(sigset_t *);
int sigsuspend(const sigset_t *);
int kill(pid_t, int);
sighandler_t signal(int, sighandler_t);
int sigemptyset(sigset_t *);
int sigfillset(sigset_t *);
int sigaddset(sigset_t *, int);
int sigdelset(sigset_t *, int);
int sigismember(const sigset_t *, int);
int sigaltstack(const stack_t *, stack_t *);
int sigtimedwait(const sigset_t *, siginfo_t *, const struct timespec *);
int sigwaitinfo(const sigset_t *, siginfo_t *);
int sigwait(const sigset_t *, int *);
int sigqueue(pid_t, int, const union sigval);
int raise(int);
void abort(void) __attribute__((noreturn));
#endif
