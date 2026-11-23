/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Other programs mikuterm runs and reads, on the POSIX systems
 * (program.h).
 *
 * A program named without a folder is looked for in the folders of PATH
 * only (an empty one, the current directory, is passed over), so a file
 * left in the directory the user happens to be in is not run in its
 * place.  A path must be one from the root.
 *
 * A program run gets its arguments split from one line as a shell splits
 * a simple command (proxy-command.c), without a shell; it reads the null
 * device, and its output and errors go to one pipe, which is collected.
 * It runs in a process group of its own with every other descriptor
 * closed.  The wait has a limit and gives up when the tab closes; a
 * program given up is ended with its group.
 */

#include "program.h"
#include "mikuterm.h"
#include "proxy-command.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* How long one round of the wait is, in milliseconds, and the bytes read at once. */
#define PROGRAM_ROUND_MS	50
#define PROGRAM_CHUNK		4096U

/* The most arguments a run has, the program's own among them. */
#define PROGRAM_WORDS_MAX	64U

/* The longest line about arguments that cannot be read (not shown: the run fails with EINVAL). */
#define PROGRAM_ERROR_MAX	PROXY_COMMAND_ERROR_MAX

/* What a run reads in place of input. */
#define PROGRAM_NULL_DEVICE	"/dev/null"

/* The exit code given for a program a signal ended: 128 and the signal's number, as the shells give it. */
#define PROGRAM_SIGNALED	128UL

extern char **environ;

static int program_is_file(const char *path);
static int program_spawn(const char *program, char **arguments, int output, pid_t *pid);
static int program_end(pid_t pid, unsigned long *code, int kill_first);
static uint64_t program_now_ms(void);

/*
 * Finds a program: a path from the root is taken when a file the user
 * may run is there, and a name without a folder is looked for in each
 * folder of PATH in turn.  Writes its path into path (size bytes).
 * Returns 0, ENOENT when it is nowhere, EINVAL for a name that names a
 * folder relative to the current one, or ENAMETOOLONG.
 */
int
mikuterm_program_find(
	const char *name,
	mikuterm_program_char *path,
	size_t size)
{
	const char *search;
	const char *folder;
	const char *end;
	const char *slash;
	size_t length;
	size_t folder_length;
	int written;
	int found;

	/* A name with a folder in it must be a path from the root. */
	length = strlen(name);
	if (length == 0U)
		return EINVAL;
	slash = strchr(name, '/');
	if (slash != NULL) {
		if (name[0] != '/')
			return EINVAL;
		if (length >= size)
			return ENAMETOOLONG;
		found = program_is_file(name);
		if (!found)
			return ENOENT;
		memcpy(path, name, length + 1U);
		return 0;
	}

	/* Each folder of PATH in turn (none: not found). */
	search = getenv("PATH");
	if (search == NULL)
		return ENOENT;
	for (folder = search; *folder != '\0'; folder = end) {
		/* The folder up to the next colon. */
		end = strchr(folder, ':');
		if (end == NULL)
			end = folder + strlen(folder);
		folder_length = (size_t)(end - folder);
		if (*end == ':')
			end++;

		/* An empty folder, or one not from the root, is the current directory's: passed over. */
		if (folder_length == 0U || folder[0] != '/')
			continue;

		/* The name in the folder, when it is a program there. */
		written = snprintf(path, size, "%.*s/%s", (int)folder_length, folder, name);
		if (written < 0 || (size_t)written >= size)
			continue;
		found = program_is_file(path);
		if (found)
			return 0;
	}

	/* Not in any folder. */
	path[0] = '\0';
	return ENOENT;
}

/*
 * Runs a program with the arguments of one line (as a shell splits a
 * simple command, without a shell) and collects what it writes, its
 * errors among it, up to capacity bytes (the rest is read and dropped):
 * *size bytes, and its exit code in *code (128 and the signal's number
 * for one a signal ended).  It may take milliseconds; stop is asked each
 * round and a true answer gives up.  Returns 0, ENOENT when the program
 * is not there, EINVAL for arguments that cannot be read, ETIMEDOUT,
 * ECANCELED (stop), or another errno value; a program given up is ended.
 */
int
mikuterm_program_run(
	const mikuterm_program_char *program,
	const char *arguments,
	unsigned milliseconds,
	int (*stop)(void *data),
	void *data,
	unsigned char *output,
	size_t capacity,
	size_t *size,
	unsigned long *code)
{
	char *words[PROGRAM_WORDS_MAX + 2U];
	char error_text[PROGRAM_ERROR_MAX];
	unsigned char chunk[PROGRAM_CHUNK];
	struct pollfd wait;
	char *buffer;
	size_t buffer_size;
	size_t room;
	unsigned count;
	uint64_t deadline;
	uint64_t now;
	ssize_t got;
	pid_t pid;
	int pipe_ends[2];
	int ready;
	int stopped;
	int error;

	/* Nothing yet. */
	*size = 0U;
	*code = 0UL;

	/* The arguments after the program's own. */
	buffer_size = strlen(arguments) + 1U;
	buffer = malloc(buffer_size);
	if (buffer == NULL)
		return ENOMEM;
	error = proxy_command_words(arguments, buffer, buffer_size, words + 1, PROGRAM_WORDS_MAX, &count, error_text, sizeof(error_text));
	if (error == ENOENT) {
		count = 0U;
		error = 0;
	}
	if (error != 0) {
		free(buffer);
		return EINVAL;
	}
	words[0] = (char *)program;
	words[count + 1U] = NULL;

	/* The pipe its output and errors go to. */
	error = pipe2(pipe_ends, O_CLOEXEC);
	if (error != 0) {
		error = errno;
		free(buffer);
		return error;
	}

	/* The program; the pipe's write end is its own after. */
	error = program_spawn(program, words, pipe_ends[1], &pid);
	(void)close(pipe_ends[1]);
	free(buffer);
	if (error != 0) {
		(void)close(pipe_ends[0]);
		return error;
	}

	/* What it writes until it closes the pipe, the time is up, or the tab closes. */
	deadline = program_now_ms() + milliseconds;
	error = 0;
	for (;;) {
		/* The tab closed. */
		stopped = 0;
		if (stop != NULL)
			stopped = stop(data);
		if (stopped) {
			error = ECANCELED;
			break;
		}

		/* The time is up. */
		now = program_now_ms();
		if (now >= deadline) {
			error = ETIMEDOUT;
			break;
		}

		/* Something to read, or a round's wait. */
		wait.fd = pipe_ends[0];
		wait.events = POLLIN;
		wait.revents = 0;
		ready = poll(&wait, 1U, PROGRAM_ROUND_MS);
		if (ready < 0 && errno != EINTR) {
			error = errno;
			break;
		}
		if (ready <= 0)
			continue;

		/* What came: kept while there is room; the pipe's end is the program's. */
		got = read(pipe_ends[0], chunk, sizeof(chunk));
		if (got < 0 && errno == EINTR)
			continue;
		if (got <= 0)
			break;
		room = capacity - *size;
		if ((size_t)got < room)
			room = (size_t)got;
		memcpy(output + *size, chunk, room);
		*size += room;
	}
	(void)close(pipe_ends[0]);

	/* The program ends: reaped when it closed the pipe, ended with its group when given up. */
	if (error != 0) {
		(void)program_end(pid, code, 1);
		*size = 0U;
		return error;
	}
	error = program_end(pid, code, 0);
	if (error != 0)
		return error;

	/* Succeeded: what it wrote and its exit code. */
	return 0;
}

/* Tells whether a path is a file the user may run (1) or not (0). */
static int
program_is_file(
	const char *path)
{
	struct stat status;
	int error;

	/* A file, not a directory. */
	error = stat(path, &status);
	if (error != 0)
		return 0;
	if (!S_ISREG(status.st_mode))
		return 0;

	/* One the user may run. */
	error = access(path, X_OK);
	if (error != 0)
		return 0;

	/* Runs. */
	return 1;
}

/*
 * Starts a program in a process group of its own, reading the null
 * device, with output as its standard output and errors, the signals as a
 * new program has them, and every other descriptor closed.  Returns 0
 * with *pid, or posix_spawn's error.
 */
static int
program_spawn(
	const char *program,
	char **arguments,
	int output,
	pid_t *pid)
{
	posix_spawn_file_actions_t actions;
	posix_spawnattr_t attributes;
	sigset_t defaults;
	sigset_t none;
	int error;

	/* Its three standard descriptors, and none other. */
	error = posix_spawn_file_actions_init(&actions);
	if (error != 0)
		return error;
	error = posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, PROGRAM_NULL_DEVICE, O_RDONLY, 0);
	if (error == 0)
		error = posix_spawn_file_actions_adddup2(&actions, output, STDOUT_FILENO);
	if (error == 0)
		error = posix_spawn_file_actions_adddup2(&actions, output, STDERR_FILENO);
	if (error == 0)
		error = posix_spawn_file_actions_addclosefrom_np(&actions, STDERR_FILENO + 1);
	if (error != 0) {
		(void)posix_spawn_file_actions_destroy(&actions);
		return error;
	}

	/* Its own process group, and the signals of a program started anew. */
	error = posix_spawnattr_init(&attributes);
	if (error != 0) {
		(void)posix_spawn_file_actions_destroy(&actions);
		return error;
	}
	(void)sigfillset(&defaults);
	(void)sigdelset(&defaults, SIGKILL);
	(void)sigdelset(&defaults, SIGSTOP);
	(void)sigemptyset(&none);
	error = posix_spawnattr_setsigdefault(&attributes, &defaults);
	if (error == 0)
		error = posix_spawnattr_setsigmask(&attributes, &none);
	if (error == 0)
		error = posix_spawnattr_setpgroup(&attributes, 0);
	if (error == 0)
		error = posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETPGROUP);

	/* The program. */
	if (error == 0)
		error = posix_spawn(pid, program, &actions, &attributes, arguments, environ);
	(void)posix_spawnattr_destroy(&attributes);
	(void)posix_spawn_file_actions_destroy(&actions);
	if (error != 0)
		return error;

	/* Succeeded: it runs. */
	return 0;
}

/*
 * Reaps a program (its process group killed first when asked) and gives
 * its exit code.  Returns 0, or waitpid's error.
 */
static int
program_end(
	pid_t pid,
	unsigned long *code,
	int kill_first)
{
	pid_t done;
	int status;

	/* A program given up ends with every program of its group. */
	if (kill_first)
		(void)kill(-pid, SIGKILL);

	/* Its end. */
	do {
		done = waitpid(pid, &status, 0);
	} while (done < 0 && errno == EINTR);
	if (done < 0)
		return errno;

	/* Its exit code, or 128 and the signal that ended it. */
	*code = 0UL;
	if (WIFEXITED(status))
		*code = (unsigned long)WEXITSTATUS(status);
	else if (WIFSIGNALED(status))
		*code = PROGRAM_SIGNALED + (unsigned long)WTERMSIG(status);

	/* Succeeded: reaped. */
	return 0;
}

/* The monotonic clock in milliseconds. */
static uint64_t
program_now_ms(void)
{
	struct timespec now;

	/* The clock that does not go back. */
	(void)clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
}
