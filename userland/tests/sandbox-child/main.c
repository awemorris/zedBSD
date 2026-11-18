/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The child of the sandbox test (ws168-p002): a static program that
 * sandboxtest starts with sandbox_spawn and that reports, on fd 1, what
 * it could do inside.  It writes with snprintf and write only (stdio's
 * buffers are not needed) and calls the kernel directly for the calls it
 * tries, so that each one is exactly the call named.
 *
 *   sandbox-child echo          copies fd 0 to fd 1
 *   sandbox-child fds           names the descriptors 0 to 15 that are open ("fds 0 1 9")
 *   sandbox-child limits        the limits of memory, CPU time, written size, files and core
 *   sandbox-child signals       whether SIGTERM, SIGINT, SIGHUP and SIGPIPE have their default action
 *   sandbox-child allowed       anonymous memory, malloc, the time, entropy, the PID, a sleep, a terminal query
 *   sandbox-child memory        file, shared and executable memory, each refused (EPERM)
 *   sandbox-child deny NAME     one call outside the set; its raw answer ("deny open result=-1")
 *   sandbox-child threads       a thread made and joined (SANDBOX_ALLOW_THREADS)
 *
 * The status is 0 when the mode ran, 64 for an unknown mode.
 */

#include "src/libc/syscall.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <uapi/fcntl.h>
#include <uapi/ioctl.h>
#include <uapi/syscall.h>
#include <uapi/termios.h>

/* The longest line the child writes. */
#define CHILD_LINE_MAX	256U

/* The status of an unknown mode. */
#define CHILD_USAGE	64

/* One call outside the set, by the name the parent gives, and its arguments. */
struct child_call {
	const char *name;
	uint32_t number;
	uintptr_t args[6];
};

static void child_say(const char *format, ...) __attribute__((format(printf, 1, 2)));
static int child_echo(void);
static int child_fds(void);
static int child_limits(void);
static int child_signals(void);
static int child_allowed(void);
static int child_memory(void);
static int child_deny(const char *name);
static int child_threads(void);
static void *child_thread(void *argument);

/* The path the path-taking calls are tried with, and a buffer for getcwd. */
static const char child_root[] = "/";
static char child_buffer[64];

/* The calls outside the set the parent names, with harmless arguments. */
static const struct child_call child_calls[] = {
	{ "open", KERN_SYS_open, { (uintptr_t)child_root, O_RDONLY, 0, 0, 0, 0 } },
	{ "openat", KERN_SYS_openat, { (uintptr_t)AT_FDCWD, (uintptr_t)child_root, O_RDONLY, 0, 0, 0 } },
	{ "stat", KERN_SYS_stat, { (uintptr_t)child_root, (uintptr_t)child_buffer, 0, 0, 0, 0 } },
	{ "getcwd", KERN_SYS_getcwd, { (uintptr_t)child_buffer, sizeof(child_buffer), 0, 0, 0, 0 } },
	{ "chdir", KERN_SYS_chdir, { (uintptr_t)child_root, 0, 0, 0, 0, 0 } },
	{ "fchdir", KERN_SYS_fchdir, { 0, 0, 0, 0, 0, 0 } },
	{ "socket", KERN_SYS_socket, { 2, 2, 0, 0, 0, 0 } },
	{ "socketpair", KERN_SYS_socketpair, { 1, 1, 0, (uintptr_t)child_buffer, 0, 0 } },
	{ "pipe", KERN_SYS_pipe, { (uintptr_t)child_buffer, 0, 0, 0, 0, 0 } },
	{ "dup", KERN_SYS_dup, { 0, 0, 0, 0, 0, 0 } },
	{ "fcntl", KERN_SYS_fcntl, { 0, F_GETFL, 0, 0, 0, 0 } },
	{ "fork", KERN_SYS_fork, { 0, 0, 0, 0, 0, 0 } },
	{ "vfork", KERN_SYS_vfork, { 0, 0, 0, 0, 0, 0 } },
	{ "execve", KERN_SYS_execve, { (uintptr_t)child_root, 0, 0, 0, 0, 0 } },
	{ "sandbox_spawn", KERN_SYS_sandbox_spawn, { 0, 0, 0, 0, 0, 0 } },
	{ "ioctl", KERN_SYS_ioctl, { 1, FIONBIO, (uintptr_t)child_buffer, 0, 0, 0 } },
	{ "sysctl", KERN_SYS_sysctl, { 0, 0, 0, 0, 0, 0 } },
	{ "kill", KERN_SYS_kill, { 1, 0, 0, 0, 0, 0 } },
	{ "setuid", KERN_SYS_setuid, { 0, 0, 0, 0, 0, 0 } },
	{ "setrlimit", KERN_SYS_setrlimit, { RLIMIT_CORE, (uintptr_t)child_buffer, 0, 0, 0, 0 } },
	{ "recvmsg", KERN_SYS_recvmsg, { 0, (uintptr_t)child_buffer, 0, 0, 0, 0 } },
	{ "mount", KERN_SYS_mount, { 0, 0, 0, 0, 0, 0 } },
	{ "thread_create", KERN_SYS_thread_create, { 0, 0, 0, 0, 0, 0 } },
};

/*
 * Runs the mode the first argument names.
 */
int
main(
	int argc,
	char **argv)
{
	int same;
	int status;

	/* A mode. */
	if (argc < 2) {
		child_say("usage\n");
		return CHILD_USAGE;
	}

	/* Each mode in turn. */
	status = CHILD_USAGE;
	same = strcmp(argv[1], "echo");
	if (same == 0)
		status = child_echo();
	same = strcmp(argv[1], "fds");
	if (same == 0)
		status = child_fds();
	same = strcmp(argv[1], "limits");
	if (same == 0)
		status = child_limits();
	same = strcmp(argv[1], "signals");
	if (same == 0)
		status = child_signals();
	same = strcmp(argv[1], "allowed");
	if (same == 0)
		status = child_allowed();
	same = strcmp(argv[1], "memory");
	if (same == 0)
		status = child_memory();
	same = strcmp(argv[1], "threads");
	if (same == 0)
		status = child_threads();
	same = strcmp(argv[1], "deny");
	if (same == 0 && argc == 3)
		status = child_deny(argv[2]);

	/* The mode's status. */
	if (status == CHILD_USAGE)
		child_say("unknown mode\n");
	return status;
}

/* Writes a line to fd 1 (stdio is not used: its buffers need nothing more, but the line must go at once). */
static void
child_say(
	const char *format,
	...)
{
	char line[CHILD_LINE_MAX];
	va_list arguments;
	int length;

	/* The line, cut to the buffer. */
	va_start(arguments, format);
	length = vsnprintf(line, sizeof(line), format, arguments);
	va_end(arguments);
	if (length < 0)
		return;
	if ((size_t)length >= sizeof(line))
		length = (int)sizeof(line) - 1;

	/* Written as it is. */
	(void)write(1, line, (size_t)length);
}

/* Copies fd 0 to fd 1. */
static int
child_echo(void)
{
	char buffer[512];
	ssize_t got;
	ssize_t put;

	/* Until the end of fd 0. */
	for (;;) {
		got = read(0, buffer, sizeof(buffer));
		if (got <= 0)
			break;
		put = write(1, buffer, (size_t)got);
		if (put != got)
			return 1;
	}

	/* Succeeded: copied. */
	return 0;
}

/* Names the descriptors 0 to 15 that are open. */
static int
child_fds(void)
{
	char line[CHILD_LINE_MAX];
	struct stat status;
	size_t used;
	int fd;
	int error;

	/* Each descriptor fstat knows. */
	used = (size_t)snprintf(line, sizeof(line), "fds");
	for (fd = 0; fd < 16; fd++) {
		error = fstat(fd, &status);
		if (error != 0)
			continue;
		used += (size_t)snprintf(line + used, sizeof(line) - used, " %d", fd);
	}

	/* The line. */
	child_say("%s\n", line);
	return 0;
}

/* The limits of memory, CPU time, written size, files and core. */
static int
child_limits(void)
{
	static const int resources[] = { RLIMIT_AS, RLIMIT_CPU, RLIMIT_FSIZE, RLIMIT_NOFILE, RLIMIT_CORE };
	static const char *const names[] = { "as", "cpu", "fsize", "nofile", "core" };
	struct rlimit limit;
	unsigned index;
	int error;

	/* Each limit, its current value (the most a number can be is "inf"). */
	for (index = 0U; index < sizeof(resources) / sizeof(resources[0]); index++) {
		error = getrlimit(resources[index], &limit);
		if (error != 0) {
			child_say("limit %s error=%d\n", names[index], errno);
			continue;
		}
		if (limit.rlim_cur == RLIM_INFINITY)
			child_say("limit %s inf\n", names[index]);
		else
			child_say("limit %s %llu\n", names[index], (unsigned long long)limit.rlim_cur);
	}

	/* Succeeded. */
	return 0;
}

/* Whether SIGTERM, SIGINT, SIGHUP and SIGPIPE have their default action. */
static int
child_signals(void)
{
	static const int numbers[] = { SIGTERM, SIGINT, SIGHUP, SIGPIPE };
	struct sigaction action;
	unsigned defaults;
	unsigned index;
	int error;

	/* Each signal's action, read. */
	defaults = 0U;
	for (index = 0U; index < sizeof(numbers) / sizeof(numbers[0]); index++) {
		error = sigaction(numbers[index], NULL, &action);
		if (error == 0 && action.sa_handler == SIG_DFL)
			defaults++;
	}

	/* How many have the default. */
	child_say("signals default=%u of %u\n", defaults, (unsigned)(sizeof(numbers) / sizeof(numbers[0])));
	return 0;
}

/* Anonymous memory, malloc, the time, entropy, the PID, a sleep and a terminal query, each working. */
static int
child_allowed(void)
{
	struct timespec now;
	struct timespec pause;
	unsigned char random[16];
	char *memory;
	char *heap;
	pid_t self;
	int terminal;
	int error;

	/* Anonymous private memory, written and given back. */
	memory = mmap(NULL, 65536U, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (memory == MAP_FAILED) {
		child_say("allowed mmap error=%d\n", errno);
		return 1;
	}
	memset(memory, 0x5a, 65536U);
	error = munmap(memory, 65536U);
	if (error != 0) {
		child_say("allowed munmap error=%d\n", errno);
		return 1;
	}

	/* The heap, beyond its first arena. */
	heap = malloc(1048576U);
	if (heap == NULL) {
		child_say("allowed malloc error=%d\n", errno);
		return 1;
	}
	memset(heap, 0x33, 1048576U);
	free(heap);

	/* The time, entropy, the PID, a short sleep. */
	error = clock_gettime(CLOCK_MONOTONIC, &now);
	if (error != 0) {
		child_say("allowed clock_gettime error=%d\n", errno);
		return 1;
	}
	error = getentropy(random, sizeof(random));
	if (error != 0) {
		child_say("allowed getentropy error=%d\n", errno);
		return 1;
	}
	self = getpid();
	pause.tv_sec = 0;
	pause.tv_nsec = 1000000L;
	error = nanosleep(&pause, NULL);
	if (error != 0) {
		child_say("allowed nanosleep error=%d\n", errno);
		return 1;
	}

	/* The terminal query is answered "not a terminal" (libc's start asks it). */
	terminal = isatty(1);
	if (terminal) {
		child_say("allowed isatty is a terminal\n");
		return 1;
	}

	/* Succeeded: everything worked. */
	child_say("allowed ok pid=%d\n", (int)self);
	return 0;
}

/* File, shared and executable memory, and making memory executable, each refused with EPERM. */
static int
child_memory(void)
{
	intptr_t answer;
	char *memory;
	unsigned refused;

	/* A file's memory (fd 0). */
	refused = 0U;
	answer = __syscall6(KERN_SYS_mmap, 0, 4096U, PROT_READ, MAP_PRIVATE, 0, 0);
	if (answer == -EPERM)
		refused++;
	child_say("memory file result=%ld\n", (long)answer);

	/* Shared anonymous memory. */
	answer = __syscall6(KERN_SYS_mmap, 0, 4096U, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, (uintptr_t)-1, 0);
	if (answer == -EPERM)
		refused++;
	child_say("memory shared result=%ld\n", (long)answer);

	/* Executable anonymous memory. */
	answer = __syscall6(KERN_SYS_mmap, 0, 4096U, PROT_READ | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, (uintptr_t)-1, 0);
	if (answer == -EPERM)
		refused++;
	child_say("memory exec result=%ld\n", (long)answer);

	/* Memory made executable afterwards. */
	memory = mmap(NULL, 4096U, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (memory == MAP_FAILED) {
		child_say("memory anonymous error=%d\n", errno);
		return 1;
	}
	answer = __syscall6(KERN_SYS_mprotect, (uintptr_t)memory, 4096U, PROT_READ | PROT_EXEC, 0, 0, 0);
	if (answer == -EPERM)
		refused++;
	child_say("memory mprotect result=%ld\n", (long)answer);

	/* How many were refused. */
	child_say("memory refused=%u of 4\n", refused);
	return 0;
}

/* One call outside the set, by name; its raw answer (-1 for EPERM). */
static int
child_deny(
	const char *name)
{
	const struct child_call *call;
	intptr_t answer;
	unsigned index;
	int same;

	/* The call the name means. */
	call = NULL;
	for (index = 0U; index < sizeof(child_calls) / sizeof(child_calls[0]); index++) {
		same = strcmp(child_calls[index].name, name);
		if (same == 0) {
			call = &child_calls[index];
			break;
		}
	}

	/* An unknown name. */
	if (call == NULL)
		return CHILD_USAGE;

	/* The call, and its answer. */
	answer = __syscall6(call->number, call->args[0], call->args[1], call->args[2], call->args[3], call->args[4], call->args[5]);
	child_say("deny %s result=%ld\n", name, (long)answer);
	return 0;
}

/* A thread made and joined. */
static int
child_threads(void)
{
	pthread_t thread;
	void *result;
	int error;

	/* The thread. */
	error = pthread_create(&thread, NULL, child_thread, NULL);
	if (error != 0) {
		child_say("threads create error=%d\n", error);
		return 1;
	}

	/* Its end. */
	error = pthread_join(thread, &result);
	if (error != 0 || result != (void *)child_thread) {
		child_say("threads join error=%d\n", error);
		return 1;
	}

	/* Succeeded. */
	child_say("threads ok\n");
	return 0;
}

/* The thread: gives its own start back. */
static void *
child_thread(
	void *argument)
{
	(void)argument;
	return (void *)child_thread;
}
