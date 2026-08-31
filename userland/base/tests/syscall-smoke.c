/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Exercises the zedBSD syscall smoke userland behavior.
 */

#include <fcntl.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#ifndef ZEDBSD_USER_PAGE_SIZE
#define ZEDBSD_USER_PAGE_SIZE 4096U
#endif
#define TEST_PAGE_SIZE ((size_t)ZEDBSD_USER_PAGE_SIZE)

#ifdef ZEDBSD_USER_ABI_LP64
_Static_assert(sizeof(void *) == 8, "LP64 pointer ABI");
_Static_assert(sizeof(long) == 8, "LP64 long ABI");
_Static_assert(sizeof(off_t) == 8, "LP64 off_t ABI");
_Static_assert(sizeof(time_t) == 8, "LP64 time_t ABI");
_Static_assert(LONG_MAX > INT_MAX, "LP64 LONG_MAX");
_Static_assert(ULONG_MAX > UINT_MAX, "LP64 ULONG_MAX");
#else
_Static_assert(sizeof(long) == 4, "ILP32 long ABI");
_Static_assert(LONG_MAX == INT_MAX, "ILP32 LONG_MAX");
#endif

static volatile unsigned int caught_signals;
static volatile unsigned int nested_signal_depth;
static volatile unsigned int nested_signal_max_depth;
static volatile unsigned int nested_signal_error;
static pid_t nested_signal_pid;
static volatile unsigned int siginfo_error;
static volatile unsigned int siginfo_count;
static pid_t siginfo_pid;
static uintptr_t expected_fault_address;
static volatile unsigned int sigchld_count;
static volatile int sigchld_code;
static volatile int sigchld_status;
static volatile pid_t sigchld_pid;

static int run_test(int argc, char **argv, char **envp);
static void report_stage(unsigned int stage);
static void report_checkpoint(const char *name);
static void catch_signal(int signo);
static void catch_nested_signal(int signo);
static void catch_siginfo(int signo, siginfo_t *info, void *opaque_context);
static void catch_fault_siginfo(int signo, siginfo_t *info, void *opaque_context);
static void catch_sigchld_info(int signo, siginfo_t *info, void *opaque_context);

/*
 * Runs the tests command.
 */
int
main(
	int argc,
	char **argv,
	char **envp)
{
	unsigned int value;
	int result;
	char failure[sizeof("POSIX_R1_FAIL:000\n")];

	/* Validates the command-line arguments. */
	if (argc <= 1)
		(void)write(1, "POSIX_R1_BEGIN\n", 15);
	result = run_test(argc, argv, envp);

	/* Validates the command-line arguments. */
	if (argc <= 1 && result != 37) {
		strcpy(failure, "POSIX_R1_FAIL:000\n");
				value = result < 0 ? 999U : (unsigned int)result;
		failure[14] = (char)('0' + value / 100U % 10U);
		failure[15] = (char)('0' + value / 10U % 10U);
		failure[16] = (char)('0' + value % 10U);
		(void)write(1, failure, sizeof(failure) - 1U);
	}

	/* Returns the computed result. */
	return result;
}

/* Supports the run test operation. */
static int
run_test(
	int argc,
	char **argv,
	char **envp)
{
	int function_result;
	char byte_local;
	int image_local;
	char value_local;
	struct sigaction action_local;
	sigset_t current_local, unblock_local;
	struct sigaction action_local1;
	sigset_t empty_local, current_local2;
	unsigned char *mapping_local;
	struct stat image_status_local;
	int image_local4;
	off_t past_eof_local;
	unsigned char *mapping_local5;
	volatile unsigned char value_local3;
	struct stat image_status_local7;
	struct sigaction action_local8;
	int image_local9;
	off_t past_eof_local10;
	unsigned char *mapping_local11;
	volatile unsigned char value_local6;
	struct stat status_local;
	int root_local;
	int image_local12;
	char byte_local13;
	int file_local, root_local14, directory_local;
	int system_fd;
	struct iovec vector[2];
	pid_t grandchild;
	struct sigaction nested;
	sigset_t blocked;
	sigset_t pending;
	sigset_t block;
	char result;
	pid_t member;
	char persisted;
	int read_only;
	unsigned char *read_only_shared;
	unsigned char *shared1;
	unsigned char *shared2;
	unsigned char *private_map;
	struct stat attributes;
	const char message[] = "BOOT_USER_SYSCALL_OK\n";
	const char child_message[] = "FORK_EXEC_PIPE_OK\n";
	unsigned char *allocation;
	char received[sizeof(child_message)];
	int descriptors[2], status;
	pid_t child;
	size_t offset;
	char *child_argv[3];
	struct timespec delay;
	struct timespec remaining;
	struct timespec times[2];

	(void)envp;

	/* Handles the selected command-line operation. */
	if (argc > 1 && strcmp(argv[1], "fork-child") == 0) {
		/* Computes the function result. */
		function_result = write(1, child_message, sizeof(child_message) - 1U) ==
			       (ssize_t)(sizeof(child_message) - 1U)
			   ? 23
			   : 24;

		/* Returns the computed result. */
		return function_result;
	}
	allocation = malloc(256U * 1024U);

	/* Handles the allocation availability. */
	if (allocation == NULL)
		return 2;
#ifdef ZEDBSD_USER_ABI_LP64

	/* Handles the uintptr t condition. */
	if ((uintptr_t)&message <= UINT32_MAX)
		return 201;
#endif

	/* Process each element required by the operation. */
	for (offset = 0; offset < 256U * 1024U; offset += 4096U)
		allocation[offset] = (unsigned char)(offset >> 12);

	/* Handles the allocation condition. */
	if (allocation[63U * 4096U] != 63U) {
		free(allocation);

		/* Returns the computed result. */
		return 3;
	}
	free(allocation);
	report_stage(1);
	report_checkpoint("pipe-fork-exec-begin");

	/* Handles a failed pipe operation. */
	if (pipe(descriptors) != 0)
		return 4;

	/* Handles a failed fcntl operation. */
	if (fcntl(descriptors[0], F_SETFD, FD_CLOEXEC) != 0 ||
	    fcntl(descriptors[0], F_GETFD) != FD_CLOEXEC)

		/* Returns the computed result. */
		return 5;
	child = fork();

	/* Checks the child process state. */
	if (child < 0)
		return 6;

	/* Checks the child process state. */
	if (child == 0) {
		child_argv[0] = "/init.elf";
		child_argv[1] = "fork-child";
		child_argv[2] = NULL;
		(void)close(descriptors[0]);

		/* Handles a failed dup2 operation. */
		if (dup2(descriptors[1], 1) != 1)
			_exit(25);
		(void)close(descriptors[1]);
		execve(child_argv[0], child_argv, envp);
		_exit(26);
	}
	report_checkpoint("pipe-fork-exec-parent-read");
	(void)close(descriptors[1]);

	/* Handles a failed read operation. */
	if (read(descriptors[0], received, sizeof(child_message) - 1U) !=
		(ssize_t)(sizeof(child_message) - 1U) ||
	    memcmp(received, child_message, sizeof(child_message) - 1U) != 0)

		/* Returns the computed result. */
		return 7;
	report_checkpoint("pipe-fork-exec-parent-wait");

	/* Handles a failed close operation. */
	if (close(descriptors[0]) != 0 || waitpid(child, &status, 0) != child ||
	    !WIFEXITED(status) || WEXITSTATUS(status) != 23)

		/* Returns the computed result. */
		return 8;
	report_checkpoint("pipe-fork-exec-done");

	/* Only the console character device has terminal capability. */
	if (!isatty(1))
		return 251;

	system_fd = open("/dev/system", O_RDONLY);

	/* Handles the system fd condition. */
	if (system_fd < 0)
		return 252;
	errno = 0;

	/* Handles the reported system error. */
	if (isatty(system_fd) != 0 || errno != ENOTTY ||
	    close(system_fd) != 0)

		/* Returns the computed result. */
		return 253;
	report_checkpoint("terminal-done");

	/* A bad status pointer must not consume the zombie. */
	child = fork();

	/* Checks the child process state. */
	if (child < 0)
		return 239;

	/* Checks the child process state. */
	if (child == 0)
		_exit(42);
	errno = 0;

	/* Handles the reported system error. */
	if (waitpid(child, (int *)(uintptr_t)1, 0) != -1 || errno != EFAULT ||
	    waitpid(child, &status, 0) != child || !WIFEXITED(status) ||
	    WEXITSTATUS(status) != 42)

		/* Returns the computed result. */
		return 240;
	report_checkpoint("bad-wait-done");

	/* Invalid read and readv output must not advance or consume input. */

	byte_local = 0;
	image_local = open("/init.elf", O_RDONLY);

	/* Handles the image local condition. */
	if (image_local < 0)
		return 241;
	errno = 0;

	/* Handles the reported system error. */
	if (read(image_local, (void *)(uintptr_t)1, 1) != -1 ||
	    errno != EFAULT || read(image_local, &byte_local, 1) != 1 ||
	    byte_local != 0x7f || lseek(image_local, 0, SEEK_SET) != 0)

		/* Returns the computed result. */
		return 242;
	vector[0].iov_base = &byte_local;
	vector[0].iov_len = 1;
	vector[1].iov_base = (void *)(uintptr_t)1;
	vector[1].iov_len = 1;
	errno = 0;

	/* Handles the reported system error. */
	if (readv(image_local, vector, 2) != -1 || errno != EFAULT ||
	    read(image_local, &byte_local, 1) != 1 || byte_local != 0x7f ||
	    close(image_local) != 0)

		/* Returns the computed result. */
		return 243;
	report_checkpoint("bad-read-done");

	/* Handles a failed pipe operation. */
	if (pipe(descriptors) != 0 || write(descriptors[1], "p", 1) != 1)
		return 244;
	errno = 0;

	/* Handles the reported system error. */
	if (read(descriptors[0], (void *)(uintptr_t)1, 1) != -1 ||
	    errno != EFAULT || read(descriptors[0], received, 1) != 1 ||
	    received[0] != 'p' || close(descriptors[0]) != 0 ||
	    close(descriptors[1]) != 0)

		/* Returns the computed result. */
		return 245;
	report_checkpoint("pipe-efault-done");

	/*
 * Orphans are reparented to init (PID 1), which eventually reaps them.
	 */
	if (pipe(descriptors) != 0)
		return 246;
	child = fork();

	/* Checks the child process state. */
	if (child < 0)
		return 247;

	/* Checks the child process state. */
	if (child == 0) {
				grandchild = fork();

		/* Handles the grandchild condition. */
		if (grandchild < 0)
			_exit(1);

		/* Handles the grandchild condition. */
		if (grandchild == 0) {
			delay.tv_sec = 0;
			delay.tv_nsec = 20000000;

			(void)close(descriptors[0]);
			(void)write(1, "SYSCALL_ORPHAN_SLEEP\n",
				    sizeof("SYSCALL_ORPHAN_SLEEP\n") - 1U);
			(void)nanosleep(&delay, NULL);
			(void)write(1, "SYSCALL_ORPHAN_WAKE\n",
				    sizeof("SYSCALL_ORPHAN_WAKE\n") - 1U);
			value_local = getppid() == 1 ? 'o' : 'x';
			(void)write(descriptors[1], &value_local, 1);
			_exit(value_local == 'o' ? 0 : 1);
		}
		_exit(0);
	}
	(void)close(descriptors[1]);
	report_checkpoint("orphan-parent-wait");

	/* Handles a failed waitpid operation. */
	if (waitpid(child, &status, 0) != child || !WIFEXITED(status) ||
	    WEXITSTATUS(status) != 0 ||
	    read(descriptors[0], received, 1) != 1 || received[0] != 'o' ||
	    close(descriptors[0]) != 0)

		/* Returns the computed result. */
		return 248;
	report_checkpoint("orphan-done");

	/* Exercise the child-exit/wait registration edge repeatedly. */
	for (offset = 0; offset < 64; offset++) {
		child = fork();

		/* Checks the child process state. */
		if (child < 0)
			return 249;

		/* Checks the child process state. */
		if (child == 0)
			_exit(0);

		/* Handles a failed waitpid operation. */
		if (waitpid(child, &status, 0) != child || !WIFEXITED(status))
			return 250;

		/* Checks the current offset. */
		if ((offset & 7U) == 7U)
			report_checkpoint("wait-race-progress");
	}
	report_checkpoint("wait-race-done");
	report_stage(2);

	/* Handles a failed signal operation. */
	if (signal(SIGUSR1, catch_signal) == SIG_ERR ||
	    signal(SIGUSR2, catch_signal) == SIG_ERR)

		/* Returns the computed result. */
		return 9;

	/* Handles a failed kill operation. */
	if (kill(getpid(), SIGUSR1) != 0 ||
	    (caught_signals & (1U << SIGUSR1)) == 0)

		/* Returns the computed result. */
		return 10;

	/*
 * A signal may interrupt a handler; kernel and HAL frames must be LIFO.
	 */

	memset(&nested, 0, sizeof(nested));
	nested.sa_handler = (uint64_t)(uintptr_t)catch_nested_signal;
	nested.sa_flags = SA_NODEFER;
	nested_signal_pid = getpid();
	nested_signal_depth = 0;
	nested_signal_max_depth = 0;
	nested_signal_error = 0;

	/* Handles an operation failure. */
	if (sigaction(SIGUSR1, &nested, NULL) != 0 ||
	    kill(nested_signal_pid, SIGUSR1) != 0 ||
	    nested_signal_depth != 0 || nested_signal_max_depth != 8U ||
	    nested_signal_error != 0 ||
	    signal(SIGUSR1, catch_signal) == SIG_ERR)

		/* Returns the computed result. */
		return 258;

	/*
 * SA_SIGINFO carries a fixed-width record and an editable signal mask.
	 */

	unblock_local = 1U << (SIGUSR2 - 1);

	memset(&action_local, 0, sizeof(action_local));
	action_local.sa_handler = (uint64_t)(uintptr_t)catch_siginfo;
	action_local.sa_flags = SA_SIGINFO;
	siginfo_pid = getpid();
	siginfo_error = 0;
	siginfo_count = 0;

	/* Handles an operation failure. */
	if (sigaction(SIGUSR1, &action_local, NULL) != 0 ||
	    kill(siginfo_pid, SIGUSR1) != 0 || siginfo_count != 1U ||
	    siginfo_error != 0 ||
	    sigprocmask(SIG_SETMASK, NULL, &current_local) != 0 ||
	    (current_local & unblock_local) == 0 ||
	    sigprocmask(SIG_UNBLOCK, &unblock_local, NULL) != 0 ||
	    signal(SIGUSR1, catch_signal) == SIG_ERR)

		/* Returns the computed result. */
		return 260;

	/*
 * SIGCHLD identifies the child and reports its unencoded exit status.
	 */

	memset(&action_local1, 0, sizeof(action_local1));
	action_local1.sa_handler = (uint64_t)(uintptr_t)catch_sigchld_info;
	action_local1.sa_flags = SA_SIGINFO | SA_RESTART;
	sigchld_count = 0;
	sigchld_pid = -1;
	sigchld_code = 0;
	sigchld_status = -1;

	/* Handles a failed sigaction operation. */
	if (sigaction(SIGCHLD, &action_local1, NULL) != 0)
		return 262;
	child = fork();

	/* Checks the child process state. */
	if (child < 0)
		return 262;

	/* Checks the child process state. */
	if (child == 0)
		_exit(47);

	/* Handles a failed waitpid operation. */
	if (waitpid(child, &status, 0) != child || !WIFEXITED(status) ||
	    WEXITSTATUS(status) != 47)

		/* Returns the computed result. */
		return 263;

	/* Handles the sigchld count condition. */
	if (sigchld_count != 1U)
		return 264;

	/* Handles the sigchld pid condition. */
	if (sigchld_pid != child)
		return 265;

	/* Handles the sigchld code condition. */
	if (sigchld_code != CLD_EXITED)
		return 266;

	/* Handles the sigchld status condition. */
	if (sigchld_status != 47)
		return 267;
	memset(&action_local1, 0, sizeof(action_local1));
	action_local1.sa_handler = SIG_DFL;

	/* Handles a failed sigaction operation. */
	if (sigaction(SIGCHLD, &action_local1, NULL) != 0)
		return 262;
	report_stage(3);

	blocked = 1U << (SIGUSR2 - 1);
	pending = 0;

	/* Handles a failed sigprocmask operation. */
	if (sigprocmask(SIG_BLOCK, &blocked, NULL) != 0 ||
	    kill(getpid(), SIGUSR2) != 0 || sigpending(&pending) != 0 ||
	    (pending & blocked) == 0 ||
	    (caught_signals & (1U << SIGUSR2)) != 0)

		/* Returns the computed result. */
		return 11;

	/* Handles a failed sigprocmask operation. */
	if (sigprocmask(SIG_UNBLOCK, &blocked, NULL) != 0 ||
	    (caught_signals & (1U << SIGUSR2)) == 0)

		/* Returns the computed result. */
		return 12;
	report_stage(4);
	caught_signals &= ~(1U << SIGUSR1);
	child = fork();

	/* Checks the child process state. */
	if (child < 0)
		return 13;

	/* Checks the child process state. */
	if (child == 0) {
		delay.tv_sec = 0;
		delay.tv_nsec = 20000000;
		(void)write(1, "POSIX_R1_CHILD_SLEEP\n", 21);
		(void)nanosleep(&delay, NULL);
		(void)write(1, "POSIX_R1_CHILD_WAKE\n", 20);
		_exit(kill(getppid(), SIGUSR1) == 0 ? 0 : 1);
	}
		delay.tv_sec = 1;
		delay.tv_nsec = 0;
		remaining.tv_sec = 0;
		remaining.tv_nsec = 0;
	(void)write(1, "POSIX_R1_PARENT_SLEEP\n", 22);
	errno = 0;

	/* Handles the reported system error. */
	if (nanosleep(&delay, &remaining) != -1 || errno != EINTR ||
	    remaining.tv_sec < 0 ||
	    (caught_signals & (1U << SIGUSR1)) == 0)

		/* Returns the computed result. */
		return 14;

	/* Handles a failed waitpid operation. */
	if (waitpid(child, &status, 0) != child || !WIFEXITED(status) ||
	    WEXITSTATUS(status) != 0)

		/* Returns the computed result. */
		return 15;
	report_stage(5);

	block = 1U << (SIGUSR1 - 1);
	empty_local = 0;
	current_local2 = 0;
	caught_signals &= ~(1U << SIGUSR1);

	/* Handles a failed sigprocmask operation. */
	if (sigprocmask(SIG_BLOCK, &block, NULL) != 0)
		return 15;
	child = fork();

	/* Checks the child process state. */
	if (child < 0)
		return 15;

	/* Checks the child process state. */
	if (child == 0)
		_exit(kill(getppid(), SIGUSR1) == 0 ? 0 : 1);
	errno = 0;

	/* Handles the reported system error. */
	if (sigsuspend(&empty_local) != -1 || errno != EINTR ||
	    (caught_signals & (1U << SIGUSR1)) == 0 ||
	    sigprocmask(SIG_SETMASK, NULL, &current_local2) != 0 ||
	    (current_local2 & block) == 0 ||
	    sigprocmask(SIG_UNBLOCK, &block, NULL) != 0 ||
	    waitpid(child, &status, 0) != child || !WIFEXITED(status) ||
	    WEXITSTATUS(status) != 0)

		/* Returns the computed result. */
		return 15;
	report_stage(6);
	child = fork();

	/* Checks the child process state. */
	if (child < 0)
		return 15;

	/* Checks the child process state. */
	if (child == 0) {
		(void)kill(getpid(), SIGSTOP);
		_exit(0);
	}

	/* Handles a failed waitpid operation. */
	if (waitpid(child, &status, WUNTRACED) != child ||
	    !WIFSTOPPED(status) || WSTOPSIG(status) != SIGSTOP ||
	    kill(child, SIGCONT) != 0 ||
	    waitpid(child, &status, WCONTINUED) != child ||
	    !WIFCONTINUED(status) || waitpid(child, &status, 0) != child ||
	    !WIFEXITED(status) || WEXITSTATUS(status) != 0)

		/* Returns the computed result. */
		return 15;

	/*
 * An orphaned stopped process group receives SIGHUP followed by
	 * SIGCONT. */
	if (pipe(descriptors) != 0)
		return 259;
	child = fork();

	/* Checks the child process state. */
	if (child < 0)
		return 259;

	/* Checks the child process state. */
	if (child == 0) {
				member = fork();

		/* Handles the member condition. */
		if (member < 0)
			_exit(1);

		/* Handles the member condition. */
		if (member == 0) {

			(void)close(descriptors[0]);
			caught_signals = 0;

			/* Handles a failed setpgid operation. */
			if (setpgid(0, 0) != 0 ||
			    signal(SIGHUP, catch_signal) == SIG_ERR)
				_exit(2);
			(void)kill(getpid(), SIGSTOP);
			result =
			    (caught_signals & (1U << SIGHUP)) != 0 ? 'h' : 'x';
			(void)write(descriptors[1], &result, 1);
			_exit(result == 'h' ? 0 : 3);
		}
		(void)close(descriptors[0]);
		(void)close(descriptors[1]);

		/* Handles a failed waitpid operation. */
		if (waitpid(member, &status, WUNTRACED) != member ||
		    !WIFSTOPPED(status))
			_exit(4);

		/*
 * Exiting reparents member and makes its process group
		 * orphaned. */
		_exit(0);
	}
	(void)close(descriptors[1]);

	/* Handles a failed waitpid operation. */
	if (waitpid(child, &status, 0) != child || !WIFEXITED(status) ||
	    WEXITSTATUS(status) != 0 ||
	    read(descriptors[0], received, 1) != 1 || received[0] != 'h' ||
	    close(descriptors[0]) != 0)

		/* Returns the computed result. */
		return 259;
	report_stage(7);

	mapping_local = mmap(NULL, 3U * TEST_PAGE_SIZE, PROT_READ | PROT_WRITE,
		 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

	/* Handles an operation failure. */
	if (mapping_local == MAP_FAILED)
		return 16;
#ifdef ZEDBSD_USER_ABI_LP64

	/* Handles the uintptr t condition. */
	if ((uintptr_t)mapping_local <= UINT32_MAX)
		return 202;
#endif
	mapping_local[0] = 1;
	mapping_local[TEST_PAGE_SIZE] = 2;
	mapping_local[2U * TEST_PAGE_SIZE] = 3;

	/* Handles a failed mprotect operation. */
	if (mprotect(mapping_local + TEST_PAGE_SIZE, TEST_PAGE_SIZE,
		     PROT_READ) != 0 ||
	    msync(mapping_local, TEST_PAGE_SIZE, MS_SYNC) != 0 ||
	    munmap(mapping_local + TEST_PAGE_SIZE, TEST_PAGE_SIZE) != 0 ||
	    munmap(mapping_local, TEST_PAGE_SIZE) != 0 ||
	    munmap(mapping_local + 2U * TEST_PAGE_SIZE, TEST_PAGE_SIZE) != 0)

		/* Returns the computed result. */
		return 17;

	/* Normal file mappings fault with SIGBUS on a page wholly past EOF. */

	image_local4 = open("/init.elf", O_RDONLY);

	/* Handles a failed fstat operation. */
	if (image_local4 < 0 || fstat(image_local4, &image_status_local) != 0)
		return 254;
	past_eof_local = (image_status_local.st_size + TEST_PAGE_SIZE - 1) &
		   ~(off_t)(TEST_PAGE_SIZE - 1);
	mapping_local5 = mmap(NULL, TEST_PAGE_SIZE, PROT_READ, MAP_PRIVATE,
		       image_local4, past_eof_local);

	/* Handles an operation failure. */
	if (mapping_local5 == MAP_FAILED || close(image_local4) != 0)
		return 255;
	child = fork();

	/* Checks the child process state. */
	if (child < 0)
		return 256;

	/* Checks the child process state. */
	if (child == 0) {
					value_local3 = mapping_local5[0];
		(void)value_local3;
		_exit(1);
	}

	/* Handles a failed waitpid operation. */
	if (waitpid(child, &status, 0) != child ||
	    !WIFSIGNALED(status) || WTERMSIG(status) != SIGBUS ||
	    munmap(mapping_local5, TEST_PAGE_SIZE) != 0)

		/* Returns the computed result. */
		return 257;

	/* A synchronous VM fault reports BUS_ADRERR and the exact address. */

	image_local9 = open("/init.elf", O_RDONLY);

	/* Handles a failed fstat operation. */
	if (image_local9 < 0 || fstat(image_local9, &image_status_local7) != 0)
		return 261;
	past_eof_local10 = (image_status_local7.st_size + TEST_PAGE_SIZE - 1) &
		   ~(off_t)(TEST_PAGE_SIZE - 1);
	mapping_local11 = mmap(NULL, TEST_PAGE_SIZE, PROT_READ, MAP_PRIVATE,
		       image_local9, past_eof_local10);

	/* Handles an operation failure. */
	if (mapping_local11 == MAP_FAILED || close(image_local9) != 0)
		return 261;
	child = fork();

	/* Checks the child process state. */
	if (child < 0)
		return 261;

	/* Checks the child process state. */
	if (child == 0) {

		memset(&action_local8, 0, sizeof(action_local8));
		action_local8.sa_handler =
		    (uint64_t)(uintptr_t)catch_fault_siginfo;
		action_local8.sa_flags = SA_SIGINFO;
		expected_fault_address = (uintptr_t)mapping_local11;

		/* Handles a failed sigaction operation. */
		if (sigaction(SIGBUS, &action_local8, NULL) != 0)
			_exit(2);
		value_local6 = mapping_local11[0];
		(void)value_local6;
		_exit(3);
	}

	/* Handles a failed waitpid operation. */
	if (waitpid(child, &status, 0) != child || !WIFEXITED(status) ||
	    WEXITSTATUS(status) != 0 ||
	    munmap(mapping_local11, TEST_PAGE_SIZE) != 0)

		/* Returns the computed result. */
		return 261;
	report_stage(8);

	root_local = open("/", O_RDONLY | O_DIRECTORY);

	/* Handles the root local condition. */
	if (root_local < 0)
		return 18;
	image_local12 = openat(root_local, "init.elf", O_RDONLY);

	/* Handles a failed fstatat operation. */
	if (image_local12 < 0 || fstatat(root_local, "init.elf", &status_local, 0) != 0 ||
	    !S_ISREG(status_local.st_mode) || status_local.st_size <= 0 ||
	    status_local.st_blksize != 512 || close(image_local12) != 0 ||
	    close(root_local) != 0)

		/* Returns the computed result. */
		return 19;
	report_stage(9);

	byte_local13 = 0;
	root_local14 = open("/", O_RDONLY | O_DIRECTORY);

	/* Handles a failed mkdirat operation. */
	if (root_local14 < 0 || mkdirat(root_local14, "r1", 0777) != 0)
		return 20;
	directory_local = openat(root_local14, "r1", O_RDONLY | O_DIRECTORY);
	file_local = openat(directory_local, "a", O_CREAT | O_RDWR | O_EXCL, 0666);

	/* Handles a failed write operation. */
	if (file_local < 0 || write(file_local, "z", 1) != 1)
		return 21;

	persisted = 0;
	read_only = openat(directory_local, "a", O_RDONLY);

	/* Handles the read only condition. */
	if (read_only < 0)
		return 21;
	read_only_shared = mmap(NULL, TEST_PAGE_SIZE, PROT_READ,
				MAP_SHARED, read_only, 0);
	shared1 =
	    mmap(NULL, TEST_PAGE_SIZE, PROT_READ | PROT_WRITE,
		 MAP_SHARED, file_local, 0);
	shared2 =
	    mmap(NULL, TEST_PAGE_SIZE, PROT_READ | PROT_WRITE,
		 MAP_SHARED, file_local, 0);

	/* Handles an operation failure. */
	if (close(read_only) != 0 ||
	    read_only_shared == MAP_FAILED ||
	    shared1 == MAP_FAILED || shared2 == MAP_FAILED)

		/* Returns the computed result. */
		return 21;
	shared1[0] = 'm';

	/* Handles the shared2 condition. */
	if (shared2[0] != 'm' || read_only_shared[0] != 'm')
		return 21;
	child = fork();

	/* Checks the child process state. */
	if (child < 0)
		return 21;

	/* Checks the child process state. */
	if (child == 0) {
		shared2[0] = 'c';
		_exit(msync(shared2, TEST_PAGE_SIZE, MS_SYNC) ==
			      0
			  ? 0
			  : 1);
	}

	/* Handles a failed waitpid operation. */
	if (waitpid(child, &status, 0) != child ||
	    !WIFEXITED(status) || WEXITSTATUS(status) != 0)

		/* Returns the computed result. */
		return 211;

	/* Handles the shared1 condition. */
	if (shared1[0] != 'c')
		return 215;

	/* Handles a failed msync operation. */
	if (msync(shared1, TEST_PAGE_SIZE, MS_SYNC) != 0)
		return 216;

	/* Handles a failed pread operation. */
	if (pread(file_local, &persisted, 1, 0) != 1)
		return 217;

	/* Handles the persisted condition. */
	if (persisted != 'c')
		return 218;

	/* Handles a failed munmap operation. */
	if (munmap(read_only_shared, TEST_PAGE_SIZE) != 0 ||
	    munmap(shared1, TEST_PAGE_SIZE) != 0 ||
	    munmap(shared2, TEST_PAGE_SIZE) != 0)

		/* Returns the computed result. */
		return 219;
	private_map =
	    mmap(NULL, TEST_PAGE_SIZE, PROT_READ | PROT_WRITE,
		 MAP_PRIVATE, file_local, 0);

	/* Handles an operation failure. */
	if (private_map == MAP_FAILED)
		return 212;
	private_map[0] = 'p';
	child = fork();

	/* Checks the child process state. */
	if (child < 0)
		return 21;

	/* Checks the child process state. */
	if (child == 0) {
		private_map[0] = 'q';
		_exit(private_map[0] == 'q' ? 0 : 1);
	}

	/* Handles a failed waitpid operation. */
	if (waitpid(child, &status, 0) != child ||
	    !WIFEXITED(status) || WEXITSTATUS(status) != 0 ||
	    private_map[0] != 'p')

		/* Returns the computed result. */
		return 213;
	persisted = 0;

	/* Handles a failed msync operation. */
	if (msync(private_map, TEST_PAGE_SIZE, MS_SYNC) != 0 ||
	    pread(file_local, &persisted, 1, 0) != 1 ||
	    persisted != 'c' ||
	    munmap(private_map, TEST_PAGE_SIZE) != 0)

		/* Returns the computed result. */
		return 214;
#ifdef ZEDBSD_USER_ABI_LP64
			times[0].tv_sec = (time_t)2208988800LL;
			times[1].tv_sec = (time_t)2208988802LL;
#else
			times[0].tv_sec = 1767225600;
			times[1].tv_sec = 1767225602;
#endif
			times[0].tv_nsec = 123456789;
			times[1].tv_nsec = 987654321;

	/* Handles a failed futimens operation. */
	if (futimens(file_local, times) != 0)
		return 231;

	/* Handles a failed fstat operation. */
	if (fstat(file_local, &attributes) != 0)
		return 232;

	/* Handles the attributes condition. */
	if (attributes.st_atime != times[0].tv_sec ||
	    attributes.st_mtime != times[1].tv_sec)

		/* Returns the computed result. */
		return 233;

	/* Handles a failed fchmod operation. */
	if (fchmod(file_local, 0555) != 0)
		return 234;

	/* Handles a failed fstat operation. */
	if (fstat(file_local, &attributes) != 0)
		return 235;

	/* Handles the attributes condition. */
	if ((attributes.st_mode & 0777U) != 0555U)
		return 236;

	/* Handles a failed fchown operation. */
	if (fchown(file_local, getuid(), getgid()) != 0)
		return 237;
	errno = 0;

	/* Handles the reported system error. */
	if (fchmod(file_local, 0644) != -1 || errno != EOPNOTSUPP)
		return 238;
#ifdef ZEDBSD_USER_ABI_LP64

	/* Handles a failed lseek operation. */
	if (lseek(file_local, (off_t)UINT32_MAX + 1, SEEK_SET) !=
	    (off_t)UINT32_MAX + 1)

		/* Returns the computed result. */
		return 229;
	errno = 0;

	/* Handles the reported system error. */
	if (write(file_local, "x", 1) != -1 || errno != EFBIG ||
	    lseek(file_local, 0, SEEK_SET) != 0)

		/* Returns the computed result. */
		return 230;
#endif

	/* Handles a failed renameat operation. */
	if (renameat(directory_local, "a", directory_local, "b") != 0 ||
	    close(file_local) != 0)

		/* Returns the computed result. */
		return 21;
	errno = 0;

	/* Handles the reported system error. */
	if (linkat(directory_local, "b", directory_local, "hard", 0) != -1 ||
	    errno != EOPNOTSUPP)

		/* Returns the computed result. */
		return 21;
	errno = 0;

	/* Handles the reported system error. */
	if (symlinkat("b", directory_local, "soft") != -1 ||
	    errno != EOPNOTSUPP)

		/* Returns the computed result. */
		return 21;
	file_local = openat(directory_local, "b", O_RDWR);

	/* Handles the file local condition. */
	if (file_local < 0)
		return 221;

	/* Handles a failed unlinkat operation. */
	if (unlinkat(directory_local, "b", 0) != 0)
		return 222;

	/* Handles a failed lseek operation. */
	if (lseek(file_local, 0, SEEK_SET) != 0)
		return 226;

	/* Handles a failed read operation. */
	if (read(file_local, &byte_local13, 1) != 1)
		return 227;

	/* Handles the byte local13 condition. */
	if (byte_local13 != 'c')
		return 228;

	/* Handles a failed close operation. */
	if (close(file_local) != 0 || close(directory_local) != 0)
		return 224;

	/* Handles a failed unlinkat operation. */
	if (unlinkat(root_local14, "r1", AT_REMOVEDIR) != 0 || close(root_local14) != 0)
		return 225;
	report_stage(10);

	/* Computes the function result. */
	function_result = write(1, message, strlen(message)) ==
		       (ssize_t)(sizeof(message) - 1U)
		   ? 37
		   : 1;

	/* Returns the computed result. */
	return function_result;
}

/* Supports the report stage operation. */
static void
report_stage(
	unsigned int stage)
{
	char message[] = "POSIX_R1_STAGE:00\n";

	message[15] = (char)('0' + stage / 10U % 10U);
	message[16] = (char)('0' + stage % 10U);
	(void)write(1, message, sizeof(message) - 1U);
}

/* Supports the report checkpoint operation. */
static void
report_checkpoint(
	const char *name)
{
	static const char prefix[] = "POSIX_R1_CHECK:";

	(void)write(1, prefix, sizeof(prefix) - 1U);
	(void)write(1, name, strlen(name));
	(void)write(1, "\n", 1);
}

/* Supports the catch signal operation. */
static void
catch_signal(
	int signo)
{
	caught_signals |= 1U << (unsigned int)signo;
}

/* Supports the catch nested signal operation. */
static void
catch_nested_signal(
	int signo)
{
	unsigned int depth;

	depth = ++nested_signal_depth;

	/* Handles the signo condition. */
	if (signo != SIGUSR1 || depth > 8U)
		nested_signal_error = 1;

	/* Handles the depth condition. */
	if (depth > nested_signal_max_depth)
		nested_signal_max_depth = depth;

	/* Handles a failed kill operation. */
	if (depth < 8U && kill(nested_signal_pid, SIGUSR1) != 0)
		nested_signal_error = 1;

	/* Handles the nested signal depth condition. */
	if (nested_signal_depth != depth)
		nested_signal_error = 1;
	nested_signal_depth--;
}

/* Supports the catch siginfo operation. */
static void
catch_siginfo(
	int signo,
	siginfo_t *info,
	void *opaque_context)
{
	ucontext_t *context;

	context = opaque_context;

	/* Handles the info availability. */
	if (signo != SIGUSR1 || info == NULL || context == NULL ||
	    info->si_signo != SIGUSR1 || info->si_code != SI_USER ||
	    info->si_pid != siginfo_pid || context->uc_mcontext.mc_pc == 0 ||
	    context->uc_mcontext.mc_sp == 0)
		siginfo_error = 1;
	else
		context->uc_sigmask |= 1U << (SIGUSR2 - 1);
	siginfo_count++;
}

/* Supports the catch fault siginfo operation. */
static void
catch_fault_siginfo(
	int signo,
	siginfo_t *info,
	void *opaque_context)
{
	ucontext_t *context;
	int valid = signo == SIGBUS && info != NULL && context != NULL &&
		    info->si_signo == SIGBUS && info->si_code == BUS_ADRERR &&
		    info->si_addr == expected_fault_address &&
		    context->uc_mcontext.mc_pc != 0 &&
		    context->uc_mcontext.mc_sp != 0;

	context = opaque_context;

	_exit(valid ? 0 : 1);
}

/* Supports the catch sigchld info operation. */
static void
catch_sigchld_info(
	int signo,
	siginfo_t *info,
	void *opaque_context)
{
	(void)opaque_context;

	/* Handles the info availability. */
	if (signo == SIGCHLD && info != NULL) {
		sigchld_code = info->si_code;
		sigchld_status = info->si_status;
		sigchld_pid = info->si_pid;
	}
	sigchld_count++;
}
