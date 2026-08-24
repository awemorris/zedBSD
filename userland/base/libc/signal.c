/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "userland/base/libc/syscall.h"
#include <zedbsd/syscall.h>
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
extern void __signal_restorer(void);
static intptr_t call(uint32_t n,uintptr_t a,uintptr_t b,uintptr_t c){intptr_t r=__syscall6(n,a,b,c,0,0,0);if(r<0){errno=(int)-r;return-1;}return r;}

#define PUBLIC_SIGNAL_MASK \
	(((sigset_t)1ULL << (unsigned)SIGRTMAX) - 1ULL)

static int public_signal_valid(int signo)
{ return signo > 0 && signo <= SIGRTMAX; }

int
sigaction(int signo, const struct sigaction *action,
	struct sigaction *old_action)
{
	struct sigaction copy;

	if (!public_signal_valid(signo)) {
		errno = EINVAL;
		return -1;
	}
	if (action != NULL) {
		copy = *action;
		copy.sa_mask &= PUBLIC_SIGNAL_MASK;
		copy.__sa_reserved = 0;
		copy.sa_restorer = (uint64_t)(uintptr_t)__signal_restorer;
		action = &copy;
	}
	return (int)call(ZEDBSD_SYS_sigaction, signo, (uintptr_t)action,
	    (uintptr_t)old_action);
}

int
sigprocmask(int how, const sigset_t *set, sigset_t *old_set)
{
	sigset_t copy;

	if (set != NULL) {
		copy = *set;
		copy &= PUBLIC_SIGNAL_MASK;
		set = &copy;
	}
	if (call(ZEDBSD_SYS_sigprocmask, how, (uintptr_t)set,
	    (uintptr_t)old_set) < 0)
		return -1;
	if (old_set != NULL)
		*old_set &= PUBLIC_SIGNAL_MASK;
	return 0;
}
int
sigpending(sigset_t *set)
{
	int result = (int)call(ZEDBSD_SYS_sigpending, (uintptr_t)set, 0, 0);

	if (result == 0 && set != NULL)
		*set &= PUBLIC_SIGNAL_MASK;
	return result;
}
int
sigsuspend(const sigset_t *set)
{
	sigset_t copy;

	if (set != NULL) {
		copy = *set;
		copy &= PUBLIC_SIGNAL_MASK;
		set = &copy;
	}
	return (int)call(ZEDBSD_SYS_sigsuspend, (uintptr_t)set, 0, 0);
}
int
kill(pid_t pid, int signo)
{
	if (signo < 0 || signo > SIGRTMAX) {
		errno = EINVAL;
		return -1;
	}
	return (int)call(ZEDBSD_SYS_kill, pid, signo, 0);
}
sighandler_t signal(int s,sighandler_t h){struct sigaction a,o;memset(&a,0,sizeof(a));a.sa_handler=(uint64_t)(uintptr_t)h;return sigaction(s,&a,&o)==0?(sighandler_t)(uintptr_t)o.sa_handler:(sighandler_t)-1;}

static int sigset_signo(int signo) { return public_signal_valid(signo); }

int
sigemptyset(sigset_t *set)
{
	if (set == NULL) { errno = EINVAL; return -1; }
	*set = 0;
	return 0;
}

int
sigfillset(sigset_t *set)
{
	if (set == NULL) { errno = EINVAL; return -1; }
	*set = PUBLIC_SIGNAL_MASK;
	return 0;
}

int
sigaddset(sigset_t *set, int signo)
{
	if (set == NULL || !sigset_signo(signo)) { errno = EINVAL; return -1; }
	*set |= (sigset_t)1ULL << ((unsigned)signo - 1U);
	return 0;
}

int
sigdelset(sigset_t *set, int signo)
{
	if (set == NULL || !sigset_signo(signo)) { errno = EINVAL; return -1; }
	*set &= ~((sigset_t)1ULL << ((unsigned)signo - 1U));
	return 0;
}

int
sigismember(const sigset_t *set, int signo)
{
	if (set == NULL || !sigset_signo(signo)) { errno = EINVAL; return -1; }
	return (*set & ((sigset_t)1ULL << ((unsigned)signo - 1U))) != 0;
}
int sigaltstack(const stack_t*n,stack_t*o){struct sigaltstack_record in,out;intptr_t r;memset(&in,0,sizeof(in));memset(&out,0,sizeof(out));if(n!=NULL){in.ss_sp=(uapi_ptr_t)(uintptr_t)n->ss_sp;in.ss_size=n->ss_size;in.ss_flags=n->ss_flags;}r=call(ZEDBSD_SYS_sigaltstack,n!=NULL?(uintptr_t)&in:0,o!=NULL?(uintptr_t)&out:0,0);if(r==0&&o!=NULL){o->ss_sp=(void*)(uintptr_t)out.ss_sp;o->ss_size=(size_t)out.ss_size;o->ss_flags=out.ss_flags;}return(int)r;}
int
sigtimedwait(const sigset_t *set, siginfo_t *information,
	const struct timespec *timeout)
{
	sigset_t copy;

	if (set == NULL) {
		errno = EINVAL;
		return -1;
	}
	copy = *set & PUBLIC_SIGNAL_MASK;
	if (copy == 0) {
		errno = EINVAL;
		return -1;
	}
	return (int)call(ZEDBSD_SYS_sigtimedwait, (uintptr_t)&copy,
	    (uintptr_t)information, (uintptr_t)timeout);
}
int sigwaitinfo(const sigset_t*s,siginfo_t*i){return sigtimedwait(s,i,NULL);}
int sigwait(const sigset_t*s,int*n){int r;if(n==NULL){errno=EINVAL;return EINVAL;}r=sigtimedwait(s,NULL,NULL);if(r<0)return errno;*n=r;return 0;}
int
sigqueue(pid_t pid, int signo, const union sigval value)
{
	uint64_t raw = 0;

	/* Like kill(pid, 0), signo zero performs existence and permission
	 * checking without queuing a signal. */
	if (signo < 0 || signo > SIGRTMAX) {
		errno = EINVAL;
		return -1;
	}
	memcpy(&raw, &value, sizeof(raw));
	return (int)call(ZEDBSD_SYS_sigqueue, pid, signo, (uintptr_t)raw);
}
int raise(int n){return kill(getpid(),n);}

static const char *
signal_description(int number)
{
	switch (number) {
	case SIGHUP: return "Hangup";
	case SIGINT: return "Interrupt";
	case SIGQUIT: return "Quit";
	case SIGILL: return "Illegal instruction";
	case SIGABRT: return "Aborted";
	case SIGFPE: return "Arithmetic exception";
	case SIGKILL: return "Killed";
	case SIGSEGV: return "Segmentation fault";
	case SIGPIPE: return "Broken pipe";
	case SIGALRM: return "Alarm clock";
	case SIGTERM: return "Terminated";
	case SIGCHLD: return "Child status changed";
	case SIGCONT: return "Continued";
	case SIGSTOP: return "Stopped";
	case SIGTSTP: return "Stopped (tty)";
	case SIGTTIN: return "Stopped (tty input)";
	case SIGTTOU: return "Stopped (tty output)";
	case SIGURG: return "Urgent I/O condition";
	case SIGWINCH: return "Window size changed";
	case SIGIO: return "I/O possible";
	case SIGXCPU: return "CPU time limit exceeded";
	case SIGXFSZ: return "File size limit exceeded";
	case SIGBUS: return "Bus error";
	case SIGTRAP: return "Trace trap";
	default: return "Unknown signal";
	}
}

struct signal_name {
	int number;
	const char *name;
};

static const struct signal_name signal_names[] = {
	{ SIGHUP, "HUP" }, { SIGINT, "INT" }, { SIGQUIT, "QUIT" },
	{ SIGILL, "ILL" }, { SIGTRAP, "TRAP" }, { SIGABRT, "ABRT" },
	{ SIGVTALRM, "VTALRM" }, { SIGFPE, "FPE" }, { SIGKILL, "KILL" },
	{ SIGBUS, "BUS" }, { SIGSEGV, "SEGV" }, { SIGPROF, "PROF" },
	{ SIGPIPE, "PIPE" }, { SIGALRM, "ALRM" }, { SIGTERM, "TERM" },
	{ SIGUSR1, "USR1" }, { SIGUSR2, "USR2" }, { SIGCHLD, "CHLD" },
	{ SIGCONT, "CONT" }, { SIGSTOP, "STOP" }, { SIGTSTP, "TSTP" },
	{ SIGTTIN, "TTIN" }, { SIGTTOU, "TTOU" }, { SIGURG, "URG" },
	{ SIGWINCH, "WINCH" }, { SIGIO, "IO" }, { SIGXCPU, "XCPU" },
	{ SIGXFSZ, "XFSZ" },
};

static int
decimal_signal(const char *name, int *number)
{
	unsigned value = 0;
	const char *cursor;

	if (name[0] == '\0')
		return -1;
	for (cursor = name; *cursor != '\0'; cursor++) {
		if (*cursor < '0' || *cursor > '9')
			return -1;
		value = value * 10U + (unsigned)(*cursor - '0');
		if (value > SIGRTMAX)
			return -1;
	}
	if (value == 0)
		return -1;
	*number = (int)value;
	return 0;
}

static int
realtime_offset(const char *name, const char *prefix, int *offset)
{
	unsigned value = 0;
	const char *cursor;

	if (strncmp(name, prefix, 6) != 0 || name[6] == '\0')
		return -1;
	for (cursor = name + 6; *cursor != '\0'; cursor++) {
		if (*cursor < '0' || *cursor > '9')
			return -1;
		value = value * 10U + (unsigned)(*cursor - '0');
		if (value > (unsigned)(SIGRTMAX - SIGRTMIN))
			return -1;
	}
	*offset = (int)value;
	return 0;
}

int
sig2str(int number, char *name)
{
	unsigned i;

	for (i = 0; i < sizeof(signal_names) / sizeof(signal_names[0]); i++)
		if (signal_names[i].number == number) {
			strcpy(name, signal_names[i].name);
			return 0;
		}
	if (number >= SIGRTMIN && number <= SIGRTMAX) {
		if (number == SIGRTMIN)
			strcpy(name, "RTMIN");
		else
			(void)snprintf(name, SIG2STR_MAX, "RTMIN+%d",
			    number - SIGRTMIN);
		return 0;
	}
	return -1;
}

int
str2sig(const char *restrict name, int *restrict number)
{
	unsigned i;
	int offset;

	if (decimal_signal(name, number) == 0)
		return 0;
	for (i = 0; i < sizeof(signal_names) / sizeof(signal_names[0]); i++)
		if (!strcmp(name, signal_names[i].name)) {
			*number = signal_names[i].number;
			return 0;
		}
	if (!strcmp(name, "POLL")) {
		*number = SIGPOLL;
		return 0;
	}
	if (!strcmp(name, "RTMIN")) {
		*number = SIGRTMIN;
		return 0;
	}
	if (!strcmp(name, "RTMAX")) {
		*number = SIGRTMAX;
		return 0;
	}
	if (realtime_offset(name, "RTMIN+", &offset) == 0) {
		*number = SIGRTMIN + offset;
		return 0;
	}
	if (realtime_offset(name, "RTMAX-", &offset) == 0) {
		*number = SIGRTMAX - offset;
		return 0;
	}
	return -1;
}

void
psignal(int number, const char *prefix)
{
	if (prefix != NULL && *prefix != '\0')
		fprintf(stderr, "%s: %s\n", prefix, signal_description(number));
	else
		fprintf(stderr, "%s\n", signal_description(number));
}

void
psiginfo(const siginfo_t *information, const char *prefix)
{
	if (information == NULL) {
		errno = EINVAL;
		return;
	}
	psignal(information->si_signo, prefix);
}
void abort(void){(void)raise(SIGABRT);_exit(128+SIGABRT);}
