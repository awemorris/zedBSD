/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static int child_checkpoint_descriptor = -1;
static pid_t shell_process;
static unsigned gate_release_calls;
static unsigned tcsetpgrp_calls;
static unsigned waitpid_calls;

extern pid_t __real_fork(void);
extern int __real_kill(pid_t, int);
extern int __real_posix_spawn(pid_t *, const char *,
			      const posix_spawn_file_actions_t *,
			      const posix_spawnattr_t *, char *const[],
			      char *const[]);
extern ssize_t __real_read(int, void *, size_t);
extern int __real_tcsetpgrp(int, pid_t);
extern pid_t __real_waitpid(pid_t, int *, int);
extern ssize_t __real_write(int, const void *, size_t);

static void __attribute__((constructor))
initialize_hooks(void)
{
	shell_process = getpid();
}

static unsigned
failure_call(const char *name)
{
	const char *text = getenv(name);
	char *end;
	unsigned long value;

	if (text == NULL || text[0] == '\0')
		return 0;
	errno = 0;
	value = strtoul(text, &end, 10);
	if (errno != 0 || *end != '\0' || value > 0xffffffffUL)
		return 0;
	return (unsigned)value;
}

static void
record_event(const char *format, ...)
{
	const char *path = getenv("P014_EVENT_LOG");
	char buffer[256];
	va_list arguments;
	int descriptor, length;

	if (path == NULL || path[0] == '\0')
		return;
	va_start(arguments, format);
	length = vsnprintf(buffer, sizeof(buffer), format, arguments);
	va_end(arguments);
	if (length < 0)
		return;
	if ((size_t)length >= sizeof(buffer))
		length = (int)sizeof(buffer) - 1;
	descriptor = open(path, O_WRONLY | O_CREAT | O_APPEND, 0600);
	if (descriptor < 0)
		return;
	(void)__real_write(descriptor, buffer, (size_t)length);
	(void)close(descriptor);
}

static void
child_checkpoint(char kind)
{
	if (child_checkpoint_descriptor < 0)
		return;
	(void)__real_write(child_checkpoint_descriptor, &kind, 1);
	(void)close(child_checkpoint_descriptor);
	child_checkpoint_descriptor = -1;
}

pid_t
__wrap_fork(void)
{
	int checkpoint[2];
	pid_t child;
	char kind = 'N';
	struct pollfd poll_descriptor;
	ssize_t count = 0;

	if (pipe2(checkpoint, O_CLOEXEC) != 0) {
		record_event(
		    "FORK_CHECKPOINT parent=%ld child=-1 kind=pipe-error\n",
		    (long)getpid());
		return __real_fork();
	}
	child = __real_fork();
	if (child == 0) {
		(void)close(checkpoint[0]);
		child_checkpoint_descriptor = checkpoint[1];
		return 0;
	}
	(void)close(checkpoint[1]);
	if (child < 0) {
		(void)close(checkpoint[0]);
		return child;
	}
	poll_descriptor.fd = checkpoint[0];
	poll_descriptor.events = POLLIN | POLLHUP;
	poll_descriptor.revents = 0;
	if (poll(&poll_descriptor, 1, 5000) > 0)
		count = __real_read(checkpoint[0], &kind, 1);
	(void)close(checkpoint[0]);
	record_event("FORK_CHECKPOINT parent=%ld child=%ld kind=%s\n",
		     (long)getpid(), (long)child,
		     count == 1 && kind == 'G'	 ? "gate"
		     : count == 1 && kind == 'E' ? "exec"
						 : "timeout");
	return child;
}

ssize_t
__wrap_read(int descriptor, void *buffer, size_t length)
{
	struct stat status;

	if (fstat(descriptor, &status) == 0 && S_ISFIFO(status.st_mode))
		child_checkpoint('G');
	return __real_read(descriptor, buffer, length);
}

int
__wrap_posix_spawn(pid_t *pid, const char *path,
		   const posix_spawn_file_actions_t *actions,
		   const posix_spawnattr_t *attributes, char *const argv[],
		   char *const environment[])
{
	int result, saved_errno;

	child_checkpoint('E');
	result = __real_posix_spawn(pid, path, actions, attributes, argv,
				    environment);
	saved_errno = errno;
	record_event("SPAWN caller=%ld child=%ld result=%d\n", (long)getpid(),
		     result == 0 ? (long)*pid : -1L, result);
	errno = saved_errno;
	return result;
}

int
__wrap_tcsetpgrp(int descriptor, pid_t group)
{
	int result;
	int injected;

	tcsetpgrp_calls++;
	injected = getpid() == shell_process &&
		   tcsetpgrp_calls == failure_call("P014_FAIL_TCSETPGRP_AT");
	if (injected) {
		errno = EIO;
		result = -1;
	} else {
		result = __real_tcsetpgrp(descriptor, group);
	}
	int saved_errno = errno;

	record_event("TCSET caller=%ld target=%ld call=%u result=%d errno=%d "
		     "injected=%d\n",
		     (long)getpid(), (long)group, tcsetpgrp_calls, result,
		     result < 0 ? saved_errno : 0, injected);
	errno = saved_errno;
	return result;
}

int
__wrap_kill(pid_t target, int signal_number)
{
	int result = __real_kill(target, signal_number);
	int saved_errno = errno;

	if (signal_number == SIGCONT)
		record_event(
		    "KILL_CONT caller=%ld target=%ld result=%d errno=%d\n",
		    (long)getpid(), (long)target, result,
		    result < 0 ? saved_errno : 0);
	errno = saved_errno;
	return result;
}

ssize_t
__wrap_write(int descriptor, const void *buffer, size_t length)
{
	const unsigned char *bytes = buffer;
	ssize_t result;
	int saved_errno;
	size_t index;
	int release = descriptor > STDERR_FILENO && length == 2;
	int injected = 0;

	for (index = 0; release && index < length; index++)
		if (bytes[index] != (unsigned char)'x')
			release = 0;
	if (release)
		gate_release_calls++;
	if (release &&
	    gate_release_calls == failure_call("P014_FAIL_GATE_WRITE_AT")) {
		injected = 1;
		errno = EIO;
		result = -1;
	} else {
		result = __real_write(descriptor, buffer, length);
	}
	saved_errno = errno;
	if (release)
		record_event(
		    "GATE_RELEASE caller=%ld fd=%d count=%zu result=%ld "
		    "injected=%d\n",
		    (long)getpid(), descriptor, length, (long)result, injected);
	errno = saved_errno;
	return result;
}

pid_t
__wrap_waitpid(pid_t pid, int *status, int options)
{
	pid_t result;
	int saved_errno;

	if (getpid() == shell_process) {
		waitpid_calls++;
		if (waitpid_calls == failure_call("P014_FAIL_WAITPID_AT")) {
			errno = ECHILD;
			record_event("WAITPID caller=%ld target=%ld call=%u "
				     "result=-1 errno=%d injected=1\n",
				     (long)getpid(), (long)pid, waitpid_calls,
				     ECHILD);
			return -1;
		}
	}
	result = __real_waitpid(pid, status, options);
	saved_errno = errno;
	if (getpid() == shell_process)
		record_event("WAITPID caller=%ld target=%ld call=%u result=%ld "
			     "errno=%d injected=0\n",
			     (long)getpid(), (long)pid, waitpid_calls,
			     (long)result, result < 0 ? saved_errno : 0);
	errno = saved_errno;
	return result;
}
