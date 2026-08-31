/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the zedBSD C library signal support.
 */

#include "userland/base/libc/syscall.h"
#include <zedbsd/syscall.h>
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
extern void __signal_restorer(void);

#define PUBLIC_SIGNAL_MASK (((sigset_t)1ULL << (unsigned)SIGRTMAX) - 1ULL)

struct signal_name {
	int number;
	const char *name;
};

static const struct signal_name signal_names[] = {
    {SIGHUP, "HUP"},	   {SIGINT, "INT"},   {SIGQUIT, "QUIT"},
    {SIGILL, "ILL"},	   {SIGTRAP, "TRAP"}, {SIGABRT, "ABRT"},
    {SIGVTALRM, "VTALRM"}, {SIGFPE, "FPE"},   {SIGKILL, "KILL"},
    {SIGBUS, "BUS"},	   {SIGSEGV, "SEGV"}, {SIGPROF, "PROF"},
    {SIGPIPE, "PIPE"},	   {SIGALRM, "ALRM"}, {SIGTERM, "TERM"},
    {SIGUSR1, "USR1"},	   {SIGUSR2, "USR2"}, {SIGCHLD, "CHLD"},
    {SIGCONT, "CONT"},	   {SIGSTOP, "STOP"}, {SIGTSTP, "TSTP"},
    {SIGTTIN, "TTIN"},	   {SIGTTOU, "TTOU"}, {SIGURG, "URG"},
    {SIGWINCH, "WINCH"},   {SIGIO, "IO"},     {SIGXCPU, "XCPU"},
    {SIGXFSZ, "XFSZ"},
};

static int public_signal_valid(int signo);
static intptr_t call(uint32_t n, uintptr_t a, uintptr_t b, uintptr_t c);
static int sigset_signo(int signo);
static int decimal_signal(const char *name, int *number);
static int realtime_offset(const char *name, const char *prefix, int *offset);
static const char *signal_description(int number);

/*
 * Implements the sigaction operation.
 */
int
sigaction(
	int signo,
	const struct sigaction *action,
	struct sigaction *old_action)
{
	int function_result;
	struct sigaction copy;

	/* Handles a failed public signal valid operation. */
	if (!public_signal_valid(signo)) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}

	/* Handles the action availability. */
	if (action != NULL) {
		copy = *action;
		copy.sa_mask &= PUBLIC_SIGNAL_MASK;
		copy.__sa_reserved = 0;
		copy.sa_restorer = (uint64_t)(uintptr_t)__signal_restorer;
		action = &copy;
	}

	/* Computes the function result. */
	function_result = (int)call(ZEDBSD_SYS_sigaction, signo, (uintptr_t)action,
			 (uintptr_t)old_action);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the sigprocmask operation.
 */
int
sigprocmask(
	int how,
	const sigset_t *set,
	sigset_t *old_set)
{
	sigset_t copy;

	/* Handles the set availability. */
	if (set != NULL) {
		copy = *set;
		copy &= PUBLIC_SIGNAL_MASK;
		set = &copy;
	}

	/* Handles a failed call operation. */
	if (call(ZEDBSD_SYS_sigprocmask, how, (uintptr_t)set,
		 (uintptr_t)old_set) < 0)

		/* Reports operation failure. */
		return -1;

	/* Handles the old set availability. */
	if (old_set != NULL)
		*old_set &= PUBLIC_SIGNAL_MASK;
	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the sigpending operation.
 */
int
sigpending(
	sigset_t *set)
{
	int result;

	result = (int)call(ZEDBSD_SYS_sigpending, (uintptr_t)set, 0, 0);

	/* Handles the set availability. */
	if (result == 0 && set != NULL)
		*set &= PUBLIC_SIGNAL_MASK;
	/* Returns the computed result. */
	return result;
}

/*
 * Implements the sigsuspend operation.
 */
int
sigsuspend(
	const sigset_t *set)
{
	int function_result;
	sigset_t copy;

	/* Handles the set availability. */
	if (set != NULL) {
		copy = *set;
		copy &= PUBLIC_SIGNAL_MASK;
		set = &copy;
	}

	/* Computes the function result. */
	function_result = (int)call(ZEDBSD_SYS_sigsuspend, (uintptr_t)set, 0, 0);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the kill operation.
 */
int
kill(
	pid_t pid,
	int signo)
{
	int function_result;

	/* Handles the signo condition. */
	if (signo < 0 || signo > SIGRTMAX) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}

	/* Computes the function result. */
	function_result = (int)call(ZEDBSD_SYS_kill, pid, signo, 0);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the signal operation.
 */
sighandler_t
signal(
	int s,
	sighandler_t h)
{
	sighandler_t function_result;
	struct sigaction a, o;

	memset(&a, 0, sizeof(a));
	a.sa_handler = (uint64_t)(uintptr_t)h;

	/* Computes the function result. */
	function_result = sigaction(s, &a, &o) == 0 ? (sighandler_t)(uintptr_t)o.sa_handler
					 : (sighandler_t)-1;

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the sigemptyset operation.
 */
int
sigemptyset(
	sigset_t *set)
{
	/* Handles the set availability. */
	if (set == NULL) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}
	*set = 0;
	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the sigfillset operation.
 */
int
sigfillset(
	sigset_t *set)
{
	/* Handles the set availability. */
	if (set == NULL) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}
	*set = PUBLIC_SIGNAL_MASK;
	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the sigaddset operation.
 */
int
sigaddset(
	sigset_t *set,
	int signo)
{
	/* Handles a failed sigset signo operation. */
	if (set == NULL || !sigset_signo(signo)) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}
	*set |= (sigset_t)1ULL << ((unsigned)signo - 1U);
	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the sigdelset operation.
 */
int
sigdelset(
	sigset_t *set,
	int signo)
{
	/* Handles a failed sigset signo operation. */
	if (set == NULL || !sigset_signo(signo)) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}
	*set &= ~((sigset_t)1ULL << ((unsigned)signo - 1U));
	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the sigismember operation.
 */
int
sigismember(
	const sigset_t *set,
	int signo)
{
	/* Handles a failed sigset signo operation. */
	if (set == NULL || !sigset_signo(signo)) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}

	/* Returns the computed result. */
	return (*set & ((sigset_t)1ULL << ((unsigned)signo - 1U))) != 0;
}

/*
 * Implements the sigaltstack operation.
 */
int
sigaltstack(
	const stack_t *n,
	stack_t *o)
{
	struct sigaltstack_record in, out;
	intptr_t r;

	memset(&in, 0, sizeof(in));
	memset(&out, 0, sizeof(out));

	/* Handles the n availability. */
	if (n != NULL) {
		in.ss_sp = (uapi_ptr_t)(uintptr_t)n->ss_sp;
		in.ss_size = n->ss_size;
		in.ss_flags = n->ss_flags;
	}
	r = call(ZEDBSD_SYS_sigaltstack, n != NULL ? (uintptr_t)&in : 0,
		 o != NULL ? (uintptr_t)&out : 0, 0);

	/* Handles the o availability. */
	if (r == 0 && o != NULL) {
		o->ss_sp = (void *)(uintptr_t)out.ss_sp;
		o->ss_size = (size_t)out.ss_size;
		o->ss_flags = out.ss_flags;
	}

	/* Returns the computed result. */
	return (int)r;
}

/*
 * Implements the sigtimedwait operation.
 */
int
sigtimedwait(
	const sigset_t *set,
	siginfo_t *information,
	const struct timespec *timeout)
{
	int function_result;
	sigset_t copy;

	/* Handles the set availability. */
	if (set == NULL) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}
	copy = *set & PUBLIC_SIGNAL_MASK;

	/* Handles the copy condition. */
	if (copy == 0) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}

	/* Computes the function result. */
	function_result = (int)call(ZEDBSD_SYS_sigtimedwait, (uintptr_t)&copy,
			 (uintptr_t)information, (uintptr_t)timeout);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the sigwaitinfo operation.
 */
int
sigwaitinfo(
	const sigset_t *s,
	siginfo_t *i)
{
	int function_result;

	/* Obtains the sigtimedwait result. */
	function_result = sigtimedwait(s, i, NULL);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the sigwait operation.
 */
int
sigwait(
	const sigset_t *s,
	int *n)
{
	int r;

	/* Handles the n availability. */
	if (n == NULL) {
		errno = EINVAL;

		/* Returns the computed result. */
		return EINVAL;
	}
	r = sigtimedwait(s, NULL, NULL);

	/* Handles the r condition. */
	if (r < 0)
		return errno;
	*n = r;
	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the sigqueue operation.
 */
int
sigqueue(
	pid_t pid,
	int signo,
	const union sigval value)
{
	int function_result;
	uint64_t raw;

	raw = 0;

	/*
 * Like kill(pid, 0), signo zero performs existence and permission
	 * checking without queuing a signal. */
	if (signo < 0 || signo > SIGRTMAX) {
		errno = EINVAL;

		/* Reports operation failure. */
		return -1;
	}
	memcpy(&raw, &value, sizeof(raw));

	/* Computes the function result. */
	function_result = (int)call(ZEDBSD_SYS_sigqueue, pid, signo, (uintptr_t)raw);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the raise operation.
 */
int
raise(
	int n)
{
	int function_result;

	/* Obtains the kill result. */
	function_result = kill(getpid(), n);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the sig2str operation.
 */
int
sig2str(
	int number,
	char *name)
{
	unsigned i;

	/* Process each remaining element. */
	for (i = 0; i < sizeof(signal_names) / sizeof(signal_names[0]); i++)

		/* Handles the signal names condition. */
		if (signal_names[i].number == number) {
			strcpy(name, signal_names[i].name);

			/* Reports successful completion. */
			return 0;
		}

	/* Handles the number condition. */
	if (number >= SIGRTMIN && number <= SIGRTMAX) {
		/* Handles the number condition. */
		if (number == SIGRTMIN)
			strcpy(name, "RTMIN");
		else
			(void)snprintf(name, SIG2STR_MAX, "RTMIN+%d",
				       number - SIGRTMIN);

		/* Reports successful completion. */
		return 0;
	}

	/* Reports operation failure. */
	return -1;
}

/*
 * Implements the str2sig operation.
 */
int
str2sig(
	const char *restrict name,
	int *restrict number)
{
	unsigned i;
	int offset;

	/* Handles a failed decimal signal operation. */
	if (decimal_signal(name, number) == 0)
		return 0;

	/* Process each remaining element. */
	for (i = 0; i < sizeof(signal_names) / sizeof(signal_names[0]); i++)

		/* Selects the matching value. */
		if (!strcmp(name, signal_names[i].name)) {
			*number = signal_names[i].number;
			/* Reports successful completion. */
			return 0;
		}

	/* Selects the matching value. */
	if (!strcmp(name, "POLL")) {
		*number = SIGPOLL;
		/* Reports successful completion. */
		return 0;
	}

	/* Selects the matching value. */
	if (!strcmp(name, "RTMIN")) {
		*number = SIGRTMIN;
		/* Reports successful completion. */
		return 0;
	}

	/* Selects the matching value. */
	if (!strcmp(name, "RTMAX")) {
		*number = SIGRTMAX;
		/* Reports successful completion. */
		return 0;
	}

	/* Handles a failed realtime offset operation. */
	if (realtime_offset(name, "RTMIN+", &offset) == 0) {
		*number = SIGRTMIN + offset;
		/* Reports successful completion. */
		return 0;
	}

	/* Handles a failed realtime offset operation. */
	if (realtime_offset(name, "RTMAX-", &offset) == 0) {
		*number = SIGRTMAX - offset;
		/* Reports successful completion. */
		return 0;
	}

	/* Reports operation failure. */
	return -1;
}

/*
 * Implements the psignal operation.
 */
void
psignal(
	int number,
	const char *prefix)
{
	/* Handles the prefix availability. */
	if (prefix != NULL && *prefix != '\0')
		fprintf(stderr, "%s: %s\n", prefix, signal_description(number));
	else
		fprintf(stderr, "%s\n", signal_description(number));
}

/*
 * Implements the psiginfo operation.
 */
void
psiginfo(
	const siginfo_t *information,
	const char *prefix)
{
	/* Handles the information availability. */
	if (information == NULL) {
		errno = EINVAL;

		/* Returns the computed result. */
		return;
	}
	psignal(information->si_signo, prefix);
}

/*
 * Implements the abort operation.
 */
void
abort(
	void)
{
	(void)raise(SIGABRT);
	_exit(128 + SIGABRT);
}

/* Supports the public signal valid operation. */
static int
public_signal_valid(
	int signo)
{
	/* Returns the computed result. */
	return signo > 0 && signo <= SIGRTMAX;
}

/* Supports the call operation. */
static intptr_t
call(
	uint32_t n,
	uintptr_t a,
	uintptr_t b,
	uintptr_t c)
{
	intptr_t r;

	r = __syscall6(n, a, b, c, 0, 0, 0);

	/* Handles the r condition. */
	if (r < 0) {
		errno = (int)-r;

		/* Reports operation failure. */
		return -1;
	}

	/* Returns the computed result. */
	return r;
}

/* Supports the sigset signo operation. */
static int
sigset_signo(
	int signo)
{
	int function_result;

	/* Obtains the public signal valid result. */
	function_result = public_signal_valid(signo);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the decimal signal operation. */
static int
decimal_signal(
	const char *name,
	int *number)
{
	unsigned value;
	const char *cursor;

	value = 0;

	/* Validates the current name. */
	if (name[0] == '\0')
		return -1;

	/* Process each element required by the operation. */
	for (cursor = name; *cursor != '\0'; cursor++) {
		/* Checks the current cursor position. */
		if (*cursor < '0' || *cursor > '9')
			return -1;
		value = value * 10U + (unsigned)(*cursor - '0');

		/* Validates the current value. */
		if (value > SIGRTMAX)
			return -1;
	}

	/* Validates the current value. */
	if (value == 0)
		return -1;
	*number = (int)value;
	/* Reports successful completion. */
	return 0;
}

/* Supports the realtime offset operation. */
static int
realtime_offset(
	const char *name,
	const char *prefix,
	int *offset)
{
	unsigned value;
	const char *cursor;

	value = 0;

	/* Selects the matching prefix. */
	if (strncmp(name, prefix, 6) != 0 || name[6] == '\0')
		return -1;

	/* Process each element required by the operation. */
	for (cursor = name + 6; *cursor != '\0'; cursor++) {
		/* Checks the current cursor position. */
		if (*cursor < '0' || *cursor > '9')
			return -1;
		value = value * 10U + (unsigned)(*cursor - '0');

		/* Validates the current value. */
		if (value > (unsigned)(SIGRTMAX - SIGRTMIN))
			return -1;
	}
	*offset = (int)value;
	/* Reports successful completion. */
	return 0;
}

/* Supports the signal description operation. */
static const char *
signal_description(
	int number)
{
	/* Dispatch the selected operation case. */
	switch (number) {
	case SIGHUP:
		/* Returns the computed result. */
		return "Hangup";
	case SIGINT:
		/* Returns the computed result. */
		return "Interrupt";
	case SIGQUIT:
		/* Returns the computed result. */
		return "Quit";
	case SIGILL:
		/* Returns the computed result. */
		return "Illegal instruction";
	case SIGABRT:
		/* Returns the computed result. */
		return "Aborted";
	case SIGFPE:
		/* Returns the computed result. */
		return "Arithmetic exception";
	case SIGKILL:
		/* Returns the computed result. */
		return "Killed";
	case SIGSEGV:
		/* Returns the computed result. */
		return "Segmentation fault";
	case SIGPIPE:
		/* Returns the computed result. */
		return "Broken pipe";
	case SIGALRM:
		/* Returns the computed result. */
		return "Alarm clock";
	case SIGTERM:
		/* Returns the computed result. */
		return "Terminated";
	case SIGCHLD:
		/* Returns the computed result. */
		return "Child status changed";
	case SIGCONT:
		/* Returns the computed result. */
		return "Continued";
	case SIGSTOP:
		/* Returns the computed result. */
		return "Stopped";
	case SIGTSTP:
		/* Returns the computed result. */
		return "Stopped (tty)";
	case SIGTTIN:
		/* Returns the computed result. */
		return "Stopped (tty input)";
	case SIGTTOU:
		/* Returns the computed result. */
		return "Stopped (tty output)";
	case SIGURG:
		/* Returns the computed result. */
		return "Urgent I/O condition";
	case SIGWINCH:
		/* Returns the computed result. */
		return "Window size changed";
	case SIGIO:
		/* Returns the computed result. */
		return "I/O possible";
	case SIGXCPU:
		/* Returns the computed result. */
		return "CPU time limit exceeded";
	case SIGXFSZ:
		/* Returns the computed result. */
		return "File size limit exceeded";
	case SIGBUS:
		/* Returns the computed result. */
		return "Bus error";
	case SIGTRAP:
		/* Returns the computed result. */
		return "Trace trap";
	default:
		/* Returns the computed result. */
		return "Unknown signal";
	}
}
