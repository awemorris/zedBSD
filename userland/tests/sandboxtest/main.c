/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The kernel's sandbox test (ws168-p002; plan/ws168/preview-sandbox-design.md section 6):
 * starts the static child /usr/libexec/sandbox-child with sandbox_spawn
 * and checks what it may do.
 *
 *   sandboxtest [CHILD]
 *
 * 1. The request: too small (EINVAL), larger with unknown bytes set
 *    (E2BIG) or zero (taken), unknown flag and allow bits (EINVAL), too
 *    many files, a number out of range or used twice (EINVAL), a file not
 *    open (EBADF), a dynamic image (ENOEXEC), an image the caller may not
 *    execute (EACCES).
 * 2. The child's state: only the files handed over, at their numbers;
 *    the smaller limits, no more files than handed over, no core; the
 *    default signal actions.
 * 3. With SANDBOX_SPAWN_DENY_ERRNO, each call outside the set answers
 *    EPERM.
 * 4. The allowed calls work (and the terminal query says "not a
 *    terminal"); with SANDBOX_ALLOW_THREADS a thread is made.
 * 5. File, shared and executable memory are refused.
 * 6. Without DENY_ERRNO, a call outside the set ends the child with
 *    SIGKILL, and a child that keeps to the set runs (libc's start keeps
 *    to it).
 * Each line is "SANDBOX ..." on standard output; the last is
 * "SANDBOX PASS" (status 0) or "SANDBOX FAIL n of m" (status 1).
 */

#include "include/libc/sandbox.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

/* The child, and a copy of it that may not be executed. */
#define TEST_CHILD	"/usr/libexec/sandbox-child"
#define TEST_NO_EXEC	"/tmp/sandbox-child-noexec"

/* The longest output of a child the test reads. */
#define TEST_OUTPUT_MAX	4096U

/* The limits a request asks for: 64 MiB of memory, 5 s of CPU time, 1 MiB written. */
#define TEST_MEMORY	(64ULL * 1024U * 1024U)
#define TEST_CPU	5U
#define TEST_WRITE	(1024U * 1024U)

/* What a child left: its output and how it ended. */
struct test_run {
	char output[TEST_OUTPUT_MAX];
	size_t length;
	int status;
};

/* The checks run and failed. */
static unsigned test_checks;
static unsigned test_failures;

/* The child's image, open. */
static int test_image;

static void test_check(int passed, const char *what);
static void test_request(struct sandbox_spawn *request, char *const *argv, const struct sandbox_fd *fds, unsigned count);
static int test_spawn_error(struct sandbox_spawn *request);
static int test_run(uint32_t flags, uint64_t allow, char *const *argv, const char *input, struct test_run *run);
static int test_has(const struct test_run *run, const char *text);
static void test_requests(void);
static void test_state(void);
static void test_denied(void);
static void test_allowed(void);
static void test_memory(void);
static void test_kill(void);

/* The calls outside the set that the child tries, by its names. */
static const char *const test_denied_calls[] = {
	"open", "openat", "stat", "getcwd", "chdir", "fchdir", "socket", "socketpair", "pipe", "dup", "fcntl", "fork",
	"vfork", "execve", "sandbox_spawn", "ioctl", "sysctl", "kill", "setuid", "setrlimit", "recvmsg", "mount",
	"thread_create",
};

/*
 * Runs every part and reports the result.
 */
int
main(
	int argc,
	char **argv)
{
	const char *child;

	/* The child's image. */
	child = TEST_CHILD;
	if (argc == 2)
		child = argv[1];
	test_image = open(child, O_RDONLY);
	if (test_image < 0) {
		printf("SANDBOX FAIL open %s errno=%d\n", child, errno);
		return 1;
	}

	/* The parts. */
	test_requests();
	test_state();
	test_denied();
	test_allowed();
	test_memory();
	test_kill();
	close(test_image);

	/* A failure. */
	if (test_failures != 0U) {
		printf("SANDBOX FAIL %u of %u\n", test_failures, test_checks);
		return 1;
	}

	/* Succeeded: every check passed. */
	printf("SANDBOX PASS %u checks\n", test_checks);
	return 0;
}

/* Counts a check and says it. */
static void
test_check(
	int passed,
	const char *what)
{
	/* Counted, and the line. */
	test_checks++;
	if (passed) {
		printf("SANDBOX ok %s\n", what);
		return;
	}

	/* A failure. */
	test_failures++;
	printf("SANDBOX FAILED %s\n", what);
}

/* Fills a request of the first version for the child with an argument vector and files. */
static void
test_request(
	struct sandbox_spawn *request,
	char *const *argv,
	const struct sandbox_fd *fds,
	unsigned count)
{
	memset(request, 0, sizeof(*request));
	request->size = SANDBOX_SPAWN_SIZE_V1;
	request->image = test_image;
	request->fd_count = count;
	request->fds = (uint64_t)(uintptr_t)fds;
	request->argv = (uint64_t)(uintptr_t)argv;
}

/* Spawns a request that should fail and gives its errno (0 when a child started: it is waited for). */
static int
test_spawn_error(
	struct sandbox_spawn *request)
{
	pid_t child;
	int status;

	/* The spawn. */
	child = sandbox_spawn(request);
	if (child < 0)
		return errno;

	/* A child started after all: waited for. */
	(void)waitpid(child, &status, 0);

	/* Succeeded: no error. */
	return 0;
}

/*
 * Runs the child with the flags, the allow bits and an argument vector:
 * fd 0 a pipe with the input (none: closed at once), fd 1 a pipe the test
 * reads, the limits of TEST_*.  Returns 0 with its output and status, or
 * an errno value.
 */
static int
test_run(
	uint32_t flags,
	uint64_t allow,
	char *const *argv,
	const char *input,
	struct test_run *run)
{
	struct sandbox_spawn request;
	struct sandbox_fd fds[2];
	ssize_t got;
	pid_t child;
	int in[2];
	int out[2];
	int error;

	/* The pipes. */
	memset(run, 0, sizeof(*run));
	error = pipe(in);
	if (error != 0)
		return errno;
	error = pipe(out);
	if (error != 0)
		return errno;

	/* The request: the pipes' ends as fd 0 and 1, the limits. */
	fds[0].from = in[0];
	fds[0].to = 0;
	fds[1].from = out[1];
	fds[1].to = 1;
	test_request(&request, argv, fds, 2U);
	request.flags = flags;
	request.allow = allow;
	request.memory_max = TEST_MEMORY;
	request.cpu_seconds = TEST_CPU;
	request.write_max = TEST_WRITE;
	child = sandbox_spawn(&request);
	error = errno;
	close(in[0]);
	close(out[1]);
	if (child < 0) {
		close(in[1]);
		close(out[0]);
		return error;
	}

	/* The input, then the end of it. */
	if (input != NULL)
		(void)write(in[1], input, strlen(input));
	close(in[1]);

	/* The output to its end, and how the child ended. */
	for (;;) {
		got = read(out[0], run->output + run->length, sizeof(run->output) - 1U - run->length);
		if (got <= 0)
			break;
		run->length += (size_t)got;
		if (run->length >= sizeof(run->output) - 1U)
			break;
	}
	run->output[run->length] = '\0';
	close(out[0]);
	(void)waitpid(child, &run->status, 0);

	/* Succeeded: it ran. */
	return 0;
}

/* Tells whether a child's output has a text. */
static int
test_has(
	const struct test_run *run,
	const char *text)
{
	const char *found;

	/* Somewhere in the output. */
	found = strstr(run->output, text);
	if (found == NULL)
		return 0;

	/* Succeeded: it is there. */
	return 1;
}

/* 1. The request's checks. */
static void
test_requests(void)
{
	static char *const echo[] = { "sandbox-child", "echo", NULL };
	unsigned char larger[SANDBOX_SPAWN_SIZE_V1 + 8U];
	char block[4096];
	struct sandbox_spawn request;
	struct sandbox_fd fds[SANDBOX_FD_MAX + 1U];
	struct sandbox_fd pair[2];
	ssize_t got;
	off_t offset;
	unsigned index;
	int closed;
	int dynamic;
	int copy;
	int error;

	/* Too small. */
	test_request(&request, echo, NULL, 0U);
	request.size = 32U;
	error = test_spawn_error(&request);
	test_check(error == EINVAL, "request: a size below the first version is EINVAL");

	/* Larger, with an unknown byte set, then all zero. */
	test_request(&request, echo, NULL, 0U);
	request.size = sizeof(larger);
	memset(larger, 0, sizeof(larger));
	memcpy(larger, &request, sizeof(request));
	larger[SANDBOX_SPAWN_SIZE_V1 + 3U] = 1U;
	error = test_spawn_error((struct sandbox_spawn *)(void *)larger);
	test_check(error == E2BIG, "request: an unknown byte set is E2BIG");
	larger[SANDBOX_SPAWN_SIZE_V1 + 3U] = 0U;
	error = test_spawn_error((struct sandbox_spawn *)(void *)larger);
	test_check(error == 0, "request: a larger one with its unknown bytes zero is taken");

	/* Unknown flag and allow bits. */
	test_request(&request, echo, NULL, 0U);
	request.flags = UINT32_C(1) << 7;
	error = test_spawn_error(&request);
	test_check(error == EINVAL, "request: an unknown flag is EINVAL");
	test_request(&request, echo, NULL, 0U);
	request.allow = UINT64_C(1) << 40;
	error = test_spawn_error(&request);
	test_check(error == EINVAL, "request: an unknown allow bit is EINVAL");

	/* Too many files, a number out of range, one used twice. */
	for (index = 0U; index <= SANDBOX_FD_MAX; index++) {
		fds[index].from = 0;
		fds[index].to = (int32_t)index;
	}
	test_request(&request, echo, fds, SANDBOX_FD_MAX + 1U);
	error = test_spawn_error(&request);
	test_check(error == EINVAL, "request: more than SANDBOX_FD_MAX files is EINVAL");
	pair[0].from = 0;
	pair[0].to = (int32_t)SANDBOX_FD_MAX;
	test_request(&request, echo, pair, 1U);
	error = test_spawn_error(&request);
	test_check(error == EINVAL, "request: a number out of range is EINVAL");
	pair[0].to = 3;
	pair[1].from = 1;
	pair[1].to = 3;
	test_request(&request, echo, pair, 2U);
	error = test_spawn_error(&request);
	test_check(error == EINVAL, "request: a number used twice is EINVAL");

	/* A file not open. */
	closed = open("/dev/null", O_RDONLY);
	close(closed);
	pair[0].from = closed;
	pair[0].to = 0;
	test_request(&request, echo, pair, 1U);
	error = test_spawn_error(&request);
	test_check(error == EBADF, "request: a file not open is EBADF");

	/* A dynamic image (this program). */
	dynamic = open("/bin/sandboxtest", O_RDONLY);
	test_request(&request, echo, NULL, 0U);
	request.image = dynamic;
	error = test_spawn_error(&request);
	test_check(dynamic >= 0 && error == ENOEXEC, "request: a dynamic image is ENOEXEC");
	if (dynamic >= 0)
		close(dynamic);

	/* An image the caller may not execute: a copy of the child without the execute bits. */
	copy = open(TEST_NO_EXEC, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	offset = 0;
	while (copy >= 0) {
		got = pread(test_image, block, sizeof(block), offset);
		if (got <= 0)
			break;
		(void)write(copy, block, (size_t)got);
		offset += (off_t)got;
	}
	if (copy >= 0)
		close(copy);
	copy = open(TEST_NO_EXEC, O_RDONLY);
	test_request(&request, echo, NULL, 0U);
	request.image = copy;
	error = test_spawn_error(&request);
	test_check(copy >= 0 && error == EACCES, "request: an image that may not be executed is EACCES");
	if (copy >= 0)
		close(copy);
	(void)unlink(TEST_NO_EXEC);
}

/* 2. The child's state: files, limits, signals; and fd 0 to fd 1. */
static void
test_state(void)
{
	static char *const echo[] = { "sandbox-child", "echo", NULL };
	static char *const fds_mode[] = { "sandbox-child", "fds", NULL };
	static char *const limits[] = { "sandbox-child", "limits", NULL };
	static char *const signals[] = { "sandbox-child", "signals", NULL };
	struct sandbox_spawn request;
	struct sandbox_fd fds[3];
	struct test_run run;
	char line[128];
	ssize_t got;
	pid_t child;
	int out[2];
	int status;
	int error;

	/* fd 0 to fd 1. */
	error = test_run(0U, 0U, echo, "through the sandbox\n", &run);
	test_check(error == 0 && WIFEXITED(run.status) && WEXITSTATUS(run.status) == 0 && test_has(&run, "through the sandbox"),
	    "state: the child copies fd 0 to fd 1");

	/* Only the files handed over, at their numbers (here 0, 1 and 9). */
	error = pipe(out);
	if (error == 0) {
		fds[0].from = out[1];
		fds[0].to = 1;
		fds[1].from = out[0];
		fds[1].to = 0;
		fds[2].from = out[1];
		fds[2].to = 9;
		test_request(&request, fds_mode, fds, 3U);
		child = sandbox_spawn(&request);
		close(out[1]);
		got = 0;
		memset(line, 0, sizeof(line));
		if (child > 0) {
			got = read(out[0], line, sizeof(line) - 1U);
			(void)waitpid(child, &status, 0);
		}
		close(out[0]);
		test_check(got > 0 && strstr(line, "fds 0 1 9\n") != NULL, "state: only the files handed over, at their numbers");
	}

	/* The smaller limits, no more files than handed over, no core. */
	error = test_run(0U, 0U, limits, NULL, &run);
	test_check(error == 0 && test_has(&run, "limit as 67108864\n"), "state: the memory limit asked for");
	test_check(error == 0 && test_has(&run, "limit cpu 5\n"), "state: the CPU limit asked for");
	test_check(error == 0 && test_has(&run, "limit fsize 1048576\n"), "state: the written size asked for");
	test_check(error == 0 && test_has(&run, "limit nofile 16\n"), "state: no more files than SANDBOX_FD_MAX");
	test_check(error == 0 && test_has(&run, "limit core 0\n"), "state: no core file");

	/* The default signal actions. */
	error = test_run(0U, 0U, signals, NULL, &run);
	test_check(error == 0 && test_has(&run, "signals default=4 of 4"), "state: the default signal actions");
}

/* 3. With DENY_ERRNO, each call outside the set answers EPERM. */
static void
test_denied(void)
{
	char *argv[4];
	char expected[96];
	char what[96];
	struct test_run run;
	unsigned index;
	int error;

	/* Each call by name. */
	argv[0] = "sandbox-child";
	argv[1] = "deny";
	argv[3] = NULL;
	for (index = 0U; index < sizeof(test_denied_calls) / sizeof(test_denied_calls[0]); index++) {
		argv[2] = (char *)test_denied_calls[index];
		error = test_run(SANDBOX_SPAWN_DENY_ERRNO, 0U, argv, NULL, &run);
		snprintf(expected, sizeof(expected), "deny %s result=%d\n", test_denied_calls[index], -EPERM);
		snprintf(what, sizeof(what), "denied: %s is EPERM", test_denied_calls[index]);
		test_check(error == 0 && WIFEXITED(run.status) && test_has(&run, expected), what);
	}
}

/* 4. The allowed calls work, and threads with SANDBOX_ALLOW_THREADS. */
static void
test_allowed(void)
{
	static char *const allowed[] = { "sandbox-child", "allowed", NULL };
	static char *const threads[] = { "sandbox-child", "threads", NULL };
	struct test_run run;
	int error;

	/* Memory, the heap, the time, entropy, the PID, a sleep, the terminal query. */
	error = test_run(SANDBOX_SPAWN_DENY_ERRNO, 0U, allowed, NULL, &run);
	test_check(error == 0 && test_has(&run, "allowed ok"), "allowed: memory, the heap, the time, entropy, the PID, a sleep, not a terminal");

	/* A thread, with the allow bit. */
	error = test_run(SANDBOX_SPAWN_DENY_ERRNO, SANDBOX_ALLOW_THREADS, threads, NULL, &run);
	test_check(error == 0 && test_has(&run, "threads ok"), "allowed: a thread with SANDBOX_ALLOW_THREADS");
}

/* 5. File, shared and executable memory are refused. */
static void
test_memory(void)
{
	static char *const memory[] = { "sandbox-child", "memory", NULL };
	struct test_run run;
	int error;

	/* The four tries, each EPERM. */
	error = test_run(SANDBOX_SPAWN_DENY_ERRNO, 0U, memory, NULL, &run);
	test_check(error == 0 && test_has(&run, "memory refused=4 of 4"), "memory: file, shared, executable and mprotect to executable refused");
}

/* 6. Without DENY_ERRNO: a call outside the set is SIGKILL; a child that keeps to the set runs. */
static void
test_kill(void)
{
	static char *const deny_open[] = { "sandbox-child", "deny", "open", NULL };
	static char *const echo[] = { "sandbox-child", "echo", NULL };
	struct test_run run;
	int error;

	/* The open ends the child. */
	error = test_run(0U, 0U, deny_open, NULL, &run);
	test_check(error == 0 && WIFSIGNALED(run.status) && WTERMSIG(run.status) == SIGKILL && !test_has(&run, "deny open result"),
	    "kill: a call outside the set ends the child with SIGKILL");

	/* A child that keeps to the set, libc's start included, runs to its end. */
	error = test_run(0U, 0U, echo, "kept to the set\n", &run);
	test_check(error == 0 && WIFEXITED(run.status) && WEXITSTATUS(run.status) == 0 && test_has(&run, "kept to the set"),
	    "kill: a child that keeps to the set (libc's start too) runs");
}
