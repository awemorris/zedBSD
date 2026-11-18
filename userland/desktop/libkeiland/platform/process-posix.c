/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The processes' side of the system on zedBSD, Linux and FreeBSD (POSIX).
 *
 * Everything a child needs -- the program's path found in PATH, its
 * arguments and its environment -- is made before it is started, so that
 * a child of a forked thread does nothing but what is safe between fork and
 * exec.  A child with pipes is started by posix_spawn; one with a terminal
 * by forkpty, which makes the terminal its controlling one at its size.
 * Either starts with every signal's default action and none blocked.
 *
 * Two threads serve a process:
 *   kl-process-io    (one per process that uses a stream) waits in poll for
 *                    its output, its error, its input's room and its control
 *                    pipe, and moves the bytes between them and the streams;
 *   kl-process-wait  (one per process) waits for its end with waitid
 *                    (WNOWAIT), notes it under the lock and reaps it there,
 *                    so that a signal is never sent to a process ID reused.
 * After the end, the reading thread reads what the pipes still hold (a
 * terminal: until it reports its end, at most PROCESS_DRAIN_MS) and then
 * tells the end, after the output.
 *
 * A launch (kl_launch) is started through a child that makes a session of
 * its own, starts the program in a child of it and ends at once: the
 * application reaps that child, and the program, whose parent is gone, is
 * the system's to wait for.  No thread serves it.  A start that fails in
 * the program's child is told back through a pipe that its exec closes.
 */

#include "userland/desktop/libkeiland/process.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#if defined(__FreeBSD__)
#include <libutil.h>
#else
#include <pty.h>
#endif

/* How long a terminal's last output is read after its process ended, in milliseconds. */
#define PROCESS_DRAIN_MS	100

/* The most bytes moved at once between a descriptor and a stream. */
#define PROCESS_CHUNK		16384U

/* The PATH a program is looked for in when the environment has none. */
#define PROCESS_PATH_DEFAULT	"/bin:/usr/bin"

/* The system's command interpreter, which runs a launch's command line. */
#define PROCESS_SHELL	"/bin/sh"

/* What a started program reads when nothing writes its standard input. */
#define PROCESS_NULL_DEVICE	"/dev/null"

/*
 * The environment the application runs with.  FreeBSD defines it in the
 * program's start file rather than in its C library, so the library refers
 * to it weakly there (the program's own is used); elsewhere the C library
 * defines it.
 */
#if defined(__FreeBSD__)
extern char **environ __attribute__((weak));
#else
extern char **environ;
#endif

/* What a child is started with, made before it is started (freed after). */
struct process_launch {
	char *path;
	char **argv;
	char **envp;
};

static int process_find(const char *name, char **path);
static int process_environment(const char *const *changes, char ***envp);
static int process_arguments(const char *const *argv, char ***copy);
static void process_launch_free(struct process_launch *launch);
static void process_spawn_cleanup(int input[2], int output[2], int error_pipe[2]);
static int process_spawn(struct kl_process *process, const struct process_launch *launch, const char *directory);
static int process_spawn_terminal(struct kl_process *process, const struct process_launch *launch, const char *directory, unsigned columns, unsigned rows);
static void process_child_terminal(const struct process_launch *launch, const char *directory);
static void process_child_launch(const struct process_launch *launch, const char *directory, int report);
static void process_child_signals(void);
static void process_child_report(int report, int error);
static int process_pipe(int ends[2]);
static void process_close(int *descriptor);
static void *process_wait_run(void *argument);
static void *process_io_run(void *argument);
static int process_io_read(struct kl_process *process, int *descriptor, struct keiui_stream *stream);
static void process_io_write(struct kl_process *process, int *descriptor);
static void process_io_finish(struct kl_process *process);
static uint64_t process_now_ms(void);

/*
 * Starts a process's child and its threads.  Returns 0, or an errno value:
 * then no child runs, the process no longer reaches the application, and
 * the program's reference is the caller's to release (a waiting thread
 * that runs holds its own).
 */
int
keiui_child_start(
	struct kl_process *process,
	const struct kl_process_options *options)
{
	struct process_launch launch;
	int error;

	/* The path, the arguments and the environment, made now. */
	memset(&launch, 0, sizeof(launch));
	error = process_find(options->argv[0], &launch.path);
	if (error == 0)
		error = process_arguments(options->argv, &launch.argv);
	if (error == 0)
		error = process_environment(options->environment, &launch.envp);
	if (error == 0)
		error = process_pipe(process->child.control);

	/* Keeps the control pipe from blocking either side. */
	if (error == 0) {
		(void)fcntl(process->child.control[0], F_SETFL, O_NONBLOCK);
		(void)fcntl(process->child.control[1], F_SETFL, O_NONBLOCK);
	}

	/* Gives up when something the child needs could not be made. */
	if (error != 0) {
		process_launch_free(&launch);
		process_close(&process->child.control[0]);
		process_close(&process->child.control[1]);
		return error;
	}

	/* The child, on a terminal or with pipes. */
	if ((options->streams & KL_PROCESS_TERMINAL) != 0U)
		error = process_spawn_terminal(process, &launch, options->directory, options->columns, options->rows);
	else
		error = process_spawn(process, &launch, options->directory);

	/* Frees what the child was started with; it has its own copy now. */
	process_launch_free(&launch);

	/* Gives up when the child could not be started. */
	if (error != 0) {
		process_close(&process->child.control[0]);
		process_close(&process->child.control[1]);
		return error;
	}

	/* The thread that waits for the end, with its reference; without it the child is ended and reaped here. */
	process->references++;
	error = keiui_thread_start(process_wait_run, process);
	if (error != 0) {
		process->references--;
		(void)kill((pid_t)process->child.pid, SIGKILL);
		(void)waitpid((pid_t)process->child.pid, NULL, 0);
		process_io_finish(process);
		process->app = NULL;
		return error;
	}

	/* The thread that moves the bytes; without it the child is ended (the waiting thread reaps it). */
	if (options->streams != 0U) {
		process->references++;
		error = keiui_thread_start(process_io_run, process);
		if (error != 0) {
			process->references--;
			keiui_lock_take(&process->lock);

			process->closed = 1;
			process->app = NULL;
			(void)kill((pid_t)process->child.pid, SIGKILL);

			keiui_lock_give(&process->lock);
			process_io_finish(process);
			return error;
		}
	}

	/* Succeeded: it runs and is served. */
	return 0;
}

/*
 * Wakes a process's reading thread to look at its streams again.
 */
void
keiui_child_poke(
	struct kl_process *process)
{
	const char byte = 1;
	ssize_t written;

	/* A byte; a full pipe wakes it already. */
	do {
		written = write(process->child.control[1], &byte, 1);
	} while (written < 0 && errno == EINTR);
}

/*
 * Sends a process the signal for a way to end, unless it was reaped
 * (under the lock, so that the ID is still its).
 */
int
keiui_child_signal(
	struct kl_process *process,
	unsigned how)
{
	int signal_number;
	int status;
	int error;

	/* The signal. */
	signal_number = SIGTERM;
	if (how == KL_PROCESS_KILL)
		signal_number = SIGKILL;
	else if (how == KL_PROCESS_HANGUP && process->child.terminal >= 0)
		signal_number = SIGHUP;

	/* Sent while the process is not reaped; a terminal hangs up too. */
	keiui_lock_take(&process->lock);

	error = 0;
	if (!process->reaped) {
		if (how == KL_PROCESS_HANGUP && (process->streams & KL_PROCESS_TERMINAL) != 0U)
			process->child.hang_up = 1;
		status = kill((pid_t)process->child.pid, signal_number);
		if (status != 0 && errno != ESRCH)
			error = errno;
	}

	keiui_lock_give(&process->lock);
	keiui_child_poke(process);

	/* Refused. */
	if (error != 0)
		return error;

	/* Succeeded: asked. */
	return 0;
}

/*
 * Tells a process's terminal its size.
 */
int
keiui_child_resize(
	struct kl_process *process,
	unsigned columns,
	unsigned rows)
{
	struct winsize size;
	int status;
	int error;

	/* The size in characters (no pixels). */
	memset(&size, 0, sizeof(size));
	size.ws_col = (unsigned short)columns;
	size.ws_row = (unsigned short)rows;

	/* Told while the terminal is open (its thread closes it under the lock). */
	keiui_lock_take(&process->lock);

	error = 0;
	if (process->child.terminal >= 0) {
		status = ioctl(process->child.terminal, TIOCSWINSZ, &size);
		if (status != 0)
			error = errno;
	}

	keiui_lock_give(&process->lock);

	/* Refused. */
	if (error != 0)
		return error;

	/* Succeeded (a terminal that hung up has no size to tell). */
	return 0;
}

/*
 * Reports the echo and the line editing a process set on its terminal.
 */
int
keiui_child_mode(
	struct kl_process *process,
	unsigned *mode)
{
	struct termios attributes;
	int status;
	int error;

	/* The terminal's attributes, while it is open. */
	keiui_lock_take(&process->lock);

	error = EIO;
	if (process->child.terminal >= 0) {
		status = tcgetattr(process->child.terminal, &attributes);
		error = 0;
		if (status != 0)
			error = errno;
	}

	keiui_lock_give(&process->lock);
	if (error != 0)
		return error;

	/* The modes. */
	*mode = 0;
	if ((attributes.c_lflag & ECHO) != 0U)
		*mode |= KL_PROCESS_TERMINAL_ECHO;
	if ((attributes.c_lflag & ICANON) != 0U)
		*mode |= KL_PROCESS_TERMINAL_LINES;

	/* Succeeded. */
	return 0;
}

/*
 * Closes what is left of a process's side when its last reference goes:
 * its control pipe (the threads closed the rest).
 */
void
keiui_child_end(
	struct kl_process *process)
{
	/* The control pipe. */
	process_close(&process->child.control[0]);
	process_close(&process->child.control[1]);
}

/*
 * Starts a launch's program apart from the application, with the
 * environment's changes given (the activation token's among them).
 * Returns 0 once it runs, or an errno value: the program's path or the
 * memory for what it is started with is missing, or its child could not
 * start it.
 */
int
keiui_launch_start(
	const struct kl_launch_options *options,
	const char *const *environment)
{
	struct process_launch launch;
	const char *shell[4];
	const char *const *argv;
	int report[2];
	int child_error;
	ssize_t got;
	pid_t child;
	pid_t reaped;
	int status;
	int error;

	/* The program's arguments, or the command interpreter's on the command line. */
	argv = options->argv;
	if (options->command != NULL) {
		shell[0] = PROCESS_SHELL;
		shell[1] = "-c";
		shell[2] = options->command;
		shell[3] = NULL;
		argv = shell;
	}

	/* The path, the arguments, the environment and the pipe the start is told back by, made now. */
	memset(&launch, 0, sizeof(launch));
	error = process_find(argv[0], &launch.path);
	if (error == 0)
		error = process_arguments(argv, &launch.argv);
	if (error == 0)
		error = process_environment(environment, &launch.envp);
	if (error == 0)
		error = process_pipe(report);
	if (error != 0) {
		process_launch_free(&launch);
		return error;
	}

	/* The child, which starts the program in a session of its own and ends. */
	child = fork();
	if (child < 0) {
		error = errno;
		process_launch_free(&launch);
		process_close(&report[0]);
		process_close(&report[1]);
		return error;
	}

	/* Runs the program's launch in the child; it never comes back. */
	if (child == 0)
		process_child_launch(&launch, options->directory, report[1]);

	/* The child ends at once and is reaped here; the program is nobody's child. */
	process_close(&report[1]);
	process_launch_free(&launch);
	do {
		reaped = waitpid(child, &status, 0);
	} while (reaped < 0 && errno == EINTR);

	/*
	 * What came of the start: the pipe ends empty when the program runs
	 * (its exec closed it), or with the errno value of what failed.
	 */
	child_error = 0;
	do {
		got = read(report[0], &child_error, sizeof(child_error));
	} while (got < 0 && errno == EINTR);
	process_close(&report[0]);

	/* The program's child could not start it. */
	if (got == (ssize_t)sizeof(child_error))
		return child_error;

	/* Succeeded: the program runs. */
	return 0;
}

/*
 * Finds a program: a name with a '/' is its path, any other is looked for
 * in PATH.  Returns 0 with the path allocated, ENOENT, EACCES or ENOMEM.
 */
static int
process_find(
	const char *name,
	char **path)
{
	const char *search;
	const char *start;
	const char *end;
	const char *slash;
	size_t length;
	size_t name_length;
	char *candidate;
	int status;
	int error;

	/* A path. */
	*path = NULL;
	slash = strchr(name, '/');
	if (slash != NULL) {
		status = access(name, X_OK);
		if (status != 0)
			return errno;
		*path = strdup(name);
		if (*path == NULL)
			return ENOMEM;
		return 0;
	}

	/* The directories of PATH. */
	search = getenv("PATH");
	if (search == NULL || search[0] == '\0')
		search = PROCESS_PATH_DEFAULT;
	name_length = strlen(name);
	error = ENOENT;

	/* Each directory, until the program is found there (an empty one is the current directory). */
	for (start = search; ; start = end + 1) {
		end = strchr(start, ':');
		if (end == NULL)
			end = start + strlen(start);
		length = (size_t)(end - start);
		candidate = malloc(length + name_length + 3U);
		if (candidate == NULL)
			return ENOMEM;
		if (length == 0U)
			candidate[0] = '.';
		else
			memcpy(candidate, start, length);
		if (length == 0U)
			length = 1U;
		candidate[length] = '/';
		memcpy(candidate + length + 1U, name, name_length + 1U);
		status = access(candidate, X_OK);
		if (status == 0) {
			*path = candidate;
			error = 0;
			break;
		}

		/* Remembers that a program was found there but may not be run. */
		if (errno == EACCES)
			error = EACCES;

		/* Gives up this directory, and stops after the last one. */
		free(candidate);
		if (*end == '\0')
			break;
	}

	/* Reports a program that was not found (or found and not to be run). */
	if (error != 0)
		return error;

	/* Succeeded: the program's path is allocated. */
	return 0;
}

/*
 * Copies the arguments into an array of the child's own.  Returns 0 or
 * ENOMEM.
 */
static int
process_arguments(
	const char *const *argv,
	char ***copy)
{
	size_t count;
	size_t index;
	char **made;

	/* How many. */
	for (count = 0; argv[count] != NULL; count++)
		continue;

	/* The array and each argument. */
	made = calloc(count + 1U, sizeof(*made));
	if (made == NULL)
		return ENOMEM;
	for (index = 0; index < count; index++) {
		made[index] = strdup(argv[index]);
		if (made[index] == NULL) {
			*copy = made;
			return ENOMEM;
		}
	}

	/* Succeeded. */
	*copy = made;
	return 0;
}

/*
 * Makes the child's environment: the application's, without the variables
 * a change names, then the changes that set one.  Returns 0 or ENOMEM.
 */
static int
process_environment(
	const char *const *changes,
	char ***envp)
{
	const char *equals;
	size_t inherited;
	size_t added;
	size_t length;
	size_t index;
	size_t change;
	size_t used;
	char **made;
	int replaced;
	int same;

	/* How many there may be. */
	for (inherited = 0; environ != NULL && environ[inherited] != NULL; inherited++)
		continue;
	for (added = 0; changes != NULL && changes[added] != NULL; added++)
		continue;
	made = calloc(inherited + added + 1U, sizeof(*made));
	if (made == NULL)
		return ENOMEM;

	/* Each inherited variable that no change names. */
	used = 0;
	for (index = 0; index < inherited; index++) {
		replaced = 0;
		for (change = 0; change < added; change++) {
			equals = strchr(changes[change], '=');
			length = strlen(changes[change]);
			if (equals != NULL)
				length = (size_t)(equals - changes[change]);
			same = strncmp(environ[index], changes[change], length);
			if (same == 0 && environ[index][length] == '=') {
				replaced = 1;
				break;
			}
		}

		/* Leaves out a variable a change names. */
		if (replaced)
			continue;

		/* Copies the variable into the child's environment. */
		made[used] = strdup(environ[index]);
		if (made[used] == NULL) {
			*envp = made;
			return ENOMEM;
		}

		/* Counts the copy made. */
		used++;
	}

	/* Each change that sets a variable. */
	for (change = 0; change < added; change++) {
		equals = strchr(changes[change], '=');
		if (equals == NULL)
			continue;

		/* Copies the change into the child's environment. */
		made[used] = strdup(changes[change]);
		if (made[used] == NULL) {
			*envp = made;
			return ENOMEM;
		}

		/* Counts the copy made. */
		used++;
	}

	/* Succeeded. */
	*envp = made;
	return 0;
}

/* Frees what a child was started with. */
static void
process_launch_free(
	struct process_launch *launch)
{
	size_t index;

	/* The arguments, the environment and the path. */
	for (index = 0; launch->argv != NULL && launch->argv[index] != NULL; index++)
		free(launch->argv[index]);
	free(launch->argv);
	for (index = 0; launch->envp != NULL && launch->envp[index] != NULL; index++)
		free(launch->envp[index]);
	free(launch->envp);
	free(launch->path);
}

/*
 * Starts a child with pipes for the streams the program uses (an input it
 * does not use is the null device), by posix_spawn.  Returns 0 or an errno
 * value.
 */
static int
process_spawn(
	struct kl_process *process,
	const struct process_launch *launch,
	const char *directory)
{
	posix_spawn_file_actions_t actions;
	posix_spawnattr_t attributes;
	sigset_t defaults;
	sigset_t none;
	int input[2];
	int output[2];
	int error_pipe[2];
	pid_t pid;
	int error;

	/* The pipes of the streams used. */
	input[0] = input[1] = -1;
	output[0] = output[1] = -1;
	error_pipe[0] = error_pipe[1] = -1;
	error = 0;
	if ((process->streams & KL_PROCESS_INPUT) != 0U)
		error = process_pipe(input);
	if (error == 0 && (process->streams & KL_PROCESS_OUTPUT) != 0U)
		error = process_pipe(output);
	if (error == 0 && (process->streams & KL_PROCESS_ERROR) != 0U)
		error = process_pipe(error_pipe);

	/* Gives up when a pipe could not be made. */
	if (error != 0) {
		process_spawn_cleanup(input, output, error_pipe);
		return error;
	}

	/* The child's descriptors: the pipes' ends, the null device, nothing else. */
	error = posix_spawn_file_actions_init(&actions);
	if (error != 0) {
		process_spawn_cleanup(input, output, error_pipe);
		return error;
	}

	/* Gives the program its input: the pipe, or the null device. */
	if (input[0] >= 0)
		error = posix_spawn_file_actions_adddup2(&actions, input[0], STDIN_FILENO);
	else
		error = posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, PROCESS_NULL_DEVICE, O_RDONLY, 0);
	if (error == 0 && output[1] >= 0)
		error = posix_spawn_file_actions_adddup2(&actions, output[1], STDOUT_FILENO);
	if (error == 0 && error_pipe[1] >= 0)
		error = posix_spawn_file_actions_adddup2(&actions, error_pipe[1], STDERR_FILENO);
	if (error == 0 && directory != NULL)
		error = posix_spawn_file_actions_addchdir_np(&actions, directory);
	if (error == 0)
		error = posix_spawn_file_actions_addclosefrom_np(&actions, STDERR_FILENO + 1);

	/* Every signal's default action, none blocked. */
	if (error == 0)
		error = posix_spawnattr_init(&attributes);

	/* Gives up when the child's attributes could not be made. */
	if (error != 0) {
		(void)posix_spawn_file_actions_destroy(&actions);
		process_spawn_cleanup(input, output, error_pipe);
		return error;
	}

	/* Sets every signal to its default action and none blocked. */
	(void)sigfillset(&defaults);
	(void)sigdelset(&defaults, SIGKILL);
	(void)sigdelset(&defaults, SIGSTOP);
	(void)sigemptyset(&none);
	error = posix_spawnattr_setsigdefault(&attributes, &defaults);
	if (error == 0)
		error = posix_spawnattr_setsigmask(&attributes, &none);
	if (error == 0)
		error = posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSIGMASK);

	/* The child. */
	if (error == 0)
		error = posix_spawn(&pid, launch->path, &actions, &attributes, launch->argv, launch->envp);

	/* Frees the attributes and the descriptor actions, which the child has copied. */
	(void)posix_spawnattr_destroy(&attributes);
	(void)posix_spawn_file_actions_destroy(&actions);

	/* Gives up when the child could not be started. */
	if (error != 0) {
		process_spawn_cleanup(input, output, error_pipe);
		return error;
	}

	/* The child's ends close here; the application's do not block. */
	process_close(&input[0]);
	process_close(&output[1]);
	process_close(&error_pipe[1]);
	process->child.pid = (long)pid;
	process->child.input = input[1];
	process->child.output = output[0];
	process->child.error = error_pipe[0];
	if (input[1] >= 0)
		(void)fcntl(input[1], F_SETFL, O_NONBLOCK);
	if (output[0] >= 0)
		(void)fcntl(output[0], F_SETFL, O_NONBLOCK);
	if (error_pipe[0] >= 0)
		(void)fcntl(error_pipe[0], F_SETFL, O_NONBLOCK);

	/* Succeeded: it runs. */
	return 0;
}

/* Closes the pipes of a child that could not be started. */
static void
process_spawn_cleanup(
	int input[2],
	int output[2],
	int error_pipe[2])
{
	/* Each end of each pipe. */
	process_close(&input[0]);
	process_close(&input[1]);
	process_close(&output[0]);
	process_close(&output[1]);
	process_close(&error_pipe[0]);
	process_close(&error_pipe[1]);
}

/*
 * Starts a child on a terminal of its own at a size, by forkpty.  Returns
 * 0 or an errno value.
 */
static int
process_spawn_terminal(
	struct kl_process *process,
	const struct process_launch *launch,
	const char *directory,
	unsigned columns,
	unsigned rows)
{
	struct winsize size;
	int terminal;
	pid_t pid;
	int flags;

	/* The terminal's size. */
	memset(&size, 0, sizeof(size));
	size.ws_col = (unsigned short)columns;
	size.ws_row = (unsigned short)rows;

	/* The child on the terminal; it runs nothing but the program. */
	pid = forkpty(&terminal, NULL, NULL, &size);
	if (pid < 0)
		return errno;
	if (pid == 0)
		process_child_terminal(launch, directory);

	/* The application's end: not passed on, not blocking. */
	(void)fcntl(terminal, F_SETFD, FD_CLOEXEC);
	flags = fcntl(terminal, F_GETFL);
	if (flags >= 0)
		(void)fcntl(terminal, F_SETFL, flags | O_NONBLOCK);
	process->child.pid = (long)pid;
	process->child.terminal = terminal;

	/* Succeeded: it runs. */
	return 0;
}

/*
 * Runs the program in a terminal's child: only what is safe after a fork
 * of a process with threads.
 */
static void
process_child_terminal(
	const struct process_launch *launch,
	const char *directory)
{
	int status;

	/* Every signal's default action, none blocked. */
	process_child_signals();

	/* Enters the directory. */
	if (directory != NULL) {
		status = chdir(directory);
		if (status != 0)
			_exit(127);
	}

	/* Leaves nothing open but the terminal, and runs the program. */
	closefrom(STDERR_FILENO + 1);
	(void)execve(launch->path, launch->argv, launch->envp);
	_exit(127);
}

/*
 * Starts a launch's program from the child that fork made: only what is
 * safe after a fork of a process with threads.  The child makes a session
 * of its own and ends once it made the program's; the program's child
 * starts with the signals' default actions, an empty input, nothing open
 * but the standard streams, and the directory.  What fails is told through
 * report.
 */
static void
process_child_launch(
	const struct process_launch *launch,
	const char *directory,
	int report)
{
	pid_t grandchild;
	int null_device;
	int status;

	/* A session of its own, and the program's child; this one ends. */
	(void)setsid();
	grandchild = fork();
	if (grandchild < 0)
		process_child_report(report, errno);
	if (grandchild > 0)
		_exit(0);

	/* Every signal's default action, none blocked. */
	process_child_signals();

	/* The report's end, kept just above the standard streams (its exec still closes it); nothing else open. */
	if (report != STDERR_FILENO + 1) {
		status = dup2(report, STDERR_FILENO + 1);
		if (status < 0)
			_exit(127);
		report = STDERR_FILENO + 1;
		(void)fcntl(report, F_SETFD, FD_CLOEXEC);
	}

	/* Leaves nothing else open. */
	closefrom(report + 1);

	/* Gives the program an empty input. */
	null_device = open(PROCESS_NULL_DEVICE, O_RDONLY);
	if (null_device < 0)
		process_child_report(report, errno);
	if (null_device != STDIN_FILENO) {
		status = dup2(null_device, STDIN_FILENO);
		if (status < 0)
			process_child_report(report, errno);
		(void)close(null_device);
	}

	/* The directory. */
	if (directory != NULL) {
		status = chdir(directory);
		if (status != 0)
			process_child_report(report, errno);
	}

	/* The program; only a failed exec comes back. */
	(void)execve(launch->path, launch->argv, launch->envp);
	process_child_report(report, errno);
}

/*
 * Gives every signal its default action and blocks none, in a child that
 * fork made (only calls safe there).
 */
static void
process_child_signals(void)
{
	struct sigaction action;
	sigset_t none;
	int number;

	/* Each signal's default action. */
	memset(&action, 0, sizeof(action));
	action.sa_handler = SIG_DFL;
	for (number = 1; number < NSIG; number++)
		(void)sigaction(number, &action, NULL);

	/* None blocked. */
	(void)sigemptyset(&none);
	(void)sigprocmask(SIG_SETMASK, &none, NULL);
}

/*
 * Tells the application why a launch's child could not start the program,
 * and ends the child.
 */
static void
process_child_report(
	int report,
	int error)
{
	ssize_t written;

	/* The errno value, whole or not at all (the pipe is empty). */
	do {
		written = write(report, &error, sizeof(error));
	} while (written < 0 && errno == EINTR);
	_exit(127);
}

/* Makes a pipe whose ends are not passed on.  Returns 0 or an errno value. */
static int
process_pipe(
	int ends[2])
{
	int status;

	/* The pipe. */
	status = pipe2(ends, O_CLOEXEC);
	if (status != 0) {
		ends[0] = -1;
		ends[1] = -1;
		return errno;
	}

	/* Succeeded. */
	return 0;
}

/* Closes a descriptor that is open and marks it closed. */
static void
process_close(
	int *descriptor)
{
	/* Open ones only. */
	if (*descriptor < 0)
		return;
	(void)close(*descriptor);
	*descriptor = -1;
}

/*
 * The thread that waits for a process's end: the end noted and the process
 * reaped under the lock, then the reading thread (or, without one, the
 * application) told.
 */
static void *
process_wait_run(
	void *argument)
{
	struct kl_process *process;
	siginfo_t information;
	pid_t pid;
	int status;

	/* The process; its ID is read once (it never changes). */
	keiui_thread_name("kl-process-wait");
	process = argument;
	pid = (pid_t)process->child.pid;

	/* Its end, without reaping it yet. */
	memset(&information, 0, sizeof(information));
	do {
		status = waitid(P_PID, (id_t)pid, &information, WEXITED | WNOWAIT);
	} while (status != 0 && errno == EINTR);

	/* How it ended, and reaped while nobody signals it. */
	keiui_lock_take(&process->lock);

	process->how = KL_PROCESS_EXITED;
	process->code = 127;
	if (status == 0 && information.si_code == CLD_EXITED)
		process->code = information.si_status;
	else if (status == 0) {
		process->how = KL_PROCESS_SIGNALED;
		process->code = information.si_status;
	}

	/* Reaps the process while nobody signals it. */
	do {
		status = waitpid(pid, NULL, 0);
	} while (status < 0 && errno == EINTR);
	process->reaped = 1;

	/* Without a reading thread the end is told now. */
	if (process->streams == 0U) {
		process->ready |= KL_PROCESS_READY_EXIT;
		keiui_process_tell(process);
	}

	keiui_lock_give(&process->lock);

	/* The reading thread reads what is left, then tells the end. */
	if (process->streams != 0U)
		keiui_child_poke(process);
	keiui_process_release(process);
	return NULL;
}

/*
 * The thread that moves a process's bytes: its output and error into
 * their streams while they have room, its input from its stream while
 * some waits, until the program closes the process or the process ended
 * and its output was read.
 */
static void *
process_io_run(
	void *argument)
{
	struct kl_process *process;
	struct pollfd descriptors[4];
	struct pollfd *output;
	struct pollfd *error;
	struct pollfd *input;
	uint64_t drain_until;
	unsigned used;
	size_t output_room;
	size_t error_room;
	uint64_t now;
	ssize_t got;
	char bytes[64];
	int timeout;
	int closed;
	int reaped;
	int full;
	int status;

	/* The process. */
	keiui_thread_name("kl-process-io");
	process = argument;
	drain_until = 0;

	/* Each round: what to wait for, the wait, then what became ready. */
	for (;;) {
		keiui_lock_take(&process->lock);

		closed = process->closed;
		reaped = process->reaped;
		output_room = keiui_stream_room(&process->output);
		error_room = keiui_stream_room(&process->error);

		/* A stream with no room stops the final read until the program reads it. */
		full = 0;
		if (process->child.output >= 0 && output_room == 0U)
			full = 1;
		else if (process->child.error >= 0 && error_room == 0U)
			full = 1;

		/* A terminal to hang up, and an input to close once written. */
		if (process->child.hang_up && process->child.terminal >= 0) {
			process_close(&process->child.terminal);
			process->output.ended = 1;
			process->input_closed = 1;
		}

		/* An input whose writing is done closes. */
		if (process->input_closing && process->input.count == 0U && process->child.input >= 0) {
			process_close(&process->child.input);
			process->input_closed = 1;
		}

		/* The control pipe, then the streams that can move. */
		used = 0;
		descriptors[used].fd = process->child.control[0];
		descriptors[used].events = POLLIN;
		descriptors[used].revents = 0;
		used++;
		output = NULL;
		error = NULL;
		input = NULL;
		if (process->child.terminal >= 0) {
			output = &descriptors[used];
			output->fd = process->child.terminal;
			output->events = 0;
			if (output_room != 0U)
				output->events |= POLLIN;
			if (process->input.count != 0U)
				output->events |= POLLOUT;
			output->revents = 0;
			used++;
		}

		/* The output pipe, while its stream has room. */
		if (process->child.output >= 0 && output_room != 0U) {
			output = &descriptors[used];
			output->fd = process->child.output;
			output->events = POLLIN;
			output->revents = 0;
			used++;
		}

		/* The error pipe, while its stream has room. */
		if (process->child.error >= 0 && error_room != 0U) {
			error = &descriptors[used];
			error->fd = process->child.error;
			error->events = POLLIN;
			error->revents = 0;
			used++;
		}

		/* The input pipe, while input waits. */
		if (process->child.input >= 0 && process->input.count != 0U) {
			input = &descriptors[used];
			input->fd = process->child.input;
			input->events = POLLOUT;
			input->revents = 0;
			used++;
		}

		keiui_lock_give(&process->lock);

		/* The program closed it: the streams close, the thread ends. */
		if (closed)
			break;

		/*
		 * After the end, read what is left without waiting (a terminal: a
		 * while; a full stream: until the program reads), then tell the end.
		 */
		timeout = -1;
		if (reaped && !full) {
			if (drain_until == 0U)
				drain_until = process_now_ms() + PROCESS_DRAIN_MS;
			timeout = 0;
			now = process_now_ms();
			if (process->child.terminal >= 0 && now < drain_until)
				timeout = (int)(drain_until - now);
			if (process->child.terminal < 0 && process->child.output < 0 && process->child.error < 0)
				break;
		}

		/* The wait. */
		status = poll(descriptors, (nfds_t)used, timeout);
		if (status < 0 && errno == EINTR)
			continue;
		if (status < 0)
			break;

		/* Nothing more after the end: the rest is not waited for. */
		if (status == 0 && reaped && !full)
			break;

		/* The control pipe's bytes. */
		if ((descriptors[0].revents & POLLIN) != 0) {
			do {
				got = read(process->child.control[0], bytes, sizeof(bytes));
			} while (got > 0);
		}

		/* A terminal is written to when it can take input. */
		if (output != NULL && output->fd == process->child.terminal && (output->revents & POLLOUT) != 0)
			process_io_write(process, &process->child.terminal);

		/* The output (a terminal's too) is read when it has bytes or ended. */
		if (output != NULL && (output->revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
			if (output->fd == process->child.terminal)
				(void)process_io_read(process, &process->child.terminal, &process->output);
			else
				(void)process_io_read(process, &process->child.output, &process->output);
		}

		/* The error is read when it has bytes or ended. */
		if (error != NULL && (error->revents & (POLLIN | POLLHUP | POLLERR)) != 0)
			(void)process_io_read(process, &process->child.error, &process->error);

		/* The input is written when its pipe can take bytes or broke. */
		if (input != NULL && (input->revents & (POLLOUT | POLLHUP | POLLERR)) != 0)
			process_io_write(process, &process->child.input);
	}

	/* The streams close, the end is told, and the thread's reference goes. */
	process_io_finish(process);
	keiui_process_release(process);
	return NULL;
}

/*
 * Reads what a descriptor has into its stream (as much as there is room
 * for), and tells the application when the stream was empty.  The end of
 * the descriptor (a terminal: an error, once its other side closed) ends
 * the stream.  Returns how many bytes were read.
 */
static int
process_io_read(
	struct kl_process *process,
	int *descriptor,
	struct keiui_stream *stream)
{
	unsigned char bytes[PROCESS_CHUNK];
	size_t room;
	ssize_t got;
	int empty;

	/* No more than there is room for. */
	keiui_lock_take(&process->lock);

	room = keiui_stream_room(stream);

	keiui_lock_give(&process->lock);
	if (room > sizeof(bytes))
		room = sizeof(bytes);
	if (room == 0U)
		return 0;

	/* The bytes. */
	do {
		got = read(*descriptor, bytes, room);
	} while (got < 0 && errno == EINTR);
	if (got < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
		return 0;

	/* Into the stream, or the stream's end (a terminal's end is an error). */
	keiui_lock_take(&process->lock);

	if (got > 0) {
		empty = 0;
		if (stream->count == 0U)
			empty = 1;
		keiui_stream_put(stream, bytes, (size_t)got);
		if (empty)
			keiui_process_tell(process);
	} else {
		process_close(descriptor);
		stream->ended = 1;
		if (stream->count == 0U)
			keiui_process_tell(process);
	}

	keiui_lock_give(&process->lock);

	/* What was read. */
	if (got < 0)
		return 0;
	return (int)got;
}

/*
 * Writes what waits of the input to a descriptor, and tells the
 * application of the room when a short write wanted it.  A broken pipe
 * closes the input.
 */
static void
process_io_write(
	struct kl_process *process,
	int *descriptor)
{
	unsigned char bytes[PROCESS_CHUNK];
	size_t count;
	ssize_t written;
	size_t index;
	size_t at;
	size_t room;

	/* A copy of what waits (taken out only once written). */
	keiui_lock_take(&process->lock);

	count = process->input.count;
	if (count > sizeof(bytes))
		count = sizeof(bytes);
	for (index = 0; index < count; index++) {
		at = (process->input.first + index) % KL_PROCESS_BUFFER;
		bytes[index] = process->input.bytes[at];
	}

	keiui_lock_give(&process->lock);
	if (count == 0U)
		return;

	/* The write. */
	do {
		written = write(*descriptor, bytes, count);
	} while (written < 0 && errno == EINTR);
	if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
		return;

	/* What was written goes; a broken input drops the rest and refuses more. */
	keiui_lock_take(&process->lock);

	if (written > 0) {
		(void)keiui_stream_take(&process->input, NULL, (size_t)written);
	} else {
		(void)keiui_stream_take(&process->input, NULL, process->input.count);
		process->input_closed = 1;
		if (descriptor == &process->child.input)
			process_close(descriptor);
	}

	/* Tells the application of the room a short write wanted. */
	room = keiui_stream_room(&process->input);
	if (process->input_wanted && room != 0U) {
		process->input_wanted = 0;
		process->ready |= KL_PROCESS_READY_INPUT;
		keiui_process_tell(process);
	}

	keiui_lock_give(&process->lock);
}

/*
 * Ends the reading thread's work: the descriptors close, the streams end,
 * and the end is told when the process was reaped (after the output read).
 */
static void
process_io_finish(
	struct kl_process *process)
{
	/* Under the lock, so that nobody uses a descriptor being closed. */
	keiui_lock_take(&process->lock);

	process_close(&process->child.terminal);
	process_close(&process->child.output);
	process_close(&process->child.error);
	process_close(&process->child.input);
	process->output.ended = 1;
	process->error.ended = 1;
	process->input_closed = 1;
	if (process->reaped && !process->closed) {
		process->ready |= KL_PROCESS_READY_EXIT;
		keiui_process_tell(process);
	}

	keiui_lock_give(&process->lock);
}

/* Reports a monotonic time in milliseconds. */
static uint64_t
process_now_ms(void)
{
	struct timespec now;

	/* The monotonic clock. */
	(void)clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
}
